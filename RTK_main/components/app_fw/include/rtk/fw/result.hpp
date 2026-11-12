// app_fw 自带的结果码：app_fw 是通用组件（不依赖 rtk_*），不能用 rtk::Status；
// 产品侧在 main 中做一次映射（rtk::Status ↔ rtk::fw::Result）。
// 冻结接口（ADR-0007）。
#pragma once

#include <cstdint>

namespace rtk::fw {

/** 可恢复错误码；无状态、可平凡拷贝。 */
enum class Result : uint8_t {
    kOk = 0,
    kInvalidArg,
    kNoSpace,
    kTimeout,
    kNotFound,
    kIoError,
    kNotSupported,
    kBusy,
    kInvalidState,
};

[[nodiscard]] constexpr bool is_ok(Result r) noexcept
{
    return r == Result::kOk;
}

/** 返回静态字符串（"ok"、"invalid_arg"…）；未知值返回 "unknown"。线程安全。 */
[[nodiscard]] const char *to_string(Result r) noexcept;

} // namespace rtk::fw
