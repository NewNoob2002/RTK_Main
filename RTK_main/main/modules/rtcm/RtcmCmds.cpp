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

int cmdRtcmStats(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: rtcm stats");
    }
    printRtcmStatus();
    return 0;
}

int cmdRtcmReset(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
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
    {"stats", nullptr, "show RTCM forwarding and demux counters", cmdRtcmStats},
    {"reset", nullptr, "clear RTCM and demux statistics", cmdRtcmReset},
}};

// 命令表项：签名由 cli::Cmd 的处理器类型固定；默认动词/参数在此注入。
int cmdRtcm(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kRtcmVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 1U> kRtcmCmds{{
    {"rtcm", "rtcm <stats|reset>: RTCM forwarding statistics", "<stats|reset>", cmdRtcm, nullptr},
}};

} // namespace

void printRtcmStatus() noexcept
{
    const auto &data = appContext().data;
    const auto rtcm = data.rtcmStats.read();
    const auto demux = data.demuxStats.read();
    const auto stats = data.linkStats.read();

    const uint64_t kibTenths = (static_cast<uint64_t>(rtcm.bytes) * kTenthsPerUnit) / kBytesPerKib;
    cli::printSection("rtcm");
    cli::printKv("frames", "%" PRIu32, rtcm.frames);
    cli::printKv("bytes", "%" PRIu32 " B (%llu.%llu KB)", rtcm.bytes,
                 static_cast<unsigned long long>(kibTenths / kTenthsPerUnit),
                 static_cast<unsigned long long>(kibTenths % kTenthsPerUnit));
    cli::printKv("rate", "%" PRIu32 " B/s", rtcm.bytesPerSec);
    cli::printKv("crc32", "0x%08" PRIX32, rtcm.crc32);
    cli::printKv("last_msg", "%" PRIu16, rtcm.lastMsg);
    cli::printKv("rejected_role", "%" PRIu32, rtcm.rejectedRole);
    cli::printKv("unknown_bytes", "%" PRIu32 " B", demux.unknownBytes);
    cli::printKv("prod_rejects", "%" PRIu32, demux.productRejects);
    cli::printKv("rtcm_rejects", "%" PRIu32, demux.rtcmRejects);
    cli::printKv("reset_epoch", "%" PRIu32, rtcm.resetEpoch);
    cli::printKv("pipe_push",
                 "%" PRIu32 " frames / %" PRIu32 " B (drop=%" PRIu32 " frames / %" PRIu32 " B)",
                 rtcm.pipeAcceptedFrames, rtcm.pipeAcceptedBytes, rtcm.pipeDroppedFrames,
                 rtcm.pipeDroppedBytes);
#if RTK_GNSS_ENABLED
    const auto pipeStats = appContext().rtcmPipe.stats();
    const auto pipeIo = appContext().rtcmPipe.ioStats();
    cli::printKv("pipe_uart",
                 "written=%" PRIu32 " B pending=%" PRIu32 " B discarded=%" PRIu32
                 " B write_err=%" PRIu32 " paused=%s",
                 pipeStats.writtenBytes, pipeIo.pendingBytes, pipeIo.discardedBytes,
                 pipeIo.uartWriteErrors, pipeIo.paused ? "yes" : "no");
#endif

    // 保留 ADR-0006 §2 逐字格式行，供 tools/hil/spp_rtcm_mix.py 正则匹配。
    cli::outPrintf("rtcm: frames=%" PRIu32 " bytes=%" PRIu32 " crc32=0x%08" PRIX32
                   " last_msg=%" PRIu16 " rejected_role=%" PRIu32 " unknown_bytes=%" PRIu32
                   " product_rejects=%" PRIu32 " rtcm_rejects=%" PRIu32 "\n",
                   rtcm.frames, rtcm.bytes, rtcm.crc32, rtcm.lastMsg, rtcm.rejectedRole,
                   demux.unknownBytes, demux.productRejects, demux.rtcmRejects);
    cli::outPrintf("stats: frames_rx=%" PRIu32 " replies_tx=%" PRIu32 " write_errors=%" PRIu32
                   " pushes_tx=%" PRIu32 " disconnects=%" PRIu32 " rx_dropped=%" PRIu32 "\n",
                   stats.framesRx, stats.repliesTx, stats.writeErrors, stats.pushesTx,
                   stats.disconnects, stats.rxDropped);
#if RTK_GNSS_ENABLED
    cli::outPrintf("rtcm_pipe: accepted=%" PRIu32 "/%" PRIu32 " dropped=%" PRIu32 "/%" PRIu32
                   " written=%" PRIu32 " pending=%" PRIu32 " discarded=%" PRIu32
                   " uart_err=%" PRIu32 " epoch=%" PRIu32 "\n",
                   rtcm.pipeAcceptedFrames, rtcm.pipeAcceptedBytes, rtcm.pipeDroppedFrames,
                   rtcm.pipeDroppedBytes, pipeStats.writtenBytes, pipeIo.pendingBytes,
                   pipeIo.discardedBytes, pipeIo.uartWriteErrors, rtcm.resetEpoch);
#else
    cli::outPrintf("rtcm_pipe: accepted=%" PRIu32 "/%" PRIu32 " dropped=%" PRIu32 "/%" PRIu32
                   " epoch=%" PRIu32 "\n",
                   rtcm.pipeAcceptedFrames, rtcm.pipeAcceptedBytes, rtcm.pipeDroppedFrames,
                   rtcm.pipeDroppedBytes, rtcm.resetEpoch);
#endif
}

std::span<const cli::Cmd> rtcmCmds() noexcept
{
    return kRtcmCmds;
}

std::span<const cli::Verb> rtcmVerbs() noexcept
{
    return kRtcmVerbs;
}

} // namespace app
