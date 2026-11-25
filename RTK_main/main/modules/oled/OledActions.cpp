// OLED 通信动作与蓝牙只读快照的 kLink 桥接。OLED 刷新任务通过 UiEventBus 提交动作。
// 仅 kLink 调用，非 ISR。配对快照持短临界区，无堆分配。
#include "modules/oled/OledModule.hpp"

#include <algorithm>

#include "AppContext.hpp"
#include "SppServerPairing.h"
#include "app/PowerRequests.hpp"
#include "modules/bt_link/BtPairing.hpp"

namespace app {

bool OledModule::poll(uint32_t nowMs) noexcept
{
    if (!present_ || context_->bootMode != board::BootMode::kNormal) {
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
    case OledAction::kNone:
    default:
        break;
    }
}

} // namespace app
