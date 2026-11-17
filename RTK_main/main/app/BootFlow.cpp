// 启动流程（R8a）：13 步有序步骤表、降级/回滚/停机处理、横幅与单行摘要、步骤记录查询。
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

uint32_t now_ms_hook() noexcept
{
    // 启动至今的毫秒数远小于 uint32 上限（约 49 天），截断安全。
    return static_cast<uint32_t>(esp_timer_get_time() / kUsPerMs);
}

app::BootHeap heap_hook() noexcept
{
    app::BootHeap heap{};
    heap.free_bytes = heap_caps_get_free_size(kHeapCaps);
    heap.min_bytes = heap_caps_get_minimum_free_size(kHeapCaps);
    heap.largest_bytes = heap_caps_get_largest_free_block(kHeapCaps);
    return heap;
}

/**
 * @brief  拉高电源保持脚，松开按键后仍保持供电
 * @retval true 成功；false GPIO 配置失败（已记 E 级日志）
 */
bool enable_power_hold() noexcept
{
    // 保留基线 GPIO26 高有效的电源保持逻辑；本任务不执行硬件动作。
    const esp_err_t level_err = gpio_set_level(GPIO_NUM_26, 1U);
    if (level_err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power hold level failed: %s", esp_err_to_name(level_err));
        return false;
    }
    gpio_config_t power_hold{};
    power_hold.pin_bit_mask = 1ULL << GPIO_NUM_26;
    power_hold.mode = GPIO_MODE_OUTPUT;
    power_hold.pull_up_en = GPIO_PULLUP_DISABLE;
    power_hold.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_hold.intr_type = GPIO_INTR_DISABLE;
    const esp_err_t power_err = gpio_config(&power_hold);
    if (power_err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "power hold init failed: %s", esp_err_to_name(power_err));
        return false;
    }
    return true;
}

/**
 * @brief  启动步骤 1：安装日志直写通道（此后启动日志带时间戳与类别），再做电源保持
 * @param  ctx: 启动上下文（未用）
 * @retval kOk 成功；kFail 电源保持失败
 */
app::BootResult step_power(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    (void)app::install_direct_log(); // 只会因重复安装失败，此时已有通道可用
    return enable_power_hold() ? app::BootResult::kOk : app::BootResult::kFail;
}

/**
 * @brief  启动步骤 2：横幅与复位原因；PANIC/看门狗/欠压用 W 级
 * @param  ctx: 启动上下文（未用）
 * @retval kOk 总是
 */
