// GNSS App 优先级记录环实现（P1 Rev B）：优先保留 GGA，满环按 GSV→GSA→GST→RMC→其它→GGA 淘汰。
// 线程安全：所有共享状态由注入的 fw::LockOps 保护；不阻塞、不分配堆，非 ISR 安全。

#include "data/GnssAppRing.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>

#include "gnss/Types.hpp"

namespace app::data {

namespace {

constexpr size_t kHeaderBytes = fw::kFanoutTypedHeaderBytes;
constexpr size_t kBitsPerByte = 8U;
constexpr size_t kByteMask = 0xFFU;
constexpr uint8_t kGgaType = static_cast<uint8_t>(::gnss::OutputMsg::kGga);
constexpr uint8_t kPrioOther = 0U;
constexpr uint8_t kPrioGsv = 1U;
constexpr uint8_t kPrioGsa = 2U;
constexpr uint8_t kPrioGst = 3U;
constexpr uint8_t kPrioRmc = 4U;
constexpr uint8_t kPrioGga = 10U;

/**
 * @brief 返回语句类型的保留优先级（数值越小越先淘汰，GGA 最高最后淘汰）。
 * @param type: 语句类型下标（gnss::OutputMsg）。
 * @retval 0=其它非GGA（如 SATSINFOA/HEADINGA），1=GSV，2=GSA，3=GST，4=RMC，10=GGA。
 * @note 纯函数，线程/ISR 安全；确保 NMEA 满环严格按 GSV→GSA→GST→RMC→GGA 淘汰，且大体积 SATS 不会挤掉 NMEA。
 */
constexpr uint8_t retentionPriority(uint8_t type) noexcept
{
    switch (type) {
    case static_cast<uint8_t>(::gnss::OutputMsg::kGsv):
        return kPrioGsv;
    case static_cast<uint8_t>(::gnss::OutputMsg::kGsa):
        return kPrioGsa;
    case static_cast<uint8_t>(::gnss::OutputMsg::kGst):
        return kPrioGst;
    case static_cast<uint8_t>(::gnss::OutputMsg::kRmc):
        return kPrioRmc;
    case static_cast<uint8_t>(::gnss::OutputMsg::kGga):
        return kPrioGga;
    default:
        return kPrioOther;
    }
}

} // namespace

fw::Result GnssAppRing::initTyped(std::span<uint8_t> storage, size_t maxRecord,
                                  const fw::LockOps &lock) noexcept
{
    if (inited_) {
        return fw::Result::kInvalidState;
    }
    if ((storage.data() == nullptr) || (maxRecord == 0U) ||
        (maxRecord > std::numeric_limits<uint16_t>::max()) ||
        (storage.size() < (maxRecord + kHeaderBytes)) ||
        (storage.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) ||
        ((lock.lock == nullptr) != (lock.unlock == nullptr))) {
        return fw::Result::kInvalidArg;
    }
    lock_ = lock;
    buf_ = storage.data();
    cap_ = storage.size();
    max_record_ = maxRecord;
    read_pos_ = 0U;
    write_pos_ = 0U;
    reader_ = Reader{};
    wstats_ = fw::FanoutWriterStats{};
    typed_wstats_ = fw::FanoutTypedWriterStats{};
    gga_drop_ = 0U;
    inited_ = true;
    return fw::Result::kOk;
}

fw::Result GnssAppRing::attach(fw::FanoutMask mask, uint8_t &readerId) noexcept
{
    if (!inited_) {
        return fw::Result::kInvalidState;
    }
    const fw::LockGuard guard(lock_);
    if (reader_.attached) {
        return fw::Result::kNoSpace;
    }
    if (reader_.generation == std::numeric_limits<uint32_t>::max()) {
        return fw::Result::kInvalidState;
    }
    const uint32_t nextGen = reader_.generation + 1U;
    reader_ = Reader{};
    reader_.attached = true;
    reader_.mask = mask;
    reader_.generation = nextGen;
    read_pos_ = 0U;
    write_pos_ = 0U;
    readerId = 0U;
    return fw::Result::kOk;
}

fw::Result GnssAppRing::detach(uint8_t readerId) noexcept
{
    if (!inited_) {
        return fw::Result::kInvalidState;
    }
    if (readerId != 0U) {
        return fw::Result::kInvalidArg;
    }
    const fw::LockGuard guard(lock_);
    if (!reader_.attached) {
        return fw::Result::kInvalidArg;
    }
    if (reader_.pinned) {
        return fw::Result::kBusy;
    }
    reader_.attached = false;
    reader_.mask = 0U;
    read_pos_ = 0U;
    write_pos_ = 0U;
    return fw::Result::kOk;
}

fw::Result GnssAppRing::setMask(uint8_t readerId, fw::FanoutMask mask) noexcept
{
    if (!inited_) {
        return fw::Result::kInvalidState;
    }
    const fw::LockGuard guard(lock_);
    if ((readerId != 0U) || !reader_.attached) {
        return fw::Result::kInvalidArg;
    }
    if (reader_.pinned) {
        return fw::Result::kBusy;
    }
    reader_.mask = mask;
    // 与 FanoutRing::setMask 一致：切换订阅掩码后跳过历史未读记录，不计 dropped。
    read_pos_ = 0U;
    write_pos_ = 0U;
    return fw::Result::kOk;
}

uint16_t GnssAppRing::recordLen(size_t pos) const noexcept
{
    const std::span<const uint8_t> storage(buf_, cap_);
    const uint32_t lo = storage[pos];
    const uint32_t hi = storage[pos + 1U];
    // 两字节拼接范围 0..65535，缩窄到 uint16_t 无损。
    return static_cast<uint16_t>(lo | (hi << kBitsPerByte));
}

uint8_t GnssAppRing::recordType(size_t pos) const noexcept
{
    const std::span<const uint8_t> storage(buf_, cap_);
    return storage[pos + fw::kFanoutHeaderBytes];
}

size_t GnssAppRing::recordTotal(size_t pos) const noexcept
{
    return kHeaderBytes + static_cast<size_t>(recordLen(pos));
}

void GnssAppRing::compactToFront() noexcept
{
    if (read_pos_ == 0U) {
        return;
    }
    const size_t used = write_pos_ - read_pos_;
    if (used > 0U) {
        const std::span<uint8_t> storage(buf_, cap_);
        std::memmove(storage.data(), storage.subspan(read_pos_).data(), used);
    }
    read_pos_ = 0U;
    write_pos_ = used;
}

void GnssAppRing::evictAt(size_t victimPos) noexcept
{
    const uint8_t victimType = recordType(victimPos);
    const size_t victimTotal = recordTotal(victimPos);
    const size_t tailPos = victimPos + victimTotal;
    const size_t moveBytes = write_pos_ - tailPos;
    if (moveBytes > 0U) {
        const std::span<uint8_t> storage(buf_, cap_);
        std::memmove(storage.subspan(victimPos).data(), storage.subspan(tailPos).data(), moveBytes);
    }
    write_pos_ -= victimTotal;
    ++wstats_.overwritten;
    if ((reader_.mask & (fw::FanoutMask{1U} << victimType)) != 0U) {
        ++reader_.stats.dropped;
    } else {
        ++reader_.filtered;
    }
    if (victimType == kGgaType) {
        ++gga_drop_;
    }
}

fw::Result GnssAppRing::makeSpaceFor(uint8_t incomingType, size_t need) noexcept
{
    const size_t scanBegin = reader_.pinned ? (read_pos_ + recordTotal(read_pos_)) : read_pos_;
    const uint8_t incomingPrio = retentionPriority(incomingType);
    const size_t deficit = need - (cap_ - write_pos_);

    // 只读预扫：确认在不破坏更高优先级记录的前提下可腾出的总空间。
    size_t evictableBytes = 0U;
    for (size_t pos = scanBegin; pos < write_pos_;) {
        const size_t total = recordTotal(pos);
        if (retentionPriority(recordType(pos)) <= incomingPrio) {
            evictableBytes += total;
        }
        pos += total;
    }

    if (evictableBytes < deficit) {
        const size_t pinnedProtected = reader_.pinned ? (scanBegin - read_pos_) : 0U;
        if (reader_.pinned && ((evictableBytes + pinnedProtected + read_pos_) >= deficit)) {
            ++typed_wstats_.pinnedDrop;
            ++reader_.pinnedDrop;
            if (incomingType == kGgaType) {
                ++gga_drop_;
            }
            return fw::Result::kBusy;
        }
        // 环内被更高优先级记录（如 GGA）占满：直接丢弃本次低优先级新帧。
        ++reader_.stats.dropped;
        if (incomingType == kGgaType) {
            ++gga_drop_;
        }
        return fw::Result::kNoSpace;
    }

    while ((cap_ - write_pos_) < need) {
        size_t victimPos = write_pos_;
        uint8_t victimPrio = std::numeric_limits<uint8_t>::max();
        for (size_t cur = scanBegin; cur < write_pos_;) {
            const uint8_t prio = retentionPriority(recordType(cur));
            if (prio < victimPrio) {
                victimPrio = prio;
                victimPos = cur;
            }
            cur += recordTotal(cur);
        }
        if (victimPos >= write_pos_) {
            return fw::Result::kNoSpace;
        }
        evictAt(victimPos);
    }
    return fw::Result::kOk;
}

void GnssAppRing::appendRecord(uint8_t type, std::span<const uint8_t> payload) noexcept
{
    const std::span<uint8_t> storage(buf_, cap_);
    const size_t len = payload.size();
    // 写入前已校验 len <= max_record_ <= 65535，拆高低字节无损。
    storage[write_pos_] = static_cast<uint8_t>(len & kByteMask);
    storage[write_pos_ + 1U] = static_cast<uint8_t>((len >> kBitsPerByte) & kByteMask);
    storage[write_pos_ + fw::kFanoutHeaderBytes] = type;
    std::memcpy(storage.subspan(write_pos_ + kHeaderBytes).data(), payload.data(), len);
    write_pos_ += kHeaderBytes + len;
    ++wstats_.records;
    // len <= 65535，无损扩宽到 uint32_t。
    wstats_.bytes += static_cast<uint32_t>(len);
    typed_wstats_.highWaterBytes = std::max(typed_wstats_.highWaterBytes, write_pos_ - read_pos_);
}

/**
 * @brief 写入一条带类型的记录；空间不足时按优先级淘汰低优先级记录。
 * @param type: 语句类型下标（0..31）。
 * @param payload: 负载字节视图（1..maxRecord）。
 * @retval fw::Result::kOk 写入成功或无订阅者。
 * @retval fw::Result::kInvalidArg 参数或长度越界。
 * @retval fw::Result::kNoSpace 环被更高优先级记录占满，新帧已丢弃。
 * @retval fw::Result::kBusy 读侧 pin 阻塞。
 * @retval fw::Result::kInvalidState 尚未初始化。
 * @note 仅写者任务调用，持注入锁，不阻塞、不分配堆，非 ISR。
 */
fw::Result GnssAppRing::write(uint8_t type, std::span<const uint8_t> payload) noexcept
{
    if (!inited_) {
        return fw::Result::kInvalidState;
    }
    const fw::LockGuard guard(lock_);
    if ((type >= std::numeric_limits<fw::FanoutMask>::digits) || payload.empty() ||
        (payload.size() > max_record_)) {
        ++wstats_.rejected;
        if ((payload.size() > max_record_) || (payload.size() > (cap_ - kHeaderBytes))) {
            ++typed_wstats_.oversizeDrop;
        }
        return fw::Result::kInvalidArg;
    }
    const fw::FanoutMask bit = fw::FanoutMask{1U} << type;
    if (!reader_.attached || ((reader_.mask & bit) == 0U)) {
        ++typed_wstats_.noSubscriber;
        return fw::Result::kOk;
    }

    const size_t need = kHeaderBytes + payload.size();
    if (!reader_.pinned && (read_pos_ > 0U) && ((cap_ - write_pos_) < need)) {
        compactToFront();
    }
    if ((cap_ - write_pos_) < need) {
        const fw::Result spaceRes = makeSpaceFor(type, need);
        if (spaceRes != fw::Result::kOk) {
            return spaceRes;
        }
    }
    appendRecord(type, payload);
    return fw::Result::kOk;
}

fw::Result GnssAppRing::peek(uint8_t readerId, fw::FanoutView &view) noexcept
{
    view = fw::FanoutView{};
    if (!inited_) {
        return fw::Result::kInvalidState;
    }
    const fw::LockGuard guard(lock_);
    if ((readerId != 0U) || !reader_.attached) {
        return fw::Result::kInvalidArg;
    }
    if (reader_.pinned) {
        return fw::Result::kBusy;
    }
    while (read_pos_ < write_pos_) {
        const size_t len = recordLen(read_pos_);
        const uint8_t type = recordType(read_pos_);
        if ((reader_.mask & (fw::FanoutMask{1U} << type)) == 0U) {
            read_pos_ += kHeaderBytes + len;
            ++reader_.filtered;
            continue;
        }
        if (reader_.serial == std::numeric_limits<uint32_t>::max()) {
            return fw::Result::kInvalidState;
        }
        ++reader_.serial;
        reader_.pinned = true;
        const std::span<const uint8_t> storage(buf_, cap_);
        view = {
            .lease = {reader_.generation, reader_.serial, readerId},
            .type = type,
            .first = storage.subspan(read_pos_ + kHeaderBytes, len),
            .second = {},
        };
        return fw::Result::kOk;
    }
    read_pos_ = 0U;
    write_pos_ = 0U;
    return fw::Result::kNotFound;
}

fw::Result GnssAppRing::finishLease(const fw::FanoutLease &lease, bool consume) noexcept
{
    if (!inited_) {
        return fw::Result::kInvalidState;
    }
    const fw::LockGuard guard(lock_);
    if ((lease.readerId != 0U) || !reader_.attached || !reader_.pinned ||
        (reader_.generation != lease.generation) || (reader_.serial != lease.serial)) {
        return fw::Result::kInvalidArg;
    }
    if (consume) {
        const size_t len = recordLen(read_pos_);
        read_pos_ += kHeaderBytes + len;
        ++reader_.stats.records;
        // len <= 65535，无损扩宽到 uint32_t。
        reader_.stats.bytes += static_cast<uint32_t>(len);
    }
    reader_.pinned = false;
    if (read_pos_ == write_pos_) {
        read_pos_ = 0U;
        write_pos_ = 0U;
    } else if (read_pos_ >= (cap_ / 2U)) {
        compactToFront();
    }
    return fw::Result::kOk;
}

fw::Result GnssAppRing::commit(const fw::FanoutLease &lease) noexcept
{
    return finishLease(lease, true);
}

fw::Result GnssAppRing::cancel(const fw::FanoutLease &lease) noexcept
{
    return finishLease(lease, false);
}

fw::FanoutTypedReaderStats GnssAppRing::typedReaderStats(uint8_t readerId) const noexcept
{
    if (!inited_ || (readerId != 0U)) {
        return {};
    }
    const fw::LockGuard guard(lock_);
    if (!reader_.attached) {
        return {};
    }
    return {reader_.stats, reader_.filtered, reader_.pinnedDrop};
}

fw::FanoutTypedWriterStats GnssAppRing::typedWriterStats() const noexcept
{
    if (!inited_) {
        return {};
    }
    const fw::LockGuard guard(lock_);
    fw::FanoutTypedWriterStats stats = typed_wstats_;
    stats.base = wstats_;
    return stats;
}

uint32_t GnssAppRing::ggaDropCount() const noexcept
{
    if (!inited_) {
        return 0U;
    }
    const fw::LockGuard guard(lock_);
    return gga_drop_;
}

} // namespace app::data
