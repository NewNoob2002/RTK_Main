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

    /**
     * @brief  用外供存储区创建静态 StreamBuffer（xStreamBufferCreateStatic）
     * @param  storage: 存储区，寿命必须覆盖 StreamBuffer 的生命期，必须非空
     * @param  triggerLevel: 接收方被唤醒所需的字节数，必须在 1..storage.size()
     * @retval fw::Status::kOk 创建成功
     * @retval fw::Status::kBusy 已初始化
     * @retval fw::Status::kInvalidArg storage 为空或 triggerLevel 越界
     * @retval fw::Status::kNoSpace 创建失败
     * @note   启动期调用；不分配堆；成功后 capacity() 返回 storage.size()
     */
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

    /** @brief 删除 StreamBuffer（vStreamBufferDelete）、清空容量记录并置为未初始化。 */
    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vStreamBufferDelete(handle_);
            handle_ = nullptr;
            capacity_ = 0U;
        }
    }

    /**
     * @brief  写入字节流（xStreamBufferSend）
     * @param  data: 待写数据
     * @param  timeoutMs: 空间不足时的等待毫秒数，0 表示不等待
     * @retval 实际写入的字节数；未初始化或 data 为空返回 0
     * @note   单写者语义；回调中用 timeoutMs=0；timeoutMs>0 会阻塞
     */
    [[nodiscard]] size_t send(std::span<const uint8_t> data, uint32_t timeoutMs) noexcept
    {
        if ((handle_ == nullptr) || data.empty()) {
            return 0U;
        }
        return xStreamBufferSend(handle_, data.data(), data.size(), pdMS_TO_TICKS(timeoutMs));
    }

    /**
     * @brief  读取字节流（xStreamBufferReceive）
     * @param[out] out: 接收缓冲区；成功时写入读到的字节，失败时内容不变
     * @param  timeoutMs: 无数据时的等待毫秒数，0 表示不等待
     * @retval 实际读取的字节数；未初始化或 out 为空返回 0
     * @note   单读者语义；timeoutMs=0 为非阻塞
     */
    [[nodiscard]] size_t receive(std::span<uint8_t> out, uint32_t timeoutMs) noexcept
    {
        if ((handle_ == nullptr) || out.empty()) {
            return 0U;
        }
        return xStreamBufferReceive(handle_, out.data(), out.size(), pdMS_TO_TICKS(timeoutMs));
    }

    /**
     * @brief  当前可写入的字节数
     * @retval 剩余空间字节数；未初始化返回 0
     */
    [[nodiscard]] size_t space() const noexcept
    {
        return (handle_ != nullptr) ? xStreamBufferSpacesAvailable(handle_) : 0U;
    }

    /**
     * @brief  当前可读取的字节数
     * @retval 已缓存字节数；未初始化返回 0
     */
    [[nodiscard]] size_t available() const noexcept
    {
        return (handle_ != nullptr) ? xStreamBufferBytesAvailable(handle_) : 0U;
    }

    /**
     * @brief  初始化时登记的存储区容量
     * @retval 字节数；未初始化返回 0
     */
    [[nodiscard]] size_t capacity() const noexcept
    {
        return capacity_;
    }

    /** @brief 清空缓冲内容（xStreamBufferReset）；未初始化时无副作用。 */
    void reset() noexcept
    {
        if (handle_ != nullptr) {
            xStreamBufferReset(handle_);
        }
    }

    /**
     * @brief  StreamBuffer 是否已创建
     * @retval true 已初始化；false 未初始化或已 deinit
     */
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
