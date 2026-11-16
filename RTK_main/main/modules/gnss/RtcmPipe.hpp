// RTCM 到 GNSS 的 SPSC 字节管道（ADR-0010 §2.8）：封装 StaticStreamBuffer 与管道统计。
// 线程/并发契约：init 仅启动前单线程；push 仅 kLink（RtcmModule::poll）调用，read/available/discard/note_written
// 仅 kIo（GnssModule::poll）调用。FreeRTOS StreamBuffer 在严格单写单读且无并发 reset 时免锁安全；
// discard 采用非阻塞排空而非 xStreamBufferReset，避免与 kLink 的 send 并发冲突。stats_ 由 spinlock 保护跨核读写。
// 非 ISR；无阻塞（send/receive 均使用 timeout_ms=0）；无堆分配。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "def/GnssDef.hpp"
#include "def/RtcmDef.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtos/BootStreamBuffer.hpp"

namespace app {

/** RTCM -> GNSS UART 字节管道（kLink 写、kIo 读，整帧原子写入）。 */
class RtcmPipe final {
public:
    constexpr RtcmPipe() noexcept = default;
    ~RtcmPipe() = default;
    RtcmPipe(const RtcmPipe &) = delete;
    RtcmPipe &operator=(const RtcmPipe &) = delete;
    RtcmPipe(RtcmPipe &&) = delete;
    RtcmPipe &operator=(RtcmPipe &&) = delete;

    /** 启动前单线程初始化自旋锁（支持降级模式下安全读 stats/io_stats）。 */
    void init_lock() noexcept
    {
        portMUX_INITIALIZE(&mux_);
    }

    /** 启动前单线程绑定外供管道存储；非空且 >= kRtcmPipeBytes 才返回 kOk。 */
    [[nodiscard]] fw::Status bind_storage(std::span<uint8_t> storage) noexcept
    {
        if (buf_.valid()) {
            return fw::Status::kInvalidState;
        }
        if ((storage.data() == nullptr) || (storage.size() < kRtcmPipeBytes)) {
            return fw::Status::kInvalidArg;
        }
        storage_ = storage.first(kRtcmPipeBytes);
        return fw::Status::kOk;
    }

    /** 仅启动绑定回滚、尚未 init 时调用；清除外供引用，非 ISR。 */
    void unbind_storage() noexcept
    {
        storage_ = {};
    }

    /** 启动前单线程初始化静态 StreamBuffer；trigger_level=1，未绑定存储或失败返回 kInvalidState。 */
    [[nodiscard]] fw::Status init() noexcept
    {
        portMUX_INITIALIZE(&mux_);
        if (storage_.size() < kRtcmPipeBytes) {
            return fw::Status::kInvalidState;
        }
        return buf_.init(storage_, 1U);
    }

    /** 仅 kLink 调用，非 ISR、非阻塞；剩余空间不足整帧时整帧丢弃、计数并上报 kPipeDrop。 */
    [[nodiscard]] bool push(std::span<const uint8_t> frame) noexcept
    {
        if (!buf_.valid() || frame.empty()) {
            return false;
        }
        const size_t avail = buf_.space();
        if (avail < frame.size()) {
            {
                const CriticalGuard guard(mux_);
                ++stats_.dropped_frames;
                stats_.dropped_bytes += static_cast<uint32_t>(frame.size());
            }
            trace_gnss(GnssTrace::kPipeDrop, static_cast<uint32_t>(frame.size()), static_cast<uint32_t>(avail));
            return false;
        }
        const size_t sent = buf_.send(frame, 0U);
        if (sent != frame.size()) {
            {
                const CriticalGuard guard(mux_);
                ++stats_.dropped_frames;
                stats_.dropped_bytes += static_cast<uint32_t>(frame.size());
            }
            trace_gnss(GnssTrace::kPipeDrop, static_cast<uint32_t>(frame.size()), static_cast<uint32_t>(avail));
            return false;
        }
        const CriticalGuard guard(mux_);
        ++stats_.pushed_frames;
        stats_.pushed_bytes += static_cast<uint32_t>(sent);
        return true;
    }

