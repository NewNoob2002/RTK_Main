// GNSS 控制台维护命令，按 kIo 分配的完成序号等待结果。
// 仅 console REPL 任务调用，等待有界，非 ISR。
#include "modules/gnss/GnssCmdsPrivate.hpp"

#include <bit>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "modules/gnss/GnssConsoleValue.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/gnss/GnssMaintenance.hpp"
#include "modules/gnss/GnssModulePrivate.hpp"
#include "support/ConsoleSupport.hpp"

namespace app::detail {
namespace {

constexpr uint32_t kMaintenancePollMs = 50U;
constexpr uint32_t kMaintenanceWaitMs = 6000U;

/**
 * @brief 估算复位后的探测和配置等待上限，覆盖一次完整重探。
 * @retval 毫秒上限。
 */
uint32_t factoryResetWaitMs() noexcept
{
    // 启用的驱动表大小和波特率候选上限均为小的静态常量。
    const auto candidates = static_cast<uint32_t>(
        ::gnss::enabledReceivers().size() * GnssModule::kMaxBaudCandidates + 1U);
    constexpr uint32_t kIoWaitMs = rtos::kExecIdleWaitMs;
    constexpr uint32_t kBaseSteps = 3U; // 三家驱动的基础脚本最多三步，均使用 500 ms 应答上限。
    constexpr uint32_t kConfigSteps = kBaseSteps + ::gnss::kOutputMsgCount;
    return ::gnss::kTimingBootMs +
           candidates * ::gnss::kTimingProbeRounds *
               (::gnss::kTimingProbeTimeoutMs + 2U * kIoWaitMs) +
           kConfigSteps * (::gnss::kTimingStepRetries + 1U) * (::gnss::kStepTimeoutMs + kIoWaitMs) +
           kMaintenanceWaitMs; // 存储操作、轮询和调度余量。
}

/**
 * @brief 打印维护命令结果。
 * @param status: 操作结果。
 * @param success: 成功文本。
 * @retval 0 成功；其他已输出 ERR。
 */
int reportMaintenance(::gnss::MaintenanceResult status, const char *success) noexcept
{
    switch (status) {
    case ::gnss::MaintenanceResult::kOk:
        cli::outPrintf("%s\n", success);
        return 0;
    case ::gnss::MaintenanceResult::kNack:
        return cli::fail("receiver rejected command");
    case ::gnss::MaintenanceResult::kUnsupported:
        return cli::fail("not supported");
    case ::gnss::MaintenanceResult::kTimeout:
        return cli::fail("timeout");
    case ::gnss::MaintenanceResult::kIoError:
        return cli::fail("GNSS UART or settings write failed");
    default:
        return cli::fail("unknown receiver result");
    }
}

/**
 * @brief 等待本次维护结果，复位还需等重探和配置结束。
 * @param seq: kIo 返回的操作序号。
 * @param reset: 是否是出厂复位。
 * @param success: 成功文本。
 * @retval 0 成功；其他已输出 ERR。
 */
int waitMaintenance(uint32_t seq, bool reset, const char *success) noexcept
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(reset ? factoryResetWaitMs() : kMaintenanceWaitMs);
    do {
        const auto snapshot = appContext().data.gnssStatus.read();
        if (snapshot.ctrl.maintenanceSeq == seq) {
            const auto result = snapshot.ctrl.maintenanceResult;
            if (!reset || result != ::gnss::MaintenanceResult::kOk ||
                snapshot.state == ::gnss::State::kRunning) {
                return reportMaintenance(result, success);
            }
            if (snapshot.state == ::gnss::State::kFailed) {
                return cli::fail("receiver re-probe failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kMaintenancePollMs));
    } while ((xTaskGetTickCount() - start) < limit);
    return cli::fail("timeout");
}

/**
 * @brief 提交一次维护命令并等待该次完成。
 * @param verb: 命令种类。
 * @param success: 成功文本。
 * @param baud: 切速用的参数。
 * @retval 0 成功；其他已输出 ERR。
 */
int submitMaintenance(GnssMaintenanceVerb verb, const char *success, uint32_t baud = 0U) noexcept
{
    const auto &context = appContext();
    if (context.ioExecutor == nullptr) {
        return cli::fail("gnss not running");
    }
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(verb);
    if (verb == GnssMaintenanceVerb::kBaud) {
        cmd.argLen = sizeof(baud);
        writeU32(baud, std::span{cmd.args}.first(sizeof(baud)));
    }
    fw::CommandResult result{};
    if (!submitRawTo(*context.ioExecutor, ModuleKind::kGnss, "gnss not running", cmd, result)) {
        return cli::kCmdFail;
    }
    if (result.status != fw::Result::kOk) {
        return cli::fail("%s",
                         result.status == fw::Result::kBusy ? "busy" : fw::toString(result.status));
    }
    return waitMaintenance(std::bit_cast<uint32_t>(result.detail),
                           verb == GnssMaintenanceVerb::kFactoryReset, success);
}

} // namespace

int cmdGnssSave(void *, int argc, char **) noexcept
{
    return argc == 1 ? submitMaintenance(GnssMaintenanceVerb::kSave, "saved")
                     : cli::fail("usage: gnss save");
}

int cmdGnssFactoryReset(void *, int argc, char **argv) noexcept
{
    if (argc != 2 || argv == nullptr || std::string_view{std::span{argv, 2U}[1]} != "yes") {
        return cli::fail(
            "gnss freset restores receiver and GNSS defaults, confirm with gnss freset yes");
    }
    return submitMaintenance(GnssMaintenanceVerb::kFactoryReset,
                             "restored defaults, receiver ready");
}

int cmdGnssBaud(void *, int argc, char **argv) noexcept
{
    uint32_t baud = 0U;
    if (argc != 2 || argv == nullptr || !parseGnssUnsigned(std::span{argv, 2U}[1], baud) ||
        !gnss_store::validUserBaud(baud)) {
        return cli::fail("usage: gnss baud <9600|19200|38400|57600|115200|230400|460800|921600>");
    }
    return submitMaintenance(GnssMaintenanceVerb::kBaud, "baud applied", baud);
}

} // namespace app::detail
