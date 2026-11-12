// 静态 StreamBuffer 包装：存储与控制块都在对象内部，init() 后不分配堆。
// 回调中可用 timeout_ms=0 的非阻塞读写；构造 constexpr、不访问 RTOS。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "rtk/status.hpp"

namespace rtk::os {

template <size_t N> class StaticStreamBuffer {
public:
    static_assert(N > 0U, "stream buffer must be non-empty");

    constexpr StaticStreamBuffer() noexcept = default;

    StaticStreamBuffer(const StaticStreamBuffer &) = delete;
    StaticStreamBuffer &operator=(const StaticStreamBuffer &) = delete;
    StaticStreamBuffer(StaticStreamBuffer &&) = delete;
    StaticStreamBuffer &operator=(StaticStreamBuffer &&) = delete;
    ~StaticStreamBuffer() = default;

    /** 创建静态 StreamBuffer；trigger_level 必须在 1..N。 */
    [[nodiscard]] Status init(size_t trigger_level) noexcept
    {
        if (handle_ != nullptr) {
            return Status::kBusy;
        }
        if ((trigger_level == 0U) || (trigger_level > N)) {
            return Status::kInvalidArg;
        }
        handle_ = xStreamBufferCreateStatic(N, trigger_level, storage_.data(), &buffer_);
        return (handle_ != nullptr) ? Status::kOk : Status::kNoSpace;
    }

    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vStreamBufferDelete(handle_);
            handle_ = nullptr;
        }
    }

    /** 写入，返回实际写入字节数；回调中传 timeout_ms=0。 */
    [[nodiscard]] size_t send(std::span<const uint8_t> data, uint32_t timeout_ms) noexcept
    {
        if ((handle_ == nullptr) || data.empty()) {
            return 0;
        }
        return xStreamBufferSend(handle_, data.data(), data.size(), pdMS_TO_TICKS(timeout_ms));
    }

    /** 读取，返回实际读取字节数；timeout_ms=0 为非阻塞。 */
    [[nodiscard]] size_t receive(std::span<uint8_t> out, uint32_t timeout_ms) noexcept
    {
        if ((handle_ == nullptr) || out.empty()) {
            return 0;
        }
        return xStreamBufferReceive(handle_, out.data(), out.size(), pdMS_TO_TICKS(timeout_ms));
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

} // namespace rtk::os
