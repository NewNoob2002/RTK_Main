// gnss 命令组（ADR-0010 §2.7）：gnss status|fix|sats|probe|reset|rate <hz>|send <text>。
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
#include "rtos/EspStatus.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

constexpr int64_t kE9Scale = 1000000000LL;
constexpr int32_t kMmPerMeter = 1000;
constexpr uint32_t kPercentBase = 100U;
constexpr uint32_t kTenthsBase = 10U;
constexpr int32_t kMinSignedInterval = -128;
constexpr int32_t kMaxSignedInterval = 127;
constexpr uint32_t kStaticObsReservedBytes = 16384U;
/** 等待变更判定的轮询间隔与上限：须长于 GnssModule 的 kApplyTimeoutMs（5 s，频率重配含基础脚本）。 */
constexpr uint32_t kApplyPollMs = 50U;
constexpr uint32_t kApplyPollRounds = 120U;
constexpr size_t kLatLonBufBytes = 24U;
constexpr size_t kMetersBufBytes = 20U;
constexpr size_t kDopBufBytes = 16U;
constexpr int kDecimalBase = 10;
constexpr int kOutSetArgc = 3;

constexpr std::array<const char *, data::kGnssAppMsgCount> kAppMsgNames = {
    {"gga", "sats", "heading", "gst", "rmc", "gsa"}};

bool parseMsgIndex(std::string_view name, uint8_t &outIdx, bool &isAll) noexcept
{
    isAll = false;
    if (name == "all") {
        isAll = true;
        return true;
    }
    for (size_t i = 0U; i < kAppMsgNames.size(); ++i) {
        if (name == std::string_view{kAppMsgNames[i]}) {
            outIdx = static_cast<uint8_t>(i);
            return true;
        }
    }
    return false;
}

bool parseI8Range(const char *text, int8_t &outVal) noexcept
{
    if ((text == nullptr) || (*text == '\0')) {
        return false;
    }
    char *end = nullptr;
    const long val = std::strtol(text, &end, kDecimalBase);
    if ((end == text) || (*end != '\0') || (val < kMinSignedInterval) ||
        (val > kMaxSignedInterval)) {
        return false;
    }
    outVal = static_cast<int8_t>(val);
    return true;
}

const char *fixQualityName(::gnss::FixQuality q) noexcept
{
    switch (q) {
    case ::gnss::FixQuality::kInvalid:
        return "invalid";
    case ::gnss::FixQuality::kSingle:
        return "single";
    case ::gnss::FixQuality::kDgps:
        return "dgps";
    case ::gnss::FixQuality::kPps:
        return "pps";
    case ::gnss::FixQuality::kRtkFixed:
        return "rtk-fixed";
    case ::gnss::FixQuality::kRtkFloat:
        return "rtk-float";
    case ::gnss::FixQuality::kDeadReckoning:
        return "dr";
    case ::gnss::FixQuality::kManual:
        return "manual";
    case ::gnss::FixQuality::kSimulator:
        return "sim";
    case ::gnss::FixQuality::kUnknown:
    default:
        return "unknown";
    }
}

const char *constellationName(::gnss::Constellation sys) noexcept
{
    switch (sys) {
    case ::gnss::Constellation::kGps:
        return "GPS";
    case ::gnss::Constellation::kGlonass:
        return "GLO";
    case ::gnss::Constellation::kGalileo:
        return "GAL";
    case ::gnss::Constellation::kBeidou:
        return "BDS";
    case ::gnss::Constellation::kQzss:
        return "QZS";
    case ::gnss::Constellation::kNavic:
        return "IRN";
    case ::gnss::Constellation::kSbas:
        return "SBAS";
    case ::gnss::Constellation::kOther:
    default:
        return "OTH";
    }
}

void formatE9(int64_t val, std::span<char, kLatLonBufBytes> out) noexcept
{
    const char *sign = (val < 0) ? "-" : "";
    const int64_t absVal = (val < 0) ? -val : val;
    const int64_t whole = absVal / kE9Scale;
    const int64_t frac = absVal % kE9Scale;
    (void)std::snprintf(out.data(), out.size(), "%s%lld.%09lld", sign,
                        static_cast<long long>(whole), static_cast<long long>(frac));
}

