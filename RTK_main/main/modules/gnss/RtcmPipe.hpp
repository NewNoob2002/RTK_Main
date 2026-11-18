// RTCM 到 GNSS 的 SPSC 字节管道与跨核统计（ADR-0010 §2.8）。
// 启动前初始化；kLink 单写、kIo 单读；统计使用自旋锁，非 ISR。
// 不分配堆；收发超时为 0，discard 循环排空而不 reset，允许写者并行发送。
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

    /**
     * @brief 启动前单线程初始化自旋锁（支持降级模式下安全读 stats/ioStats）。
     * @note 仅启动前单线程，非 ISR，不阻塞。
     */
    void initLock() noexcept
    {
        portMUX_INITIALIZE(&mux_);
    }

    /**
     * @brief 启动前单线程绑定外供管道存储；非空且 >= kRtcmPipeBytes 才返回 kOk。
     * @param storage: 外供存储，生命周期覆盖所有消费者
     * @retval kOk 已绑定
     * @retval kInvalidState 管道已初始化
     * @retval kInvalidArg 存储为空或不足
     * @note 仅启动前单线程，非 ISR，不分配、不阻塞。
     */
    [[nodiscard]] fw::Status bindStorage(std::span<uint8_t> storage) noexcept
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

    /**
     * @brief 仅启动绑定回滚、尚未 init 时调用；清除外供引用，非 ISR。
     * @note 仅启动绑定回滚、消费者尚未启动时调用；不阻塞，非 ISR。
     */
    void unbindStorage() noexcept
    {
        storage_ = {};
    }

    /**
     * @brief 初始化 GNSS 模块（UART、分流、控制器）
     * @retval kOk: 已就绪；其他: 失败原因（已回滚）
     * @note 仅 IO 任务启动期调用；可阻塞（UART 安装），非 ISR。
     */
    [[nodiscard]] fw::Status init() noexcept
    {
        portMUX_INITIALIZE(&mux_);
        if (storage_.size() < kRtcmPipeBytes) {
            return fw::Status::kInvalidState;
        }
        return buf_.init(storage_, 1U);
    }

    /**
     * @brief 仅 kLink 调用，非 ISR、非阻塞；剩余空间不足整帧时整帧丢弃、计数并上报 kPipeDrop。
     * @param frame: 完整帧字节
     * @retval true 整帧已入管道
     * @retval false 未初始化、空帧、容量不足或发送不足
     * @note 仅 kLink 单写者；非 ISR、非阻塞；不足整帧时累计丢弃。
     */
    [[nodiscard]] bool push(std::span<const uint8_t> frame) noexcept
    {
        if (!buf_.valid() || frame.empty()) {
            return false;
        }
        const size_t avail = buf_.space();
        if (avail < frame.size()) {
            {
                const CriticalGuard guard(mux_);
                ++stats_.droppedFrames;
                stats_.droppedBytes += static_cast<uint32_t>(frame.size());
            }
            traceGnss(GnssTrace::kPipeDrop, static_cast<uint32_t>(frame.size()),
                      static_cast<uint32_t>(avail));
            return false;
        }
        const size_t sent = buf_.send(frame, 0U);
        if (sent != frame.size()) {
            {
                const CriticalGuard guard(mux_);
                ++stats_.droppedFrames;
                stats_.droppedBytes += static_cast<uint32_t>(frame.size());
            }
            traceGnss(GnssTrace::kPipeDrop, static_cast<uint32_t>(frame.size()),
                      static_cast<uint32_t>(avail));
            return false;
        }
        const CriticalGuard guard(mux_);
        ++stats_.pushedFrames;
        stats_.pushedBytes += static_cast<uint32_t>(sent);
        return true;
    }

    /**
     * @brief 仅 kIo 调用，非 ISR、非阻塞；从管道读取最多 out.size() 字节，返回实际读出字节数。
     * @param[out] out: 字节输出，仅实际读出的前缀被改写
     * @retval 实际读出字节数；未初始化、空输出或管道空时为 0
     * @note 仅 kIo 单读者；非 ISR、非阻塞。
     */
    [[nodiscard]] size_t read(std::span<uint8_t> out) noexcept
    {
        if (!buf_.valid() || out.empty()) {
            return 0U;
        }
        return buf_.receive(out, 0U);
    }

    /**
     * @brief 仅 kIo 调用，非 ISR、非阻塞；记录本次实际写入 GNSS UART 的字节数。
     * @param bytes: 本次累计的字节数
     * @note 仅 kIo；内部短临界区更新统计，非 ISR。
     */
    void noteWritten(size_t bytes) noexcept
    {
        const CriticalGuard guard(mux_);
        stats_.writtenBytes += static_cast<uint32_t>(bytes);
    }

    /**
     * @brief 仅 kIo 调用，非 ISR、非阻塞；通过循环读取排空管道内容（SPSC 安全，不调用 xStreamBufferReset）。
     * @note 仅 kIo；非阻塞排空，不 reset 管道，允许 kLink 同时写；非 ISR。
     */
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
        io_stats_.discardedBytes += static_cast<uint32_t>(dropped);
        io_stats_.pendingBytes = 0U;
        io_stats_.paused = false;
    }

    /**
     * @brief 仅 kIo 调用，非 ISR、非阻塞；记录因断电/失效显式丢弃的挂起块字节数。
     * @param bytes: 本次累计的字节数
     * @note 仅 kIo；内部短临界区更新统计，非 ISR。
     */
    void noteDiscarded(size_t bytes) noexcept
    {
        const CriticalGuard guard(mux_);
        io_stats_.discardedBytes += static_cast<uint32_t>(bytes);
        io_stats_.pendingBytes = 0U;
        io_stats_.paused = false;
    }

    /**
     * @brief 仅 kIo 调用，非 ISR、非阻塞；记录不可恢复 UART 写错误及被丢弃的挂起字节数。
     * @param droppedBytes: 被丢弃的挂起字节数
     * @note 仅 kIo；内部短临界区更新统计，非 ISR。
     */
    void noteWriteError(size_t droppedBytes) noexcept
    {
        const CriticalGuard guard(mux_);
        ++io_stats_.uartWriteErrors;
        io_stats_.discardedBytes += static_cast<uint32_t>(droppedBytes);
        io_stats_.pendingBytes = 0U;
        io_stats_.paused = false;
    }

    /**
     * @brief 仅 kIo 调用，非 ISR、非阻塞；更新 kIo 待写块剩余字节与暂停状态。
     * @param pendingBytes: kIo 待写块剩余字节数
     * @param paused: 是否暂停写入
     * @note 仅 kIo；内部短临界区更新统计，非 ISR。
     */
    void setPending(size_t pendingBytes, bool paused) noexcept
    {
        const CriticalGuard guard(mux_);
        io_stats_.pendingBytes = static_cast<uint32_t>(pendingBytes);
        io_stats_.paused = paused;
    }

    /**
     * @brief 仅 kIo 调用，非 ISR；当前管道内可读字节数。
     * @retval 管道当前可读字节数；未初始化时为 0
     * @note 仅 kIo 单读者，非 ISR、非阻塞。
     */
    [[nodiscard]] size_t available() const noexcept
    {
        return buf_.valid() ? buf_.available() : 0U;
    }

    /**
     * @brief 跨任务安全（kIo/kLink/console），非 ISR；返回统计快照。
     * @retval 受自旋锁保护的管道累计统计副本
     * @note kIo/kLink/console 跨任务安全；非 ISR，不阻塞。
     */
    [[nodiscard]] RtcmPipeStats stats() const noexcept
    {
        const CriticalGuard guard(mux_);
        return stats_;
    }

    /**
     * @brief 跨任务安全（kIo/kLink/console），非 ISR；返回 kIo 消费与写回状态快照。
     * @retval 受自旋锁保护的 kIo 消费状态副本
     * @note kIo/kLink/console 跨任务安全；非 ISR，不阻塞。
     */
    [[nodiscard]] RtcmPipeIoStats ioStats() const noexcept
    {
        const CriticalGuard guard(mux_);
        return io_stats_;
    }

private:
    class CriticalGuard final {
    public:
        /**
         * @brief 进入管道统计的 SMP 临界区
         * @param mux: 统计锁，生命周期覆盖 guard
         * @note 仅任务上下文；持有期间不得阻塞或调用 FreeRTOS API，非 ISR。
         */
        explicit CriticalGuard(portMUX_TYPE &mux) noexcept : mux_(mux)
        {
            taskENTER_CRITICAL(&mux_);
        }
        /**
         * @brief 退出管道统计的 SMP 临界区
         * @note 在创建 guard 的任务中退出，非 ISR，不阻塞。
         */
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
