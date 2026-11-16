// 固件入口与组合根：装配静态模块表、执行器与控制台命令表，启动期经 boot_alloc 申请数据通路/栈缓冲并按顺序启动。
// 只在 app_main 启动任务中运行，启动期一次性完成；之后本文件不再执行。
#include "app_context.hpp"

#include <array>
#include <cstdio>
#include <span>
#include <string_view>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "esp_system.h"
#endif
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "cli/console.hpp"
#include "spp_server_storage.h"

namespace {
constexpr const char *kTag = "app";
constinit app::AppContext s_app{};
#if defined(ESP_PLATFORM)
constinit rtos::Executor s_link_executor{};
#if RTK_GNSS_ENABLED
constinit rtos::Executor s_io_executor{};
#endif
#endif

struct BootBuffers {
    std::span<StackType_t> link_stack{};
#if RTK_GNSS_ENABLED
    std::span<StackType_t> io_stack{};
    std::span<uint8_t> gnss_demux{};
    std::span<uint8_t> rtcm_pipe{};
    std::span<uint8_t> gnss_app_ring{};
    std::span<uint8_t> gnss_latest{};
#endif
    std::span<StackType_t> spp_worker_stack{};
    std::span<uint8_t> spp_rx_0{};
    std::span<uint8_t> spp_rx_1{};
    std::span<uint8_t> spp_reply_0{};
    std::span<uint8_t> spp_reply_1{};
    std::span<uint8_t> spp_stream{};
    std::span<uint8_t> spp_merge{};
#if FW_TRACE_ENABLED
    std::span<StackType_t> trace_task_stack{};
    std::span<fw::TraceEvent> trace_ring{};
    std::span<uint8_t> log_ring{};
#endif
};

bool allocate_boot_buffers(BootBuffers &out, const rtos::BootAllocatorOps *ops = nullptr) noexcept
{
    if (!fw::is_ok(s_app.boot_arena.open(ops))) {
        (void)s_app.boot_arena.abort("boot_alloc:open");
        return false;
    }
    const auto fail_alloc = [&out](const char *name) noexcept -> bool {
        out = {};
        std::array<char, rtos::BootArena::kMaxReasonBytes> reason{};
        (void)std::snprintf(reason.data(), reason.size(), "boot_alloc:%s", name);
        (void)s_app.boot_arena.abort(reason.data());
        ESP_LOGE(kTag, "boot_alloc failed (%s); entering console-only mode", name);
        return false;
    };

    constexpr size_t kStackAlign = 16U;
    constexpr size_t kBufAlign = 4U;

    if (!fw::is_ok(s_app.boot_arena.alloc_array<StackType_t>(
            "task.link", app::AppContext::kLinkStackBytes / sizeof(StackType_t), out.link_stack, kStackAlign))) {
        return fail_alloc("task.link");
    }
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(s_app.boot_arena.alloc_array<StackType_t>(
            "task.io", app::AppContext::kIoStackBytes / sizeof(StackType_t), out.io_stack, kStackAlign))) {
        return fail_alloc("task.io");
    }
#endif
    if (!fw::is_ok(s_app.boot_arena.alloc_array<StackType_t>("task.spp_worker",
                                                             SPP_SERVER_WORKER_STACK_BYTES / sizeof(StackType_t),
                                                             out.spp_worker_stack, kStackAlign))) {
        return fail_alloc("task.spp_worker");
    }
#if FW_TRACE_ENABLED
    if (!fw::is_ok(s_app.boot_arena.alloc_array<StackType_t>("task.trace_out",
                                                             app::DebugModule::kTaskStackBytes / sizeof(StackType_t),
                                                             out.trace_task_stack, kStackAlign))) {
        return fail_alloc("task.trace_out");
    }
#endif
    if (!fw::is_ok(
            s_app.boot_arena.alloc_array<uint8_t>("spp.rx.0", SPP_SERVER_RX_BUF_SIZE, out.spp_rx_0, kBufAlign))) {
        return fail_alloc("spp.rx.0");
    }
    if (!fw::is_ok(
            s_app.boot_arena.alloc_array<uint8_t>("spp.rx.1", SPP_SERVER_RX_BUF_SIZE, out.spp_rx_1, kBufAlign))) {
        return fail_alloc("spp.rx.1");
    }
#if FW_TRACE_ENABLED
    if (!fw::is_ok(s_app.boot_arena.alloc_array<fw::TraceEvent>("trace.ring", app::DebugModule::kRingEvents,
                                                                out.trace_ring, alignof(fw::TraceEvent)))) {
        return fail_alloc("trace.ring");
    }
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("log.ring", app::DebugModule::kLogRingBytes, out.log_ring,
                                                         alignof(fw::LogHeader)))) {
        return fail_alloc("log.ring");
    }
