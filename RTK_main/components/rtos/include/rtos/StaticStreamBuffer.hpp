// 静态 StreamBuffer 包装：存储与控制块都在对象内部，init() 后不分配堆。
// 回调中可用 timeoutMs=0 的非阻塞读写；构造 constexpr、不访问 RTOS。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "fw/Status.hpp"

namespace rtos {

template <size_t N> class StaticStreamBuffer {
public:
    static_assert(N > 0U, "stream buffer must be non-empty");

    constexpr StaticStreamBuffer() noexcept = default;

    StaticStreamBuffer(const StaticStreamBuffer &) = delete;
    StaticStreamBuffer &operator=(const StaticStreamBuffer &) = delete;
    StaticStreamBuffer(StaticStreamBuffer &&) = delete;
    StaticStreamBuffer &operator=(StaticStreamBuffer &&) = delete;
    ~StaticStreamBuffer() = default;

    /** 创建静态 StreamBuffer；triggerLevel 必须在 1..N。 */
    [[nodiscard]] fw::Status init(size_t triggerLevel) noexcept
    {
        if (handle_ != nullptr) {
            return fw::Status::kBusy;
        }
        if ((triggerLevel == 0U) || (triggerLevel > N)) {
            return fw::Status::kInvalidArg;
        }
        handle_ = xStreamBufferCreateStatic(N, triggerLevel, storage_.data(), &buffer_);
        return (handle_ != nullptr) ? fw::Status::kOk : fw::Status::kNoSpace;
    }

    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vStreamBufferDelete(handle_);
            handle_ = nullptr;
        }
    }

    /** 写入，返回实际写入字节数；回调中传 timeoutMs=0。 */
    [[nodiscard]] size_t send(std::span<const uint8_t> data, uint32_t timeoutMs) noexcept
    {
        if ((handle_ == nullptr) || data.empty()) {
            return 0;
        }
        return xStreamBufferSend(handle_, data.data(), data.size(), pdMS_TO_TICKS(timeoutMs));
    }

    /** 读取，返回实际读取字节数；timeoutMs=0 为非阻塞。 */
    [[nodiscard]] size_t receive(std::span<uint8_t> out, uint32_t timeoutMs) noexcept
    {
        if ((handle_ == nullptr) || out.empty()) {
            return 0;
        }
        return xStreamBufferReceive(handle_, out.data(), out.size(), pdMS_TO_TICKS(timeoutMs));
    }

    /** 当前可写入的字节数（单写者据此整帧写入或整帧丢弃）；未初始化返回 0。 */
    [[nodiscard]] size_t space() const noexcept
    {
        return (handle_ != nullptr) ? xStreamBufferSpacesAvailable(handle_) : 0U;
    }

    /** 当前可读取的字节数；未初始化返回 0。 */
    [[nodiscard]] size_t available() const noexcept
    {
        return (handle_ != nullptr) ? xStreamBufferBytesAvailable(handle_) : 0U;
    }

    void reset() noexcept
    {
        if (handle_ != nullptr) {
            xStreamBufferReset(handle_);
        }
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr;
    }

private:
    StaticStreamBuffer_t buffer_{};
    std::array<uint8_t, N> storage_{};
    StreamBufferHandle_t handle_ = nullptr;
};

} // namespace rtos
