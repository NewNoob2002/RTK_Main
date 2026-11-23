// 调试模块（kLink 执行器）：追踪环、唯一格式化输出任务与 debug 命令组的状态所有者（ADR-0009）。
// init() 只在启动前单线程调用；onCommand 只在 kLink 执行器任务调用；trace_out 任务独立运行。
// 均非 ISR；不分配堆（环与任务栈都是静态存储）。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "sdkconfig.h"

#include "cli/Console.hpp"
#include "fw/Command.hpp"
#include "fw/StoreKey.hpp"
#include "fw/Tlog.hpp"
#include "fw/Trace.hpp"
#include "rtos/BootTask.hpp"

namespace app {
struct AppContext;

/**
 * 调试模块：拥有 TraceRing 与输出任务。
 * 事件由各任务通过 fw::trace() 写入环，本模块的输出任务是唯一的格式化者。
 */
class DebugModule final {
public:
    /** 启动期外供存储视图（trace 环 + 文本日志环 + trace_out 栈）。 */
    struct StorageView {
        std::span<fw::TraceEvent> traceRing{};
        std::span<uint8_t> logRing{};
        std::span<StackType_t> taskStack{};
    };

    /** 环容量：256 条 × 16 字节 = 4 KB 静态存储，够覆盖 20 ms 输出间隔的突发。 */
    static constexpr size_t kRingEvents = 256;
    /** 输出任务轮询周期；有事件时立即输出，不必等满一个周期。 */
    static constexpr uint32_t kPollPeriodMs = 20;
    /** 单行事件文本上限，超长由 formatEvent 截断。 */
    static constexpr size_t kLineBytes = 192;
    /** 输出任务栈；上板 7 类全开、40 KB/s 负载下实测占用约 1.7 KB（HWM 2412/4096），取 3 KB 留约 1.3 KB 余量。 */
    static constexpr size_t kTaskStackBytes = 3072;
    /**
     * 文本日志环容量（字节，Kconfig 默认 4096）：变长记录，容纳 GNSS 句子与十六进制转储。
     * 由启动期 boot_alloc 分配；容量不足时整条丢弃并计数（ADR-0009 §2.11）。
     */
    static constexpr size_t kLogRingBytes = CONFIG_RTK_LOG_RING_BYTES;
    /** 十六进制转储每行字节数（ADR-0009 §2.11：16 字节一行）。 */
    static constexpr size_t kHexBytesPerLine = 16;
    /** 十六进制转储正文缓冲：4 位偏移 + 冒号 + 16×3 + 间隔 + 16 字节 ASCII，取 128 留余量。 */
    static constexpr size_t kHexLineBytes = 128;

    constexpr DebugModule() noexcept = default;
    /**
     * @brief 记录所属上下文，不初始化硬件或 RTOS
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅对象构造时调用，不分配、不阻塞；非 ISR。
     */
    constexpr explicit DebugModule(AppContext &context) noexcept : context_(&context) {}
    /**
     * @brief 绑定模块所属上下文
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅消费者启动前单线程调用；不分配、不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~DebugModule() = default;
    DebugModule(const DebugModule &) = delete;
    DebugModule &operator=(const DebugModule &) = delete;
    DebugModule(DebugModule &&) = delete;
    DebugModule &operator=(DebugModule &&) = delete;

    /**
     * @brief 启动前单线程：绑定启动期外供存储视图；未满足容量要求返回 kInvalidArg。
     * @param storage: 外供存储，生命周期覆盖所有消费者
     * @retval kOk 已绑定满足容量的视图
     * @retval kInvalidState 环已安装或任务已运行
     * @retval kInvalidArg 任一存储为空或不足
     * @note 仅启动前单线程，非 ISR；不分配、不阻塞。
     */
    [[nodiscard]] fw::Status bindStorage(const StorageView &storage) noexcept;