void formatMmAsMeters(int32_t mm, std::span<char, kMetersBufBytes> out) noexcept
{
    const char *sign = (mm < 0) ? "-" : "";
    const int64_t absMm = (mm < 0) ? -static_cast<int64_t>(mm) : static_cast<int64_t>(mm);
    const int64_t whole = absMm / kMmPerMeter;
    const int64_t frac = absMm % kMmPerMeter;
    (void)std::snprintf(out.data(), out.size(), "%s%lld.%03lld", sign,
                        static_cast<long long>(whole), static_cast<long long>(frac));
}

void formatX100(uint16_t val, std::span<char, kDopBufBytes> out) noexcept
{
    if (val == ::gnss::kU16Unknown) {
        (void)std::snprintf(out.data(), out.size(), "unknown");
        return;
    }
    const uint32_t whole = static_cast<uint32_t>(val) / kPercentBase;
    const uint32_t frac = static_cast<uint32_t>(val) % kPercentBase;
    (void)std::snprintf(out.data(), out.size(), "%" PRIu32 ".%02" PRIu32, whole, frac);
}

bool reportGnssResult(const fw::CommandResult &res) noexcept
{
    switch (res.status) {
    case fw::Result::kOk:
        return true;
    case fw::Result::kInvalidArg:
        (void)cli::fail("invalid argument");
        return false;
    case fw::Result::kInvalidState:
        (void)cli::fail("gnss not ready");
        return false;
    case fw::Result::kBusy:
        (void)cli::fail("gnss busy");
        return false;
    case fw::Result::kIoError:
        (void)cli::fail("gnss io error: %s", esp_err_to_name(res.detail));
        return false;
    default:
        (void)cli::fail("gnss error %u", static_cast<unsigned>(res.status));
        return false;
    }
}

bool submitGnss(const fw::Command &cmd) noexcept;

/** 打印本轮输出计划的逐项结果；成功返回 true。 */
bool reportOutputPlan(const GnssStatus &before, const GnssStatus &after) noexcept
{
    bool ok = true;
    for (size_t i = 0U; i < data::kGnssAppMsgCount; ++i) {
        const uint32_t bit = 1U << i;
        const char *state = "off";
        if ((after.unsupportedMask & bit) != 0U) {
            state = "unsupported";
        } else if ((after.failedMask & bit) != 0U) {
            state = "failed";
            ok = false;
        } else if ((((after.appliedMask | before.appliedMask) & bit) != 0U) &&
                   (after.rxPeriodMs[i] != 0U)) {
            // applied 位也包含已 ACK 的关闭命令（UNLOG），是否在输出看计划周期。
            cli::outPrintf("  %-8s on %" PRIu32 "ms\n", kAppMsgNames[i], after.rxPeriodMs[i]);
            continue;
        }
        cli::outPrintf("  %-8s %s\n", kAppMsgNames[i], state);
    }
    return ok;
}

/**
 * 提交命令并等待两阶段提交判定（applySeq 前进），再按 lastApply 给 OK/ERR。
 * 仅在 console REPL 任务调用；阻塞轮询，最长 6 s（不阻塞 kIo）。
 */
bool submitGnssAndWait(const fw::Command &cmd) noexcept
{
    const GnssStatus before = appContext().data.gnssStatus.read();
    if (!submitGnss(cmd)) {
        return false;
    }
    for (uint32_t round = 0U; round < kApplyPollRounds; ++round) {
        const GnssStatus after = appContext().data.gnssStatus.read();
        if (after.applySeq != before.applySeq) {
            if (after.lastApply == GnssApply::kRestored) {
                (void)cli::fail("receiver rejected output change, reverted");
                return false;
            }
            if (after.lastApply != GnssApply::kOk) {
                (void)cli::fail("output applied but settings save failed");
                return false;
            }
            return reportOutputPlan(before, after);
        }
        vTaskDelay(pdMS_TO_TICKS(kApplyPollMs));
    }
    (void)cli::fail("gnss output apply timeout");
    return false;
}

