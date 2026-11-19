// gnss 只读命令（ADR-0010 §2.7）：gnss status|fix|sats 的 DataCenter 快照打印。
// 仅在 console REPL 任务调用，非 ISR；只读命令读 DataCenter Slot，改状态命令经 ioExecutor::call 投递。

#include "modules/gnss/GnssModule.hpp"

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "core/services/GnssService.hpp"
#include "data/GnssOutput.hpp"
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "gnss/Controller.hpp"
#include "gnss/Types.hpp"
#include "modules/gnss/GnssBaseConfig.hpp"
#include "modules/gnss/GnssCmdsPrivate.hpp"
#include "rtos/EspStatus.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

using detail::constellationName;
using detail::fixQualityName;
using detail::formatE9;
using detail::formatMmAsMeters;
using detail::formatX100;
using detail::kAppMsgNames;
using detail::kDopBufBytes;
using detail::kLatLonBufBytes;
using detail::kMetersBufBytes;
using detail::kMmPerMeter;
using detail::kStaticObsReservedBytes;
using detail::kTenthsBase;

/**
 * @brief 打印 GNSS 模块与链路统计（`gnss status` 前半段）
 * @param st: GNSS 状态快照
 * @note 仅 console REPL 任务；一次快照内打印，避免同一报告内数值前后不一致。
 */
void printGnssCore(const GnssStatus &st) noexcept
{
    cli::printSection("gnss");
    cli::printKv("enabled", "%s", st.enabled ? "yes" : "no");
    cli::printKv("state", "%s", ::gnss::stateName(st.state));
    cli::printKv("family", "%s", ::gnss::familyName(st.family));
    cli::printKv("model", "%s", ::gnss::modelName(st.model));
    cli::printKv("fw", "%s", (st.fwVersion[0] != '\0') ? st.fwVersion.data() : "-");
    cli::printKv("baud", "%" PRIu32 "  baud_fail=%" PRIu32, st.hostBaud,
                 appContext().data.gnssBaudFailCount.load(std::memory_order_relaxed));
    cli::printKv("caps", "0x%02" PRIX32, st.caps);
    cli::printKv("ctrl",
                 "probes=%" PRIu32 " steps=%" PRIu32 " nacks=%" PRIu32 " to=%" PRIu32
                 " reprobes=%" PRIu32 " fail=%" PRIu32,
                 st.ctrl.probesSent, st.ctrl.stepsSent, st.ctrl.stepNacks, st.ctrl.stepTimeouts,
                 st.ctrl.reprobes, st.ctrl.failures);
    cli::printKv("nmea",
                 "gga=%" PRIu32 " rmc=%" PRIu32 " gsa=%" PRIu32 " gsv=%" PRIu32 " gst=%" PRIu32
                 " ign=%" PRIu32 " bad=%" PRIu32,
                 st.nmea.gga, st.nmea.rmc, st.nmea.gsa, st.nmea.gsv, st.nmea.gst, st.nmea.ignored,
                 st.nmea.malformed);
    const auto wm = appContext().data.gnssWorkMode.read();
    const auto bcfg = appContext().gnss.readBaseConfig();
    if (wm.hAccCm != ::gnss::kU16Unknown) {
        cli::printKv("mode", "%s (state=%s base_type=%s elapsed=%us hAcc=%ucm)",
                     workModeName(bcfg.mode), workModeStateName(wm.state),
                     baseTypeName(bcfg.baseType), static_cast<unsigned>(wm.elapsedS),
                     static_cast<unsigned>(wm.hAccCm));
    } else {
        cli::printKv("mode", "%s (state=%s base_type=%s elapsed=%us hAcc=-)",
                     workModeName(bcfg.mode), workModeStateName(wm.state),
                     baseTypeName(bcfg.baseType), static_cast<unsigned>(wm.elapsedS));
    }
    cli::printKv("uart", "rx=%" PRIu32 " tx=%" PRIu32, st.uartRxBytes, st.uartTxBytes);
    cli::printKv("demux", "unknown=%" PRIu32 " resyncs=%" PRIu32, st.demuxUnknown, st.demuxResyncs);
    cli::printKv("output",
                 "seq=%" PRIu32 " applied=0x%02" PRIX32 " unsupported=0x%02" PRIX32
                 " failed=0x%02" PRIX32 " supported=0x%02" PRIX32 " last=%s",
                 st.outputSeq, st.appliedMask, st.unsupportedMask, st.failedMask, st.supportedMask,
                 (st.lastApply == GnssApply::kOk)         ? "ok"
                 : (st.lastApply == GnssApply::kFailed)   ? "save-failed"
                 : (st.lastApply == GnssApply::kRestored) ? "reverted"
                                                          : "-");
    cli::printKv("pipe",
                 "pushed=%" PRIu32 "/%" PRIu32 "B drop=%" PRIu32 "/%" PRIu32 "B written=%" PRIu32
                 "B",
                 st.pipe.pushedFrames, st.pipe.pushedBytes, st.pipe.droppedFrames,
                 st.pipe.droppedBytes, st.pipe.writtenBytes);
    const auto gate = appContext().data.ggaGateStatus.read();
    cli::printKv("gate",
                 "gga_gate=%s  min_sats=%u  blocked=%s  gga_blocked=%" PRIu32
                 "  gate_trans=%" PRIu32,
                 gate.enabled ? "on" : "off", static_cast<unsigned>(gate.minSats),
                 gate.blocked ? "yes" : "no", gate.ggaBlocked, gate.gateTrans);
}