#endif
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("gnss.demux", app::GnssModule::kDemuxBufBytes, out.gnss_demux,
                                                         kBufAlign))) {
        return fail_alloc("gnss.demux");
    }
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("rtcm.pipe", app::kRtcmPipeBytes, out.rtcm_pipe, kBufAlign))) {
        return fail_alloc("rtcm.pipe");
    }
#endif
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("spp.tx.reply.0", SPP_SERVER_REPLY_CAPACITY, out.spp_reply_0,
                                                         kBufAlign))) {
        return fail_alloc("spp.tx.reply.0");
    }
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("spp.tx.reply.1", SPP_SERVER_REPLY_CAPACITY, out.spp_reply_1,
                                                         kBufAlign))) {
        return fail_alloc("spp.tx.reply.1");
    }
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("spp.tx.stream.phone", SPP_SERVER_STREAM_CAPACITY,
                                                         out.spp_stream, kBufAlign))) {
        return fail_alloc("spp.tx.stream.phone");
    }
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("spp.tx.merge", SPP_SERVER_MERGE_CAPACITY, out.spp_merge,
                                                         kBufAlign))) {
        return fail_alloc("spp.tx.merge");
    }
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("gnss.app_ring", app::kGnssAppRingBytes, out.gnss_app_ring,
                                                         kBufAlign))) {
        return fail_alloc("gnss.app_ring");
    }
    if (!fw::is_ok(s_app.boot_arena.alloc_array<uint8_t>("gnss.latest", app::data::kGnssLatestTotalBytes,
                                                         out.gnss_latest, kBufAlign))) {
        return fail_alloc("gnss.latest");
    }
#endif
    return true;
}

void rollback_boot_bindings(BootBuffers &bufs, const char *reason) noexcept
{
#if FW_TRACE_ENABLED
    s_app.debug.unbind_storage();
#endif
#if RTK_GNSS_ENABLED
    s_app.gnss.unbind_storage();
    s_app.rtcm_pipe.unbind_storage();
    s_app.data.unbind_gnss_buffers();
#endif
    const esp_err_t unbound = spp_server_unbind_storage();
    bufs = {};
    if (unbound != ESP_OK) {
        // 若约束被破坏、C worker 已存在，保留全部块，绝不释放它可能使用的栈/缓冲。
        s_app.boot_arena.set_degraded_reason("rollback:spp_in_use");
        return;
    }
    if (!fw::is_ok(s_app.boot_arena.abort(reason))) {
        s_app.boot_arena.set_degraded_reason(reason);
    }
}

