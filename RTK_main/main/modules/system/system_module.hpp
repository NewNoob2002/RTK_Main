// 系统模块（kSystem 执行器）：SN/设备名准备、系统与执行器状态发布，以及 `status` 命令用的纯函数辅助。
#pragma once
#include <span>

#include "data/app_data.hpp"
#include "rtk/console/console.hpp"
namespace rtk::app {
struct AppContext;
/** 系统模块（kLink 执行器）：周期发布状态快照并处理 system 命令。 */
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
/** 按 SN 生成设备名（无 SN 时用回退名）；写入 out 并返回是否成功。 */
[[nodiscard]] bool build_device_name(std::string_view sn, std::span<char, kDeviceNameBuf> out) noexcept;
/** 系统模块的控制台命令表；静态存储，进程期有效。 */
[[nodiscard]] std::span<const rtk::console::Cmd> system_cmds() noexcept;
} // namespace rtk::app
