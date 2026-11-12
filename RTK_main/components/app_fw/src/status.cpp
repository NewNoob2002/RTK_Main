// 全工程统一的可恢复错误码到静态字符串（日志用）。
// 无状态、无分配、不阻塞；可在任意任务/ISR 中调用，未知枚举值返回 "?"。

#include "fw/status.hpp"

namespace fw {

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

} // namespace fw