    /**
     * @brief 仅启动绑定回滚、消费者尚未启动时调用；清除全部外供引用，非 ISR。
     * @note 仅启动绑定回滚、消费者尚未启动时调用；不阻塞，非 ISR。
     */
    void unbindStorage() noexcept
    {
        storage_ = {};
        storage_bound_ = false;
    }

    /**
     * @brief  安装 trace 环与 spp 钩子，从 NVS 恢复 trace 掩码、日志级别与报告周期
     * @retval fw::Result::kOk 成功；kInvalidState 存储未绑定
     */
    [[nodiscard]] fw::Result init() noexcept;

    /**
     * @brief  把组件日志从直写通路切到文本环（ADR-0009 §2.12）
     * @retval kOk 已切换（重复调用也返回 kOk）
     * @retval kInvalidState init 未成功
     * @note   启动任务在启动摘要之后、REPL 之前调用：启动期日志同步直写、按序出现在提示符之前，
     *         运行期日志再交给 trace_out 与 trace 事件按时间合并。
     */
    [[nodiscard]] fw::Result attachLog() noexcept;

    /**
     * @brief 创建静态输出任务；重复调用返回 kBusy。仅执行器启动阶段调用。
     * @retval kOk 输出任务已创建
     * @retval kInvalidState 尚未初始化或绑定存储
     * @retval kBusy 输出任务已运行
     * @retval kNoSpace 静态任务创建失败
     * @retval kInvalidArg 任务参数不合法
     * @note 仅执行器启动阶段，非 ISR；栈与控制块均外供，不分配堆。
     */
    [[nodiscard]] fw::Result start() noexcept;

    /**
     * @brief 组合根单线程在 link 启动确认后读取，非 ISR。
     * @retval true 输出任务已创建
     * @retval false 输出任务未创建
     * @note 仅组合根在启动屏障后读取，不阻塞，非 ISR。
     */
    [[nodiscard]] bool taskRunning() const noexcept
    {
        return task_.running();
    }

    /**
     * @brief 仅 kLink：debug report/save 动词。
     * @param cmd: 待处理指令
     * @param[out] result: 执行结果输出
     * @note 仅所属模块执行器任务调用，非 ISR；涉及存储时可同步等待 NVS。
     */
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept;

    /**
     * @brief 本模块登记的持久化键（ADR-0009 §2.4），由 storage 模块在启动时加载。
     * @retval 静态持久化键表，生命周期覆盖固件
     * @note 启动期由 storage 读取；不分配、不阻塞，非 ISR。
     */
    [[nodiscard]] static std::span<const fw::StoreKey> keys() noexcept;