/**
 * @brief 打印应用环、订阅者与最新值统计（`gnss status` 后半段）
 * @note 仅 console REPL 任务；各自读取自己的快照。
 */
void printGnssAppRing() noexcept
{
    const auto &data = appContext().data;
    const auto rWr = data.gnssAppRing.typedWriterStats();
    const auto rRd = data.gnssAppRing.typedReaderStats(0U);
    const auto sub = data.gnssAppSub.snapshot();
    const auto latest = data.gnssLatest.snapshot();
    cli::printKv("app_ring",
                 "cap=%uB hwm=%zuB wr=%" PRIu32 "/%" PRIu32 "B overwr=%" PRIu32 " nosub=%" PRIu32
                 " ovsz=%" PRIu32 " pin=%" PRIu32 " gga_drop=%" PRIu32,
                 static_cast<unsigned>(kGnssAppRingBytes), rWr.highWaterBytes, rWr.base.records,
                 rWr.base.bytes, rWr.base.overwritten, rWr.noSubscriber, rWr.oversizeDrop,
                 rWr.pinnedDrop, data.gnssAppRing.ggaDropCount());
    cli::printKv("app_reader",
                 "mask=0x%02" PRIX32 " rec=%" PRIu32 "/%" PRIu32 "B drop=%" PRIu32 " filt=%" PRIu32
                 " pin=%" PRIu32,
                 sub.activeMask, rRd.base.records, rRd.base.bytes, rRd.base.dropped, rRd.filtered,
                 rRd.pinnedDrop);
    cli::printKv("app_qovsz", "query_oversize=%" PRIu32, sub.queryOversizeTotal);
    for (size_t i = 0U; i < data::kGnssAppMsgCount; ++i) {
        const bool en = (sub.activeMask & (1U << i)) != 0U;
        cli::outPrintf("  sub[%s] %s period=%" PRIu32 "ms sent=%" PRIu32 " skip=%" PRIu32
                       " ring_ovsz=%" PRIu32 " latest=%uB(upd=%" PRIu32 ",ovsz=%" PRIu32 ")\n",
                       kAppMsgNames[i], en ? "on" : "off", sub.periodsMs[i], sub.txSent[i],
                       sub.skipped[i], sub.ringOversize[i], static_cast<unsigned>(latest[i].len),
                       latest[i].updates, latest[i].oversizeDrops);
    }
    cli::printKv("obs_ring", "reserved=%" PRIu32 "B not allocated", kStaticObsReservedBytes);
}

int cmdGnssStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: gnss status");
    }
    const auto st = appContext().data.gnssStatus.read();
    printGnssCore(st);
    printGnssAppRing();
    return 0;
}

