// 应用组合根单例与启动屏障实现（R8a）：AppContext 全零静态存储，await_boot 供执行器任务等待放行。
// 线程：app_context() 任意任务可调用；await_boot 只在执行器/trace 任务内调用，可阻塞，非 ISR。
#include "AppContext.hpp"

#include <cstdint>

#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app/App.hpp"

namespace {

/** 看门狗超时判定用的高位标记：boot_stopped 中仅 bit0/1/2 表示执行器/追踪任务。 */
constexpr uint32_t kWatchdogStopBit = 0x80000000U;

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
            boot_stopped.fetch_or(kWatchdogStopBit, std::memory_order_release);
        }
    }
    boot_stopped.fetch_or(task_bit, std::memory_order_release);
    return false;
}

} // namespace app
