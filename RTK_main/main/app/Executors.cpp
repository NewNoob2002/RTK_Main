// 执行器装配（ADR-0009）：静态模块表、链路/IO 执行器配置与消费者启停。
// 线程：startLink/startIo/waitStarted/stopBootTasks 只在启动任务调用，可阻塞；
//       模块表与配置表为静态只读存储。不分配堆。
#include "app/App.hpp"
#include "app/BootReasons.hpp"
#include "app/NetBlock.hpp"
#include "app/SettingsBackend.hpp"
#include "board/BoardProfile.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "SppServerStorage.h"

namespace {

template <typename T> constexpr auto kModuleOps = fw::makeOps<T>();

/**
 * @brief 按启动模式生成执行器的静态模块表
 * @retval 仅包含已启用模块的定长数组
 * @note 仅编译期调用。
 */
template <app::board::BootMode Boot, app::RadioMode Mode> consteval auto makeModules() noexcept
{
    constexpr std::array kEnabled{
#define MODULE_DEF(type, member, name, kind, executor, period, enable)                             \
    app::moduleEnabled(Boot, Mode, app::ModuleEnable::enable),
#include "module_list.inc"
#undef MODULE_DEF
    };
    // count 的结果不超过静态模块表长度，转为无符号安全。
    std::array<fw::Module, static_cast<size_t>(std::ranges::count(kEnabled, true))> modules{};
    size_t index = 0U;
// X-macro 生成异构对象的注册项，constexpr 函数无法替代清单展开。
#define MODULE_DEF(type, member, name, kind, executor, period, enable)                             \
    if constexpr (app::moduleEnabled(Boot, Mode, app::ModuleEnable::enable)) {                     \
        modules[index++] = {name,                                                                  \
                            app::id(app::ModuleKind::kind),                                        \
                            app::id(app::ExecKind::executor),                                      \
                            period,                                                                \
                            &app::s_app.member,                                                    \
                            &kModuleOps<app::type>};                                               \
    }
#include "module_list.inc"
#undef MODULE_DEF
    return modules;
}

constexpr auto kBtModules =
    makeModules<app::board::BootMode::kNormal, app::RadioMode::kBluetooth>();
constexpr auto kWifiModules =
    makeModules<app::board::BootMode::kNormal, app::RadioMode::kWifiConfig>();
constexpr auto kChargeStandbyModules =
    makeModules<app::board::BootMode::kChargeStandby, app::RadioMode::kBluetooth>();
// Executor::init 以整张模块表校验 kExecMaxModules，超限时所有执行器 init 失败（N1 HIL：BT 模式 9 个模块
// 导致 link_init 失败、只剩控制台）。在编译期拦截，避免主机测试使用不同模块表时漏检。
static_assert(kBtModules.size() <= rtos::kExecMaxModules, "BT 模块表超过 kExecMaxModules");
static_assert(kWifiModules.size() <= rtos::kExecMaxModules, "Wi-Fi 模块表超过 kExecMaxModules");
static_assert(kChargeStandbyModules.size() <= rtos::kExecMaxModules,
              "充电待机模块表超过 kExecMaxModules");

/**
 * @brief 取得本次启动交给执行器的模块表
 * @retval 固件全生命周期有效的静态表视图
 * @note 仅启动任务在模式选定后调用，非 ISR，不阻塞。
 */
std::span<const fw::Module> activeModules() noexcept
{
    if (app::s_app.bootMode == app::board::BootMode::kChargeStandby) {
        return std::span<const fw::Module>{kChargeStandbyModules};
    }
    return app::s_app.radioMode == app::RadioMode::kBluetooth
               ? std::span<const fw::Module>{kBtModules}
               : std::span<const fw::Module>{kWifiModules};
}

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
        [](void * /*ctx*/, uint32_t maxMs) noexcept {
            if (!app::s_app.awaitBoot(app::BootTask::kIo, true)) {
                vTaskDelete(nullptr);
                return;
            }
            app::s_app.gnss.wait(maxMs);
        },
    .waitCtx = nullptr,
};
#endif // RTK_GNSS_ENABLED
// kNet 绑 core0、prio 8（低于 kIo 9 与 kLink 10）；阻塞在 UART1 事件队列，由 NetModule::wait 计算超时（ADR-0021 §2.2）。
constexpr rtos::ExecutorConfig kNetConfig{
    .name = "net",
    .id = app::id(app::ExecKind::kNet),
    .priority = 8,
    .core = 0,
    .idleWaitMs = 50,
    .roundBudgetMs = 20,
    .yieldTicks = 1,
    .watchdog = true,
    .wait =
        [](void * /*ctx*/, uint32_t maxMs) noexcept {
            if (!app::s_app.awaitBoot(app::BootTask::kNet, true)) {
                vTaskDelete(nullptr);
                return;
            }
            app::s_app.net.wait(maxMs);
        },
    .waitCtx = nullptr,
};
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
        [](void * /*ctx*/, uint32_t maxMs) noexcept {
            if (!app::s_app.awaitBoot(app::BootTask::kLink, true)) {
                vTaskDelete(nullptr);
                return;
            }
            if ((app::s_app.bootMode == app::board::BootMode::kNormal) &&
                (app::s_app.radioMode == app::RadioMode::kBluetooth)) {
                app::s_app.bt_link.wait(maxMs);
            } else if (maxMs != 0U) {
                // 没有 SPP 可唤醒执行器，按执行器给出的空闲等待间隔处理命令、按键与存储工作。
                const TickType_t ticks = pdMS_TO_TICKS(maxMs);
                vTaskDelay(ticks == 0U ? 1U : ticks);
            }
        },
    .waitCtx = nullptr,
};

