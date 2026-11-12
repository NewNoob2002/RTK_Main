// 全工程统一的可恢复错误码到静态字符串（日志用）。
// 未知枚举值返回 "?"，不越界。
//
// Generated-by: omp + DeepSeek-V4.1-Flash
// Owner: UNKNOWN（待人工审查签字）

#include "rtk/status.hpp"

namespace rtk {

const char *to_string(Status status) noexcept
{
    switch (status) {
    case Status::kOk:
        return "ok";
    case Status::kInvalidArg:
        return "invalid-arg";
    case Status::kNoSpace:
        return "no-space";
    case Status::kTimeout:
        return "timeout";
    case Status::kNotFound:
        return "not-found";
    case Status::kIoError:
        return "io-error";
    case Status::kNotSupported:
        return "not-supported";
    case Status::kBusy:
        return "busy";
    case Status::kCorrupt:
        return "corrupt";
    case Status::kInvalidState:
        return "invalid-state";
    default:
        return "?";
    }
}

} // namespace rtk
