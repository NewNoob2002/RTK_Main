// 外供存储静态 StreamBuffer 包装：控制块在对象内部，存储区由启动期 boot_alloc() 注入。
// 线程安全：单写者/单读者语义同 FreeRTOS StreamBuffer；timeoutMs=0 可用于非阻塞回调。ISR 安全：否。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "fw/Status.hpp"

namespace rtos {

/** 接收外部 uint8_t 存储区 span 的静态 StreamBuffer 包装。 */
class BootStreamBuffer {
public:
    constexpr BootStreamBuffer() noexcept = default;

    BootStreamBuffer(const BootStreamBuffer &) = delete;
    BootStreamBuffer &operator=(const BootStreamBuffer &) = delete;
    BootStreamBuffer(BootStreamBuffer &&) = delete;
    BootStreamBuffer &operator=(BootStreamBuffer &&) = delete;
    ~BootStreamBuffer() = default;

    /** 使用外部存储区初始化 StreamBuffer；triggerLevel 必须在 1..storage.size()。 */
    [[nodiscard]] fw::Status init(std::span<uint8_t> storage, size_t triggerLevel) noexcept
    {
        if (handle_ != nullptr) {
            return fw::Status::kBusy;
        }
        if (storage.empty() || (storage.data() == nullptr) || (triggerLevel == 0U) ||
            (triggerLevel > storage.size())) {
            return fw::Status::kInvalidArg;
        }
        handle_ = xStreamBufferCreateStatic(storage.size(), triggerLevel, storage.data(), &buffer_);
        if (handle_ == nullptr) {
            return fw::Status::kNoSpace;
        }
        capacity_ = storage.size();
        return fw::Status::kOk;
    }

    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vStreamBufferDelete(handle_);
            handle_ = nullptr;
            capacity_ = 0U;
        }
    }

    /** 写入，返回实际写入字节数；回调中传 timeoutMs=0。 */
    [[nodiscard]] size_t send(std::span<const uint8_t> data, uint32_t timeoutMs) noexcept
    {
        if ((handle_ == nullptr) || data.empty()) {
            return 0U;
        }
        return xStreamBufferSend(handle_, data.data(), data.size(), pdMS_TO_TICKS(timeoutMs));
    }

    /** 读取，返回实际读取字节数；timeoutMs=0 为非阻塞。 */
    [[nodiscard]] size_t receive(std::span<uint8_t> out, uint32_t timeoutMs) noexcept
    {
        if ((handle_ == nullptr) || out.empty()) {
            return 0U;
        }
        return xStreamBufferReceive(handle_, out.data(), out.size(), pdMS_TO_TICKS(timeoutMs));
    }

    [[nodiscard]] size_t space() const noexcept
    {
        return (handle_ != nullptr) ? xStreamBufferSpacesAvailable(handle_) : 0U;
    }

    [[nodiscard]] size_t available() const noexcept
    {
        return (handle_ != nullptr) ? xStreamBufferBytesAvailable(handle_) : 0U;
    }

    [[nodiscard]] size_t capacity() const noexcept
    {
        return capacity_;
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
    StreamBufferHandle_t handle_ = nullptr;
    size_t capacity_ = 0U;
};

} // namespace rtos