int cmdGnssFix(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: gnss fix");
    }
    const auto fix = appContext().data.gnssFix.read();
    const uint32_t now = rtos::nowMs();
    cli::printSection("gnss_fix");
    cli::printKv("valid", "%s", fix.valid ? "yes" : "no");
    cli::printKv("quality", "%s (%u)", fixQualityName(fix.quality),
                 static_cast<unsigned>(fix.quality));
    if (fix.updateMs != 0U) {
        const uint32_t sec = fix.utc.ms / 1000U;
        const uint32_t ms = fix.utc.ms % 1000U;
        cli::printKv("utc", "%04u-%02u-%02u %02u:%02u:%02" PRIu32 ".%03" PRIu32, fix.utc.year,
                     fix.utc.month, fix.utc.day, fix.utc.hour, fix.utc.minute, sec, ms);
        cli::printKv("age_ms", "%" PRIu32, now - fix.updateMs);
    } else {
        cli::printKv("utc", "-");
        cli::printKv("age_ms", "-");
    }
    std::array<char, kLatLonBufBytes> lat{};
    std::array<char, kLatLonBufBytes> lon{};
    formatE9(fix.latE9, lat);
    formatE9(fix.lonE9, lon);
    cli::printKv("lat_deg", "%s", lat.data());
    cli::printKv("lon_deg", "%s", lon.data());

    std::array<char, kMetersBufBytes> alt{};
    std::array<char, kMetersBufBytes> geoid{};
    formatMmAsMeters(fix.altMm, alt);
    formatMmAsMeters(fix.geoidMm, geoid);
    cli::printKv("alt_m", "%s (geoid %s)", alt.data(), geoid.data());
    cli::printKv("sats_used", "%u", fix.satsUsed);

    std::array<char, kDopBufBytes> hdop{};
    std::array<char, kDopBufBytes> pdop{};
    std::array<char, kDopBufBytes> vdop{};
    formatX100(fix.hdopX100, hdop);
    formatX100(fix.pdopX100, pdop);
    formatX100(fix.vdopX100, vdop);
    cli::printKv("dop", "h=%s p=%s v=%s", hdop.data(), pdop.data(), vdop.data());

    if (fix.diffAgeDs != ::gnss::kU16Unknown) {
        const uint32_t ageWhole = static_cast<uint32_t>(fix.diffAgeDs) / kTenthsBase;
        const uint32_t ageTenths = static_cast<uint32_t>(fix.diffAgeDs) % kTenthsBase;
        cli::printKv("diff", "age=%" PRIu32 ".%" PRIu32 "s station=%u", ageWhole, ageTenths,
                     fix.diffStation);
    } else {
        cli::printKv("diff", "none");
    }
    std::array<char, kDopBufBytes> course{};
    formatX100(fix.courseCdeg, course);
    cli::printKv("motion", "speed=%" PRIu32 ".%03" PRIu32 "m/s course=%sdeg",
                 fix.speedMmps / kMmPerMeter, fix.speedMmps % kMmPerMeter, course.data());
    return 0;
}

int cmdGnssSats(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: gnss sats");
    }
    const auto table = appContext().data.gnssSats.read();
    uint32_t usedCount = 0U;
    for (uint8_t i = 0U; (i < table.count) && (i < table.sats.size()); ++i) {
        if (table.sats[i].used) {
            ++usedCount;
        }
    }
    cli::printSection("gnss_sats");
    cli::printKv("count", "%u (used=%" PRIu32 " update_ms=%" PRIu32 ")", table.count, usedCount,
                 table.updateMs);
    for (uint8_t i = 0U; (i < table.count) && (i < table.sats.size()); ++i) {
        const auto &sat = table.sats[i];
        cli::outPrintf("  %s-%03u elev=%d az=%u snr=%u %s\n", constellationName(sat.system),
                       sat.prn, static_cast<int>(sat.elevDeg), sat.azimDeg, sat.snrDbhz,
                       sat.used ? "*" : "-");
    }
    return 0;
}

} // namespace app
