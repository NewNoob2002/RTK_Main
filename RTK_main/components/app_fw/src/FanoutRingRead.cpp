// FanoutRing 读侧实现：读取消费、读指针统计与类型化读侧统计（ADR-0007 §3.6-1）。纯 C++20。
// 只由调用者在任务上下文调用；不阻塞、不分配堆；临界区经注入的 LockOps 界定。

#include "fw/FanoutRing.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include "FanoutRingRing.hpp"

namespace fw {

namespace {

using detail::readHeaderLen;
using detail::ringRead;

} // namespace

/**
 * @brief  读取一块最老的数据
 * @param  readerId: 读者编号
 * @param[out] out: 输出缓冲
 * @param[out] len: 实际长度；空间不足时为需要的长度
 * @retval fw::Result 读取结果
 */
Result FanoutRing::read(uint8_t readerId, std::span<uint8_t> out, size_t &len) noexcept
{
    len = 0U;
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (typed_) {
        return Result::kNotSupported;
    }
    if (readerId >= kFanoutMaxReaders) {
        return Result::kInvalidArg;
    }
    const LockGuard guard(lock_);

    Reader &reader = readers_[readerId];
    if (!reader.attached) {
        return Result::kInvalidArg;
    }
    if (reader.tail == head_) {
        return Result::kNotFound;
    }

    const std::span<const uint8_t> ring(buf_, cap_);
    const uint16_t payloadLen = readHeaderLen(ring, oldest_, reader.tail);
    const size_t payloadSz = static_cast<size_t>(payloadLen);
    len = payloadSz;
    if (out.size() < payloadSz) {
        return Result::kNoSpace;
    }

    const uint32_t payloadPos = reader.tail + static_cast<uint32_t>(kFanoutHeaderBytes);
    ringRead(ring, oldest_, payloadPos, out.first(payloadSz));

    reader.tail += static_cast<uint32_t>(kFanoutHeaderBytes + payloadSz);
    ++reader.stats.records;
    reader.stats.bytes += static_cast<uint32_t>(payloadSz);
    return Result::kOk;
}

/**
 * @brief  读取单个读者的统计快照
 * @param  readerId: 读者编号
 * @retval 统计快照（非法编号返回空统计）
 */
FanoutReaderStats FanoutRing::readerStats(uint8_t readerId) const noexcept
{
    if ((!inited_) || (readerId >= kFanoutMaxReaders)) {
        return FanoutReaderStats{};
    }
    const LockGuard guard(lock_);
    if (!readers_[readerId].attached) {
        return FanoutReaderStats{};
    }
    return readers_[readerId].stats;
}

/**
 * @brief  读取写侧统计快照
 * @retval 统计快照
 */
FanoutWriterStats FanoutRing::writerStats() const noexcept
{
    if (!inited_) {
        return FanoutWriterStats{};
    }
    const LockGuard guard(lock_);
    return wstats_;
}

// 类型化记录长度：把内部缓冲包装成 span 交给 readHeaderLen（类型适配，非纯转发）。
/**
 * @brief  读取指定环位置的记录长度
 * @param  pos: 环内位置
 * @retval 记录长度；越界或数据不足时为 0
 */
uint16_t FanoutRing::typedLength(uint32_t pos) const noexcept
{
    return readHeaderLen({buf_, cap_}, 0U, pos);
}

/**
 * @brief  读取指定环位置的记录类型
 * @param  pos: 环内位置
 * @retval 记录类型；越界或数据不足时为 0
 */
uint8_t FanoutRing::typedType(uint32_t pos) const noexcept
{
    // cap 是 2 的幂且 <2^31；低位掩码在 u32 回绕前后保持同一物理位置。
    return std::span<const uint8_t>(buf_, cap_)[(size_t{pos} + kFanoutHeaderBytes) & (cap_ - 1U)];
}

/**
 * @brief  读取单个读者的类型化统计快照
 * @param  readerId: 读者编号
 * @retval 统计快照（非法编号返回空统计）
 */
FanoutTypedReaderStats FanoutRing::typedReaderStats(uint8_t readerId) const noexcept
{
    if (!inited_ || !typed_) {
        return {};
    }
    const LockGuard guard(lock_);
    if ((readerId >= readers_.size()) || !readers_[readerId].attached) {
        return {};
    }
    const Reader &reader = readers_[readerId];
    return {reader.stats, reader.filtered, reader.pinnedDrop};
}

/**
 * @brief  读取写侧类型化统计快照
 * @retval 统计快照
 */
FanoutTypedWriterStats FanoutRing::typedWriterStats() const noexcept
{
    if (!inited_ || !typed_) {
        return {};
    }
    const LockGuard guard(lock_);
    FanoutTypedWriterStats stats = typed_wstats_;
    stats.base = wstats_;
    return stats;
}

} // namespace fw
