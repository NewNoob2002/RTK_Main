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
    /**
     * @brief 记录所属上下文，不初始化硬件或 RTOS
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅对象构造时调用，不分配、不阻塞；非 ISR。
     */
    constexpr explicit SystemModule(AppContext &context) noexcept : context_(&context) {}
    /**
     * @brief 绑定模块所属上下文
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅消费者启动前单线程调用；不分配、不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~SystemModule() = default;
    SystemModule(const SystemModule &) = delete;
    SystemModule &operator=(const SystemModule &) = delete;
    SystemModule(SystemModule &&) = delete;
    SystemModule &operator=(SystemModule &&) = delete;
    /**
     * @brief  计算设备名与身份信息并发布到 DataCenter（BT 启动前调用）
     * @note 仅启动期单线程调用；顺序为先身份后蓝牙，二者都不在 ISR。
     */
    void prepare() noexcept;
    /**
     * @brief 用已准备的设备名启动 SPP 服务，结果写回 systemStatus
     * @note 仅启动期在 prepare 之后调用；可阻塞，非 ISR。
     */
    void startBt() noexcept;
    /**
     * @brief 发布系统和执行器状态，待确认镜像达到延时后确认
     * @note 仅 kLink，非 ISR；一次性确认可能访问 Flash。
     * @param nowMs: 当前单调时间（毫秒）
     * @retval false 总是，无需立即再轮询
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;

private:
    /**
     * @brief 读取并回填身份信息（SN 缺失时用蓝牙 MAC 派生，失败退化为 "0000"）
     * @note 仅启动期单线程调用；esp_read_mac 非 ISR，不阻塞控制台。
     */
    void fillIdentity() noexcept;

    /**
     * @brief 取得已绑定的应用上下文
     * @retval 模块所属上下文引用
     * @note 仅模块所属任务调用；须已 bindContext，非 ISR，不阻塞。
     */
    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    AppContext *context_ = nullptr;
    SystemStatus status_{};
};
/**
 * @brief 从蓝牙 MAC 尾两字节派生四位十六进制 SN
 * @param mac: 六字节蓝牙地址
 * @param[out] out: SN 输出，至少可容纳四字符与终止符
 * @retval true 总是，已写入四位 SN 与终止符
 * @note 纯计算，线程/ISR 安全，不阻塞。
 */
[[nodiscard]] bool deriveSnFromMac(std::span<const uint8_t, core::kAddrLen> mac,
                                   std::span<char, NVS_STORE_SN_MAX + 1U> out) noexcept;
/**
 * @brief 按产品名与合法 SN 生成蓝牙设备名（`<product>-<SN>`）
 * @param product: 产品短名视图（如 `"P2"`、`"E1"`、`"RTK"`）
 * @param sn: 序列号视图
 * @param[out] out: 设备名输出；输入校验失败不变，超长或格式化失败首字节清零
 * @retval true 名称已生成
 * @retval false 产品名或 SN 为空、含换行、SN 超长、总长超限或格式化失败
 * @note 纯计算，线程/ISR 安全，不阻塞。
 */
[[nodiscard]] bool buildDeviceName(std::string_view product, std::string_view sn,
                                   std::span<char, kDeviceNameBuf> out) noexcept;
/**
 * @brief 取得系统命令表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Cmd> systemCmds() noexcept;
/**
 * @brief 取得状态子命令动词表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> statusVerbs() noexcept;
/**
 * @brief 取得日志子命令动词表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> logVerbs() noexcept;
/**
 * @brief 取得系统诊断子命令动词表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> sysVerbs() noexcept;
/**
 * @brief 取得工厂命令表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 * @note 隐藏的 factory 命令表与动词表（ADR-0009 §2.10）；help 列表不显示。
 */
[[nodiscard]] std::span<const cli::Cmd> factoryCmds() noexcept;
/**
 * @brief 取得工厂子命令动词表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> factoryVerbs() noexcept;
/**
 * @brief 查询工厂命令是否已解锁（仅 console REPL 任务）
 * @retval true 已解锁
 * @retval false 已锁定
 * @note 仅 console REPL 任务调用，不阻塞，非 ISR。
 */
[[nodiscard]] bool factoryUnlocked() noexcept;
} // namespace app
