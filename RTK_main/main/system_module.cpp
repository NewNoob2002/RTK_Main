// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "system_module.hpp"
#include <cstdio>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "rtk_bt_spp.h"

namespace rtk::app {
constinit SystemModule s_system_module{};
namespace {
constexpr std::string_view kHexDigits = "0123456789ABCDEF";
constexpr size_t kMacSnHighByte = 4;
constexpr size_t kMacSnLowByte = 5;
constexpr uint8_t kNibbleMask = 0x0F;
constexpr unsigned kNibbleShift = 4;
constexpr size_t kDeviceNameMax = kDeviceNameBuf - 1U;
} // namespace
bool derive_sn_from_mac(std::span<const uint8_t, core::kAddrLen> mac,
                        std::span<char, RTK_STORE_SN_MAX + 1U> out) noexcept
{
    const std::array nibbles{mac[kMacSnHighByte] >> kNibbleShift, mac[kMacSnHighByte] & kNibbleMask,
                             mac[kMacSnLowByte] >> kNibbleShift, mac[kMacSnLowByte] & kNibbleMask};
    for (size_t i = 0; i < nibbles.size(); ++i) {
        // 掩码/右移后的半字节位于 0..15，转换为 size_t 无损。
        out[i] = kHexDigits[static_cast<size_t>(nibbles[i])];
    }
    out[nibbles.size()] = '\0';
    return true;
}
bool build_device_name(std::string_view sn, std::span<char, kDeviceNameBuf> out) noexcept
{
    if (sn.empty() || sn.size() > RTK_STORE_SN_MAX || sn.find_first_of("\r\n") != std::string_view::npos) {
        return false;
    }
    // SN 最多 16 B，精度转换到 int 无损。
    const int written = snprintf(out.data(), out.size(), "E1-Lite-%.*s", static_cast<int>(sn.size()), sn.data());
    // written >= 0 时转换为 size_t 无损，之后按固定输出上限检查。
    if (written < 0 || static_cast<size_t>(written) > kDeviceNameMax) {
        out[0] = '\0';
        return false;
    }
    return true;
}
void system_prepare(os::Executor &executor) noexcept
{
    auto &module = s_system_module;
    module.executor = &executor;
    module.status = data::kSystemStatus.read();
    module.status.free_heap = esp_get_free_heap_size();
    module.status.min_free_heap = esp_get_minimum_free_heap_size();
    if (module.status.sn[0] == '\0') {
        std::array<uint8_t, core::kAddrLen> mac{};
        const esp_err_t error = esp_read_mac(mac.data(), ESP_MAC_BT);
        if (error != ESP_OK || !derive_sn_from_mac(mac, module.status.sn)) {
            const int written = snprintf(module.status.sn.data(), module.status.sn.size(), "%s", "0000");
            if (written < 0) {
                ESP_LOGE("system", "fallback SN formatting failed");
            }
        }
    }
    if (!build_device_name(module.status.sn.data(), module.status.device_name)) {
        ESP_LOGE("system", "invalid SN for BT name");
        data::kSystemStatus.write(module.status);
        return;
    }
    ESP_LOGI("system", "sn=%s bt_name=%s policy=%s", module.status.sn.data(), module.status.device_name.data(),
             policy_name(policy_from_config()));
    const esp_err_t error = rtk_bt_spp_init(module.status.device_name.data());
    module.status.bt_ready = error == ESP_OK;
    if (error != ESP_OK) {
        ESP_LOGE("system", "bt_spp init failed: %s; console only", esp_err_to_name(error));
    }
    data::kSystemStatus.write(module.status);
}
bool system_poll(void *self, uint32_t /*now_ms*/) noexcept
{
    // ModuleOps 固定绑定 SystemModule，只在 kLink 访问。
    auto &module = *static_cast<SystemModule *>(self);
    module.status.link_running = module.executor->running() && module.executor->module_ok(id(ModuleKind::kBtLink));
    module.status.free_heap = esp_get_free_heap_size();
    module.status.min_free_heap = esp_get_minimum_free_heap_size();
    data::kSystemStatus.write(module.status);
    data::kExecStats.write(module.executor->stats());
    return false;
}
os::Executor &link_executor() noexcept
{
    return *s_system_module.executor;
}
} // namespace rtk::app