// 硬件装配表仅用于 IDF；host 直接检查同一文件内的分配、绑定、回滚与命令门控。
#if defined(ESP_PLATFORM)
template <typename T> constexpr auto kModuleOps = fw::make_ops<T>();
// 编号由 id() 保证无窄化；对象与操作表同为静态存储。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, readability-identifier-naming)
constinit const fw::Module kModules[]{
// X-macro 生成异构对象的注册项，constexpr 函数无法替代清单展开。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MODULE_DEF(type, member, name, kind, executor, period)                                                         \
    {name,          app::id(app::ModuleKind::kind), app::id(app::ExecKind::executor), period,                          \
     &s_app.member, &kModuleOps<app::type>},
#include "module_list.inc"
#undef MODULE_DEF
};
#if RTK_GNSS_ENABLED
#if defined(CONFIG_ESP_CONSOLE_UART_NUM)
static_assert(CONFIG_RTK_GNSS_UART_NUM != CONFIG_ESP_CONSOLE_UART_NUM, "GNSS UART must not conflict with console UART");
#endif
// IO 执行器绑 core0（prio 9，低于 link 的 prio 10），不抢占蓝牙链路实时性；阻塞于 GNSS UART 读。
constexpr rtos::ExecutorConfig kIoConfig{
    .name = "io",
    .id = app::id(app::ExecKind::kIo),
    .priority = 9,
    .core = 0,
    .idle_wait_ms = 20,
    .round_budget_ms = 20,
    .yield_ticks = 1,
    .watchdog = true,
    .wait =
        [](void * /*ctx*/, uint32_t max_ms) noexcept {
            if (!s_app.await_boot(2U, true)) {
                vTaskDelete(nullptr);
                return;
            }
            s_app.gnss.wait(max_ms);
        },
    .wait_ctx = nullptr,
};
#endif
// 链路绑 core1，BT worker/控制台在 core0；每轮至少让出 1 tick 给 IDLE1。
constexpr rtos::ExecutorConfig kLinkConfig{
    .name = "link",
    .id = app::id(app::ExecKind::kLink),
    .priority = 10,
    .core = 1,
    .idle_wait_ms = 20,
    .round_budget_ms = 20,
    .yield_ticks = 1,
    .watchdog = true,
    .wait =
        [](void * /*ctx*/, uint32_t max_ms) noexcept {
            if (!s_app.await_boot(1U, true)) {
                vTaskDelete(nullptr);
                return;
            }
            s_app.bt_link.wait(max_ms);
        },
    .wait_ctx = nullptr,
};

const char *reset_reason_name(esp_reset_reason_t reason) noexcept
{
    switch (reason) {
    case ESP_RST_UNKNOWN:
        return "unknown";
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external-pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-wdt";
    case ESP_RST_TASK_WDT:
        return "task-wdt";
    case ESP_RST_WDT:
        return "other-wdt";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse";
    case ESP_RST_PWR_GLITCH:
        return "power-glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu-lockup";
    default:
        return "unknown";
    }
}

bool enable_power_hold() noexcept
{
    // 保留基线 GPIO26 高有效的电源保持逻辑；本任务不执行硬件动作。
    const esp_err_t level_err = gpio_set_level(GPIO_NUM_26, 1U);
    if (level_err != ESP_OK) {
        ESP_LOGE(kTag, "power hold level failed: %s", esp_err_to_name(level_err));
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
        ESP_LOGE(kTag, "power hold init failed: %s", esp_err_to_name(power_err));
        return false;
    }
    return true;
}

bool wait_started(uint32_t bits) noexcept
{
    const TickType_t begin = xTaskGetTickCount();
    while ((s_app.boot_arrived.load(std::memory_order_acquire) & bits) != bits) {
        if ((xTaskGetTickCount() - begin) >= pdMS_TO_TICKS(5000U)) {
            s_app.boot_arena.set_degraded_reason("task_start_timeout");
            return false;
        }
        vTaskDelay(1U);
    }
    return true;
}

bool modules_ready(const rtos::Executor &executor, fw::ExecutorId id) noexcept
{
    for (const auto &module : kModules) {
        if ((module.executor == id) && !executor.module_ok(module.id)) {
            return false;
        }
    }
    return true;
}

bool start_link(std::span<StackType_t> link_stack, uint32_t &launched) noexcept
{
    const auto initialized =
        s_app.link_executor->init(kLinkConfig, kModules, app::view_of(s_app.link_storage, link_stack));
    if (!fw::is_ok(initialized) || !modules_ready(*s_app.link_executor, kLinkConfig.id) ||
        !s_app.data.system_status.read().bt_ready) {
        s_app.boot_arena.set_degraded_reason("link_init");
        return false;
    }
    if (!fw::is_ok(s_app.link_executor->start())) {
        s_app.boot_arena.set_degraded_reason("link_task_start");
        return false;
    }
    launched |= 1U;
    if (!wait_started(1U) || !modules_ready(*s_app.link_executor, kLinkConfig.id)) {
        s_app.boot_arena.set_degraded_reason("link_module_start");
        return false;
    }
    return true;
}

