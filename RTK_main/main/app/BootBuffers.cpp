// 启动期外供缓冲的分配、绑定与回滚实现（R8a，自 AppMain.cpp 原样迁入）：owner 顺序、对齐与容量按 K5。
// 线程：仅启动任务调用一次，非 ISR；分配走 BootArena 的内部 8-bit 堆，失败路径必须释放已申请块。
#include "app/BootBuffers.hpp"
#include "app/BootReasons.hpp"

#include <array>
#include <cstdio>

#include "esp_log.h"

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

constexpr const char *kTag = "app";

/** 组合根单例；本文件只在启动期访问，避免把 s_app 暴露到其他翻译单元。 */
AppContext &self() noexcept
{
    return app_context();
}

} // namespace

bool allocate_boot_buffers(BootBuffers &out, const rtos::BootAllocatorOps *ops) noexcept
{
    if (!fw::is_ok(self().boot_arena.open(ops))) {
        (void)self().boot_arena.abort(boot_reason::kAllocOpen);
        return false;
    }
    const auto fail_alloc = [&out](const char *name) noexcept -> bool {
        out = {};
        std::array<char, rtos::BootArena::kMaxReasonBytes> reason{};
        (void)std::snprintf(reason.data(), reason.size(), "boot_alloc:%s", name);
        (void)self().boot_arena.abort(reason.data());
        ESP_LOGE(kTag, "boot_alloc failed (%s); entering console-only mode", name);
        return false;
    };

    constexpr size_t kStackAlign = 16U;
    constexpr size_t kBufAlign = 4U;

    if (!fw::is_ok(self().boot_arena.alloc_array<StackType_t>(
            "task.link", AppContext::kLinkStackBytes / sizeof(StackType_t), out.link_stack, kStackAlign))) {
        return fail_alloc("task.link");
    }
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(self().boot_arena.alloc_array<StackType_t>(
            "task.io", AppContext::kIoStackBytes / sizeof(StackType_t), out.io_stack, kStackAlign))) {
        return fail_alloc("task.io");
    }
#endif
    if (!fw::is_ok(self().boot_arena.alloc_array<StackType_t>("task.spp_worker",
                                                              SPP_SERVER_WORKER_STACK_BYTES / sizeof(StackType_t),
                                                              out.spp_worker_stack, kStackAlign))) {
        return fail_alloc("task.spp_worker");
    }
#if FW_TRACE_ENABLED
    if (!fw::is_ok(self().boot_arena.alloc_array<StackType_t>(
            "task.trace_out", DebugModule::kTaskStackBytes / sizeof(StackType_t), out.trace_task_stack, kStackAlign))) {
        return fail_alloc("task.trace_out");
    }
#endif
    if (!fw::is_ok(
            self().boot_arena.alloc_array<uint8_t>("spp.rx.0", SPP_SERVER_RX_BUF_SIZE, out.spp_rx_0, kBufAlign))) {
        return fail_alloc("spp.rx.0");
    }
    if (!fw::is_ok(
            self().boot_arena.alloc_array<uint8_t>("spp.rx.1", SPP_SERVER_RX_BUF_SIZE, out.spp_rx_1, kBufAlign))) {
        return fail_alloc("spp.rx.1");
    }
#if FW_TRACE_ENABLED
    if (!fw::is_ok(self().boot_arena.alloc_array<fw::TraceEvent>("trace.ring", DebugModule::kRingEvents, out.trace_ring,
                                                                 alignof(fw::TraceEvent)))) {
        return fail_alloc("trace.ring");
    }
    if (!fw::is_ok(self().boot_arena.alloc_array<uint8_t>("log.ring", DebugModule::kLogRingBytes, out.log_ring,
                                                          alignof(fw::LogHeader)))) {
        return fail_alloc("log.ring");
    }
#endif
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(self().boot_arena.alloc_array<uint8_t>("gnss.demux", GnssModule::kDemuxBufBytes, out.gnss_demux,
                                                          kBufAlign))) {
        return fail_alloc("gnss.demux");
    }
    if (!fw::is_ok(self().boot_arena.alloc_array<uint8_t>("rtcm.pipe", kRtcmPipeBytes, out.rtcm_pipe, kBufAlign))) {
        return fail_alloc("rtcm.pipe");
    }
#endif
    if (!fw::is_ok(self().boot_arena.alloc_array<uint8_t>("spp.tx.reply.0", SPP_SERVER_REPLY_CAPACITY, out.spp_reply_0,
                                                          kBufAlign))) {
        return fail_alloc("spp.tx.reply.0");
    }
    if (!fw::is_ok(self().boot_arena.alloc_array<uint8_t>("spp.tx.reply.1", SPP_SERVER_REPLY_CAPACITY, out.spp_reply_1,
                                                          kBufAlign))) {
        return fail_alloc("spp.tx.reply.1");
    }
    if (!fw::is_ok(self().boot_arena.alloc_array<uint8_t>("spp.tx.stream.phone", SPP_SERVER_STREAM_CAPACITY,
                                                          out.spp_stream, kBufAlign))) {
        return fail_alloc("spp.tx.stream.phone");
    }
    if (!fw::is_ok(self().boot_arena.alloc_array<uint8_t>("spp.tx.merge", SPP_SERVER_MERGE_CAPACITY, out.spp_merge,
                                                          kBufAlign))) {
        return fail_alloc("spp.tx.merge");
    }
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(
            self().boot_arena.alloc_array<uint8_t>("gnss.app_ring", kGnssAppRingBytes, out.gnss_app_ring, kBufAlign))) {
        return fail_alloc("gnss.app_ring");
    }
    if (!fw::is_ok(self().boot_arena.alloc_array<uint8_t>("gnss.latest", data::kGnssLatestTotalBytes, out.gnss_latest,
                                                          kBufAlign))) {
        return fail_alloc("gnss.latest");
    }
#endif
    return true;
}

const char *bind_boot_buffers(const BootBuffers &bufs) noexcept
{
#if FW_TRACE_ENABLED
    if (!fw::is_ok(self().debug.bind_storage({bufs.trace_ring, bufs.log_ring, bufs.trace_task_stack}))) {
        return boot_reason::kBindDebug;
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
        return boot_reason::kBindSpp;
    }
#if RTK_GNSS_ENABLED
    if (!fw::is_ok(self().data.init_gnss_buffers(bufs.gnss_app_ring, bufs.gnss_latest)) ||
        !fw::is_ok(self().rtcm_pipe.bind_storage(bufs.rtcm_pipe)) ||
        !fw::is_ok(self().gnss.bind_storage(bufs.gnss_demux))) {
        return boot_reason::kBindGnss;
    }
#endif
    return nullptr;
}

void rollback_boot_bindings(BootBuffers &bufs, const char *reason) noexcept
{
#if FW_TRACE_ENABLED
    self().debug.unbind_storage();
#endif
#if RTK_GNSS_ENABLED
    self().gnss.unbind_storage();
    self().rtcm_pipe.unbind_storage();
    self().data.unbind_gnss_buffers();
#endif
    const esp_err_t unbound = spp_server_unbind_storage();
    bufs = {};
    if (unbound != ESP_OK) {
        // 若约束被破坏、C worker 已存在，保留全部块，绝不释放它可能使用的栈/缓冲。
        self().boot_arena.set_degraded_reason(boot_reason::kRollbackSppInUse);
        return;
    }
    if (!fw::is_ok(self().boot_arena.abort(reason))) {
        self().boot_arena.set_degraded_reason(reason);
    }
}

} // namespace app
