// 控制台公共辅助实现（从原 app_console.cpp 拆出，行为与输出文本逐字节一致）。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "support/console_support.hpp"

#include <cstddef>
#include <cstdio>

#include "esp_err.h"

#include "app_context.hpp"

namespace rtk::app {

const char *role_name(core::Role role) noexcept
{
    switch (role) {
    case core::Role::kNone:
        return "NONE";
    case core::Role::kPhone:
        return "PHONE";
    case core::Role::kAlarm:
        return "ALARM";
    case core::Role::kPending:
        return "PENDING";
    default:
        return "?";
    }
}

void print_mac(const core::BtAddr &addr) noexcept
{
    for (size_t idx = 0U; idx < core::kAddrLen; idx++) {
        printf("%s%02X", (idx == 0U) ? "" : ":", static_cast<unsigned>(addr.bytes[idx]));
    }
}

bool report(const char *what, const fw::CommandResult &result) noexcept
{
    switch (result.status) {
    case fw::Result::kOk:
        printf("%s: ok\n", what);
        return true;
    case fw::Result::kInvalidArg:
        printf("%s: invalid argument\n", what);
        return false;
    case fw::Result::kInvalidState:
        printf("%s: link not running; NOT executed\n", what);
        return false;
    case fw::Result::kBusy:
        printf("%s: link busy; NOT executed\n", what);
        return false;
    case fw::Result::kIoError:
        printf("%s: store error: %s\n", what, esp_err_to_name(result.detail));
        return false;
    default:
        printf("%s: unknown result\n", what);
        return false;
    }
}

bool submit(const fw::Command &command, const char *what) noexcept
{
    fw::CommandResult result{};
    auto &executor = app_context().link_executor;
    if (!executor.running() || !executor.module_ok(id(ModuleKind::kBtLink))) {
        printf("%s: link not running; NOT executed\n", what);
        return false;
    }
    if (executor.call(command, result, kCommandTimeoutMs) != fw::Result::kOk) {
        printf("%s: no response; execution result unknown\n", what);
        return false;
    }
    return report(what, result);
}

} // namespace rtk::app
