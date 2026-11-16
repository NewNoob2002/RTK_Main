// 启动期外供缓冲计划与绑定接口（R8a）：BootArena owner 名称、顺序、对齐与容量保持 K5 语义不变。
// 线程：仅启动任务调用，绑定/回滚须在消费者任务启动前完成，非 ISR。
#pragma once

#include <cstdint>
#include <span>

#include "app_config.hpp" // RTK_GNSS_ENABLED；并带入 StackType_t（经 rtos/executor.hpp）
#include "fw/trace.hpp"   // FW_TRACE_ENABLED、fw::TraceEvent
#include "rtos/boot_alloc.hpp"

namespace app {

/** 启动期从 BootArena 申请的栈与数据通路视图；全空表示尚未分配或已回收。 */
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

/**
 * 打开 BootArena 并按固定顺序申请全部块；任一失败清空 out、置 boot_alloc:<owner> 原因并 abort 释放。
 * ops 为 nullptr 时用默认内部 8-bit 分配器；host 测试注入替身。返回 false 时不得再绑定。
 */
[[nodiscard]] bool allocate_boot_buffers(BootBuffers &out, const rtos::BootAllocatorOps *ops = nullptr) noexcept;

/** 把已申请的块绑定到各消费者；成功返回 nullptr，失败返回 bind:debug/bind:spp/bind:gnss。 */
[[nodiscard]] const char *bind_boot_buffers(const BootBuffers &bufs) noexcept;

/** 撤销绑定并回收全部块（仅在消费者尚未启动时调用）；SPP worker 已存在则保留全部块。 */
void rollback_boot_bindings(BootBuffers &bufs, const char *reason) noexcept;

} // namespace app
