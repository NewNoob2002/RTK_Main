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

    /**
     * @brief  用对象内部存储创建静态 StreamBuffer（xStreamBufferCreateStatic）
     * @param  triggerLevel: 接收方被唤醒所需的字节数，必须在 1..N
     * @retval fw::Status::kOk 创建成功
     * @retval fw::Status::kBusy 已初始化
     * @retval fw::Status::kInvalidArg triggerLevel 不在 1..N
     * @retval fw::Status::kNoSpace 创建失败
     * @note   启动期调用；不分配堆
     */
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

    /** @brief 删除 StreamBuffer（vStreamBufferDelete）并置为未初始化。 */
    void deinit() noexcept
    {
        if (handle_ != nullptr) {
            vStreamBufferDelete(handle_);
            handle_ = nullptr;
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
            return 0;
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
            return 0;
        }
        return xStreamBufferReceive(handle_, out.data(), out.size(), pdMS_TO_TICKS(timeoutMs));
    }

    /**
     * @brief  当前可写入的字节数
     * @retval 剩余空间字节数；未初始化返回 0
     * @note   单写者据此整帧写入或整帧丢弃；非阻塞
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

    /** @brief 清空缓冲内容（xStreamBufferReset）；未初始化时无副作用。 */
    void reset() noexcept
    {
        if (handle_ != nullptr) {
            xStreamBufferReset(handle_);
        }
    }

    /**
     * @brief  缓冲是否已创建
     * @retval true 已初始化；false 未初始化或已 deinit
     */
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