bool submitGnss(const fw::Command &cmd) noexcept
{
    if (appContext().ioExecutor == nullptr) {
        (void)cli::fail("gnss not running");
        return false;
    }
    fw::CommandResult res{};
    if (!submitRawTo(*appContext().ioExecutor, ModuleKind::kGnss, "gnss not running", cmd, res)) {
        return false;
    }
    return reportGnssResult(res);
}

int cmdGnssStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: gnss status");
    }
    const auto st = appContext().data.gnssStatus.read();
    cli::printSection("gnss");
    cli::printKv("enabled", "%s", st.enabled ? "yes" : "no");
    cli::printKv("state", "%s", ::gnss::stateName(st.state));
    cli::printKv("family", "%s", ::gnss::familyName(st.family));
    cli::printKv("model", "%s", ::gnss::modelName(st.model));
    cli::printKv("fw", "%s", (st.fwVersion[0] != '\0') ? st.fwVersion.data() : "-");
    cli::printKv("baud", "%" PRIu32, st.hostBaud);
    cli::printKv("caps", "0x%02" PRIX32, st.caps);
    cli::printKv("ctrl",
                 "probes=%" PRIu32 " steps=%" PRIu32 " nacks=%" PRIu32 " to=%" PRIu32
                 " reprobes=%" PRIu32 " fail=%" PRIu32,
                 st.ctrl.probesSent, st.ctrl.stepsSent, st.ctrl.stepNacks, st.ctrl.stepTimeouts,
                 st.ctrl.reprobes, st.ctrl.failures);
    cli::printKv("nmea",
                 "gga=%" PRIu32 " rmc=%" PRIu32 " gsa=%" PRIu32 " gsv=%" PRIu32 " ign=%" PRIu32
                 " bad=%" PRIu32,
                 st.nmea.gga, st.nmea.rmc, st.nmea.gsa, st.nmea.gsv, st.nmea.ignored,
                 st.nmea.malformed);
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

    const auto &data = appContext().data;
    const auto rWr = data.gnssAppRing.typedWriterStats();
    const auto rRd = data.gnssAppRing.typedReaderStats(0U);
    const auto sub = data.gnssAppSub.snapshot();
    const auto latest = data.gnssLatest.snapshot();
    cli::printKv("app_ring",
                 "cap=%uB hwm=%zuB wr=%" PRIu32 "/%" PRIu32 "B overwr=%" PRIu32 " nosub=%" PRIu32
                 " ovsz=%" PRIu32 " pin=%" PRIu32,
                 static_cast<unsigned>(kGnssAppRingBytes), rWr.highWaterBytes, rWr.base.records,
                 rWr.base.bytes, rWr.base.overwritten, rWr.noSubscriber, rWr.oversizeDrop,
                 rWr.pinnedDrop);
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
    cli::printKv("obs_ring", "reserved=%" PRIu32 "B not allocated (K6)", kStaticObsReservedBytes);
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

int cmdGnssProbe(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: gnss probe");
    }
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssVerb::kProbe);
    return submitGnss(cmd) ? 0 : cli::kCmdFail;
}

int cmdGnssReset(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: gnss reset");
    }
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssVerb::kReset);
    return submitGnss(cmd) ? 0 : cli::kCmdFail;
}

int cmdGnssRate(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != 2) || (argv == nullptr)) {
        return cli::fail("usage: gnss rate <1|2|5|10|20>");
    }
    const std::span<char *const> args{argv, 2U};
    uint8_t hz = 0U;
    if (!cli::parseU8(args[1], hz)) {
        return cli::fail("invalid rate: %s", args[1]);
    }
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssVerb::kSetRate);
    cmd.argLen = 1U;
    cmd.args[0] = hz;
    return submitGnssAndWait(cmd) ? 0 : cli::kCmdFail;
}

