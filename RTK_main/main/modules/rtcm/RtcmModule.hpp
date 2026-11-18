// RTCM 模块（kLink 执行器）：累计分流器交付的 RTCM 帧统计，处理 `rtcm` 命令的查询与复位。
// 由 kLink 执行器任务调用；控制台经 rtos::Executor::call 投递命令，非 ISR 安全。
#pragma once
#include <span>

#include "data/AppData.hpp"
#include "cli/Console.hpp"
namespace app {
struct AppContext;
/** RTCM 模块（kLink 执行器）：消费批次视图并处理 rtcm 命令。 */
class RtcmModule final {
public:
    constexpr RtcmModule() noexcept = default;
    /**
     * @brief 记录所属上下文，不初始化硬件或 RTOS
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅对象构造时调用，不分配、不阻塞；非 ISR。
     */
    constexpr explicit RtcmModule(AppContext &context) noexcept : context_(&context) {}
    /**
     * @brief 绑定模块所属上下文
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅消费者启动前单线程调用；不分配、不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~RtcmModule() = default;
    RtcmModule(const RtcmModule &) = delete;
    RtcmModule &operator=(const RtcmModule &) = delete;
    RtcmModule(RtcmModule &&) = delete;
    RtcmModule &operator=(RtcmModule &&) = delete;
    /**
     * @brief 每轮把 bt_link 累计折算进 RtcmStats 并在变化时发布（ADR-0006 兼容字段）
     * @param nowMs: 当前毫秒
     * @retval 恒 false（无跨轮积压）
     * @note 仅 kLink 任务；非阻塞、非 ISR、不分配堆。
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;
    /**
     * @brief 仅 kLink，非 ISR、非阻塞；校验并处理统计复位，状态写入 result。
     * @param cmd: 待处理指令
     * @param[out] result: 执行结果输出
     * @note 仅所属模块执行器任务调用，非 ISR；涉及存储时可同步等待 NVS。
     */
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /**
     * @brief 仅 kLink 或启动前读取，非 ISR；返回当前转发统计的只读引用。
     * @retval 当前模块累计统计的只读引用
     * @note 仅 kLink 或启动前，非 ISR、非阻塞。
     */
    [[nodiscard]] const RtcmStats &stats() const noexcept;

private:
    /**
     * @brief 取得已绑定的应用上下文
     * @retval 模块所属上下文引用
     * @note 仅模块所属任务调用；须已 bindContext，非 ISR，不阻塞。
     */
    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }
    /**
     * @brief 首个 poll 轮次初始化速率窗口
     * @param nowMs: 当前毫秒
     * @note 仅 kLink 任务；非阻塞、不分配堆。
     */
    void startRateWindow(uint32_t nowMs) noexcept;
    /**
     * @brief 按统计世代把 bt_link 快照折算成 next
     * @param snap: bt_link 侧累计快照
     * @param next: 输出，本轮的累计值
     * @note 仅 kLink 任务；世代一致时直接采用，否则按 reset 基线扣减，避免回滚旧累计。
     */
    void takeSnapshot(const RtcmLinkSnapshot &snap, RtcmStats &next) const noexcept;
    /**
     * @brief 速率窗口到期时刷新 bytesPerSec
     * @param nowMs: 当前毫秒
     * @param next: 输入输出，本轮累计与速率
     * @retval true: 速率已变化（需发布）；false: 未到期或速率未变
     * @note 仅 kLink 任务；elapsed ≥ 1000 保证换算不溢出。
     */
    [[nodiscard]] bool refreshRate(uint32_t nowMs, RtcmStats &next) noexcept;

    AppContext *context_ = nullptr;
    RtcmStats stats_{};
    RtcmLinkSnapshot baseline_{};
    uint32_t reset_epoch_ = 0;
    uint32_t rate_window_ms_ = 0;
    uint32_t rate_window_bytes_ = 0;
    bool rate_started_ = false;
};
/**
 * @brief 打印 RTCM 转发、分流与 UART 管道统计
 * @note 仅 console REPL 任务；使用主题快照，输出可阻塞，非 ISR。
 * @note 打印 [rtcm] 分节及 ADR-0006 兼容的 rtcm/stats 两行；仅 console REPL 任务读取 DataCenter。
 */
void printRtcmStatus() noexcept;
/**
 * @brief 取得RTCM命令表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Cmd> rtcmCmds() noexcept;
/**
 * @brief 取得RTCM子命令动词表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> rtcmVerbs() noexcept;
} // namespace app
