// 全工程统一的可恢复错误码到静态字符串（日志用）。
// 无状态、无分配、不阻塞；可在任意任务/ISR 中调用，未知枚举值返回 "?"。

#include "fw/Status.hpp"

namespace fw {

/**
 * @brief  把状态转成稳定的静态字符串。
 * @param  status: 待转换的状态。
 * @retval 指向静态字符串的指针（"ok"、"invalid-arg"、"no-space"…）；未知值返回 "?"
 * @note   纯函数，任意任务/ISR 可调用；不分配、不阻塞。
 */
const char *toString(Status status) noexcept
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
