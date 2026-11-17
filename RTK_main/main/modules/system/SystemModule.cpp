// 系统模块实现：prepare() 解析 SN、生成设备名并初始化蓝牙，poll() 发布系统/执行器状态。
// prepare() 只在启动期由 BootFlow 调用；poll() 与命令在 kLink 任务；蓝牙/NVS/日志调用可阻塞，非 ISR 安全。
#include "modules/system/SystemModule.hpp"
#include "AppContext.hpp"
#include <cstdio>
#include "fw/Log.hpp"
#include "esp_mac.h"
#include "esp_system.h"
#include "SppServer.h"

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
                        std::span<char, NVS_STORE_SN_MAX + 1U> out) noexcept
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
    if (sn.empty() || sn.size() > NVS_STORE_SN_MAX ||
        sn.find_first_of("\r\n") != std::string_view::npos) {
        return false;
    }
    // SN 最多 16 B，精度转换到 int 无损。
    const int written =
        snprintf(out.data(), out.size(), "E1-Lite-%.*s", static_cast<int>(sn.size()), sn.data());
    // written >= 0 时转换为 size_t 无损，之后按固定输出上限检查。
    if (written < 0 || static_cast<size_t>(written) > kDeviceNameMax) {
        out[0] = '\0';
        return false;
    }
    return true;
}

/**
 * @brief  计算设备名与身份信息并发布到 DataCenter（BT 启动前调用）
 */
void SystemModule::prepare() noexcept
{
    /* 读取持久化 SN 和启动期堆状态。 */
    status_ = ctx().data.system_status.read();
    status_.free_heap = esp_get_free_heap_size();
    status_.min_free_heap = esp_get_minimum_free_heap_size();
    if (status_.sn[0] == '\0') {
        std::array<uint8_t, core::kAddrLen> mac{};
        const esp_err_t error = esp_read_mac(mac.data(), ESP_MAC_BT);
        if (error != ESP_OK || !derive_sn_from_mac(mac, status_.sn)) {
            const int written = snprintf(status_.sn.data(), status_.sn.size(), "%s", "0000");
            if (written < 0) {
                FW_LOGE(fw::TraceCat::kSys, "fallback SN formatting failed");
            }
        }
    }
    /* 名称校验通过后才启动 BT，失败仍发布状态供控制台读取。 */
    if (!build_device_name(status_.sn.data(), status_.device_name)) {
        FW_LOGE(fw::TraceCat::kSys, "invalid SN for BT name");
        ctx().data.system_status.write(status_);
        return;
    }
    FW_LOGI(fw::TraceCat::kSys, "sn=%s bt_name=%s policy=%s", status_.sn.data(),
            status_.device_name.data(), policy_name(policy_from_config()));
    const esp_err_t error = spp_server_init(status_.device_name.data());
    status_.bt_ready = error == ESP_OK;
    if (error != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "bt_spp init failed: %s; console only", esp_err_to_name(error));
    }
    ctx().data.system_status.write(status_);
}
bool SystemModule::poll(uint32_t /*now_ms*/) noexcept
{
    /* 先更新系统状态，再发布执行器统计，保持原主题写入顺序。 */
    status_.link_running = (ctx().link_executor != nullptr) && ctx().link_executor->running() &&
                           ctx().link_executor->moduleOk(id(ModuleKind::kBtLink));
    status_.free_heap = esp_get_free_heap_size();
    status_.min_free_heap = esp_get_minimum_free_heap_size();
    ctx().data.system_status.write(status_);
    if (ctx().link_executor != nullptr) {
        ctx().data.exec_stats.write(ctx().link_executor->stats());
    }
#if RTK_GNSS_ENABLED
    if (ctx().io_executor != nullptr) {
        ctx().data.io_exec_stats.write(ctx().io_executor->stats());
    }
#endif
    return false;
}
} // namespace app
