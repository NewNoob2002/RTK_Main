// fw::Result 文字描述（ADR-0007 §3.5）。
// 纯函数，无状态；可在任意任务/ISR 中调用，不分配、不阻塞。
#include "fw/Result.hpp"

namespace fw {

/**
 * @brief  把结果码转成稳定的静态字符串。
 * @param  r: 结果码。
 * @retval 指向静态字符串的指针（"ok"、"invalid_arg"、"no_space"…）；未知值返回 "unknown"
 * @note   纯函数，任意任务/ISR 可调用；不分配、不阻塞。
 */
const char *toString(Result r) noexcept
{
    switch (r) {
    case Result::kOk:
        return "ok";
    case Result::kInvalidArg:
        return "invalid_arg";
    case Result::kNoSpace:
        return "no_space";
    case Result::kTimeout:
        return "timeout";
    case Result::kNotFound:
        return "not_found";
    case Result::kIoError:
        return "io_error";
    case Result::kNotSupported:
        return "not_supported";
    case Result::kBusy:
        return "busy";
    case Result::kInvalidState:
        return "invalid_state";
    default:
        return "unknown";
    }
}

} // namespace fw
