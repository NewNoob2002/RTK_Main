// 启动期外供缓冲计划与绑定接口（R8a）：BootArena owner 名称、顺序、对齐与容量保持 K5 语义不变。
// 线程：仅启动任务调用，绑定/回滚须在消费者任务启动前完成，非 ISR。
#pragma once

#include <cstdint>
#include <span>

#include "AppConfig.hpp" // RTK_GNSS_ENABLED；并带入 StackType_t（经 rtos/Executor.hpp）
#include "fw/Trace.hpp"  // FW_TRACE_ENABLED、fw::TraceEvent
#include "rtos/BootAlloc.hpp"

namespace app {

/** 启动期从 BootArena 申请的栈与数据通路视图；全空表示尚未分配或已回收。 */
struct BootBuffers {
    std::span<StackType_t> linkStack{};
#if RTK_GNSS_ENABLED
    std::span<StackType_t> ioStack{};
    std::span<uint8_t> gnssDemux{};
    std::span<uint8_t> rtcmPipe{};
    std::span<uint8_t> gnssAppRing{};
    std::span<uint8_t> gnssLatest{};
#endif
    std::span<StackType_t> sppWorkerStack{};
    std::span<uint8_t> sppRx0{};
    std::span<uint8_t> sppRx1{};
    std::span<uint8_t> sppReply0{};
    std::span<uint8_t> sppReply1{};
    std::span<uint8_t> sppStream{};
    std::span<uint8_t> sppMerge{};
#if FW_TRACE_ENABLED
    std::span<StackType_t> traceTaskStack{};
    std::span<fw::TraceEvent> traceRing{};
    std::span<uint8_t> logRing{};
#endif
};

/**
 * 打开 BootArena 并按固定顺序申请全部块；任一失败清空 out、置 boot_alloc:<owner> 原因并 abort 释放。
 * ops 为 nullptr 时用默认内部 8-bit 分配器；host 测试注入替身。返回 false 时不得再绑定。
 */
[[nodiscard]] bool allocateBootBuffers(BootBuffers &out,
                                       const rtos::BootAllocatorOps *ops = nullptr) noexcept;

/** 把已申请的块绑定到各消费者；成功返回 nullptr，失败返回 bind:debug/bind:spp/bind:gnss。 */
[[nodiscard]] const char *bindBootBuffers(const BootBuffers &bufs) noexcept;

/** 撤销绑定并回收全部块（仅在消费者尚未启动时调用）；SPP worker 已存在则保留全部块。 */
void rollbackBootBindings(BootBuffers &bufs, const char *reason) noexcept;

} // namespace app
