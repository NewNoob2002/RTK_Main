// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include "app_data.hpp"
namespace rtk::app {
struct SystemModule {
    os::Executor *executor = nullptr;
    SystemStatus status{};
};
extern SystemModule s_system_module;
// 只在启动前单线程调用：解析 SN、生成设备名、初始化 BT，失败仍保留状态供控制台读取。非 ISR。
void system_prepare(os::Executor &executor) noexcept;
// ModuleOps 仅 kLink 调用；非 ISR。
[[nodiscard]] bool system_poll(void *self, uint32_t now_ms) noexcept;
inline constexpr ::app::ModuleOps kSystemOps{nullptr, nullptr, system_poll, nullptr};
// 纯函数，线程/ISR 安全。
[[nodiscard]] bool derive_sn_from_mac(std::span<const uint8_t, core::kAddrLen> mac,
                                      std::span<char, RTK_STORE_SN_MAX + 1U> out) noexcept;
[[nodiscard]] bool build_device_name(std::string_view sn, std::span<char, kDeviceNameBuf> out) noexcept;
// 控制台唯一调用者使用，非 ISR；执行器生命周期为整个固件运行期。
[[nodiscard]] os::Executor &link_executor() noexcept;
} // namespace rtk::app
