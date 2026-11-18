// 启动期外供缓冲计划与绑定接口（ADR-0009）：BootArena owner 名称、顺序、对齐与容量遵守 ADR-0011 §7。
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
 * @brief  从启动分配器一次性申请各模块的运行缓冲
 * @param[out] out: 申请到的缓冲
 * @param  ops: 分配器操作表（host 测试可注入）
 * @retval true 全部成功；false 任一失败（已回滚并记录原因）
 * @note 不拆（ADR-0013 §2.4）：启动期线性分配台账，每项只有名字与对齐差异，顺序即布局。
 */
[[nodiscard]] bool allocateBootBuffers(BootBuffers &out,
                                       const rtos::BootAllocatorOps *ops = nullptr) noexcept;

/**
 * @brief 把已申请的块绑定到各消费者；成功返回 nullptr，失败返回 bind:debug/bind:spp/bind:gnss。
 * @param bufs: 已分配且尚未封存的存储视图
 * @retval nullptr 所有消费者绑定成功
 * @retval 非空 debug、SPP 或 GNSS 绑定失败原因
 * @note 仅启动前单线程；失败可能已有部分绑定，须调用回滚；非 ISR。
 */
[[nodiscard]] const char *bindBootBuffers(const BootBuffers &bufs) noexcept;

/**
 * @brief 撤销绑定并回收全部块（仅在消费者尚未启动时调用）；SPP worker 已存在则保留全部块。
 * @param bufs: 待撤销的视图；返回时清空
 * @param reason: 诊断原因，须活过使用方
 * @note 仅消费者未启动时调用；SPP 拒绝解绑时保留全部块，非 ISR。
 */
void rollbackBootBindings(BootBuffers &bufs, const char *reason) noexcept;

} // namespace app
