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

#include "def/gnss_def.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtos/static_stream_buffer.hpp"

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

    /** 启动前单线程初始化静态 StreamBuffer；trigger_level=1，失败返回 kInvalidState。 */
    [[nodiscard]] fw::Status init() noexcept
    {
        return buf_.init(1U);
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
        while (buf_.receive(scratch, 0U) > 0U) {
        }
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

    rtos::StaticStreamBuffer<kRtcmPipeBytes> buf_{};
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    RtcmPipeStats stats_{};
};

} // namespace app