int cmdGnssSend(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc < 2) || (argv == nullptr)) {
        return cli::fail("usage: gnss send <text>");
    }
    // argc 已验证为正；转换为 size_t 不改变数值。
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssVerb::kSend);
    size_t pos = 0U;
    for (size_t i = 1U; i < args.size(); ++i) {
        const size_t partLen = std::strlen(args[i]);
        const size_t need = (i > 1) ? (partLen + 1U) : partLen;
        if (need > (cmd.args.size() - pos)) {
            return cli::fail("command too long (max %u)", static_cast<unsigned>(cmd.args.size()));
        }
        if (i > 1) {
            cmd.args[pos] = static_cast<uint8_t>(' ');
            ++pos;
        }
        std::memcpy(std::span{cmd.args}.subspan(pos, partLen).data(), args[i], partLen);
        pos += partLen;
    }
    if (pos == 0U) {
        return cli::fail("empty command");
    }
    cmd.argLen = static_cast<uint8_t>(pos);
    return submitGnss(cmd) ? 0 : cli::kCmdFail;
}

int cmdGnssOut(void * /*ctx*/, int argc, char **argv) noexcept
{
    const std::span<char *const> args{(argv != nullptr) ? argv : nullptr,
                                      ((argv != nullptr) && (argc > 0)) ? static_cast<size_t>(argc)
                                                                        : 0U};
    auto cfg = appContext().data.gnssOutCfg.read();
    if ((args.size() <= 1U) || ((args.size() == 2U) && (std::string_view{args[1]} == "show"))) {
        cli::printSection("gnss_out");
        cli::printKv("mask", "0x%02X", static_cast<unsigned>(cfg.mask));
        const GnssStatus st = appContext().data.gnssStatus.read();
        for (size_t i = 0U; i < data::kGnssAppMsgCount; ++i) {
            const bool en = (cfg.mask & (1U << i)) != 0U;
            const uint32_t period =
                core::services::GnssService::intervalToPeriodMs(cfg.intervals[i]);
            const uint32_t bit = 1U << i;
            const char *applied = (st.appliedMask & bit) != 0U ? "on" : "-";
            if ((st.unsupportedMask & bit) != 0U) {
                applied = "unsupported";
            } else if ((st.failedMask & bit) != 0U) {
                applied = "failed";
            }
            cli::outPrintf("  %-8s %s interval=%d push=%" PRIu32 "ms rx=%" PRIu32
                           "ms receiver=%s\n",
                           kAppMsgNames[i], en ? "on" : "off", static_cast<int>(cfg.intervals[i]),
                           period, st.rxPeriodMs[i], applied);
        }
        return 0;
    }
    if ((args.size() == 2U) && (std::string_view{args[1]} == "default")) {
        cfg = data::GnssOutConfig::defaults();
    } else if (args.size() == static_cast<size_t>(kOutSetArgc)) {
        uint8_t idx = 0U;
        bool isAll = false;
        if (!parseMsgIndex(std::string_view{args[1]}, idx, isAll)) {
            return cli::fail("invalid msg (gga|sats|heading|gst|rmc|gsa|all)");
        }
        const std::string_view argVal{args[2]};
        const bool turnOff = (argVal == "off");
        int8_t iv = 0;
        if (!turnOff && !parseI8Range(args[2], iv)) {
            return cli::fail("invalid interval (-128..127 or off)");
        }
        const size_t begin = isAll ? 0U : idx;
        const size_t end = isAll ? data::kGnssAppMsgCount : (static_cast<size_t>(idx) + 1U);
        for (size_t i = begin; i < end; ++i) {
            if (!fw::isOk(cfg.applyInterval(i, turnOff ? int8_t{0} : iv))) {
                return cli::fail("unsupported interval");
            }
        }
    } else {
        return cli::fail("usage: gnss out [show|default|<msg|all> <off|-128..127>]");
    }

    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kSetOut);
    std::array<uint8_t, data::GnssOutConfig::kBlobBytes> blob{};
    size_t blobLen = 0U;
    if (!fw::isOk(cfg.encode(std::span<uint8_t>{blob}, blobLen))) {
        return cli::fail("encode out config failed");
    }
    static_assert(data::GnssOutConfig::kBlobBytes <= fw::kCommandArgBytes);
    std::memcpy(cmd.args.data(), blob.data(), blobLen);
    cmd.argLen = static_cast<uint8_t>(blobLen);
    return submitGnssAndWait(cmd) ? 0 : cli::kCmdFail;
}

