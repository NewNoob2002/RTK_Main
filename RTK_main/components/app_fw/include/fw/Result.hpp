// app_fw 的模块结果码；与 fw::Status 分别保留既有枚举值及字符串映射。
// 产品侧在 main 中做一次映射（fw::Status ↔ fw::Result）。
// 冻结接口（ADR-0007）。
#pragma once

#include <cstdint>

namespace fw {

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

/**
 * @brief  判断结果码是否为成功。
 * @param  r: 待判断的结果码。
 * @retval true  仅当 r == Result::kOk
 * @retval false 其余所有错误码
 * @note   constexpr、无状态，任意上下文可用。
 */
[[nodiscard]] constexpr bool isOk(Result r) noexcept
{
    return r == Result::kOk;
}

/** 返回静态字符串（"ok"、"invalid_arg"…）；未知值返回 "unknown"。线程安全。 */
[[nodiscard]] const char *toString(Result r) noexcept;

} // namespace fw