#if RTK_GNSS_ENABLED
bool start_io(std::span<StackType_t> io_stack, uint32_t &launched) noexcept
{
    const auto initialized = s_app.io_executor->init(kIoConfig, kModules, app::view_of(s_app.io_storage, io_stack));
    if (!fw::is_ok(initialized) || !modules_ready(*s_app.io_executor, kIoConfig.id)) {
        s_app.boot_arena.set_degraded_reason("io_init");
        return false;
    }
    if (!fw::is_ok(s_app.io_executor->start())) {
        s_app.boot_arena.set_degraded_reason("io_task_start");
        return false;
    }
    launched |= 2U;
    if (!wait_started(2U) || !modules_ready(*s_app.io_executor, kIoConfig.id)) {
        s_app.boot_arena.set_degraded_reason("io_module_start");
        return false;
    }
    return true;
}
#endif

#endif // ESP_PLATFORM

struct GatedCommand {
    cli::Handler handler = nullptr;
    void *ctx = nullptr;
    bool safe = false;
};
#if defined(ESP_PLATFORM)
constinit std::array<GatedCommand, cli::kMaxCmds> s_command_gates{};
constinit std::array<cli::Cmd, cli::kMaxCmds> s_gated_commands{};
#endif

// NOLINTNEXTLINE(misc-const-correctness): cli::Handler 的固定 ABI 要求 void*，gate 本身只读。
int gated_command(void *ctx, int argc, char **argv) noexcept
{
    const auto &gate = *static_cast<const GatedCommand *>(ctx); // ctx 只指向组合根的静态命令 gate。
    const auto phase = s_app.boot_phase.load(std::memory_order_acquire);
    if ((phase == app::BootPhase::kStarting) || (phase == app::BootPhase::kStopping)) {
        return cli::fail("unavailable: booting");
    }
    if ((phase == app::BootPhase::kConsoleOnly) && !gate.safe) {
        return cli::fail("unavailable: console-only");
    }
    return gate.handler(gate.ctx, argc, argv);
}

void set_console_only() noexcept
{
    s_app.boot_phase.store(app::BootPhase::kConsoleOnly, std::memory_order_release);
}

#if defined(ESP_PLATFORM)
bool start_console() noexcept
{
    constexpr const char *kConsolePrompt = "console> ";
    struct VerbGroupDef {
        const char *name;
        const char *summary;
        std::span<const cli::Verb> verbs;
    };

    const std::array cmd_tables{
        app::system_cmds(),  app::link_cmds(), app::config_cmds(), app::rtcm_cmds(),
#if RTK_GNSS_ENABLED
        app::gnss_cmds(),
#endif
#if FW_TRACE_ENABLED
        app::debug_cmds(),
#endif
        app::factory_cmds(),
    };
    // 命令表地址在 REPL 启动后不可变；handler 经 phase 原子门控，控制台先于业务堆分配建立。
    size_t used = 0U;
    for (const auto table : cmd_tables) {
        for (const auto &command : table) {
            if (used >= s_gated_commands.size()) {
                return false;
            }
            const std::string_view name{command.name};
            s_command_gates[used] = {command.handler, command.ctx,
                                     name == "status" || name == "log" || name == "sys" || name == "config"};
            s_gated_commands[used] = command;
            s_gated_commands[used].handler = &gated_command;
            s_gated_commands[used].ctx = &s_command_gates[used];
            ++used;
        }
    }
    if (cli::add(std::span{s_gated_commands}.first(used)) != ESP_OK) {
        ESP_LOGE(kTag, "console add failed");
        return false;
    }
    const std::array verb_groups{
        VerbGroupDef{"status", "system and link status (default overview)", app::status_verbs()},
        VerbGroupDef{"log", "runtime IDF log level", app::log_verbs()},
        VerbGroupDef{"sys", "system info, heap, mem, tasks and reboot", app::sys_verbs()},
        VerbGroupDef{"rtcm", "RTCM forwarding stats and reset", app::rtcm_verbs()},
#if RTK_GNSS_ENABLED
        VerbGroupDef{"gnss", "GNSS receiver control and telemetry", app::gnss_verbs()},
#endif
#if FW_TRACE_ENABLED
        VerbGroupDef{"debug", "trace categories, report and NVS", app::debug_verbs()},
#endif
        VerbGroupDef{"factory", "production settings (hidden, needs unlock)", app::factory_verbs()},
    };
    /* 必须在 start 之前登记所有动词表，供 help / help <group> 展示。 */
    for (const auto &group : verb_groups) {
        const esp_err_t verbs_err = cli::add_verbs(group.name, group.summary, group.verbs);
        if (verbs_err != ESP_OK) {
            ESP_LOGE(kTag, "console verbs failed: %s", esp_err_to_name(verbs_err));
            return false;
        }
    }
    const esp_err_t console_err = cli::start(kConsolePrompt);
    if (console_err != ESP_OK) {
        ESP_LOGE(kTag, "console start failed: %s", esp_err_to_name(console_err));
        return false;
    }
    return true;
}