app::BootResult step_banner(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    const esp_reset_reason_t reason = esp_reset_reason();
    const char *name = app::reset_reason_name(reason);
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

/** 步骤 3：绑定数据主题共享锁与执行器指针（K5 顺序：contexts 先于 console）。 */
app::BootResult step_contexts(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    app::AppContext &self = app::app_context();
    self.bind_contexts();
    app::bind_executors();
#if RTK_GNSS_ENABLED
    self.rtcm_pipe.init_lock();
#endif
    self.data.init();
    return app::BootResult::kOk;
}

/**
 * 步骤 4：只建立控制台输出通道并登记命令，不放行 REPL。
 * REPL 到 boot() 末尾才 run：此前提示符/IDF 提示语若先出现，会与后续各步骤日志交错。
 * UART 驱动重装必须在业务任务启动前完成，所以 init 留在这里而不是挪到末尾。
 */
app::BootResult step_console(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    return app::init_console() ? app::BootResult::kOk : app::BootResult::kFail;
}

/** 步骤 5：按固定 owner 顺序一次性分配数据通路与栈缓冲。 */
app::BootResult step_alloc(app::BootCtx &ctx) noexcept
{
    auto &bufs = *static_cast<app::BootBuffers *>(ctx.user); // user 只指向启动任务的栈上 BootBuffers。
    if (app::allocate_boot_buffers(bufs)) {
        return app::BootResult::kOk;
    }
    // 分配器 abort 时已记下 boot_alloc:<owner>，步骤记录引用同一原因供日志与 sys boot 显示。
    ctx.reason = app::app_context().boot_arena.reason();
    return app::BootResult::kFail;
}

/** 步骤 6：把缓冲交给各 C 驱动；失败时按 K5 语义回滚已绑定部分。 */
app::BootResult step_bind(app::BootCtx &ctx) noexcept
{
    auto &bufs = *static_cast<app::BootBuffers *>(ctx.user);
    const char *error = app::bind_boot_buffers(bufs);
    if (error == nullptr) {
        return app::BootResult::kOk;
    }
    app::rollback_boot_bindings(bufs, error);
    ctx.reason = error;
    return app::BootResult::kFail;
}

/** 步骤 7：封存分配器；失败时回滚绑定（SEALED 后无法释放块）。 */
app::BootResult step_seal(app::BootCtx &ctx) noexcept
{
    auto &bufs = *static_cast<app::BootBuffers *>(ctx.user);
    if (fw::is_ok(app::app_context().boot_arena.seal())) {
        return app::BootResult::kOk;
    }
    app::rollback_boot_bindings(bufs, app::boot_reason::kAllocSeal);
    ctx.reason = app::boot_reason::kAllocSeal;
    return app::BootResult::kFail;
}

/** 步骤 8：安装 trace 全局 sink（编译关闭时记 kSkipped，不从表里删除）。 */
app::BootResult step_debug(app::BootCtx &ctx) noexcept
{
    (void)ctx;
#if FW_TRACE_ENABLED
    // 所有绑定成功、封存后才安装全局 sink，回滚时不会留下悬空的日志引用。
    if (fw::is_ok(app::app_context().debug.init())) {
        return app::BootResult::kOk;
    }
    ctx.reason = app::boot_reason::kDebugInit;
    return app::BootResult::kFail;
#else
    return app::BootResult::kSkipped;
#endif
}

/** 步骤 9：加载 NVS/SN；必须先于 BT 初始化，执行器再次 init 时为幂等。 */
app::BootResult step_storage(app::BootCtx &ctx) noexcept
{
    if (fw::is_ok(app::app_context().storage.init())) {
        return app::BootResult::kOk;
    }
    ctx.reason = app::boot_reason::kStorageInit;
    return app::BootResult::kFail;
}

/** 步骤 10：系统模块准备（SN/蓝牙名/策略，写 system_status）；失败不可恢复，交由后续步骤暴露。 */
app::BootResult step_system(app::BootCtx &ctx) noexcept
{
    (void)ctx;
    app::app_context().system.prepare();
    return app::BootResult::kOk;
}

/** trace_out 任务在 debug 步骤已启动；进入任务启动阶段前计入 launched，失败停机时一并等待它停稳。 */
void mark_trace_launched(app::BootCtx &ctx) noexcept
{
#if FW_TRACE_ENABLED
    if (app::app_context().debug.task_running()) {
        ctx.launched |= app::task_bit(app::BootTask::kTrace);
    }
#else
    (void)ctx;
#endif
}

/** 步骤 11：链路执行器初始化、启动并等待首批模块就绪。 */
app::BootResult step_link(app::BootCtx &ctx) noexcept
{
    // 与 K5 一致：从这里起任何失败（含 launched==0）都走 stop_boot_tasks，撤销 GNSS/SPP 启动状态。
    ctx.stop_required = true;
    mark_trace_launched(ctx);
    auto &bufs = *static_cast<app::BootBuffers *>(ctx.user);
    const char *error = app::start_link(bufs.link_stack, ctx.launched);
    if (error == nullptr) {
        return app::BootResult::kOk;
    }
    ctx.reason = error;
    return app::BootResult::kFail;
}

/** 步骤 12：IO 执行器（GNSS）；编译关闭时记 kSkipped。 */
app::BootResult step_io(app::BootCtx &ctx) noexcept
{
#if RTK_GNSS_ENABLED
    auto &bufs = *static_cast<app::BootBuffers *>(ctx.user);
    const char *error = app::start_io(bufs.io_stack, ctx.launched);
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

/** 步骤 13：等待全部消费者到达启动屏障并放行；失败标记需要停机。 */
app::BootResult step_started(app::BootCtx &ctx) noexcept
{
    app::AppContext &self = app::app_context();
    ctx.stop_required = true;
    mark_trace_launched(ctx);
    if (!app::wait_started(ctx.launched)) {
        ctx.stop_required = true;
        ctx.reason = app::boot_reason::kTaskStartTimeout;
        return app::BootResult::kFail;
    }
    if (self.boot_phase.load(std::memory_order_acquire) != app::BootPhase::kStarting) {
        // 消费者在看门狗路径放弃了放行等待；不再进入 kRunning，按停机处理。
        ctx.stop_required = true;
        return app::BootResult::kFail;
    }
    self.boot_phase.store(app::BootPhase::kRunning, std::memory_order_release);
    return app::BootResult::kOk;
}

constexpr std::array<app::BootStep, 13U> kBootSteps{{
    {"power", &step_power, app::OnFail::kDegrade},
    {"banner", &step_banner, app::OnFail::kDegrade},
    {"contexts", &step_contexts, app::OnFail::kDegrade},
    {"console", &step_console, app::OnFail::kDegrade},
    {"alloc", &step_alloc, app::OnFail::kDegrade},
    {"bind", &step_bind, app::OnFail::kDegrade},
    {"seal", &step_seal, app::OnFail::kDegrade},
    {"debug", &step_debug, app::OnFail::kDegrade},
    {"storage", &step_storage, app::OnFail::kDegrade},
    {"system", &step_system, app::OnFail::kDegrade},
    {"link", &step_link, app::OnFail::kDegrade},
    {"io", &step_io, app::OnFail::kDegrade},
    {"started", &step_started, app::OnFail::kDegrade},
}};

constinit std::array<app::BootRecord, std::size(kBootSteps)> s_boot_records{};
size_t s_boot_executed{};

/** 降级处理：记录原因；进入任务启动阶段后停机（K5 语义），未封存时回滚绑定，否则只留 console-only。 */
void handle_degrade(app::BootCtx &ctx, const char *reason) noexcept
{
    app::AppContext &self = app::app_context();
    if (reason != nullptr) {
        // SEALED 后仍保留首个原因；未封存的分配器同样留住原因供 sys info 显示。
        self.boot_arena.set_degraded_reason(reason);
    }
    if (ctx.stop_required || (ctx.launched != 0U)) {
        app::stop_boot_tasks(ctx.launched);
        return;
    }
    if ((self.boot_arena.state() == rtos::ArenaState::kOpen) && (ctx.user != nullptr)) {
        // 故障注入跳过 bind/seal 步骤体时分配器仍为 OPEN：与 K5 一致先解绑再 abort，不留下半绑定的块。
        app::rollback_boot_bindings(*static_cast<app::BootBuffers *>(ctx.user),
                                    (reason != nullptr) ? reason : app::boot_reason::kAllocAbort);
    }
    app::set_console_only();
}

constexpr app::BootHooks kBootHooks{&now_ms_hook, &heap_hook, &handle_degrade};

/**
 * @brief  启动日志之后把组件日志切到文本环（由 trace_out 与 trace 事件按时间合并输出）
 * @note   放在 REPL 放行前：此后的日志异步输出，不阻塞调用任务；debug 未就绪时继续直写。
 */
void attach_debug_log() noexcept
{
#if FW_TRACE_ENABLED
    app::DebugModule &debug = app::app_context().debug;
    if (debug.task_running()) {
        (void)debug.attach_log(); // 失败只意味着继续走直写通道
    }
#endif
}

/**
 * @brief  成功路径的启动摘要（SYS I 级日志）；数据全部来自 DataCenter，不额外探测硬件
 * @param  records: 已执行的启动步骤记录（用于总耗时）
 */
void print_boot_summary(std::span<const app::BootRecord> records) noexcept
{
    app::AppContext &self = app::app_context();
    const auto system = self.data.system_status.read();
    const auto sessions = self.data.sessions.read();
    const auto link = self.data.link_status.read();
    FW_LOGI(fw::TraceCat::kSys, "storage online, SN %s, whitelist %u", system.sn.data(),
            static_cast<unsigned>(link.whitelist_count));
    core::BtAddr address{};
    // 蓝牙已在链路执行器 init 中启用，此处取控制器地址；未启用时保持全零。
    if (const uint8_t *raw = esp_bt_dev_get_address(); raw != nullptr) {
        static_assert(sizeof(address.bytes) == ESP_BD_ADDR_LEN, "controller address length must match BtAddr");
        std::memcpy(address.bytes.data(), raw, sizeof(address.bytes));
    }
    std::array<char, app::kMacStrBuf> mac{};
    app::format_mac(address, mac);
    FW_LOGI(fw::TraceCat::kSys, "bluetooth online, %s %s, %u/%u sessions", system.device_name.data(), mac.data(),
            static_cast<unsigned>(sessions.count), static_cast<unsigned>(app::kMaxSessions));
#if RTK_GNSS_ENABLED
    const auto gnss = self.data.gnss_status.read();
    // 状态槽尚未被 GNSS 模块 poll 刷新时回落到 Kconfig 默认波特率。
    const uint32_t baud = gnss.host_baud != 0U ? gnss.host_baud : static_cast<uint32_t>(CONFIG_RTK_GNSS_DEFAULT_BAUD);
    FW_LOGI(fw::TraceCat::kSys, "<gnss> UART%d started, %u baud", CONFIG_RTK_GNSS_UART_NUM,
            static_cast<unsigned>(baud));
#endif
    const uint32_t total_ms = (records.back().start_ms + records.back().took_ms) - records.front().start_ms;
    const uint32_t free_heap = esp_get_free_heap_size();
    // 千位字节转 KB 十位小数；上限远小于 uint32。
    FW_LOGI(fw::TraceCat::kSys, "boot done in %u ms, heap %u.%u KB free", static_cast<unsigned>(total_ms),
            static_cast<unsigned>(free_heap / kBytesPerKb),
            static_cast<unsigned>((free_heap % kBytesPerKb) * kTenthsPerKb / kBytesPerKb));
}

} // namespace

namespace app {

std::span<const BootRecord> boot_records() noexcept
{
    return std::span{s_boot_records}.first(s_boot_executed);
}

/**
 * @brief  按步骤表完成启动；任一步失败则回滚并降级为 console-only，最后启动 REPL
 * @note   只在 app_main 任务调用一次。
 */
void AppContext::boot() noexcept
{
    BootBuffers bufs{};
    // 静态存储：BootRecord::reason 可能指向 ctx.fault_reason，`sys boot` 在启动任务返回后仍会读取它。
    static constinit BootCtx ctx{};
    ctx = BootCtx{};
    ctx.hooks = &kBootHooks;
    ctx.user = &bufs;
    ctx.fault_step = kFaultStep;
    s_boot_executed = run_boot_steps(kBootSteps, ctx, s_boot_records);
    const auto executed = std::span{s_boot_records}.first(s_boot_executed);
    for (const BootRecord &record : executed) {
        if (record.result != BootResult::kFail) {
            continue;
        }
        if (record.reason != nullptr) {
            FW_LOGE(fw::TraceCat::kSys, "boot step %s failed: %s, console-only", record.name, record.reason);
        } else {
            FW_LOGE(fw::TraceCat::kSys, "boot step %s failed, console-only", record.name);
        }
        attach_debug_log();
        run_console(); // 降级同样放行 REPL：console-only 阶段仍可用诊断命令
        return;
    }
    print_boot_summary(executed);
    if (kBootVerbose) {
        print_boot_table(executed);
    }
    attach_debug_log();
    run_console(); // 最后一步：摘要之后才出现提示符
}

} // namespace app
