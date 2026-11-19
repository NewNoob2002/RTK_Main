// 网页配置协议错误码的字符串与 fw::Result 映射（docs/web/ws-protocol.md §5）。
// 纯函数、无状态；任意任务调用，不阻塞，不分配堆。
#include "web_proto/Error.hpp"

namespace web {

const char *toString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::kNone:
        return "ok";
    case ErrorCode::kBadJson:
        return "bad_json";
    case ErrorCode::kTooLong:
        return "too_long";
    case ErrorCode::kUnknownKey:
        return "unknown_key";
    case ErrorCode::kBadValue:
        return "bad_value";
    case ErrorCode::kLocked:
        return "locked";
    case ErrorCode::kBusy:
        return "busy";
    case ErrorCode::kUnsupported:
        return "unsupported";
    case ErrorCode::kFailed:
        return "failed";
    case ErrorCode::kTimeout:
        return "timeout";
    default:
        return "failed";
    }
}

ErrorCode fromResult(fw::Result result) noexcept
{
    switch (result) {
    case fw::Result::kOk:
        return ErrorCode::kNone;
    case fw::Result::kInvalidArg:
        return ErrorCode::kBadValue;
    case fw::Result::kBusy:
        return ErrorCode::kBusy;
    case fw::Result::kNotSupported:
        return ErrorCode::kUnsupported;
    case fw::Result::kTimeout:
        return ErrorCode::kTimeout;
    case fw::Result::kNoSpace:
    case fw::Result::kNotFound:
    case fw::Result::kIoError:
    case fw::Result::kInvalidState:
    default:
        return ErrorCode::kFailed;
    }
}

} // namespace web
