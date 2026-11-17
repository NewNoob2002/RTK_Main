// 全工程统一的可恢复错误码；SDK 错误码（esp_err_t）只在 rtos 中映射到这里（CPP-014）。
// 无状态、无分配、不阻塞；可在任意任务/ISR 中调用。
#pragma once

#include <cstdint>

namespace fw {

/** 全工程统一的可恢复错误码；无状态、可平凡拷贝。 */
enum class Status : uint8_t {
    kOk = 0,
    kInvalidArg,   // 参数非法（空 span、越界长度等）
    kNoSpace,      // 输出缓冲区或表容量不足
    kTimeout,      // 有界等待超时
    kNotFound,     // 句柄/键不存在
    kIoError,      // 底层驱动/SDK 失败
    kNotSupported, // 功能预留或目标不支持
    kBusy,         // 资源被占用（如重复初始化）
    kCorrupt,      // 持久化数据校验失败
    kInvalidState, // 调用顺序错误（如未 init）
};

/**
 * @brief  判断状态码是否为成功。
 * @param  status: 待判断的状态码。
 * @retval true  仅当 status == Status::kOk
 * @retval false 其余所有错误码
 * @note   constexpr、无状态、不阻塞，线程/ISR 安全。
 */
[[nodiscard]] constexpr bool isOk(Status status) noexcept
{
    return status == Status::kOk;
}

/** 返回静态字符串，用于日志；未知值返回 "?"。线程/ISR 安全。 */
[[nodiscard]] const char *toString(Status status) noexcept;

} // namespace fw
