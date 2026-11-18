// 启动流程（ADR-0009）：13 步有序步骤表、降级/回滚/停机处理、横幅与单行摘要、步骤记录查询。
// 线程：仅 app_main 的启动任务调用（boot()、各步骤函数）；非 ISR，可阻塞，启动期一次性完成。
// 启动完成后 s_boot_records 只读，供 `sys boot` 与 RTK_BOOT_VERBOSE 查询。
#include "app/App.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "driver/gpio.h"
#include "esp_bt_device.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "fw/Log.hpp"
#include "esp_system.h"
#include "esp_timer.h"

#include "app/BootBuffers.hpp"
#include "app/BootReport.hpp"
#include "app/BootReasons.hpp"
#include "core/Types.hpp"
#include "support/ConsoleSupport.hpp"
#include "support/ResetReason.hpp"

namespace {

#if defined(CONFIG_RTK_BOOT_VERBOSE) && CONFIG_RTK_BOOT_VERBOSE
constexpr bool kBootVerbose = true;
#else
constexpr bool kBootVerbose = false;
#endif
#ifdef CONFIG_RTK_BOOT_FAULT_STEP
constexpr const char *kFaultStep = CONFIG_RTK_BOOT_FAULT_STEP;
#else
constexpr const char *kFaultStep = "";
#endif

/** 每步采样堆水位用的能力掩码：只看内部 8 位可分配堆，与 DRAM 口径一致。 */
constexpr uint32_t kHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
/** 微秒到毫秒的换算基数（esp_timer 分辨率为微秒）。 */
constexpr int64_t kUsPerMs = 1000;
/** 每 KB 的十位小数个数，用于打印 x.y KB。 */
constexpr uint32_t kTenthsPerKb = 10U;
constexpr uint32_t kBytesPerKb = 1024U;

/**
 * @brief 采样启动步骤使用的单调时钟
 * @retval 当前单调时间（毫秒）
 * @note 仅启动任务，非 ISR，不阻塞。
 */
uint32_t nowMsHook() noexcept
{
    // 启动至今的毫秒数远小于 uint32 上限（约 49 天），截断安全。
    return static_cast<uint32_t>(esp_timer_get_time() / kUsPerMs);
}

/**
 * @brief 采样内部 8-bit 堆容量
 * @retval 空闲、历史最小空闲与最大可分配块（字节）
 * @note 仅启动任务，非 ISR。
 */
app::BootHeap heapHook() noexcept
{
    app::BootHeap heap{};
    heap.freeBytes = heap_caps_get_free_size(kHeapCaps);
    heap.minBytes = heap_caps_get_minimum_free_size(kHeapCaps);
    heap.largestBytes = heap_caps_get_largest_free_block(kHeapCaps);
    return heap;
}

/**
 * @brief  拉高电源保持脚，松开按键后仍保持供电
 * @retval true 成功；false GPIO 配置失败（已记 E 级日志）
 */
bool enablePowerHold() noexcept
{
    // 保留基线 GPIO26 高有效的电源保持逻辑；本任务不执行硬件动作。
    const esp_err_t levelErr = gpio_set_level(GPIO_NUM_26, 1U);
    if (levelErr != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power hold level failed: %s", esp_err_to_name(levelErr));
        return false;
    }
    gpio_config_t powerHold{};
    powerHold.pin_bit_mask = 1ULL << GPIO_NUM_26;
    powerHold.mode = GPIO_MODE_OUTPUT;
    powerHold.pull_up_en = GPIO_PULLUP_DISABLE;
    powerHold.pull_down_en = GPIO_PULLDOWN_DISABLE;
    powerHold.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t powerErr = gpio_config(&powerHold);
    if (powerErr != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power hold init failed: %s", esp_err_to_name(powerErr));
        return false;
    }
    return true;
}

// NOLINTBEGIN(misc-const-correctness): step* 的 BootCtx & 形参由 BootStep::fn 签名固定
/**
 * @brief  启动步骤 1：安装日志直写通道（此后启动日志带时间戳与类别），再做电源保持
 * @param  ctx: 启动上下文（未用）
 * @retval kOk 成功；kFail 电源保持失败
 */
app::BootResult stepPower(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    (void)app::installDirectLog(); // 只会因重复安装失败，此时已有通道可用
    return enablePowerHold() ? app::BootResult::kOk : app::BootResult::kFail;
}

/**
 * @brief  启动步骤 2：横幅与复位原因；PANIC/看门狗/欠压用 W 级
 * @param  ctx: 启动上下文（未用）
 * @retval kOk 总是
 */
app::BootResult stepBanner(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    const esp_reset_reason_t reason = esp_reset_reason();
    const char *name = app::resetReasonName(reason);
    // 版本串短于 int 上限，精度转换安全。
    FW_LOGI(fw::TraceCat::kSys, "E1-Lite RTK_main %.*s", static_cast<int>(app::kAppVersion.size()),
            app::kAppVersion.data());
    if ((reason == ESP_RST_PANIC) || (reason == ESP_RST_INT_WDT) || (reason == ESP_RST_TASK_WDT) ||
        (reason == ESP_RST_BROWNOUT)) {
        FW_LOGW(fw::TraceCat::kSys, "reset reason %s", name);
    } else {
        FW_LOGI(fw::TraceCat::kSys, "reset reason %s", name);
    }
    return app::BootResult::kOk;
}

/**
 * @brief  启动步骤 3：绑定数据主题共享锁与执行器指针（启动顺序：contexts 先于 console）
 * @param  ctx: 启动上下文（未用）
 * @retval kOk 总是
 */
app::BootResult stepContexts(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    app::AppContext &self = app::appContext();
    self.bindContexts();
    app::bindExecutors();
#if RTK_GNSS_ENABLED
    self.rtcmPipe.initLock();
#endif
    self.data.init();
    return app::BootResult::kOk;
}

/**
 * @brief  启动步骤 4：只建立控制台输出通道并登记命令，不放行 REPL
 * @param  ctx: 启动上下文（未用）
 * @retval kOk 成功；kFail 控制台初始化失败
 * @note   REPL 到 boot() 末尾才 run：此前提示符/IDF 提示语若先出现，会与后续各步骤日志交错。
 *         UART 驱动重装必须在业务任务启动前完成，所以 init 留在这里而不是挪到末尾。
 */
app::BootResult stepConsole(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    return app::initConsole() ? app::BootResult::kOk : app::BootResult::kFail;
}

/**
 * @brief  启动步骤 5：按固定 owner 顺序一次性分配数据通路与栈缓冲
 * @param  ctx: 启动上下文（读写 user/reason）
 * @retval kOk 成功；kFail 分配失败（原因已写 ctx.reason，未封存可回滚）
 */
app::BootResult stepAlloc(app::BootCtx &ctx) noexcept
{
    auto &bufs =
        *static_cast<app::BootBuffers *>(ctx.user); // user 只指向启动任务的栈上 BootBuffers。
    if (app::allocateBootBuffers(bufs)) {
        return app::BootResult::kOk;
    }
    // 分配器 abort 时已记下 boot_alloc:<owner>，步骤记录引用同一原因供日志与 sys boot 显示。
    ctx.reason = app::appContext().bootArena.reason();
    return app::BootResult::kFail;
}

/**
 * @brief  启动步骤 6：把缓冲交给各 C 驱动；失败时按 ADR-0011 §7 语义回滚已绑定部分
 * @param  ctx: 启动上下文（读写 user/reason）
 * @retval kOk 成功；kFail 绑定失败
 */
app::BootResult stepBind(app::BootCtx &ctx) noexcept
{
    auto &bufs = *static_cast<app::BootBuffers *>(ctx.user);
    const char *error = app::bindBootBuffers(bufs);
    if (error == nullptr) {
        return app::BootResult::kOk;
    }
    app::rollbackBootBindings(bufs, error);
    ctx.reason = error;
    return app::BootResult::kFail;
}

/**
 * @brief  启动步骤 7：封存分配器；失败时回滚绑定（SEALED 后无法释放块）
 * @param  ctx: 启动上下文（读 user，写 reason）
 * @retval kOk 成功；kFail 封存失败
 */
app::BootResult stepSeal(app::BootCtx &ctx) noexcept
{
    auto &bufs = *static_cast<app::BootBuffers *>(ctx.user);
    if (fw::isOk(app::appContext().bootArena.seal())) {
        return app::BootResult::kOk;
    }
    app::rollbackBootBindings(bufs, app::boot_reason::kAllocSeal);
    ctx.reason = app::boot_reason::kAllocSeal;
    return app::BootResult::kFail;
}

/**
 * @brief  启动步骤 8：安装 trace 全局 sink（编译关闭时记 kSkipped，不从表里删除）
 * @param  ctx: 启动上下文（未用）
 * @retval kOk 成功；kSkipped 未编译 trace；kFail sink 安装失败
 */
app::BootResult stepDebug(app::BootCtx &ctx) noexcept
{
    (void)ctx;
#if FW_TRACE_ENABLED
    // 所有绑定成功、封存后才安装全局 sink，回滚时不会留下悬空的日志引用。
    if (fw::isOk(app::appContext().debug.init())) {
        return app::BootResult::kOk;
    }
    ctx.reason = app::boot_reason::kDebugInit;
    return app::BootResult::kFail;
#else
    return app::BootResult::kSkipped;
#endif
}

/**
 * @brief  启动步骤 9：加载 NVS/SN；必须先于 BT 初始化，执行器再次 init 时为幂等
 * @param  ctx: 启动上下文（写 reason）
 * @retval kOk 成功；kFail 存储初始化失败
 */
app::BootResult stepStorage(app::BootCtx &ctx) noexcept
{
    if (fw::isOk(app::appContext().storage.init())) {
        return app::BootResult::kOk;
    }
    ctx.reason = app::boot_reason::kStorageInit;
    return app::BootResult::kFail;
}

/**
 * @brief  启动步骤 10：系统模块准备（SN/蓝牙名/策略，写 systemStatus）
 * @param  ctx: 启动上下文（未用）
 * @retval kOk 总是；失败不可恢复，交由后续步骤暴露
 */
app::BootResult stepSystem(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    app::appContext().system.prepare();
    return app::BootResult::kOk;
}

/**
 * @brief 在输出任务已创建时登记启动掩码
 * @param ctx: 启动上下文，launched 可追加 trace 位
 * @note 仅启动任务，非 ISR，不阻塞。
 * @note trace_out 任务在 debug 步骤已启动；进入任务启动阶段前计入 launched，失败停机时一并等待它停稳。
 */
void markTraceLaunched(app::BootCtx &ctx) noexcept
{
#if FW_TRACE_ENABLED
    if (app::appContext().debug.taskRunning()) {
        ctx.launched |= app::taskBit(app::BootTask::kTrace);
    }
#else
    (void)ctx;
#endif
}

/**
 * @brief  启动步骤 11：链路执行器初始化、启动并等待首批模块就绪
 * @param  ctx: 启动上下文（读写 launched/reason/stopRequired）
 * @retval kOk 成功；kFail 启动失败（需停机回滚）
 */
app::BootResult stepLink(app::BootCtx &ctx) noexcept
{
    // 按启动回滚约束：从这里起任何失败（含 launched==0）都走 stopBootTasks，撤销 GNSS/SPP 启动状态。
    ctx.stopRequired = true;
    markTraceLaunched(ctx);
    const auto &bufs = *static_cast<app::BootBuffers *>(ctx.user);
    const char *error = app::startLink(bufs.linkStack, ctx.launched);
    if (error == nullptr) {
        return app::BootResult::kOk;
    }
    ctx.reason = error;
    return app::BootResult::kFail;
}

/**
 * @brief  启动步骤 12：IO 执行器（GNSS）；编译关闭时记 kSkipped
 * @param  ctx: 启动上下文（读写 launched/reason）
 * @retval kOk 成功；kFail 启动失败；kSkipped 未编译 GNSS
 */
app::BootResult stepIo(app::BootCtx &ctx) noexcept
{
#if RTK_GNSS_ENABLED
    const auto &bufs = *static_cast<app::BootBuffers *>(ctx.user);
    const char *error = app::startIo(bufs.ioStack, ctx.launched);
    if (error == nullptr) {
        return app::BootResult::kOk;
    }
    ctx.reason = error;
    return app::BootResult::kFail;
#else
    (void)ctx;
    return app::BootResult::kSkipped;
#endif
}

/**
 * @brief  启动步骤 13：等待全部消费者到达启动屏障并放行
 * @param  ctx: 启动上下文（读写 reason/stopRequired）
 * @retval kOk 成功；kFail 屏障等待失败
 */
app::BootResult stepStarted(app::BootCtx &ctx) noexcept
{
    app::AppContext &self = app::appContext();
    ctx.stopRequired = true;
    markTraceLaunched(ctx);
    if (!app::waitStarted(ctx.launched)) {
        ctx.stopRequired = true;
        ctx.reason = app::boot_reason::kTaskStartTimeout;
        return app::BootResult::kFail;
    }
    if (self.bootPhase.load(std::memory_order_acquire) != app::BootPhase::kStarting) {
        // 消费者在看门狗路径放弃了放行等待；不再进入 kRunning，按停机处理。
        ctx.stopRequired = true;
        return app::BootResult::kFail;
    }
    self.bootPhase.store(app::BootPhase::kRunning, std::memory_order_release);
    return app::BootResult::kOk;
}
// NOLINTEND(misc-const-correctness)

constexpr std::array<app::BootStep, 13U> kBootSteps{{
    {"power", &stepPower, app::OnFail::kDegrade},
    {"banner", &stepBanner, app::OnFail::kDegrade},
    {"contexts", &stepContexts, app::OnFail::kDegrade},
    {"console", &stepConsole, app::OnFail::kDegrade},
    {"alloc", &stepAlloc, app::OnFail::kDegrade},
    {"bind", &stepBind, app::OnFail::kDegrade},
    {"seal", &stepSeal, app::OnFail::kDegrade},
    {"debug", &stepDebug, app::OnFail::kDegrade},
    {"storage", &stepStorage, app::OnFail::kDegrade},
    {"system", &stepSystem, app::OnFail::kDegrade},
    {"link", &stepLink, app::OnFail::kDegrade},
    {"io", &stepIo, app::OnFail::kDegrade},
    {"started", &stepStarted, app::OnFail::kDegrade},
}};

constinit std::array<app::BootRecord, std::size(kBootSteps)> s_boot_records{};
size_t s_boot_executed{};

/**
 * @brief 按已启动的消费者决定停机或回滚
 * @param ctx: 启动上下文，携带任务掩码与外供缓冲
 * @param reason: 诊断原因，须活过使用方
 * @note 仅启动任务，可等待任务停稳，非 ISR。
 */
void handleDegrade(
    app::BootCtx &ctx, // NOLINT(misc-const-correctness): 由 BootStep::onDegrade 签名固定
    const char *reason) noexcept
{
    app::AppContext &self = app::appContext();
    if (reason != nullptr) {
        // SEALED 后仍保留首个原因；未封存的分配器同样留住原因供 sys info 显示。
        self.bootArena.setDegradedReason(reason);
    }
    if (ctx.stopRequired || (ctx.launched != 0U)) {
        app::stopBootTasks(ctx.launched);
        return;
    }
    if ((self.bootArena.state() == rtos::ArenaState::kOpen) && (ctx.user != nullptr)) {
        // 故障注入跳过 bind/seal 步骤体时分配器仍为 OPEN：按启动回滚约束先解绑再 abort，不留下半绑定的块。
        app::rollbackBootBindings(*static_cast<app::BootBuffers *>(ctx.user),
                                  (reason != nullptr) ? reason : app::boot_reason::kAllocAbort);
    }
    app::setConsoleOnly();
}

constexpr app::BootHooks kBootHooks{&nowMsHook, &heapHook, &handleDegrade};

/**
 * @brief  启动日志之后把组件日志切到文本环（由 trace_out 与 trace 事件按时间合并输出）
 * @note   放在 REPL 放行前：此后的日志异步输出，不阻塞调用任务；debug 未就绪时继续直写。
 */
void attachDebugLog() noexcept
{
#if FW_TRACE_ENABLED
    app::DebugModule &debug = app::appContext().debug;
    if (debug.taskRunning()) {
        (void)debug.attachLog(); // 失败只意味着继续走直写通道
    }
#endif
}

/**
 * @brief  成功路径的启动摘要（SYS I 级日志）；数据全部来自 DataCenter，不额外探测硬件
 * @param  records: 已执行的启动步骤记录（用于总耗时）
 */
void printBootSummary(std::span<const app::BootRecord> records) noexcept
{
    const app::AppContext &self = app::appContext();
    const auto system = self.data.systemStatus.read();
    const auto sessions = self.data.sessions.read();
    const auto link = self.data.linkStatus.read();
    FW_LOGI(fw::TraceCat::kSys, "storage online, SN %s, whitelist %u", system.sn.data(),
            static_cast<unsigned>(link.whitelistCount));
    core::BtAddr address{};
    // 蓝牙已在链路执行器 init 中启用，此处取控制器地址；未启用时保持全零。
    if (const uint8_t *raw = esp_bt_dev_get_address(); raw != nullptr) {
        static_assert(sizeof(address.bytes) == ESP_BD_ADDR_LEN,
                      "controller address length must match BtAddr");
        std::memcpy(address.bytes.data(), raw, sizeof(address.bytes));
    }
    std::array<char, app::kMacStrBuf> mac{};
    app::formatMac(address, mac);
    FW_LOGI(fw::TraceCat::kSys, "bluetooth online, %s %s, %u/%u sessions", system.deviceName.data(),
            mac.data(), static_cast<unsigned>(sessions.count),
            static_cast<unsigned>(app::kMaxSessions));
#if RTK_GNSS_ENABLED
    const auto gnss = self.data.gnssStatus.read();
    // 状态槽尚未被 GNSS 模块 poll 刷新时回落到 Kconfig 默认波特率。
    const uint32_t baud =
        gnss.hostBaud != 0U ? gnss.hostBaud : static_cast<uint32_t>(CONFIG_RTK_GNSS_DEFAULT_BAUD);
    FW_LOGI(fw::TraceCat::kSys, "<gnss> UART%d started, %u baud", CONFIG_RTK_GNSS_UART_NUM,
            static_cast<unsigned>(baud));
#endif
    const uint32_t totalMs =
        (records.back().startMs + records.back().tookMs) - records.front().startMs;
    const uint32_t freeHeap = esp_get_free_heap_size();
    // 千位字节转 KB 十位小数；上限远小于 uint32。
    FW_LOGI(fw::TraceCat::kSys, "boot done in %u ms, heap %u.%u KB free",
            static_cast<unsigned>(totalMs), static_cast<unsigned>(freeHeap / kBytesPerKb),
            static_cast<unsigned>((freeHeap % kBytesPerKb) * kTenthsPerKb / kBytesPerKb));
}

} // namespace