/**
 * @brief 检查指定执行器的全部模块是否初始化成功
 * @param executor: 被检查的执行器
 * @param id: 执行器编号
 * @retval true 全部就绪或没有匹配模块
 * @retval false 至少一个模块未就绪
 * @note 仅启动任务读取，非 ISR，不阻塞。
 */
bool modulesReady(const rtos::Executor &executor, fw::ExecutorId id) noexcept
{
    for (const auto &module : activeModules()) {
        if ((module.executor == id) && !executor.moduleOk(module.id)) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace app {

void bindExecutors() noexcept
{
    s_app.linkExecutor = &s_link_executor;
    bindSettings(s_app);
#if RTK_GNSS_ENABLED
    s_app.ioExecutor = &s_io_executor;
#endif
}

bool waitStarted(uint32_t bits) noexcept
{
    const TickType_t begin = xTaskGetTickCount();
    while ((s_app.bootArrived.load(std::memory_order_acquire) & bits) != bits) {
        if ((xTaskGetTickCount() - begin) >= pdMS_TO_TICKS(5000U)) {
            return false;
        }
        vTaskDelay(1U);
    }
    return true;
}

const char *startLink(std::span<StackType_t> linkStack, uint32_t &launched) noexcept
{
    auto config = kLinkConfig;
    config.idleWaitMs = board::linkIdleWaitMs(board::current().powerHold);
    const auto initialized = s_app.linkExecutor->init(config, activeModules(),
                                                      app::viewOf(s_app.linkStorage, linkStack));
    const bool btMode =
        (s_app.bootMode == board::BootMode::kNormal) && (s_app.radioMode == RadioMode::kBluetooth);
    if (!fw::isOk(initialized) || !modulesReady(*s_app.linkExecutor, kLinkConfig.id) ||
        (btMode && !s_app.data.systemStatus.read().btReady)) {
        return boot_reason::kLinkInit;
    }
    if (!fw::isOk(s_app.linkExecutor->start())) {
        return boot_reason::kLinkTaskStart;
    }
    launched |= taskBit(BootTask::kLink);
    if (!waitStarted(1U) || !modulesReady(*s_app.linkExecutor, kLinkConfig.id)) {
        return boot_reason::kLinkModuleStart;
    }
    return nullptr;
}

#if RTK_GNSS_ENABLED
const char *startIo(std::span<StackType_t> ioStack, uint32_t &launched) noexcept
{
    const auto initialized =
        s_app.ioExecutor->init(kIoConfig, activeModules(), app::viewOf(s_app.ioStorage, ioStack));
    if (!fw::isOk(initialized) || !modulesReady(*s_app.ioExecutor, kIoConfig.id)) {
        return boot_reason::kIoInit;
    }
    if (!fw::isOk(s_app.ioExecutor->start())) {
        return boot_reason::kIoTaskStart;
    }
    launched |= taskBit(BootTask::kIo);
    if (!waitStarted(2U) || !modulesReady(*s_app.ioExecutor, kIoConfig.id)) {
        return boot_reason::kIoModuleStart;
    }
    return nullptr;
}
#endif // RTK_GNSS_ENABLED

const char *startNet(std::span<StackType_t> netStack, std::span<uint8_t> netBlockMem,
                     uint32_t &launched) noexcept
{
    NetBlock *block = constructNetBlock(netBlockMem);
    if (block == nullptr || netStack.empty()) {
        return boot_reason::kNetInit;
    }
    s_app.netBlock = block;
    s_app.netExecutor = &block->executor;
    s_app.net.bindRuntime(block->runtime);
    const auto initialized =
        s_app.netExecutor->init(kNetConfig, activeModules(), app::viewOf(block->storage, netStack));
    if (!fw::isOk(initialized) || !modulesReady(*s_app.netExecutor, kNetConfig.id)) {
        return boot_reason::kNetInit;
    }
    if (!fw::isOk(s_app.net.registerShutdown())) {
        return boot_reason::kNetShutdownReg;
    }
    if (!fw::isOk(s_app.netExecutor->start())) {
        return boot_reason::kNetTaskStart;
    }
    launched |= taskBit(BootTask::kNet);
    if (!waitStarted(taskBit(BootTask::kNet)) || !modulesReady(*s_app.netExecutor, kNetConfig.id)) {
        return boot_reason::kNetModuleStart;
    }
    return nullptr;
}

void stopBootTasks(uint32_t launched) noexcept
{
    s_app.bootPhase.store(app::BootPhase::kStopping, std::memory_order_release);
    const TickType_t begin = xTaskGetTickCount();
    while ((s_app.bootStopped.load(std::memory_order_acquire) & launched) != launched) {
        if ((xTaskGetTickCount() - begin) >= pdMS_TO_TICKS(5000U)) {
            s_app.bootArena.setDegradedReason(boot_reason::kStopTaskTimeout);
            // 消费者未确认停稳：不撤销驱动、回调或任何封存块，诊断控制台仍可用。
            setConsoleOnly();
            return;
        }
        vTaskDelay(1U);
    }
    const bool normal = s_app.bootMode == board::BootMode::kNormal;
#if RTK_GNSS_ENABLED
    if (normal && !fw::isOk(s_app.gnss.stopBoot())) {
        s_app.bootArena.setDegradedReason(boot_reason::kStopGnss);
    }
#endif
    if (normal && s_app.net.present() && !fw::isOk(s_app.net.stopBoot())) {
        s_app.bootArena.setDegradedReason(boot_reason::kStopNet);
    }
    if (normal && (s_app.radioMode == RadioMode::kBluetooth) &&
        (spp_server_stop_boot() != ESP_OK)) {
        s_app.bootArena.setDegradedReason(boot_reason::kStopSpp);
    }
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
    if (normal && (s_app.radioMode == RadioMode::kWifiConfig)) {
        s_app.web.stopBoot();
    }
#endif
    // SEALED 块始终保留至重启；SDK 停机失败或迟到回调也不回收其存储。
    setConsoleOnly();
}

} // namespace app
