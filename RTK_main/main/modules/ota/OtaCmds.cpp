// DEV 控制台 OTA 状态与台架操作（ADR-0019 §2.7）。
// 仅控制台任务调用命令，可阻塞访问 Flash 和 UART；非 ISR。
#include "modules/ota/OtaCmds.hpp"

#include <array>

#include "AppConfig.hpp"
#include "app/PowerRequests.hpp"
#include "app/RadioMode.hpp"
#include "cli/Out.hpp"
#include "driver/uart.h"
#include "fw/Log.hpp"
#include "modules/ota/OtaRuntime.hpp"

namespace app {
namespace {

/**
 * @brief 输出运行槽、启动槽、另一槽和最近上传结果
 * @param argc: 参数数（含动词）
 * @retval 0 已输出状态
 * @retval 非零 参数错误
 * @note 仅控制台任务，可读 Flash，非 ISR。
 */
int cmdOtaStatus(void *, int argc, char **) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: ota status");
    }
    const auto *running = esp_ota_get_running_partition();
    const auto *boot = esp_ota_get_boot_partition();
    const auto *other = esp_ota_get_next_update_partition(running);
    esp_app_desc_t desc{};
    const auto version = appVersion();
    const auto otherVersion = ota::otherSlotVersion(other, desc).value_or("-");
    cli::printSection("ota");
    // 两处描述符版本字段都是最多 32 B，精度转换无损。
    cli::printKv("running", "%s state=%s version=%.*s",
                 running != nullptr ? std::span{running->label}.data() : "-",
                 ota::stateName(ota::partitionState(running)), static_cast<int>(version.size()),
                 version.data());
    cli::printKv("boot", "%s", boot != nullptr ? std::span{boot->label}.data() : "-");
    cli::printKv("other", "%s state=%s version=%.*s",
                 other != nullptr ? std::span{other->label}.data() : "-",
                 ota::stateName(ota::partitionState(other)), static_cast<int>(otherVersion.size()),
                 otherVersion.data());
    cli::printKv("rollback_possible", "%s", esp_ota_check_rollback_is_possible() ? "yes" : "no");
    cli::printKv("last_upload", "%s", ota::lastUpload());
    cli::outPrintf("OK\n");
    return 0;
}

/**
 * @brief 手动确认当前运行镜像
 * @param argc: 参数数（含动词）
 * @retval 0 确认成功
 * @retval 非零 参数错误、忙或 SDK 失败
 * @note 仅控制台任务，可写 Flash，非 ISR。
 */
int cmdOtaValid(void *, int argc, char **) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: ota valid");
    }
    if (!power::s_power_requests.canScheduleRestart() ||
        !ota::beginOperation(ota::Operation::kConfirm)) {
        return cli::fail("ota busy");
    }
    const auto status = ota::confirm();
    ota::endOperation();
    if (!fw::isOk(status)) {
        return cli::fail("ota valid %s", fw::toString(status));
    }
    cli::outPrintf("OK\n");
    return 0;
}

/**
 * @brief 用户确认后切换至合法的另一槽，回执后重启。
 * @param argc: 参数数（含动词）。 @param argv: 参数。
 * @retval 0 显示确认提示或完成回执；非零为参数、忙或切槽失败。
 * @note 仅控制台任务，可等待 Flash 和 UART，非 ISR。
 */
int cmdOtaRollback(void *, int argc, char **argv) noexcept
{
    if (argc == 1) {
        cli::outPrintf("Confirm switching to the other firmware with 'ota rollback yes'.\n");
        return 0;
    }
    if (argc != 2 || argv == nullptr || std::string_view{std::span{argv, 2U}[1]} != "yes") {
        return cli::fail("usage: ota rollback [yes]");
    }
    esp_app_desc_t desc{};
    switch (ota::switchToOther(desc)) {
    case ota::SwitchResult::kBusy:
        return cli::fail("ota busy");
    case ota::SwitchResult::kNoValidOther:
        return cli::fail("ota rollback no_valid_other");
    case ota::SwitchResult::kFlash:
        return cli::fail("ota rollback flash");
    case ota::SwitchResult::kOk:
        break;
    default:
        return cli::fail("ota rollback failed");
    }
    constexpr uint32_t kDrainMs = 500U;
    // Kconfig 限定 0..2，转换后的端口由控制台在启动期初始化。
    constexpr auto kPort = static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM);
    cli::outPrintf("OK\n");
    const auto err = uart_wait_tx_done(kPort, pdMS_TO_TICKS(kDrainMs));
    if (err != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "<ota> rollback reply drain failed: %s", esp_err_to_name(err));
    }
    restartWithOledQuiesce(RebootBanner::kRebooting);
    return 0;
}

constexpr std::array kOtaVerbs{
    cli::Verb{"status", nullptr, "show app slot and last upload state", &cmdOtaStatus},
    cli::Verb{"valid", nullptr, "confirm running app", &cmdOtaValid},
    cli::Verb{"rollback", "[yes]", "switch to the other firmware and reboot", &cmdOtaRollback},
};

/**
 * @brief 分派 OTA 控制台动词
 * @param ctx: 命令上下文，由框架透传
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @retval 0 执行成功
 * @retval 非零 参数错误或执行失败
 * @note 仅控制台任务；rollback 成功会重启。
 */
int cmdOta(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kOtaVerbs, ctx, argc, argv, "status");
}

} // namespace

std::span<const cli::Cmd> otaCmds() noexcept
{
    static constexpr std::array kCommands{
        cli::Cmd{"ota", "application OTA and rollback", "[status|valid|rollback]", &cmdOta,
                 nullptr},
    };
    return kCommands;
}

std::span<const cli::Verb> otaVerbs() noexcept
{
    return kOtaVerbs;
}

} // namespace app