#endif // ESP_PLATFORM

const char *bind_boot_buffers(const BootBuffers &bufs) noexcept
{
#if FW_TRACE_ENABLED
    if (!fw::is_ok(s_app.debug.bind_storage({bufs.trace_ring, bufs.log_ring, bufs.trace_task_stack}))) {
        return "bind:debug";
    }
#endif
    const spp_server_storage_t storage{
        .rx_buf = {bufs.spp_rx_0.data(), bufs.spp_rx_1.data()},
        .rx_buf_bytes = {bufs.spp_rx_0.size(), bufs.spp_rx_1.size()},
        .reply_buf = {bufs.spp_reply_0.data(), bufs.spp_reply_1.data()},
        .reply_buf_bytes = {bufs.spp_reply_0.size(), bufs.spp_reply_1.size()},
        .stream_buf = bufs.spp_stream.data(),
        .stream_buf_bytes = bufs.spp_stream.size(),
        .merge_buf = bufs.spp_merge.data(),
        .merge_buf_bytes = bufs.spp_merge.size(),
        .worker_stack = bufs.spp_worker_stack.data(),
        .worker_stack_bytes = bufs.spp_worker_stack.size_bytes(),
    };
    if (spp_server_bind_storage(&storage) != ESP_OK) {
        return "bind:spp";
    }
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(s_app.data.init_gnss_buffers(bufs.gnss_app_ring, bufs.gnss_latest)) ||
        !fw::is_ok(s_app.rtcm_pipe.bind_storage(bufs.rtcm_pipe)) ||
        !fw::is_ok(s_app.gnss.bind_storage(bufs.gnss_demux))) {
        return "bind:gnss";
    }
#endif
    return nullptr;
}

#if defined(ESP_PLATFORM)
void stop_boot_tasks(uint32_t launched) noexcept
{
    s_app.boot_phase.store(app::BootPhase::kStopping, std::memory_order_release);
    const TickType_t begin = xTaskGetTickCount();
    while ((s_app.boot_stopped.load(std::memory_order_acquire) & launched) != launched) {
        if ((xTaskGetTickCount() - begin) >= pdMS_TO_TICKS(5000U)) {
            s_app.boot_arena.set_degraded_reason("stop:task_timeout");
            // 消费者未确认停稳：不撤销驱动、回调或任何封存块，诊断控制台仍可用。
            set_console_only();
            return;
        }
        vTaskDelay(1U);
    }
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(s_app.gnss.stop_boot())) {
        s_app.boot_arena.set_degraded_reason("stop:gnss");
    }
#endif
    if (spp_server_stop_boot() != ESP_OK) {
        s_app.boot_arena.set_degraded_reason("stop:spp");
    }
    // SEALED 块始终保留至重启；SDK 停机失败或迟到回调也不回收其存储。
    set_console_only();
}
#endif // ESP_PLATFORM
} // namespace

