// 控制台公共辅助实现：命令投递、回执报错与 MAC/角色格式化。
// 线程模型：仅在 console REPL 任务调用，非 ISR；format_mac/role_name 为纯函数。

#include "support/console_support.hpp"

#include <array>
#include <cstddef>
#include <cstdio>

#include "esp_err.h"

#include "app_context.hpp"
#include "cli/console.hpp"
#include "cli/out.hpp"

namespace app {

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

void format_mac(const core::BtAddr &addr, std::span<char, kMacStrBuf> out) noexcept
{
    constexpr std::string_view kHex = "0123456789ABCDEF";
    constexpr uint8_t kNibbleMask = 0x0FU;
    constexpr unsigned kNibbleShift = 4U;
    size_t pos = 0U;
    for (size_t idx = 0U; idx < core::kAddrLen; ++idx) {
        if (idx > 0U) {
            out[pos] = ':';
            ++pos;
        }
        const uint8_t byte = addr.bytes[idx];
        // 半字节范围 0..15，转 size_t 索引 kHex 安全。
        out[pos] = kHex[static_cast<size_t>(byte >> kNibbleShift)];
        ++pos;
        out[pos] = kHex[static_cast<size_t>(byte & kNibbleMask)];
        ++pos;
    }
    out[pos] = '\0';
}

void print_mac(const core::BtAddr &addr) noexcept
{
    std::array<char, kMacStrBuf> text{};
    format_mac(addr, text);
    cli::out_printf("%s", text.data());
}

bool report(const char * /*what*/, const fw::CommandResult &result) noexcept
{
    switch (result.status) {
    case fw::Result::kOk:
        return true;
    case fw::Result::kInvalidArg:
        (void)cli::fail("invalid argument");
        return false;
    case fw::Result::kInvalidState:
        (void)cli::fail("link not running");
        return false;
    case fw::Result::kBusy:
        (void)cli::fail("link busy");
        return false;
    case fw::Result::kIoError:
        (void)cli::fail("store error: %s", esp_err_to_name(result.detail));
        return false;
    default:
        (void)cli::fail("unknown result");
        return false;
    }
}

bool submit_raw(const fw::Command &command, fw::CommandResult &out) noexcept
{
    auto &executor = app_context().link_executor;
    if (!executor.running() || !executor.module_ok(id(ModuleKind::kBtLink))) {
        (void)cli::fail("link not running");
        return false;
    }
    if (executor.call(command, out, kCommandTimeoutMs) != fw::Result::kOk) {
        (void)cli::fail("no response");
        return false;
    }
    return true;
}

bool submit(const fw::Command &command, const char *what) noexcept
{
    fw::CommandResult result{};
    if (!submit_raw(command, result)) {
        return false;
    }
    return report(what, result);
}

} // namespace app
