// RTCM 模块控制台命令表（ADR-0009 §2.5）：rtcm stats / rtcm reset。
// 线程：仅在 console REPL 任务调用，非 ISR；stats 只读 DataCenter，reset 经 Executor::call 投递。

#include "modules/rtcm/RtcmModule.hpp"

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <span>

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

constexpr uint64_t kBytesPerKib = 1024ULL;
constexpr uint64_t kTenthsPerUnit = 10ULL;

int cmd_rtcm_stats(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: rtcm stats");
    }
    print_rtcm_status();
    return 0;
}

int cmd_rtcm_reset(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: rtcm reset");
    }
    fw::Command command{};
    command.target = id(ModuleKind::kRtcm);
    command.verb = verb(RtcmVerb::kReset);
    return submit(command, "rtcm reset") ? 0 : cli::kCmdFail;
}

constexpr std::array<cli::Verb, 2U> kRtcmVerbs{{
    {"stats", nullptr, "show RTCM forwarding and demux counters", cmd_rtcm_stats},
    {"reset", nullptr, "clear RTCM and demux statistics", cmd_rtcm_reset},
}};

int cmd_rtcm(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kRtcmVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 1U> kRtcmCmds{{
    {"rtcm", "rtcm <stats|reset>: RTCM forwarding statistics", "<stats|reset>", cmd_rtcm, nullptr},
}};

} // namespace

void print_rtcm_status() noexcept
{
    const auto &data = app_context().data;
    const auto rtcm = data.rtcm_stats.read();
    const auto demux = data.demux_stats.read();
    const auto stats = data.link_stats.read();

    const uint64_t kib_tenths = (static_cast<uint64_t>(rtcm.bytes) * kTenthsPerUnit) / kBytesPerKib;
    cli::printSection("rtcm");
    cli::printKv("frames", "%" PRIu32, rtcm.frames);
    cli::printKv("bytes", "%" PRIu32 " B (%llu.%llu KB)", rtcm.bytes,
                 static_cast<unsigned long long>(kib_tenths / kTenthsPerUnit),
                 static_cast<unsigned long long>(kib_tenths % kTenthsPerUnit));
    cli::printKv("rate", "%" PRIu32 " B/s", rtcm.bytes_per_sec);
    cli::printKv("crc32", "0x%08" PRIX32, rtcm.crc32);
    cli::printKv("last_msg", "%" PRIu16, rtcm.last_msg);
    cli::printKv("rejected_role", "%" PRIu32, rtcm.rejected_role);
    cli::printKv("unknown_bytes", "%" PRIu32 " B", demux.unknown_bytes);
    cli::printKv("prod_rejects", "%" PRIu32, demux.product_rejects);
    cli::printKv("rtcm_rejects", "%" PRIu32, demux.rtcm_rejects);
    cli::printKv("reset_epoch", "%" PRIu32, rtcm.reset_epoch);
    cli::printKv("pipe_push",
                 "%" PRIu32 " frames / %" PRIu32 " B (drop=%" PRIu32 " frames / %" PRIu32 " B)",
                 rtcm.pipe_accepted_frames, rtcm.pipe_accepted_bytes, rtcm.pipe_dropped_frames,
                 rtcm.pipe_dropped_bytes);
#if RTK_GNSS_ENABLED
    const auto pipe_stats = app_context().rtcm_pipe.stats();
    const auto pipe_io = app_context().rtcm_pipe.io_stats();
    cli::printKv("pipe_uart",
                 "written=%" PRIu32 " B pending=%" PRIu32 " B discarded=%" PRIu32
                 " B write_err=%" PRIu32 " paused=%s",
                 pipe_stats.written_bytes, pipe_io.pending_bytes, pipe_io.discarded_bytes,
                 pipe_io.uart_write_errors, pipe_io.paused ? "yes" : "no");
#endif

    // 保留 ADR-0006 §2 逐字格式行，供 tools/hil/spp_rtcm_mix.py 正则匹配。
    cli::outPrintf("rtcm: frames=%" PRIu32 " bytes=%" PRIu32 " crc32=0x%08" PRIX32
                   " last_msg=%" PRIu16 " rejected_role=%" PRIu32 " unknown_bytes=%" PRIu32
                   " product_rejects=%" PRIu32 " rtcm_rejects=%" PRIu32 "\n",
                   rtcm.frames, rtcm.bytes, rtcm.crc32, rtcm.last_msg, rtcm.rejected_role,
                   demux.unknown_bytes, demux.product_rejects, demux.rtcm_rejects);
    cli::outPrintf("stats: frames_rx=%" PRIu32 " replies_tx=%" PRIu32 " write_errors=%" PRIu32
                   " pushes_tx=%" PRIu32 " disconnects=%" PRIu32 " rx_dropped=%" PRIu32 "\n",
                   stats.frames_rx, stats.replies_tx, stats.write_errors, stats.pushes_tx,
                   stats.disconnects, stats.rx_dropped);
#if RTK_GNSS_ENABLED
    cli::outPrintf("rtcm_pipe: accepted=%" PRIu32 "/%" PRIu32 " dropped=%" PRIu32 "/%" PRIu32
                   " written=%" PRIu32 " pending=%" PRIu32 " discarded=%" PRIu32
                   " uart_err=%" PRIu32 " epoch=%" PRIu32 "\n",
                   rtcm.pipe_accepted_frames, rtcm.pipe_accepted_bytes, rtcm.pipe_dropped_frames,
                   rtcm.pipe_dropped_bytes, pipe_stats.written_bytes, pipe_io.pending_bytes,
                   pipe_io.discarded_bytes, pipe_io.uart_write_errors, rtcm.reset_epoch);
#else
    cli::out_printf("rtcm_pipe: accepted=%" PRIu32 "/%" PRIu32 " dropped=%" PRIu32 "/%" PRIu32
                    " epoch=%" PRIu32 "\n",
                    rtcm.pipe_accepted_frames, rtcm.pipe_accepted_bytes, rtcm.pipe_dropped_frames,
                    rtcm.pipe_dropped_bytes, rtcm.reset_epoch);
#endif
}

std::span<const cli::Cmd> rtcm_cmds() noexcept
{
    return kRtcmCmds;
}

std::span<const cli::Verb> rtcm_verbs() noexcept
{
    return kRtcmVerbs;
}

} // namespace app
