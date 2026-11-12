// 系统模块实现：prepare() 解析 SN、生成设备名并初始化蓝牙，poll() 发布系统/执行器状态（启动前或 kLink 任务）。
#include "modules/system/system_module.hpp"
#include "app_context.hpp"
#include <cstdio>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "rtk_bt_spp.h"

namespace app {
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
void SystemModule::prepare() noexcept
{
    /* 读取持久化 SN 和启动期堆状态。 */
    status_ = context_.data.system_status.read();
    status_.free_heap = esp_get_free_heap_size();
    status_.min_free_heap = esp_get_minimum_free_heap_size();
    if (status_.sn[0] == '\0') {
        std::array<uint8_t, core::kAddrLen> mac{};
        const esp_err_t error = esp_read_mac(mac.data(), ESP_MAC_BT);
        if (error != ESP_OK || !derive_sn_from_mac(mac, status_.sn)) {
            const int written = snprintf(status_.sn.data(), status_.sn.size(), "%s", "0000");
            if (written < 0) {
                ESP_LOGE("system", "fallback SN formatting failed");
            }
        }
    }
    /* 名称校验通过后才启动 BT，失败仍发布状态供控制台读取。 */
    if (!build_device_name(status_.sn.data(), status_.device_name)) {
        ESP_LOGE("system", "invalid SN for BT name");
        context_.data.system_status.write(status_);
        return;
    }
    ESP_LOGI("system", "sn=%s bt_name=%s policy=%s", status_.sn.data(), status_.device_name.data(),
             policy_name(policy_from_config()));
    const esp_err_t error = rtk_bt_spp_init(status_.device_name.data());
    status_.bt_ready = error == ESP_OK;
    if (error != ESP_OK) {
        ESP_LOGE("system", "bt_spp init failed: %s; console only", esp_err_to_name(error));
    }
    context_.data.system_status.write(status_);
}
bool SystemModule::poll(uint32_t /*now_ms*/) noexcept
{
    /* 先更新系统状态，再发布执行器统计，保持原主题写入顺序。 */
    status_.link_running =
        context_.link_executor.running() && context_.link_executor.module_ok(id(ModuleKind::kBtLink));
    status_.free_heap = esp_get_free_heap_size();
    status_.min_free_heap = esp_get_minimum_free_heap_size();
    context_.data.system_status.write(status_);
    context_.data.exec_stats.write(context_.link_executor.stats());
    return false;
}
} // namespace app
