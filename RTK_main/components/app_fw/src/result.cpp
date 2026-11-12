// rtk::fw::Result 文字描述（ADR-0007 §3.5）。
//
// Generated-by: Claude Code + claude-opus-5-5
// Owner: UNKNOWN（待人工审查签字）
#include "rtk/fw/result.hpp"

namespace rtk::fw {

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

} // namespace rtk::fw