namespace app {
AppContext &app_context() noexcept
{
    return s_app;
}

bool AppContext::await_boot(uint32_t task_bit, bool watchdog) noexcept
{
    if (boot_phase.load(std::memory_order_acquire) == BootPhase::kRunning) {
        return true;
    }
    boot_arrived.fetch_or(task_bit, std::memory_order_release);
    while (boot_phase.load(std::memory_order_acquire) == BootPhase::kStarting) {
        if (watchdog && (esp_task_wdt_status(nullptr) == ESP_OK)) {
            if (esp_task_wdt_reset() != ESP_OK) {
                boot_phase.store(BootPhase::kStopping, std::memory_order_release);
                break;
            }
        }
        vTaskDelay(1U);
    }
    if (boot_phase.load(std::memory_order_acquire) == BootPhase::kRunning) {
        return true;
    }
    if (watchdog && (esp_task_wdt_status(nullptr) == ESP_OK)) {
        if (esp_task_wdt_delete(nullptr) != ESP_OK) {
            // 标记启动诊断；仍保持停稳状态，不允许回到模块轮询。
            boot_stopped.fetch_or(0x80000000U, std::memory_order_release);
        }
    }
    boot_stopped.fetch_or(task_bit, std::memory_order_release);
    return false;
}
} // namespace app

#if defined(ESP_PLATFORM)
extern "C" void app_main(void)
{
    if (!enable_power_hold()) {
        return;
    }
    /* 输出启动原因并绑定数据主题的共享锁与执行器指针。 */
    // 固件版本字符串短于 int 上限，日志精度转换安全。
    ESP_LOGI(kTag, "RTK_main %.*s boot", static_cast<int>(app::kAppVersion.size()), app::kAppVersion.data());
    ESP_LOGI(kTag, "reset reason: %s", reset_reason_name(esp_reset_reason()));
    s_app.bind_contexts();
    s_app.link_executor = &s_link_executor;
#if RTK_GNSS_ENABLED
    s_app.io_executor = &s_io_executor;
    s_app.rtcm_pipe.init_lock();
#endif
    s_app.data.init();
    if (!start_console()) {
        return;
    }

    BootBuffers bufs{};
    if (!allocate_boot_buffers(bufs)) {
        set_console_only();
        return;
    }

    if (const char *error = bind_boot_buffers(bufs); error != nullptr) {
        rollback_boot_bindings(bufs, error);
        set_console_only();
        return;
    }

    if (!fw::is_ok(s_app.boot_arena.seal())) {
        rollback_boot_bindings(bufs, "boot_alloc:seal");
        set_console_only();
        return;
    }
#if FW_TRACE_ENABLED
    // 所有绑定成功、封存后才安装全局 sink，回滚时不会留下悬空的日志引用。
    if (!fw::is_ok(s_app.debug.init())) {
        s_app.boot_arena.set_degraded_reason("debug_init");
        set_console_only();
        return;
    }
#endif
    /* NVS/SN 必须先于 BT 初始化，执行器再次 init 存储模块时为幂等。 */
    const auto loaded = s_app.storage.init();
    if (!fw::is_ok(loaded)) {
        s_app.boot_arena.set_degraded_reason("storage_init");
        set_console_only();
        return;
    }
    s_app.system.prepare();
    uint32_t launched = 0U;
    bool ready = start_link(bufs.link_stack, launched);
#if RTK_GNSS_ENABLED
    if (ready) {
        ready = start_io(bufs.io_stack, launched);
    }
#endif
#if FW_TRACE_ENABLED
    if (s_app.debug.task_running()) {
        launched |= 4U;
    }
#endif
    if (ready && wait_started(launched) &&
        (s_app.boot_phase.load(std::memory_order_acquire) == app::BootPhase::kStarting)) {
        s_app.boot_phase.store(app::BootPhase::kRunning, std::memory_order_release);
        return;
    }
    stop_boot_tasks(launched);
}
#endif // ESP_PLATFORM