/**
 * @brief 打印一次读回结论（ERR not supported / ERR busy / 文本 + lines + OK）
 * @param rb: 已发布的读回结论
 * @return 0: 打印成功（由分发器补上 OK）；其他: cli::kCmdFail
 */
int printReadback(const GnssReadback &rb) noexcept
{
    if (!rb.supported) {
        return cli::fail("not supported");
    }
    if (rb.busy) {
        return cli::fail("busy");
    }
    if (rb.len != 0U) {
        // 文本为若干 "...\n" 行，不以 '\0' 结尾，因此按长度整块写出。
        cli::outWrite(appContext().data.gnssReadbackText.data(), rb.len);
    }
    cli::outPrintf("lines=%u%s\n", static_cast<unsigned>(rb.lines),
                   rb.truncated ? " truncated" : "");
    return 0;
}

/**
 * @brief `gnss config`：读回模块当前配置并逐行打印（ADR-0012 §2.7）
 * @param argc: 参数个数（只允许 1）
 * @param argv: 参数向量
 * @return 0: 成功；其他: cli::kCmdFail（ERR 原因已打印）
 * @note 仅 console REPL 任务，阻塞等待模块结论，最长 6 s。
 */
int cmdGnssConfig(void * /*ctx*/, int argc, char **argv) noexcept
{
    const std::span<char *const> args{(argv != nullptr) ? argv : nullptr,
                                      ((argv != nullptr) && (argc > 0)) ? static_cast<size_t>(argc)
                                                                        : 0U};
    if (args.size() > 1U) {
        return cli::fail("usage: gnss config");
    }
    if (appContext().ioExecutor == nullptr) {
        return cli::fail("gnss not running");
    }
    const uint32_t before = appContext().data.gnssReadback.read().seq;
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(GnssSubVerb::kReadback);
    fw::CommandResult res{};
    if (!submitRawTo(*appContext().ioExecutor, ModuleKind::kGnss, "gnss not running", cmd, res)) {
        return cli::kCmdFail;
    }
    // 模块在 onCommand 里同步发布被拒结论，成功则等读回窗口收尾（≤ 5 s，这里给 6 s 余量）。
    for (uint32_t round = 0U; round < kApplyPollRounds; ++round) {
        const GnssReadback rb = appContext().data.gnssReadback.read();
        if (rb.seq != before) {
            return printReadback(rb);
        }
        vTaskDelay(pdMS_TO_TICKS(kApplyPollMs));
    }
    return cli::fail("timeout");
}

// clang-format off
constexpr std::array<cli::Verb, 9U> kGnssVerbs{{
    {"status", nullptr, "GNSS status, stats and pipe", &cmdGnssStatus},
    {"fix", nullptr, "latest position/quality fix", &cmdGnssFix},
    {"sats", nullptr, "visible satellites table", &cmdGnssSats},
    {"probe", nullptr, "re-probe without power cycle", &cmdGnssProbe},
    {"reset", nullptr, "power cycle and re-probe", &cmdGnssReset},
    {"rate", "<hz>", "set output rate (1|2|5|10|20)", &cmdGnssRate},
    {"out", "[show|default|<msg|all> <val>]", "set/show default receiver output", &cmdGnssOut},
    {"config", nullptr, "read back receiver config", &cmdGnssConfig},
    {"send", "<text>", "send raw command line to GNSS", &cmdGnssSend},
}};
// clang-format on

int cmdGnss(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kGnssVerbs, ctx, argc, argv, "status");
}

constexpr std::array<cli::Cmd, 1U> kGnssCmds{{
    {
        .name = "gnss",
        .help = "GNSS receiver control and telemetry",
        .hint = "[status|fix|sats|probe|reset|rate|out|config|send]",
        .handler = &cmdGnss,
        .ctx = nullptr,
    },
}};

} // namespace

std::span<const cli::Cmd> gnssCmds() noexcept
{
    return kGnssCmds;
}

std::span<const cli::Verb> gnssVerbs() noexcept
{
    return kGnssVerbs;
}

} // namespace app
