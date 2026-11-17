// 启动期外供缓冲的分配、绑定与回滚实现（R8a，自 AppMain.cpp 原样迁入）：owner 顺序、对齐与容量按 K5。
// 线程：仅启动任务调用一次，非 ISR；分配走 BootArena 的内部 8-bit 堆，失败路径必须释放已申请块。
#include "app/BootBuffers.hpp"
#include "app/BootReasons.hpp"

#include <array>
#include <cstdio>

#include "fw/Log.hpp"

#include "AppContext.hpp"
#if FW_TRACE_ENABLED
// debug 模块只在开启跟踪时编译，且 kLogRingBytes 依赖 RTK_LOG_RING_BYTES（随 RTK_TRACE_ENABLE 裁剪）。
#include "modules/debug/DebugModule.hpp"
#endif
#if RTK_GNSS_ENABLED
#include "modules/gnss/GnssModule.hpp"
#endif
#include "SppServerStorage.h"

namespace app {

namespace {

/** 组合根单例；本文件只在启动期访问，避免把 s_app 暴露到其他翻译单元。 */
AppContext &self() noexcept
{
    return appContext();
}

} // namespace

/**
 * @brief  从启动分配器一次性申请各模块的运行缓冲
 * @param[out] out: 申请到的缓冲
 * @param  ops: 分配器操作表（host 测试可注入）
 * @retval true 全部成功；false 任一失败（已回滚并记录原因）
 * @note 不拆（S4 §2.1）：启动期线性分配台账，每项只有名字与对齐差异，顺序即布局。
 */
bool allocateBootBuffers(BootBuffers &out, // NOLINT(readability-function-size)
                         const rtos::BootAllocatorOps *ops) noexcept
{
    if (!fw::isOk(self().bootArena.open(ops))) {
        (void)self().bootArena.abort(boot_reason::kAllocOpen);
        return false;
    }
    const auto failAlloc = [&out](const char *name) noexcept -> bool {
        out = {};
        std::array<char, rtos::BootArena::kMaxReasonBytes> reason{};
        (void)std::snprintf(reason.data(), reason.size(), "boot_alloc:%s", name);
        (void)self().bootArena.abort(reason.data());
        FW_LOGE(fw::TraceCat::kSys, "boot_alloc failed (%s); entering console-only mode", name);
        return false;
    };

    constexpr size_t kStackAlign = 16U;
    constexpr size_t kBufAlign = 4U;

    if (!fw::isOk(self().bootArena.allocArray<StackType_t>(
            "task.link", AppContext::kLinkStackBytes / sizeof(StackType_t), out.linkStack,
            kStackAlign))) {
        return failAlloc("task.link");
    }
#if RTK_GNSS_ENABLED
    if (!fw::isOk(self().bootArena.allocArray<StackType_t>(
            "task.io", AppContext::kIoStackBytes / sizeof(StackType_t), out.ioStack,
            kStackAlign))) {
        return failAlloc("task.io");
    }
#endif
    if (!fw::isOk(self().bootArena.allocArray<StackType_t>(
            "task.spp_worker", SPP_SERVER_WORKER_STACK_BYTES / sizeof(StackType_t),
            out.sppWorkerStack, kStackAlign))) {
        return failAlloc("task.spp_worker");
    }
#if FW_TRACE_ENABLED
    if (!fw::isOk(self().bootArena.allocArray<StackType_t>(
            "task.trace_out", DebugModule::kTaskStackBytes / sizeof(StackType_t),
            out.traceTaskStack, kStackAlign))) {
        return failAlloc("task.trace_out");
    }
#endif
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("spp.rx.0", SPP_SERVER_RX_BUF_SIZE,
                                                       out.sppRx0, kBufAlign))) {
        return failAlloc("spp.rx.0");
    }
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("spp.rx.1", SPP_SERVER_RX_BUF_SIZE,
                                                       out.sppRx1, kBufAlign))) {
        return failAlloc("spp.rx.1");
    }
#if FW_TRACE_ENABLED
    if (!fw::isOk(self().bootArena.allocArray<fw::TraceEvent>(
            "trace.ring", DebugModule::kRingEvents, out.traceRing, alignof(fw::TraceEvent)))) {
        return failAlloc("trace.ring");
    }
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("log.ring", DebugModule::kLogRingBytes,
                                                       out.logRing, alignof(fw::LogHeader)))) {
        return failAlloc("log.ring");
    }
