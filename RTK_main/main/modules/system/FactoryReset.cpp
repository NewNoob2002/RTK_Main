// 整机恢复出厂命令。控制台确认，kLink 独占擦除 NVS，回执后重启。
#include "modules/system/FactoryReset.hpp"

#include <array>
#include <string_view>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "fw/Log.hpp"
#include "modules/system/SystemCmdsPrivate.hpp"

namespace app {
namespace {

/**
 * @brief 执行已确认的擦除。
 * @param factory: 是否擦除出厂分区。
 * @retval 0 成功；非零为已输出 ERR。
 * @note 仅 console，可以等待执行器与控制台排空。
 */
int runReset(bool factory) noexcept
{
    auto *executor = appContext().linkExecutor;
    constexpr auto kStorage = id(ModuleKind::kStorage);
    if (executor == nullptr || !executor->running() || !executor->moduleOk(kStorage)) {
        return cli::fail("storage not running");
    }
    fw::Command command{};
    command.target = kStorage;
    command.verb = factory ? StorageModule::kEraseFactory : StorageModule::kEraseUser;
    fw::CommandResult result{};
    constexpr uint32_t kEraseWaitMs = 15000U;
    if (!fw::isOk(executor->call(command, result, kEraseWaitMs))) {
        return cli::fail("no response from storage");
    }
    if (result.status == fw::Result::kBusy) {
        return cli::fail("storage busy");
    }
    if (!fw::isOk(result.status)) {
        return cli::fail("factory reset failed: %s", esp_err_to_name(result.detail));
    }
    FW_LOGI(fw::TraceCat::kStore, "Factory reset done, restarting");
    cli::outPrintf("OK\n");
    vTaskDelay(pdMS_TO_TICKS(detail::kRebootDelayMs));
    esp_restart();
    return 0;
}

/** @brief 恢复用户分区的顶层命令。 @param argc: 参数数。 @param argv: 参数。 @retval 0 成功。 */
int cmdReset(void *, int argc, char **argv) noexcept
{
    return confirmFactoryReset(argc, argv, false);
}

constexpr std::array<cli::Cmd, 1U> kResetCmds{{
    {"freset", "freset [yes]: erase user settings and restart", "[yes]", cmdReset, nullptr, false},
}};

} // namespace

int confirmFactoryReset(int argc, char **argv, bool factory) noexcept
{
    const char *usage = factory ? "factory erase yes" : "freset yes";
    if (argc == 1) {
        cli::outPrintf("Clears user settings, GNSS cache, alarm whitelist and Bluetooth bonds.\n");
        cli::outPrintf(
            "%s\n",
            factory ? "Also clears factory NVS. Keeps PHY data, core dump and receiver settings."
                    : "Keeps factory NVS, PHY data, core dump and receiver settings.");
        cli::outPrintf("Confirm with '%s'.\n", usage);
        return 0;
    }
    if (argc != 2 || argv == nullptr || std::string_view{std::span{argv, 2U}[1]} != "yes") {
        return cli::fail("usage: %s", usage);
    }
    return runReset(factory);
}

std::span<const cli::Cmd> factoryResetCmds() noexcept
{
    return kResetCmds;
}

} // namespace app
