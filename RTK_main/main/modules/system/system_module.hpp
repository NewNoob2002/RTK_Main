// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include <span>

#include "data/app_data.hpp"
#include "rtk/console/console.hpp"
namespace rtk::app {
struct AppContext;
class SystemModule final {
public:
    constexpr explicit SystemModule(AppContext &context) noexcept : context_(context) {}
    ~SystemModule() = default;
    SystemModule(const SystemModule &) = delete;
    SystemModule &operator=(const SystemModule &) = delete;
    SystemModule(SystemModule &&) = delete;
    SystemModule &operator=(SystemModule &&) = delete;
    /** 启动前单线程，非 ISR；解析 SN、生成设备名并同步初始化 BT，失败仍发布状态。 */
    void prepare() noexcept;
    /** 仅 kLink，非 ISR、非阻塞；发布系统和执行器状态，返回 false。 */
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;

private:
    AppContext &context_;
    SystemStatus status_{};
};
// 纯函数，线程/ISR 安全。
[[nodiscard]] bool derive_sn_from_mac(std::span<const uint8_t, core::kAddrLen> mac,
                                      std::span<char, RTK_STORE_SN_MAX + 1U> out) noexcept;
[[nodiscard]] bool build_device_name(std::string_view sn, std::span<char, kDeviceNameBuf> out) noexcept;
[[nodiscard]] std::span<const rtk::console::Cmd> system_cmds() noexcept;
} // namespace rtk::app
