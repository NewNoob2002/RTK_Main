// fw::Result 文字描述（ADR-0007 §3.5）。
// 纯函数，无状态；可在任意任务/ISR 中调用，不分配、不阻塞。
#include "fw/Result.hpp"

namespace fw {

const char *to_string(Result r) noexcept
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
