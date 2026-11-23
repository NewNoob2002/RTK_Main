// DEV 控制台 OTA 状态与台架操作（ADR-0019 §2.7）。
// 仅控制台任务调用命令，可阻塞访问 Flash 和 UART；非 ISR。
#include "modules/ota/OtaCmds.hpp"

#include <array>

#include "AppConfig.hpp"
#include "app/PowerRequests.hpp"
#include "cli/Out.hpp"
#include "driver/uart.h"
#include "esp_system.h"
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
    const auto otherVersion =
        other != nullptr && esp_ota_get_partition_description(other, &desc) == ESP_OK
            ? ota::imageText(desc.version)
            : std::string_view{"-"};
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
 * @brief 仅当 IDF 已成功判无效并进入 esp_restart 时发送命令成功回执
 * @note 在请求回滚的控制台任务执行。UART 驱动仍有效，可等待 TX；非 ISR。
 */
void replyBeforeRollback() noexcept
{
    constexpr uint32_t kDrainMs = 500U;
    // Kconfig 限定 0..2，转换后的端口由控制台在启动期初始化。
    constexpr auto kPort = static_cast<uart_port_t>(CONFIG_RTK_CONSOLE_UART_NUM);
    cli::outPrintf("OK\n");
    const auto err = uart_wait_tx_done(kPort, pdMS_TO_TICKS(kDrainMs));
    if (err != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "<ota> rollback reply drain failed: %s", esp_err_to_name(err));
    }
}

/**
 * @brief 标记运行镜像无效并重启
 * @param argc: 参数数（含动词）
 * @retval 非零 参数错误、忙或回滚失败；成功重启
 * @note 仅控制台任务，持有重启和 OTA 操作权后才允许操作。
 */
int cmdOtaRollback(void *, int argc, char **) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: ota rollback");
    }
    if (!ota::beginOperation(ota::Operation::kRollback)) {
        return cli::fail("ota busy");
    }
    if (!power::s_power_requests.claimRestart()) {
        ota::endOperation();
        return cli::fail("power action already in progress");
    }
    const auto registration = esp_register_shutdown_handler(&replyBeforeRollback);
    if (registration != ESP_OK) {
        power::s_power_requests.releaseAction();
        ota::endOperation();
        return cli::fail("ota rollback %s", esp_err_to_name(registration));
    }

    const auto status = ota::invalidateAndReboot();
    const auto cleanup = esp_unregister_shutdown_handler(&replyBeforeRollback);
    if (cleanup != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "<ota> unregister rollback reply: %s",
                esp_err_to_name(cleanup));
    }
    power::s_power_requests.releaseAction();
    ota::endOperation();
    return cli::fail("ota rollback %s", fw::toString(status));
}

constexpr std::array kOtaVerbs{
    cli::Verb{"status", nullptr, "show app slot and last upload state", &cmdOtaStatus},
    cli::Verb{"valid", nullptr, "confirm running app", &cmdOtaValid},
    cli::Verb{"rollback", nullptr, "invalidate running app and reboot", &cmdOtaRollback},
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