namespace app {

std::span<const BootRecord> bootRecords() noexcept
{
    return std::span{s_boot_records}.first(s_boot_executed);
}

void AppContext::boot() noexcept
{
    BootBuffers bufs{};
    // 静态存储：BootRecord::reason 可能指向 ctx.faultReason，`sys boot` 在启动任务返回后仍会读取它。
    static constinit BootCtx ctx{};
    ctx = BootCtx{};
    ctx.hooks = &kBootHooks;
    ctx.user = &bufs;
    ctx.faultStep = kFaultStep;
    s_boot_executed = runBootSteps(kBootSteps, ctx, s_boot_records);
    const auto executed = std::span{s_boot_records}.first(s_boot_executed);
    for (const BootRecord &record : executed) {
        if (record.result != BootResult::kFail) {
            continue;
        }
        if (record.reason != nullptr) {
            FW_LOGE(fw::TraceCat::kSys, "boot step %s failed: %s, console-only", record.name,
                    record.reason);
        } else {
            FW_LOGE(fw::TraceCat::kSys, "boot step %s failed, console-only", record.name);
        }
        attachDebugLog();
        runConsole(); // 降级同样放行 REPL：console-only 阶段仍可用诊断命令
        return;
    }
    printBootSummary(executed);
    if (kBootVerbose) {
        printBootTable(executed);
    }
    attachDebugLog();
    runConsole(); // 最后一步：摘要之后才出现提示符
}

} // namespace app