    /**
     * @brief 从小端字节应用追踪类别掩码
     * @param obj: 未使用；通过全局状态或单例应用
     * @param in: 4 字节小端编码
     * @retval kOk 已应用
     * @retval kInvalidArg 长度不符
     * @note 仅启动期存储加载路径，非 ISR，不阻塞。
     */
    static fw::Result loadMask(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把追踪类别掩码编码为小端字节
     * @param obj: 未使用；读取全局状态或单例
     * @param[out] out: 至少 4 字节的输出缓冲
     * @param[out] len: 成功置 4，失败不变
     * @retval kOk 已写入并设置 len
     * @retval kNoSpace 输出容量不足，len 不变
     * @note 仅 kLink 存储路径，非 ISR，不阻塞。
     */
    static fw::Result saveMask(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从小端字节应用周期报告秒数
     * @param obj: 未使用；通过全局状态或单例应用
     * @param in: 2 字节小端编码
     * @retval kOk 已应用
     * @retval kInvalidArg 长度不符或秒数超过 3600
     * @note 仅启动期存储加载路径，非 ISR，不阻塞。
     */
    static fw::Result loadReport(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把周期报告秒数编码为小端字节
     * @param obj: 未使用；读取全局状态或单例
     * @param[out] out: 至少 2 字节的输出缓冲
     * @param[out] len: 成功置 2，失败不变
     * @retval kOk 已写入并设置 len
     * @retval kNoSpace 输出容量不足，len 不变
     * @note 仅 kLink 存储路径，非 ISR，不阻塞。
     */
    static fw::Result saveReport(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief 从小端字节应用十六进制输出掩码
     * @param obj: 未使用；通过全局状态或单例应用
     * @param in: 4 字节小端编码
     * @retval kOk 已应用
     * @retval kInvalidArg 长度不符
     * @note 仅启动期存储加载路径，非 ISR，不阻塞。
     */
    static fw::Result loadHex(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief 把十六进制输出掩码编码为小端字节
     * @param obj: 未使用；读取全局状态或单例
     * @param[out] out: 至少 4 字节的输出缓冲
     * @param[out] len: 成功置 4，失败不变
     * @retval kOk 已写入并设置 len
     * @retval kNoSpace 输出容量不足，len 不变
     * @note 仅 kLink 存储路径，非 ISR，不阻塞。
     */
    static fw::Result saveHex(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;
    /**
     * @brief  NVS dbg/loglv → 各类别日志级别（打包表，每类别 2 bit）
     * @param  obj: 未用（单例）
     * @param  in: 4 字节小端 u32
     * @retval kOk 已应用；kInvalidArg 长度不对
     */
    static fw::Result loadLoglv(void *obj, std::span<const uint8_t> in) noexcept;
    /**
     * @brief  各类别日志级别 → NVS dbg/loglv
     * @param  obj: 未用（单例）
     * @param[out] out: 输出缓冲（≥ 4 字节）
     * @param[out] len: 写入字节数
     * @retval kOk 成功；kNoSpace 缓冲不足
     */
    static fw::Result saveLoglv(const void *obj, std::span<uint8_t> out, size_t &len) noexcept;

    /**
     * @brief 报告周期（秒，0 = 关闭）。跨任务读，非 ISR。
     * @retval 报告周期（秒），0 表示关闭
     * @note console 任务诊断读取；环查询内部加锁，非 ISR。
     */
    [[nodiscard]] uint16_t reportPeriodS() const noexcept
    {
        // 32 位对齐的单字读取在 ESP32 上是原子的；仅用于人为读取的控制台输出。
        return report_s_;
    }

    /**
     * @brief 使用早期读取并校验过的报告周期。
     * @param seconds: 周期秒数，0 表示关闭。
     * @note 仅启动任务在报告任务启动前调用，非 ISR。
     */
    void applyBootReport(uint16_t seconds) noexcept
    {
        report_s_ = seconds;
        report_started_ = false;
    }

    /**
     * @brief 环统计快照（console 任务读取，内部已加锁，非 ISR）。
     * @retval trace 环累计统计快照
     * @note console 任务诊断读取；环查询内部加锁，非 ISR。
     */
    [[nodiscard]] fw::TraceRingStats ringStats() const noexcept
    {
        return ring_.stats();
    }

    /**
     * @brief 环中待输出事件数（console 任务读取，内部已加锁，非 ISR）。
     * @retval trace 环待输出事件数
     * @note console 任务诊断读取；环查询内部加锁，非 ISR。
     */
    [[nodiscard]] size_t ringDepth() const noexcept
    {
        return ring_.depth();
    }

    /**
     * @brief 文本日志环统计快照（console 任务读取，内部已加锁，非 ISR）。
     * @retval 文本日志环累计统计快照
     * @note console 任务诊断读取；环查询内部加锁，非 ISR。
     */
    [[nodiscard]] fw::LogRingStats logRingStats() const noexcept
    {
        return log_ring_.stats();
    }

    /**
     * @brief 文本日志环已用字节数（console 任务读取，内部已加锁，非 ISR）。
     * @retval 文本日志环已用字节数
     * @note console 任务诊断读取；环查询内部加锁，非 ISR。
     */
    [[nodiscard]] size_t logRingUsed() const noexcept
    {
        return log_ring_.used();
    }

    /** @brief 检查输出队列和消费者是否已交付日志。 @retval true 日志均已交给输出端。 @note 任意任务，非 ISR。 */
    [[nodiscard]] bool logOutputIdle() const noexcept
    {
        // pop 和 used 取同一把环锁，读到空环后再确认消费者序号。
        return log_ring_.used() == 0U && (logOutputSeq_.load(std::memory_order_acquire) & 1U) == 0U;
    }

private:
    friend struct ModuleTestAccess;
    /**
     * @brief 等待启动屏障后进入唯一输出循环
     * @param arg: DebugModule 实例，空指针时直接结束任务
     * @note 仅 trace_out 任务，可阻塞；退出路径删除当前任务，非 ISR。
     */
    static void taskEntry(void *arg) noexcept;
    /**
     * @brief 输出任务主循环：取事件、打印丢弃计数、按周期报告。
     * @note 仅 trace_out 任务，永久循环并按负载让出 tick，非 ISR。
     */
    void run() noexcept;
    /**
     * @brief 按时间顺序合并输出 trace 与文本日志
     * @param line: 行格式化缓冲
     * @param cap: 行缓冲容量（字节）
     * @retval true 本轮输出过事件或丢弃提示
     * @retval false 本轮无输出
     * @note 仅 trace_out；输出可等待控制台锁，非 ISR。
     */
    bool pumpEvents(char *line, size_t cap) noexcept;
    /**
     * @brief 打印两个环的丢弃计数（有增量时），返回是否有输出。
     * @param[out] out: 输出缓冲或结果
     * @param cap: 输出缓冲容量（字节）
     * @retval true 本轮输出过丢弃计数
     * @retval false 两个环均无新增丢弃
     * @note 仅 trace_out 任务，可等待控制台锁，非 ISR。
     */
    bool pumpDrops(char *out, size_t cap) noexcept;
    /**
     * @brief  输出一条文本日志记录：`[ts] CAT 句子`（非 I 级带 `E:`/`W:`/`D:` 前缀）
     * @param  hdr: 记录头
     * @param  payload: 文本（不含换行）
     * @param[out] out: 行缓冲（至少 fw::kLogLineMax 字节）
     * @param  cap: out 的容量
     */
    void printLogText(const fw::LogHeader &hdr, std::span<const uint8_t> payload, char *out,
                      size_t cap) noexcept;
    /**
     * @brief 以十六进制转储打印一段载荷（每行 16 字节 + ASCII 列）
     * @param hdr: 事件头（只用 total 判断是否截断）
     * @param payload: 已截断的载荷
     * @param[out] out: 尾部提示文本缓冲
     * @param cap: out 容量
     * @note 仅 console REPL 任务；逐行写控制台，不分配堆。
     */
    void printLogHex(const fw::LogHeader &hdr, std::span<const uint8_t> payload, char *out,
                     size_t cap) noexcept;
    /**
     * @brief 到点输出一行周期报告。
     * @param nowMs: 当前单调时间（毫秒）
     * @note 仅 trace_out 任务，可等待控制台锁，非 ISR。
     */
    void pumpReport(uint32_t nowMs) noexcept;
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
    StorageView storage_{};
    bool storage_bound_ = false;
    fw::TraceRing ring_{};
    portMUX_TYPE ring_mux_{};
    fw::LogRing log_ring_{};
    std::atomic<uint32_t> logOutputSeq_{0U}; // trace_out 单写，奇数表示尚未交给输出端。
    rtos::BootTask task_{};
    bool installed_ = false;
    bool log_attached_ = false;
    uint16_t report_s_ = 0;
    uint32_t last_report_ms_ = 0;
    bool report_started_ = false;
    // 周期报告的增量基线（只在输出任务里读写）。
    uint32_t last_rtcm_bytes_ = 0;
    uint32_t last_rtcm_frames_ = 0;
    uint32_t last_rtcm_drop_ = 0;
    uint32_t last_prod_frames_ = 0;
};

/**
 * @brief 取得调试命令表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Cmd> debugCmds() noexcept;

/**
 * @brief 取得调试子命令动词表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> debugVerbs() noexcept;

} // namespace app
