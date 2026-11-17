// 系统模块（kLink 执行器）：SN/设备名准备、系统与执行器状态发布，以及 `status` 命令用的纯函数辅助。
// prepare 只在启动期调用；poll 与命令在 kLink 任务；蓝牙/NVS/日志调用可阻塞，非 ISR 安全。
#pragma once
#include <span>

#include "data/AppData.hpp"
#include "cli/Console.hpp"
namespace app {
struct AppContext;
/** 系统模块（kLink 执行器）：周期发布状态快照并处理 system 命令。 */
class SystemModule final {
public:
    constexpr SystemModule() noexcept = default;
    constexpr explicit SystemModule(AppContext &context) noexcept : context_(&context) {}
    void bind_context(AppContext &context) noexcept
    {
        context_ = &context;
    }
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
    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    AppContext *context_ = nullptr;
    SystemStatus status_{};
};
// 纯函数，线程/ISR 安全。
[[nodiscard]] bool derive_sn_from_mac(std::span<const uint8_t, core::kAddrLen> mac,
                                      std::span<char, NVS_STORE_SN_MAX + 1U> out) noexcept;
/** 按 SN 生成设备名（无 SN 时用回退名）；写入 out 并返回是否成功。 */
[[nodiscard]] bool build_device_name(std::string_view sn,
                                     std::span<char, kDeviceNameBuf> out) noexcept;
/** 系统模块的控制台命令表（status/log/sys）；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> system_cmds() noexcept;
/** status/log/sys 子命令动词表，供 cli::add_verbs 登记。 */
[[nodiscard]] std::span<const cli::Verb> status_verbs() noexcept;
[[nodiscard]] std::span<const cli::Verb> log_verbs() noexcept;
[[nodiscard]] std::span<const cli::Verb> sys_verbs() noexcept;
/** 隐藏的 factory 命令表与动词表（ADR-0009 §2.10）；help 列表不显示。 */
[[nodiscard]] std::span<const cli::Cmd> factory_cmds() noexcept;
[[nodiscard]] std::span<const cli::Verb> factory_verbs() noexcept;
} // namespace app
