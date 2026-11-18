// GAP 配对策略绑定与链路窗口命令。
// 启动任务绑定，kLink 写窗口，SPP worker 加锁读窗口和白名单快照。
#include "modules/bt_link/BtPairing.hpp"

#include <algorithm>

#include "AppContext.hpp"
#include "SppServerPairing.h"
#include "rtos/EspStatus.hpp"
#include "rtos/SpinLock.hpp"

namespace app {
namespace {

/**
 * @brief 从产品的同一个窗口和白名单取得 GAP 配对权限。
 * @param raw: GAP 提供的六字节地址。
 * @retval 当前配对策略。
 * @note 仅 SPP worker，短时间加锁，非 ISR。
 */
spp_server_pair_policy_t pairQuery(const uint8_t *raw) noexcept
{
    const auto &context = appContext();
    const auto window = context.bt_link.pairStatus(rtos::nowMs());
    if (window.remainingMs != 0U) {
        return window.confirm ? SPP_SERVER_PAIR_CONFIRM : SPP_SERVER_PAIR_ALLOW;
    }
    core::BtAddr addr{};
    const std::span<const uint8_t, core::kAddrLen> bytes{raw, core::kAddrLen};
    std::ranges::copy(bytes, addr.bytes.begin());
    const auto link = context.data.linkStatus.read();
    const auto whitelist = std::span{link.whitelist}.first(link.whitelistCount);
    return std::ranges::find(whitelist, addr) != whitelist.end() ? SPP_SERVER_PAIR_TRUSTED
                                                                 : SPP_SERVER_PAIR_DENY;
}

} // namespace

void BtLinkModule::installPairingPolicy() noexcept
{
    portMUX_INITIALIZE(&pair_mux_);
    pair_window_.bindLock(rtos::spinLockOps(pair_mux_));
    spp_server_set_pair_query(&pairQuery);
}

void BtLinkModule::openPairing(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const auto now = rtos::nowMs();
    if (cmd.argLen == 0U) {
        link_manager_.pairOpen(now);
        result.detail = core::kPairWindowMs / kPairMsPerSecond;
        return;
    }
    constexpr size_t kModeIndex = sizeof(uint16_t);
    constexpr unsigned kBitsPerByte = 8U;
    if (cmd.argLen != kModeIndex + 1U || cmd.args[kModeIndex] > 1U) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    const uint32_t seconds = cmd.args[0] | (static_cast<uint32_t>(cmd.args[1]) << kBitsPerByte);
    if (seconds < kPairMinSeconds || seconds > kPairMaxSeconds) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    core::openPairWindow(pair_window_, now, seconds * kPairMsPerSecond, cmd.args[kModeIndex] != 0U);
    result.detail = static_cast<int32_t>(seconds); // 最大 600 秒。
}

} // namespace app
