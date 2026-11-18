// 基站 RTCM 上行直推跨任务单生产单消费环形缓冲（4 KiB 静态存储，kIo 写 / kLink 读）。
// 线程安全：通过注入的 LockOps 保护全部读写；不分配堆、不阻塞，非 ISR。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/Lock.hpp"

namespace app::data {

/**
 * 4 KiB 按帧定界 RTCM3 环形缓冲：每条记录 2 B 小端长度 + 完整 RTCM3 线路帧。
 * 满时丢整帧并累计 dropFrames，绝不写入半帧，绝不阻塞生产者 kIo。
 */
class BaseRtcmRing {
public:
    static constexpr size_t kCap = 4096U;
    static constexpr size_t kLenHeaderBytes = 2U;
    static constexpr size_t kMaxFrameBytes = 1029U;
    static constexpr size_t kByteShift = 8U;
    static constexpr uint32_t kByteMask = 0xFFU;

    constexpr BaseRtcmRing() noexcept = default;

    /**
     * @brief 绑定跨任务自旋锁（仅启动前调用一次）。
     * @param lock: 锁操作表。
     */
    void bindLock(fw::LockOps lock) noexcept
    {
        lock_ = lock;
    }

    /**
     * @brief 推入一条完整 RTCM3 帧；空间不足时丢弃整帧并计数。
     * @param frame: 完整 RTCM3 字节切片（1..kMaxFrameBytes）。
     * @retval true: 整帧已入环。
     * @retval false: 参数越界或环形空间不足，已计入 dropFrames。
     */
    [[nodiscard]] bool push(std::span<const uint8_t> frame) noexcept
    {
        if (frame.empty() || (frame.size() > kMaxFrameBytes)) {
            return false;
        }
        const size_t need = kLenHeaderBytes + frame.size();
        const fw::LockGuard guard(lock_);
        if ((kCap - used_) < need) {
            ++dropFrames_;
            return false;
        }
        const auto lenLo = static_cast<uint8_t>(frame.size() & kByteMask);
        const auto lenHi = static_cast<uint8_t>((frame.size() >> kByteShift) & kByteMask);
        writeByteLocked(lenLo);
        writeByteLocked(lenHi);
        for (const uint8_t b : frame) {
            writeByteLocked(b);
        }
        used_ += need;
        return true;
    }

    /**
     * @brief 弹出一条完整 RTCM3 帧到调用方缓冲。
     * @param dst: 输出缓冲（至少 kMaxFrameBytes）。
     * @param outLen: 实际弹出帧长（字节）。
     * @retval true: 成功弹出一帧。
     * @retval false: 环空或目标缓冲不足。
     */
    [[nodiscard]] bool pop(std::span<uint8_t> dst, size_t &outLen) noexcept
    {
        outLen = 0U;
        const fw::LockGuard guard(lock_);
        if (used_ < kLenHeaderBytes) {
            return false;
        }
        const uint8_t lo = readByteLocked();
        const uint8_t hi = readByteLocked();
        const size_t len = static_cast<size_t>(lo) | (static_cast<size_t>(hi) << kByteShift);
        if ((len == 0U) || (len > kMaxFrameBytes) || (used_ < (kLenHeaderBytes + len)) ||
            (dst.size() < len)) {
            head_ = 0U;
            tail_ = 0U;
            used_ = 0U;
            ++dropFrames_;
            return false;
        }
        for (size_t i = 0U; i < len; ++i) {
            dst[i] = readByteLocked();
        }
        used_ -= (kLenHeaderBytes + len);
        outLen = len;
        return true;
    }

    /**
     * @brief 断开会话时清空未发出帧（保留 dropFrames 统计）。
     */
    void clearFrames() noexcept
    {
        const fw::LockGuard guard(lock_);
        head_ = 0U;
        tail_ = 0U;
        used_ = 0U;
    }

    /**
     * @brief 读取环溢出累计丢帧数。
     * @retval dropFrames: 因环满丢弃的整帧次数。
     */
    [[nodiscard]] uint32_t dropFrames() const noexcept
    {
        const fw::LockGuard guard(lock_);
        return dropFrames_;
    }

private:
    /**
     * @brief 在已持锁前提下写入单字节并前移写游标。
     * @param b: 待写字节。
     */
    void writeByteLocked(uint8_t b) noexcept
    {
        buf_[head_] = b;
        head_ = (head_ + 1U) % kCap;
    }

    /**
     * @brief 在已持锁前提下读出单字节并前移读游标。
     * @retval byte: 读出字节。
     */
    uint8_t readByteLocked() noexcept
    {
        const uint8_t b = buf_[tail_];
        tail_ = (tail_ + 1U) % kCap;
        return b;
    }

    fw::LockOps lock_{};
    std::array<uint8_t, kCap> buf_{};
    size_t head_ = 0U;
    size_t tail_ = 0U;
    size_t used_ = 0U;
    uint32_t dropFrames_ = 0U;
};

} // namespace app::data
