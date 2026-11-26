// 系统模块实现：prepare() 解析 SN 并生成设备名，startBt() 初始化蓝牙，poll() 发布系统与执行器状态。
// prepare()/startBt() 由启动任务调用；poll() 在 kLink 任务；蓝牙/NVS/日志调用可阻塞，非 ISR 安全。
#include "modules/system/SystemModule.hpp"
#include "AppContext.hpp"
#include "board/BoardProfile.hpp"
#include <cstdio>
#include "fw/Log.hpp"
#include "esp_mac.h"
#include "esp_system.h"
#include "SppServer.h"
#if defined(ESP_PLATFORM)
#include "modules/ota/OtaRuntime.hpp"
#endif

namespace app {
namespace {
constexpr std::string_view kHexDigits = "0123456789ABCDEF";
constexpr size_t kMacSnHighByte = 4;
constexpr size_t kMacSnLowByte = 5;
constexpr uint8_t kNibbleMask = 0x0F;
constexpr unsigned kNibbleShift = 4;
constexpr size_t kDeviceNameMax = kDeviceNameBuf - 1U;
} // namespace
bool deriveSnFromMac(std::span<const uint8_t, core::kAddrLen> mac,
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
bool buildDeviceName(std::string_view product, std::string_view sn,
                     std::span<char, kDeviceNameBuf> out) noexcept
{
    if (product.empty() || product.find_first_of("\r\n") != std::string_view::npos || sn.empty() ||
        sn.size() > NVS_STORE_SN_MAX || sn.find_first_of("\r\n") != std::string_view::npos) {
        return false;
    }
    // 总长含连字符 '-'，超限先截断判错，避免 product.size() 转 int 溢出。
    if (product.size() >= kDeviceNameMax || sn.size() > kDeviceNameMax - 1U - product.size()) {
        out[0] = '\0';
        return false;
    }
    // product 与 sn 均小于 kDeviceNameMax（31 B），精度转换到 int 无损。
    const int written =
        snprintf(out.data(), out.size(), "%.*s-%.*s", static_cast<int>(product.size()),
                 product.data(), static_cast<int>(sn.size()), sn.data());
    // written >= 0 时转换为 size_t 无损，之后按固定输出上限检查。
    if (written < 0 || static_cast<size_t>(written) > kDeviceNameMax) {
        out[0] = '\0';
        return false;
    }
    return true;
}

void SystemModule::fillIdentity() noexcept
{
    /* 读取持久化 SN 和启动期堆状态。 */
    status_ = ctx().data.systemStatus.read();
    status_.freeHeap = esp_get_free_heap_size();
    status_.minFreeHeap = esp_get_minimum_free_heap_size();
    if (status_.sn[0] != '\0') {
        return;
    }
    std::array<uint8_t, core::kAddrLen> mac{};
    const esp_err_t error = esp_read_mac(mac.data(), ESP_MAC_BT);
    if (error == ESP_OK && deriveSnFromMac(mac, status_.sn)) {
        return;
    }
    const int written = snprintf(status_.sn.data(), status_.sn.size(), "%s", "0000");
    if (written < 0) {
        FW_LOGE(fw::TraceCat::kSys, "fallback SN formatting failed");
    }
}

void SystemModule::startBt() noexcept
{
    if (ctx().radioMode != RadioMode::kBluetooth || status_.deviceName.front() == '\0') {
        return;
    }
    FW_LOGI(fw::TraceCat::kSys, "sn=%s bt_name=%s policy=%s", status_.sn.data(),
            status_.deviceName.data(), policyName(policyFromConfig()));
    ctx().bt_link.installPairingPolicy();
    const esp_err_t error = spp_server_init(status_.deviceName.data());
    status_.btReady = error == ESP_OK;
    if (error != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "bt_spp init failed: %s; console only", esp_err_to_name(error));
    }
    ctx().data.systemStatus.write(status_);
}

void SystemModule::prepare() noexcept
{
    fillIdentity();
    // 设备名供蓝牙广播与 Wi-Fi 热点 SSID 共用，在 prepare() 生成。
    if (!buildDeviceName(board::productName(ctx().boardProfile), status_.sn.data(),
                         status_.deviceName)) {
        status_.deviceName.front() = '\0';
        status_.btReady = false;
        FW_LOGE(fw::TraceCat::kSys, "invalid product or SN for device name");
    }
    ctx().data.systemStatus.write(status_);
}

bool SystemModule::poll(uint32_t /*nowMs*/) noexcept
{
#if defined(ESP_PLATFORM)
    ota::pollConfirmation();
#endif
    /* 先更新系统状态，再发布执行器统计，保持原主题写入顺序。 */
    status_.linkRunning = (ctx().linkExecutor != nullptr) && ctx().linkExecutor->running() &&
                          ctx().linkExecutor->moduleOk(id(ModuleKind::kBtLink));
    status_.freeHeap = esp_get_free_heap_size();
    status_.minFreeHeap = esp_get_minimum_free_heap_size();
    ctx().data.systemStatus.write(status_);
    if (ctx().linkExecutor != nullptr) {
        ctx().data.execStats.write(ctx().linkExecutor->stats());
    }
#if RTK_GNSS_ENABLED
    if (ctx().ioExecutor != nullptr) {
        ctx().data.ioExecStats.write(ctx().ioExecutor->stats());
    }
#endif
    return false;
}
} // namespace app