    /** 仅 kIo 调用，非 ISR、非阻塞；从管道读取最多 out.size() 字节，返回实际读出字节数。 */
    [[nodiscard]] size_t read(std::span<uint8_t> out) noexcept
    {
        if (!buf_.valid() || out.empty()) {
            return 0U;
        }
        return buf_.receive(out, 0U);
    }

    /** 仅 kIo 调用，非 ISR、非阻塞；记录本次实际写入 GNSS UART 的字节数。 */
    void note_written(size_t bytes) noexcept
    {
        const CriticalGuard guard(mux_);
        stats_.written_bytes += static_cast<uint32_t>(bytes);
    }

    /** 仅 kIo 调用，非 ISR、非阻塞；通过循环读取排空管道内容（SPSC 安全，不调用 xStreamBufferReset）。 */
    void discard() noexcept
    {
        if (!buf_.valid()) {
            return;
        }
        std::array<uint8_t, kDiscardChunkBytes> scratch{};
        size_t dropped = 0U;
        while (true) {
            const size_t n = buf_.receive(scratch, 0U);
            if (n == 0U) {
                break;
            }
            dropped += n;
        }
        const CriticalGuard guard(mux_);
        io_stats_.discarded_bytes += static_cast<uint32_t>(dropped);
        io_stats_.pending_bytes = 0U;
        io_stats_.paused = false;
    }

    /** 仅 kIo 调用，非 ISR、非阻塞；记录因断电/失效显式丢弃的挂起块字节数。 */
    void note_discarded(size_t bytes) noexcept
    {
        const CriticalGuard guard(mux_);
        io_stats_.discarded_bytes += static_cast<uint32_t>(bytes);
        io_stats_.pending_bytes = 0U;
        io_stats_.paused = false;
    }

    /** 仅 kIo 调用，非 ISR、非阻塞；记录不可恢复 UART 写错误及被丢弃的挂起字节数。 */
    void note_write_error(size_t dropped_bytes) noexcept
    {
        const CriticalGuard guard(mux_);
        ++io_stats_.uart_write_errors;
        io_stats_.discarded_bytes += static_cast<uint32_t>(dropped_bytes);
        io_stats_.pending_bytes = 0U;
        io_stats_.paused = false;
    }

    /** 仅 kIo 调用，非 ISR、非阻塞；更新 kIo 待写块剩余字节与暂停状态。 */
    void set_pending(size_t pending_bytes, bool paused) noexcept
    {
        const CriticalGuard guard(mux_);
        io_stats_.pending_bytes = static_cast<uint32_t>(pending_bytes);
        io_stats_.paused = paused;
    }

    /** 仅 kIo 调用，非 ISR；当前管道内可读字节数。 */
    [[nodiscard]] size_t available() const noexcept
    {
        return buf_.valid() ? buf_.available() : 0U;
    }

    /** 跨任务安全（kIo/kLink/console），非 ISR；返回统计快照。 */
    [[nodiscard]] RtcmPipeStats stats() const noexcept
    {
        const CriticalGuard guard(mux_);
        return stats_;
    }

    /** 跨任务安全（kIo/kLink/console），非 ISR；返回 kIo 消费与写回状态快照。 */
    [[nodiscard]] RtcmPipeIoStats io_stats() const noexcept
    {
        const CriticalGuard guard(mux_);
        return io_stats_;
    }

private:
    class CriticalGuard final {
    public:
        explicit CriticalGuard(portMUX_TYPE &mux) noexcept : mux_(mux)
        {
            taskENTER_CRITICAL(&mux_);
        }
        ~CriticalGuard()
        {
            taskEXIT_CRITICAL(&mux_);
        }
        CriticalGuard(const CriticalGuard &) = delete;
        CriticalGuard &operator=(const CriticalGuard &) = delete;

    private:
        portMUX_TYPE &mux_;
    };

    static constexpr size_t kDiscardChunkBytes = 128U;

    std::span<uint8_t> storage_{};
    rtos::BootStreamBuffer buf_{};
    mutable portMUX_TYPE mux_{};
    RtcmPipeStats stats_{};
    RtcmPipeIoStats io_stats_{};
};

} // namespace app