#endif
#if RTK_GNSS_ENABLED
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("gnss.demux", GnssModule::kDemuxBufBytes,
                                                       out.gnssDemux, kBufAlign))) {
        return failAlloc("gnss.demux");
    }
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("rtcm.pipe", kRtcmPipeBytes, out.rtcmPipe,
                                                       kBufAlign))) {
        return failAlloc("rtcm.pipe");
    }
#endif
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("spp.tx.reply.0", SPP_SERVER_REPLY_CAPACITY,
                                                       out.sppReply0, kBufAlign))) {
        return failAlloc("spp.tx.reply.0");
    }
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("spp.tx.reply.1", SPP_SERVER_REPLY_CAPACITY,
                                                       out.sppReply1, kBufAlign))) {
        return failAlloc("spp.tx.reply.1");
    }
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>(
            "spp.tx.stream.phone", SPP_SERVER_STREAM_CAPACITY, out.sppStream, kBufAlign))) {
        return failAlloc("spp.tx.stream.phone");
    }
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("spp.tx.merge", SPP_SERVER_MERGE_CAPACITY,
                                                       out.sppMerge, kBufAlign))) {
        return failAlloc("spp.tx.merge");
    }
#if RTK_GNSS_ENABLED
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("gnss.app_ring", kGnssAppRingBytes,
                                                       out.gnssAppRing, kBufAlign))) {
        return failAlloc("gnss.app_ring");
    }
    if (!fw::isOk(self().bootArena.allocArray<uint8_t>("gnss.latest", data::kGnssLatestTotalBytes,
                                                       out.gnssLatest, kBufAlign))) {
        return failAlloc("gnss.latest");
    }
#endif
    return true;
}

const char *bindBootBuffers(const BootBuffers &bufs) noexcept
{
#if FW_TRACE_ENABLED
    if (!fw::isOk(self().debug.bindStorage({bufs.traceRing, bufs.logRing, bufs.traceTaskStack}))) {
        return boot_reason::kBindDebug;
    }
#endif
    const spp_server_storage_t storage{
        .rx_buf = {bufs.sppRx0.data(), bufs.sppRx1.data()},
        .rx_buf_bytes = {bufs.sppRx0.size(), bufs.sppRx1.size()},
        .reply_buf = {bufs.sppReply0.data(), bufs.sppReply1.data()},
        .reply_buf_bytes = {bufs.sppReply0.size(), bufs.sppReply1.size()},
        .stream_buf = bufs.sppStream.data(),
        .stream_buf_bytes = bufs.sppStream.size(),
        .merge_buf = bufs.sppMerge.data(),
        .merge_buf_bytes = bufs.sppMerge.size(),
        .worker_stack = bufs.sppWorkerStack.data(),
        .worker_stack_bytes = bufs.sppWorkerStack.size_bytes(),
    };
    if (spp_server_bind_storage(&storage) != ESP_OK) {
        return boot_reason::kBindSpp;
    }
#if RTK_GNSS_ENABLED
    if (!fw::isOk(self().data.initGnssBuffers(bufs.gnssAppRing, bufs.gnssLatest)) ||
        !fw::isOk(self().rtcmPipe.bindStorage(bufs.rtcmPipe)) ||
        !fw::isOk(self().gnss.bindStorage(bufs.gnssDemux))) {
        return boot_reason::kBindGnss;
    }
#endif
    return nullptr;
}

void rollbackBootBindings(BootBuffers &bufs, const char *reason) noexcept
{
#if FW_TRACE_ENABLED
    self().debug.unbindStorage();
#endif
#if RTK_GNSS_ENABLED
    self().gnss.unbindStorage();
    self().rtcmPipe.unbindStorage();
    self().data.unbindGnssBuffers();
#endif
    const esp_err_t unbound = spp_server_unbind_storage();
    bufs = {};
    if (unbound != ESP_OK) {
        // 若约束被破坏、C worker 已存在，保留全部块，绝不释放它可能使用的栈/缓冲。
        self().bootArena.setDegradedReason(boot_reason::kRollbackSppInUse);
        return;
    }
    if (!fw::isOk(self().bootArena.abort(reason))) {
        self().bootArena.setDegradedReason(reason);
    }
}

} // namespace app
