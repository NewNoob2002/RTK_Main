// 执行器装配（R8a，自 AppMain.cpp 迁入）：静态模块表、链路/IO 执行器配置与消费者启停。
// 线程：start_link/start_io/wait_started/stop_boot_tasks 只在启动任务调用，可阻塞；
//       kModules 与配置表为静态存储，启动完成后只读。不分配堆。
#include "app/App.hpp"
#include "app/BootReasons.hpp"

#include <cstdint>
#include <span>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "SppServerStorage.h"

namespace {

template <typename T> constexpr auto kModuleOps = fw::makeOps<T>();
// 编号由 id() 保证无窄化；对象与操作表同为静态存储。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, readability-identifier-naming)
constinit const fw::Module kModules[]{
// X-macro 生成异构对象的注册项，constexpr 函数无法替代清单展开。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MODULE_DEF(type, member, name, kind, executor, period)                                     \
    {name,                                                                                         \
     app::id(app::ModuleKind::kind),                                                               \
     app::id(app::ExecKind::executor),                                                             \
     period,                                                                                       \
     &app::s_app.member,                                                                           \
     &kModuleOps<app::type>},
#include "module_list.inc"
#undef MODULE_DEF
};

constinit rtos::Executor s_link_executor{};
#if RTK_GNSS_ENABLED
constinit rtos::Executor s_io_executor{};
#if defined(CONFIG_ESP_CONSOLE_UART_NUM)
static_assert(CONFIG_RTK_GNSS_UART_NUM != CONFIG_ESP_CONSOLE_UART_NUM,
              "GNSS UART must not conflict with console UART");
#endif
// IO 执行器绑 core0（prio 9，低于 link 的 prio 10），不抢占蓝牙链路实时性；阻塞于 GNSS UART 读。
constexpr rtos::ExecutorConfig kIoConfig{
    .name = "io",
    .id = app::id(app::ExecKind::kIo),
    .priority = 9,
    .core = 0,
    .idleWaitMs = 20,
    .roundBudgetMs = 20,
    .yieldTicks = 1,
    .watchdog = true,
    .wait =
        [](void * /*ctx*/, uint32_t max_ms) noexcept {
            if (!app::s_app.await_boot(app::BootTask::kIo, true)) {
                vTaskDelete(nullptr);
                return;
            }
            app::s_app.gnss.wait(max_ms);
        },
    .waitCtx = nullptr,
};
#endif // RTK_GNSS_ENABLED
// 链路绑 core1，BT worker/控制台在 core0；每轮至少让出 1 tick 给 IDLE1。
constexpr rtos::ExecutorConfig kLinkConfig{
    .name = "link",
    .id = app::id(app::ExecKind::kLink),
    .priority = 10,
    .core = 1,
    .idleWaitMs = 20,
    .roundBudgetMs = 20,
    .yieldTicks = 1,
    .watchdog = true,
    .wait =
        [](void * /*ctx*/, uint32_t max_ms) noexcept {
            if (!app::s_app.await_boot(app::BootTask::kLink, true)) {
                vTaskDelete(nullptr);
                return;
            }
            app::s_app.bt_link.wait(max_ms);
        },
    .waitCtx = nullptr,
};

bool modules_ready(const rtos::Executor &executor, fw::ExecutorId id) noexcept
{
    for (const auto &module : kModules) {
        if ((module.executor == id) && !executor.moduleOk(module.id)) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace app {

void bind_executors() noexcept
{
    s_app.link_executor = &s_link_executor;
#if RTK_GNSS_ENABLED
    s_app.io_executor = &s_io_executor;
#endif
}

bool wait_started(uint32_t bits) noexcept
{
    const TickType_t begin = xTaskGetTickCount();
    while ((s_app.boot_arrived.load(std::memory_order_acquire) & bits) != bits) {
        if ((xTaskGetTickCount() - begin) >= pdMS_TO_TICKS(5000U)) {
            return false;
        }
        vTaskDelay(1U);
    }
    return true;
}

const char *start_link(std::span<StackType_t> link_stack, uint32_t &launched) noexcept
{
    const auto initialized = s_app.link_executor->init(
        kLinkConfig, kModules, app::view_of(s_app.link_storage, link_stack));
    if (!fw::isOk(initialized) || !modules_ready(*s_app.link_executor, kLinkConfig.id) ||
        !s_app.data.system_status.read().bt_ready) {
        return boot_reason::kLinkInit;
    }
    if (!fw::isOk(s_app.link_executor->start())) {
        return boot_reason::kLinkTaskStart;
    }
    launched |= task_bit(BootTask::kLink);
    if (!wait_started(1U) || !modules_ready(*s_app.link_executor, kLinkConfig.id)) {
        return boot_reason::kLinkModuleStart;
    }
    return nullptr;
}

#if RTK_GNSS_ENABLED
const char *start_io(std::span<StackType_t> io_stack, uint32_t &launched) noexcept
{
    const auto initialized =
        s_app.io_executor->init(kIoConfig, kModules, app::view_of(s_app.io_storage, io_stack));
    if (!fw::isOk(initialized) || !modules_ready(*s_app.io_executor, kIoConfig.id)) {
        return boot_reason::kIoInit;
    }
    if (!fw::isOk(s_app.io_executor->start())) {
        return boot_reason::kIoTaskStart;
    }
    launched |= task_bit(BootTask::kIo);
    if (!wait_started(2U) || !modules_ready(*s_app.io_executor, kIoConfig.id)) {
        return boot_reason::kIoModuleStart;
    }
    return nullptr;
}
#endif // RTK_GNSS_ENABLED

void stop_boot_tasks(uint32_t launched) noexcept
{
    s_app.boot_phase.store(app::BootPhase::kStopping, std::memory_order_release);
    const TickType_t begin = xTaskGetTickCount();
    while ((s_app.boot_stopped.load(std::memory_order_acquire) & launched) != launched) {
        if ((xTaskGetTickCount() - begin) >= pdMS_TO_TICKS(5000U)) {
            s_app.boot_arena.setDegradedReason(boot_reason::kStopTaskTimeout);
            // 消费者未确认停稳：不撤销驱动、回调或任何封存块，诊断控制台仍可用。
            set_console_only();
            return;
        }
        vTaskDelay(1U);
    }
#if RTK_GNSS_ENABLED
    if (!fw::isOk(s_app.gnss.stop_boot())) {
        s_app.boot_arena.setDegradedReason(boot_reason::kStopGnss);
    }
#endif
    if (spp_server_stop_boot() != ESP_OK) {
        s_app.boot_arena.setDegradedReason(boot_reason::kStopSpp);
    }
    // SEALED 块始终保留至重启；SDK 停机失败或迟到回调也不回收其存储。
    set_console_only();
}

} // namespace app
