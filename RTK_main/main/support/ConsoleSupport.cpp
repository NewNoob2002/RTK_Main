// 控制台公共辅助实现：命令投递、回执报错与 MAC/角色格式化。
// 线程模型：仅在 console REPL 任务调用，非 ISR；formatMac/roleName 为纯函数。

#include "support/ConsoleSupport.hpp"

#include <array>
#include <cstddef>
#include <cstdio>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppContext.hpp"
#include "app/PowerRequests.hpp"
#include "def/DebugDef.hpp"
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "def/LinkDef.hpp"
#include "modules/gnss/GnssMaintenance.hpp"
#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "rtos/EspStatus.hpp"
#include "settings/Service.hpp"

namespace app {
namespace {

/** @brief 识别原始控制台命令中的持久化操作。 @param module: 目标模块。 @param cmd: 待提交命令。 @retval true 需设置准入。 @note 仅控制台。 */
[[nodiscard]] bool persistsSettings(ModuleKind module, const fw::Command &cmd) noexcept
{
    switch (module) {
    case ModuleKind::kBtLink:
        return cmd.verb == verb(LinkVerb::kWhitelistClear) ||
               cmd.verb == verb(LinkVerb::kWhitelistAdd) ||
               cmd.verb == verb(LinkVerb::kWhitelistDel);
    case ModuleKind::kDebug:
        return cmd.verb == static_cast<uint8_t>(DebugVerb::kSave);
    case ModuleKind::kGnss:
        return cmd.verb == static_cast<uint8_t>(GnssVerb::kSetRate) ||
               cmd.verb == static_cast<uint8_t>(GnssSubVerb::kSetOut) ||
               cmd.verb == static_cast<uint8_t>(GnssSubVerb::kSetGgaGate) ||
               cmd.verb == static_cast<uint8_t>(GnssSubVerb::kSetBase) ||
               cmd.verb == static_cast<uint8_t>(GnssSubVerb::kMode) ||
               cmd.verb == static_cast<uint8_t>(GnssMaintenanceVerb::kBaud) ||
               cmd.verb == static_cast<uint8_t>(GnssMaintenanceVerb::kFactoryReset) ||
               cmd.verb == static_cast<uint8_t>(GnssMaintenanceVerb::kSave);
    default:
        return false;
    }
}

/** @brief 登记持久化请求至入队完成。 @param executor: 目标执行器。 @param command: 待执行命令。 @param[out] ticket: 入队凭据。 @retval 投递结果。 @note 仅控制台，非 ISR。 */
[[nodiscard]] fw::Result postPersistent(rtos::Executor &executor, const fw::Command &command,
                                        rtos::Ticket &ticket) noexcept
{
    const power::SettingAdmission admission;
    if (!admission.admitted()) {
        return fw::Result::kBusy;
    }
    return executor.post(command, rtos::Caller::kConsole, ticket);
}

} // namespace

bool requireBluetooth() noexcept
{
    if (appContext().radioMode == RadioMode::kWifiConfig) {
        static_cast<void>(cli::fail("%s", kUnavailableInWifi));
        return false;
    }
    return true;
}

const char *roleName(core::Role role) noexcept
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

void formatMac(const core::BtAddr &addr, std::span<char, kMacStrBuf> out) noexcept
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

void printMac(const core::BtAddr &addr) noexcept
{
    std::array<char, kMacStrBuf> text{};
    formatMac(addr, text);
    cli::outPrintf("%s", text.data());
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

bool submitRawTo(rtos::Executor &executor, ModuleKind module, const char *downMsg,
                 const fw::Command &command, fw::CommandResult &out) noexcept
{
    if (!executor.running() || !executor.moduleOk(id(module))) {
        (void)cli::fail("%s", downMsg != nullptr ? downMsg : "executor not running");
        return false;
    }
    if (persistsSettings(module, command)) {
        rtos::Ticket ticket{};
        const auto posted = postPersistent(executor, command, ticket);
        if (!fw::isOk(posted)) {
            static_cast<void>(cli::fail("%s", fw::toString(posted)));
            return false;
        }
        const uint32_t startMs = rtos::nowMs();
        auto state = executor.poll(ticket, out);
        while (state == rtos::PollState::kPending && rtos::nowMs() - startMs < kCommandTimeoutMs) {
            vTaskDelay(1U);
            state = executor.poll(ticket, out);
        }
        if (state == rtos::PollState::kDone) {
            return true;
        }
        executor.cancel(ticket);
        static_cast<void>(cli::fail("no response"));
        return false;
    }
    if (executor.call(command, out, kCommandTimeoutMs) != fw::Result::kOk) {
        (void)cli::fail("no response");
        return false;
    }
    return true;
}

bool submitRaw(const fw::Command &command, fw::CommandResult &out) noexcept
{
    // ModuleKind 与 fw::ModuleId 使用同一编号。未登记的目标由 moduleOk 拒绝。
    const auto target = static_cast<ModuleKind>(command.target);
    const bool bluetoothTarget = target == ModuleKind::kBtLink || target == ModuleKind::kRtcm;
    if (bluetoothTarget && !requireBluetooth()) {
        return false;
    }
    if (appContext().linkExecutor == nullptr) {
        (void)cli::fail("link not running");
        return false;
    }
    return submitRawTo(*appContext().linkExecutor, target, "link not running", command, out);
}

bool submit(const fw::Command &command, const char *what) noexcept
{
    fw::CommandResult result{};
    if (!submitRaw(command, result)) {
        return false;
    }
    return report(what, result);
}

const char *settingsErrorText(settings::Error error) noexcept
{
    switch (error) {
    case settings::Error::kOk:
        return "ok";
    case settings::Error::kUnknownKey:
        return "unknown key";
    case settings::Error::kBadValue:
        return "bad value";
    case settings::Error::kLocked:
        return "locked";
    case settings::Error::kBusy:
        return "busy";
    case settings::Error::kUnsupported:
        return "unsupported";
    case settings::Error::kTimeout:
        return "timeout (may have been applied)";
    case settings::Error::kFailed:
    default:
        return "failed";
    }
}

bool submitSetting(std::string_view key, std::string_view value, bool factoryUnlocked,
                   uint32_t timeoutMs, fw::CommandResult &out, settings::Error &errOut) noexcept
{
    constexpr uint32_t kPollStepMs = 20U;
    out = {};
    settings::Ticket ticket{};
    auto &service = appContext().settings;
    const settings::Error postErr =
        service.set(key, value, settings::Caller::kConsole, ticket, factoryUnlocked);
    if (postErr != settings::Error::kOk) {
        errOut = postErr;
        return false;
    }
    const uint32_t startMs = rtos::nowMs();
    for (;;) {
        const auto state = service.poll(ticket, out);
        if (state == settings::PollState::kDone) {
            errOut = settings::fromResult(out.status);
            return fw::isOk(out.status);
        }
        if (state != settings::PollState::kPending) {
            errOut = settings::Error::kFailed;
            return false;
        }
        if ((rtos::nowMs() - startMs) >= timeoutMs) {
            service.cancel(ticket);
            errOut = settings::Error::kTimeout;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(kPollStepMs));
    }
}

} // namespace app
