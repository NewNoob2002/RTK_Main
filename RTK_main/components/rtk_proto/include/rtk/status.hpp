// 冻结接口（ADR-0002）。实现方不得修改。
// 全工程统一的可恢复错误码；SDK 错误码（esp_err_t）只在 rtk_os 中映射到这里（CPP-014）。
#pragma once

#include <cstdint>

namespace rtk {

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

[[nodiscard]] constexpr bool is_ok(Status status) noexcept
{
    return status == Status::kOk;
}

/** 返回静态字符串，用于日志；未知值返回 "?"。 */
[[nodiscard]] const char *to_string(Status status) noexcept;

} // namespace rtk
