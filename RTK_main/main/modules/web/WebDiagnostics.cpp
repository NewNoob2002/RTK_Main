// WebSocket 按需读取 ESP-IDF 堆、启动台账与 Wi-Fi 运行任务的栈。
// 仅 kLink 任务，非 ISR，无动态分配；仅 diag 请求扫描完整台账或任务余栈。
#include "modules/web/WebDiagnostics.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/freertos_debug.h"
#include "freertos/task.h"

#include "modules/web/WebLogic.hpp"
#include "rtos/BootAlloc.hpp"
#include "web_proto/Writer.hpp"

namespace app::web_cfg {
namespace {

// Wi-Fi 稳态下这些应用和 SDK 任务从启动持续到 kLink 停稳后才销毁。
// 不保留任务句柄，也不枚举可能在另一核被销毁的临时任务（如启动任务 main）。
constexpr std::array kWifiTasks{
    "link",    "io",        "trace_out", "oled",         "httpd", "wifi",  "tiT",  "tcpip",
    "sys_evt", "esp_timer", "Tmr Svc",   "console_repl", "IDLE0", "IDLE1", "ipc0", "ipc1"};

/** @brief 写入无符号堆或栈值。 @param bytes: 字节数或块数。 @retval 钳制到协议 int32 上限的值。 */
int32_t diagNumber(size_t bytes) noexcept
{
    // 钳制到 int32_t 的非负范围后转换安全。
    return static_cast<int32_t>(
        std::min(bytes, static_cast<size_t>(std::numeric_limits<int32_t>::max())));
}

/**
 * @brief 写入一种能力堆的统计；两种能力口径可重叠。
 * @param writer: 已打开的诊断帧。
 * @param name: 能力堆组名。
 * @param caps: heap_caps 分配能力掩码。
 * @retval true 写入成功。
 * @retval false 帧缓冲不足。
 */
bool writeHeapInfo(web::JsonWriter &writer, std::string_view name, uint32_t caps) noexcept
{
    multi_heap_info_t info{};
    heap_caps_get_info(&info, caps);
    return writer.beginNested(name) && writer.writeInt("free", diagNumber(info.total_free_bytes)) &&
           writer.writeInt("allocated", diagNumber(info.total_allocated_bytes)) &&
           writer.writeInt("min", diagNumber(info.minimum_free_bytes)) &&
           writer.writeInt("largest", diagNumber(info.largest_free_block)) &&
           writer.writeInt("free_blocks", diagNumber(info.free_blocks)) &&
           writer.writeInt("allocated_blocks", diagNumber(info.allocated_blocks)) &&
           writer.writeInt("total_blocks", diagNumber(info.total_blocks)) && writer.endNested();
}

/**
 * @brief 写入已封存的台账及其各分区的请求量和实际块大小。
 * @param writer: 已打开的诊断帧。
 * @param arena: 已封存的 BootArena。
 * @retval true 写入成功。
 * @retval false 帧缓冲不足。
 */
bool writeArenaInfo(web::JsonWriter &writer, const rtos::BootArena &arena) noexcept
{
    if (!writer.beginNested("arena") || !writer.writeInt("req", diagNumber(arena.totalBytes())) ||
        !writer.writeInt("actual", diagNumber(arena.totalActualBytes()))) {
        return false;
    }
    for (const auto &entry : arena.entries()) {
        if (!writer.beginNested(entry.tag) || !writer.writeInt("req", diagNumber(entry.bytes)) ||
            !writer.writeInt("actual", diagNumber(entry.actualBytes)) || !writer.endNested()) {
            return false;
        }
    }
    return writer.endNested();
}

/**
 * @brief 写入当前存在的常驻任务，包括栈字节数和历史最小余栈。
 * @param writer: 已打开的诊断帧。
 * @retval true 写入成功。
 * @retval false 帧缓冲不足或栈范围无效。
 */
bool writeTaskInfo(web::JsonWriter &writer) noexcept
{
    if (!writer.writeInt("task_count", diagNumber(uxTaskGetNumberOfTasks())) ||
        !writer.beginNested("tasks")) {
        return false;
    }
    for (const char *name : kWifiTasks) {
        const TaskHandle_t handle = xTaskGetHandle(name);
        if (handle == nullptr) {
            continue;
        }
        TaskSnapshot_t snapshot{};
        // IDF 的单任务快照要求暂停本核调度；表内句柄的寿命由 Wi-Fi 启停顺序保证。
        vTaskSuspendAll();
        const BaseType_t captured = vTaskGetSnapshot(handle, &snapshot);
        static_cast<void>(xTaskResumeAll());
        const StackType_t *base = xTaskGetStackStart(handle);
        if (captured != pdTRUE || base == nullptr || snapshot.pxEndOfStack == nullptr) {
            return false;
        }
        // 两个指针属于同一任务栈；IDF 栈顶经对齐，口径为可用范围（含末元素）。
        const auto depth = snapshot.pxEndOfStack - base + 1;
        if (depth <= 0 || !writer.beginNested(name) ||
            !writer.writeInt("stack",
                             diagNumber(static_cast<size_t>(depth) * sizeof(StackType_t))) ||
            !writer.writeInt("hwm", diagNumber(uxTaskGetStackHighWaterMark(handle))) ||
            !writer.endNested()) {
            return false;
        }
    }
    return writer.endNested();
}

} // namespace

void sampleHeapStatus(DevWebStatus &dev) noexcept
{
    dev.heapLargest = diagNumber(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    dev.heapIntFree = diagNumber(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    dev.heapIntMin = diagNumber(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

size_t buildDiagFrame(uint16_t id, std::string_view name, const rtos::BootArena &arena,
                      std::span<char> out) noexcept
{
    web::JsonWriter writer{};
    if (!writer.beginObject() || !writer.writeString("t", name) || !writer.writeInt("id", id)) {
        return 0U;
    }
    const bool ok = name == "diag.tasks"
                        ? writeTaskInfo(writer)
                        : writeHeapInfo(writer, "heap_8bit", MALLOC_CAP_8BIT) &&
                              writeHeapInfo(writer, "heap_internal", MALLOC_CAP_INTERNAL) &&
                              writeArenaInfo(writer, arena);
    return (ok && writer.endObject()) ? writer.finish(out) : 0U;
}

} // namespace app::web_cfg
