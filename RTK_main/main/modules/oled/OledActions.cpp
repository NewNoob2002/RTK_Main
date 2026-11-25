// OLED 通信动作与蓝牙只读快照的 kLink 桥接。OLED 刷新任务通过 UiEventBus 提交动作。
// 仅 kLink 调用，非 ISR。配对快照持短临界区，无堆分配。
#include "modules/oled/OledModule.hpp"

#include <algorithm>

#include "AppContext.hpp"
#include "SppServerPairing.h"
#include "app/PowerRequests.hpp"
#include "modules/bt_link/BtPairing.hpp"
#include "modules/power/PowerModule.hpp"

namespace app {

bool OledModule::poll(uint32_t nowMs) noexcept
{
    if (!present_.load(std::memory_order_acquire)) {
        return false;
    }
    notifyDisplayData(nowMs);
    if (context_->bootMode != board::BootMode::kNormal) {
        return false;
    }
    const auto request = context_->data.uiEvents.takeAction();
    if (request.action != OledAction::kNone && power::s_power_requests.canScheduleRestart()) {
        applyAction(request, nowMs);
    }
    if (context_->radioMode == RadioMode::kBluetooth) {
        publishPairing(nowMs);
    }
    return false;
}

void OledModule::notifyDisplayData(uint32_t nowMs) noexcept
{
    constexpr uint32_t kDataIntervalMs = 200U;
    if ((nowMs - lastDataCheckMs_) < kDataIntervalMs) {
        return;
    }
    lastDataCheckMs_ = nowMs;

    auto &data = context_->data;
    DisplayVersions versions{};
    static_cast<void>(data.powerStatus.read(versions.power));
    if (context_->bootMode == board::BootMode::kNormal) {
        static_cast<void>(data.systemStatus.read(versions.system));
        static_cast<void>(data.sessions.read(versions.bluetooth));
#if RTK_GNSS_ENABLED
        static_cast<void>(data.gnssFix.read(versions.gnssFix));
        static_cast<void>(data.gnssSats.read(versions.gnssSats));
        static_cast<void>(data.gnssWorkMode.read(versions.gnssMode));
        const auto status = data.gnssStatus.read();
        // 仅比较 UI 消费的状态，串口计数或内部控制统计更新不需要唤醒。
        versions.gnssState =
            (static_cast<uint32_t>(status.state) << 1U) | (status.enabled ? 1U : 0U);
#endif
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
        const bool ready = data.wifiReady.load(std::memory_order_acquire);
        versions.wifi = ready ? (1U + context_->web.wifiStationCount()) : 0U;
#endif
    }
    if (versions != displayVersions_) {
        displayVersions_ = versions;
        data.uiEvents.notifyWake();
    }
}

void OledModule::publishPairing(uint32_t nowMs) noexcept
{
    auto &data = context_->data;
    OledPairingStatus next{};
    const auto window = context_->bt_link.pairStatus(nowMs);
    next.windowOpen = window.remainingMs != 0U;
    if (next.windowOpen) {
        next.windowDeadlineMs = nowMs + window.remainingMs;
    }
    if (next.windowOpen && window.confirm) {
        spp_server_pair_snapshot_t pair{};
        spp_server_pair_snapshot(&pair);
        if (pair.id != 0U) {
            next.id = pair.id;
            next.requestDeadlineMs = nowMs + pair.remaining_ms;
            next.number = pair.number;
            next.hasNumber = pair.has_number;
            static_assert(sizeof(pair.name) == OledPairingStatus::kNameBytes);
            std::ranges::copy(pair.name, next.name.begin());
            const auto suffix = std::span{pair.bda}.last<OledPairingStatus::kSuffixBytes>();
            std::ranges::copy(suffix, next.suffix.begin());
        }
    }
    if (next != data.oledPairing.read()) {
        data.oledPairing.write(next);
        data.uiEvents.notifyWake();
    }
}

void OledModule::applyAction(OledActionRequest request, uint32_t nowMs) noexcept
{
    switch (request.action) {
    case OledAction::kWifi:
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
        if (context_->radioMode == RadioMode::kBluetooth &&
            power::s_power_requests.claimRestart()) {
            requestRadioMode(RadioMode::kWifiConfig, RadioReason::kUi);
        }
#endif
        break;
    case OledAction::kBluetooth:
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
        if (context_->radioMode == RadioMode::kWifiConfig) {
            if (context_->data.wifiReady.load(std::memory_order_acquire)) {
                static_cast<void>(context_->web.requestOledExit(nowMs));
            } else if (power::s_power_requests.claimRestart()) {
                requestRadioMode(RadioMode::kBluetooth, RadioReason::kUi);
            }
        }
#endif
        break;
    case OledAction::kTogglePair:
        if (context_->radioMode == RadioMode::kBluetooth) {
            const bool open = context_->bt_link.pairStatus(nowMs).remainingMs != 0U;
            const auto cmd =
                makePairingCommand(open ? 0U : core::kPairWindowMs / kPairMsPerSecond, true);
            fw::CommandResult result{};
            context_->bt_link.onCommand(cmd, result);
        }
        break;
    case OledAction::kPairAccept:
    case OledAction::kPairReject:
        if (context_->radioMode == RadioMode::kBluetooth) {
            static_cast<void>(spp_server_pair_decide_for(
                request.pairId, request.action == OledAction::kPairAccept));
        }
        break;
    case OledAction::kShutdownKey:
        static_cast<void>(power::requestShutdown(board::ShutdownReason::kKey));
        break;
    case OledAction::kNone:
    default:
        break;
    }
}

} // namespace app
