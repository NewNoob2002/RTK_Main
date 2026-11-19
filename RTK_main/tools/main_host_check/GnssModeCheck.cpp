// Host 单元测试：覆盖 GnssBaseConfig 编解码、GnssWorkMode 状态机与 BaseRtcmRing 跨任务队列。
// 不依赖真实硬件或 FreeRTOS 调度器，验证 C5a 状态迁移、超时回退、NVS 键读写与 RTCM 分发。
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "AppContext.hpp"
#include "data/BaseRtcmRing.hpp"
#include "def/GnssSubDef.hpp"
#include "gnss/Controller.hpp"
#include "modules/gnss/GnssBaseConfig.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/gnss/GnssTxLog.hpp"
#include "modules/gnss/GnssWorkMode.hpp"

namespace {

void check(bool ok, const char *expr, int line) noexcept
{
    if (!ok) {
        std::fprintf(stderr, "GnssModeCheck line %d failed: %s\n", line, expr);
        std::abort();
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

class FakeModeReceiver final : public gnss::Receiver {
public:
    bool allowMode = true;

    [[nodiscard]] gnss::Family family() const noexcept override
    {
        return gnss::Family::kUnicore;
    }
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "fake-unicore";
    }
    [[nodiscard]] std::span<const demux::Decoder *const> decoders() const noexcept override
    {
        return {};
    }
    [[nodiscard]] uint32_t caps(gnss::Model) const noexcept override
    {
        return gnss::cap::kRtcmIn | gnss::cap::kBase;
    }
    [[nodiscard]] bool buildProbe(gnss::CmdBuf &out) const noexcept override
    {
        out.clear();
        return out.append("PROBE\r\n");
    }
    [[nodiscard]] bool matchProbe(const demux::Decoder &, std::span<const uint8_t> frame,
                                  gnss::ProbeResult &out) const noexcept override
    {
        if (frame.empty() || (frame[0] != static_cast<uint8_t>('#'))) {
            return false;
        }
        out.model = gnss::Model::kUm980;
        return true;
    }
    [[nodiscard]] bool roverStep(size_t index, gnss::Model, const gnss::RoverConfig &,
                                 gnss::ConfigStep &out) const noexcept override
    {
        if (index != 0U) {
            return false;
        }
        out = gnss::ConfigStep{};
        return out.cmd.append("ROVER0\r\n");
    }
    [[nodiscard]] gnss::Ack classifyReply(const demux::Decoder &, std::span<const uint8_t> frame,
                                          std::span<const uint8_t>) const noexcept override
    {
        if ((frame.size() == 2U) && (frame[0] == static_cast<uint8_t>('O')) &&
            (frame[1] == static_cast<uint8_t>('K'))) {
            return gnss::Ack::kAck;
        }
        if ((frame.size() == 2U) && (frame[0] == static_cast<uint8_t>('N')) &&
            (frame[1] == static_cast<uint8_t>('O'))) {
            return gnss::Ack::kNack;
        }
        return gnss::Ack::kNone;
    }
    [[nodiscard]] bool modeStep(const gnss::ModeRequest &req, uint8_t idx,
                                gnss::ConfigStep &out) const noexcept override
    {
        if (!allowMode || (idx > 0U)) {
            return false;
        }
        out = gnss::ConfigStep{};
        switch (req.action) {
        case gnss::ModeAction::kRover:
            return out.cmd.append("MODE ROVER\r\n");
        case gnss::ModeAction::kBasePrep:
            return out.cmd.append("PREP\r\n");
        case gnss::ModeAction::kBaseSurvey:
            return out.cmd.append("SURVEY\r\n");
        case gnss::ModeAction::kBaseFixed:
            return out.cmd.append("FIXED\r\n");
        default:
            return false;
        }
    }
};

constinit FakeModeReceiver s_fakeRx{};
constexpr demux::Decoder s_fakeDec{"Fake", 128U, nullptr, nullptr, nullptr};

void initRunningController(gnss::Controller &ctrl, uint32_t &nowMs) noexcept
{
    static const std::array<const gnss::Receiver *, 1U> kRx{&s_fakeRx};
    static const std::array<uint32_t, 1U> kBauds{115200U};
    gnss::RoverConfig rcfg{};
    gnss::Timing timing{};
    timing.powerOffMs = 10U;
    timing.bootMs = 10U;
    timing.probeTimeoutMs = 50U;
    timing.silenceMs = 5000U;
    CHECK(ctrl.init(kRx, kBauds, rcfg, gnss::OutputPlan{}, {}, timing));
    ctrl.start(nowMs);
    const std::array<uint8_t, 1U> hit{'#'};
    const std::array<uint8_t, 2U> ok{'O', 'K'};
    for (int step = 0; (step < 32) && (ctrl.state() != gnss::State::kRunning); ++step) {
        const auto act = ctrl.poll(nowMs);
        if (act.kind == gnss::Action::Kind::kPower) {
            nowMs += 10U;
            continue;
        }
        if (act.kind == gnss::Action::Kind::kWrite) {
            if (ctrl.state() == gnss::State::kProbing) {
                ctrl.onFrame(s_fakeDec, hit, nowMs);
            } else {
                ctrl.onFrame(s_fakeDec, ok, nowMs);
            }
        }
        ++nowMs;
    }
    CHECK(ctrl.state() == gnss::State::kRunning);
}

void feedAck(gnss::Controller &ctrl, bool ack, uint32_t nowMs) noexcept
{
    const auto act = ctrl.poll(nowMs);
    CHECK(act.kind == gnss::Action::Kind::kWrite);
    const std::array<uint8_t, 2U> ok{'O', 'K'};
    const std::array<uint8_t, 2U> no{'N', 'O'};
    ctrl.onFrame(s_fakeDec, ack ? std::span<const uint8_t>{ok} : std::span<const uint8_t>{no},
                 nowMs);
    (void)ctrl.poll(nowMs);
}

/** @brief 测试 GnssBaseConfig 缺省表、MSM4/7 互换、定点解析与 blob 编解码。 */
void testBaseConfigCodecs() noexcept
{
    auto cfg = app::GnssBaseConfig::defaults();
    CHECK(cfg.mode == app::WorkModeKind::kRover);
    CHECK(cfg.baseType == app::BaseType::kSurvey);
    CHECK(cfg.svyS == 20U);
    CHECK(cfg.svyAccCm == 150U);
    CHECK(cfg.svyToS == 120U);
    CHECK(cfg.msm == 4U);
    CHECK(cfg.rtcmOut[0U] == 1);
    CHECK(cfg.rtcmOut[2U] == -10);
    CHECK(cfg.rtcmOut[4U] == 1);
    CHECK(cfg.rtcmOut[8U] == 0);

    cfg.applyMsm(7U);
    CHECK(cfg.msm == 7U);
    CHECK(cfg.rtcmOut[4U] == 0);
    CHECK(cfg.rtcmOut[8U] == 1);

    std::array<uint8_t, app::GnssBaseConfig::kRtcmOutBlobBytes> blob{};
    size_t len = 0U;
    CHECK(cfg.encodeRtcmOut(blob, len));
    CHECK(len == app::GnssBaseConfig::kRtcmOutBlobBytes);
    auto decoded = app::GnssBaseConfig::defaults();
    CHECK(decoded.decodeRtcmOut(blob));
    CHECK(decoded.rtcmOut == cfg.rtcmOut);
    blob[0U] = 99U; // bad version
    CHECK(!decoded.decodeRtcmOut(blob));

    int64_t lat = 0;
    int64_t h = 0;
    CHECK(app::parseScaledDecimal("-31.234567890", 9U, lat) && (lat == -31234567890LL));
    CHECK(app::parseScaledDecimal("-1.234", 3U, h) && (h == -1234LL));
    CHECK(!app::parseScaledDecimal("abc", 3U, h));

    std::array<uint8_t, 8U> b64{};
    app::encodeI64Le(lat, b64);
    CHECK(app::decodeI64Le(b64) == lat);
}

/** @brief 测试 GnssWorkMode 全状态迁移：启动自动应用、待决覆盖、survey 完成与超时回退。 */
void testWorkModeTransitions() noexcept
{
    s_fakeRx.allowMode = true;
    gnss::Controller ctrl;
    uint32_t nowMs = 1000U;
    initRunningController(ctrl, nowMs);

    // 1. base+survey 成功路径：kBaseConfig -> kSurveyWait -> kSurvey -> kBaseTx -> SAVECONFIG。
    app::GnssBaseConfig cfg = app::GnssBaseConfig::defaults();
    cfg.svyS = 20U;
    cfg.svyAccCm = 150U;
    cfg.svyToS = 120U;

    app::GnssWorkMode wm;
    bool needSave = false;
    wm.requestMode(app::WorkModeKind::kBase, true);
    CHECK(wm.poll(ctrl, cfg, 2U, 1U, true, 300U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kBaseConfig);
    CHECK(!needSave);

    feedAck(ctrl, true, nowMs);
    CHECK(wm.poll(ctrl, cfg, 2U, 1U, true, 250U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kSurveyWait);

    // hAcc > svyAccCm 时留在 kSurveyWait，未到总超时前不启动 survey（elapsed/hAcc 更新会触发快照发布）。
    nowMs += 5000U;
    CHECK(wm.poll(ctrl, cfg, 2U, 1U, true, 200U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kSurveyWait);
    CHECK(wm.status().elapsedS == 5U);

    // hAcc <= svyAccCm 立即切到 kSurvey 并下发 survey 命令。
    CHECK(wm.poll(ctrl, cfg, 2U, 1U, true, 140U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kSurvey);
    feedAck(ctrl, true, nowMs);
    static_cast<void>(wm.poll(ctrl, cfg, 2U, 1U, true, 140U, nowMs, needSave));

    // elapsed >= svyS 但未见过 RTCM 1005 仍不进 kBaseTx。
    nowMs += 20000U;
    static_cast<void>(wm.poll(ctrl, cfg, 2U, 1U, true, 120U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kSurvey);

    // 收到 RTCM 1005 后立刻迁移到 kBaseTx，并触发 SAVECONFIG。
    wm.onRtcmFrame(1005U);
    CHECK(wm.poll(ctrl, cfg, 2U, 1U, true, 120U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kBaseTx);
    CHECK(wm.shouldForwardRtcm());
    CHECK(needSave);

    // 2. survey 总超时回退到 kRover 且 lastErr = kSurveyTimeout。
    wm.requestMode(app::WorkModeKind::kRover, true);
    static_cast<void>(wm.poll(ctrl, cfg, 2U, 1U, true, 120U, nowMs, needSave));
    feedAck(ctrl, true, nowMs);
    static_cast<void>(wm.poll(ctrl, cfg, 2U, 1U, true, 120U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kRover);

    wm.requestMode(app::WorkModeKind::kBase, false);
    static_cast<void>(wm.poll(ctrl, cfg, 2U, 1U, true, 500U, nowMs, needSave));
    feedAck(ctrl, true, nowMs);
    static_cast<void>(wm.poll(ctrl, cfg, 2U, 1U, true, 500U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kSurveyWait);

    nowMs += 120000U;
    CHECK(wm.poll(ctrl, cfg, 2U, 1U, true, 500U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kFallback);
    CHECK(wm.status().lastErr == app::WorkModeErr::kSurveyTimeout);
    feedAck(ctrl, true, nowMs); // 完成回退的 MODE ROVER
    CHECK(wm.poll(ctrl, cfg, 2U, 1U, true, 500U, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kRover);
    CHECK(!needSave);

    // 3. base+fixed 跳过 surveyWait/survey 直接进 kBaseTx。
    cfg.baseType = app::BaseType::kFixed;
    wm.requestMode(app::WorkModeKind::kBase, true);
    static_cast<void>(wm.poll(ctrl, cfg, 2U, 1U, true, gnss::kU16Unknown, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kBaseConfig);
    feedAck(ctrl, true, nowMs);
    CHECK(wm.poll(ctrl, cfg, 2U, 1U, true, gnss::kU16Unknown, nowMs, needSave));
    CHECK(wm.status().state == app::WorkModeState::kBaseTx);
    CHECK(needSave);

    // 4. 启动自动应用：saveOnSuccess 应为 false；unsupported 回到 kRover。
    app::GnssWorkMode bootWm;
    cfg.mode = app::WorkModeKind::kBase;
    cfg.baseType = app::BaseType::kFixed;
    CHECK(bootWm.poll(ctrl, cfg, 2U, 1U, true, gnss::kU16Unknown, nowMs, needSave));
    feedAck(ctrl, true, nowMs);
    CHECK(bootWm.poll(ctrl, cfg, 2U, 1U, true, gnss::kU16Unknown, nowMs, needSave));
    CHECK(bootWm.status().state == app::WorkModeState::kBaseTx);
    CHECK(!needSave);
}

/** @brief 验证 BaseRtcmRing 恰好写满、跨末尾回绕读写与丢帧语义。 */
void testBaseRtcmRing() noexcept
{
    app::data::BaseRtcmRing ring{};
    std::array<uint8_t, app::data::BaseRtcmRing::kMaxFrameBytes> maxFrame{};
    for (size_t i = 0U; i < maxFrame.size(); ++i) {
        maxFrame[i] = static_cast<uint8_t>((i * 7U + 3U) & 0xFFU);
    }
    std::array<uint8_t, 1001U> tailFit{};
    for (size_t i = 0U; i < tailFit.size(); ++i) {
        tailFit[i] = static_cast<uint8_t>(0xA5U ^ (i & 0xFFU));
    }
    std::array<uint8_t, app::data::BaseRtcmRing::kMaxFrameBytes> out{};
    size_t outLen = 0U;

    // 1. 恰好写满 4096 B：3*(2+1029) + 1*(2+1001) = 3093 + 1003 = 4096 B。
    CHECK(ring.push(maxFrame));
    CHECK(ring.push(maxFrame));
    CHECK(ring.push(maxFrame));
    CHECK(ring.push(tailFit));
    const std::array<uint8_t, 1U> tiny{0xD3U};
    CHECK(!ring.push(tiny));
    CHECK(ring.dropFrames() == 1U);

    // 弹出 1 条后释放 1031 B；先压 1 条让写游标停在末尾 4095 处（测试 2B 长度头+正文双重跨界）。
    CHECK(ring.pop(out, outLen) && (outLen == maxFrame.size()));
    CHECK(std::memcmp(out.data(), maxFrame.data(), maxFrame.size()) == 0);
    std::array<uint8_t, 600U> wrapFrame{};
    for (size_t i = 0U; i < wrapFrame.size(); ++i) {
        wrapFrame[i] = static_cast<uint8_t>((i + 19U) & 0xFFU);
    }
    CHECK(ring.pop(out, outLen) && (outLen == maxFrame.size()));
    CHECK(ring.pop(out, outLen) && (outLen == maxFrame.size()));
    CHECK(ring.pop(out, outLen) && (outLen == tailFit.size()));
    CHECK(std::memcmp(out.data(), tailFit.data(), tailFit.size()) == 0);

    // 目前 head == tail == 4096%4096 == 0；先推进到 offset 4095，再写一条跨界帧。
    std::array<uint8_t, 1020U> spacer{};
    for (size_t i = 0U; i < 4U; ++i) {
        CHECK(ring.push(spacer));
        CHECK(ring.pop(out, outLen));
    }
    // 已走过 4*(2+1020) = 4088 B（距 4096 还剩 8 B）；再 push 一条 5 B 占 7 B，使 head=4095。
    const std::array<uint8_t, 5U> pad5{1U, 2U, 3U, 4U, 5U};
    CHECK(ring.push(pad5));
    CHECK(ring.push(wrapFrame)); // 2B 长度头跨 4095->0，600B 正文紧随
    CHECK(ring.pop(out, outLen) && (outLen == 5U));
    CHECK(ring.pop(out, outLen) && (outLen == wrapFrame.size()));
    CHECK(std::memcmp(out.data(), wrapFrame.data(), wrapFrame.size()) == 0);

    // 再测一次“正文跨界”（长度头完全在末尾，正文跨越 4096->0 边界）。
    ring.clearFrames();
    const std::array<uint8_t, 50U> bodyCross{0xD3U, 0x00U, 0x32U, 0xAAU, 0x55U};
    // 推入/弹出 80 条 49 B（每次前进 51 B，80*51=4080，剩余 16 B）。
    const std::array<uint8_t, 49U> step49{0xD3U};
    for (size_t i = 0U; i < 80U; ++i) {
        CHECK(ring.push(step49));
        CHECK(ring.pop(out, outLen));
    }
    CHECK(ring.push(bodyCross)); // 2B 头在 4080..4081，正文前 14B 在末尾、后 36B 回绕至 0..35
    CHECK(ring.pop(out, outLen) && (outLen == bodyCross.size()));
    CHECK(std::memcmp(out.data(), bodyCross.data(), bodyCross.size()) == 0);
}

/** @brief 验证 1033/1230 按接收机族过滤支持判断与未探测后缀标签。 */
void testBaseRtcmFamilyFilter() noexcept
{
    CHECK(app::isBaseRtcmSupportedForFamily(1033U, gnss::Family::kUnknown));
    CHECK(app::isBaseRtcmSupportedForFamily(1230U, gnss::Family::kUnknown));
    CHECK(std::string_view{app::baseRtcmFamilyTag(1033U, gnss::Family::kUnknown)} == " (Unicore)");
    CHECK(std::string_view{app::baseRtcmFamilyTag(1230U, gnss::Family::kUnknown)} == " (u-blox)");
    CHECK(std::string_view{app::baseRtcmFamilyTag(1005U, gnss::Family::kUnknown)}.empty());

    CHECK(app::isBaseRtcmSupportedForFamily(1033U, gnss::Family::kUnicore));
    CHECK(!app::isBaseRtcmSupportedForFamily(1230U, gnss::Family::kUnicore));
    CHECK(std::string_view{app::baseRtcmFamilyTag(1033U, gnss::Family::kUnicore)}.empty());

    CHECK(app::isBaseRtcmSupportedForFamily(1033U, gnss::Family::kOptisens));
    CHECK(!app::isBaseRtcmSupportedForFamily(1230U, gnss::Family::kOptisens));
    CHECK(std::string_view{app::baseRtcmFamilyTag(1033U, gnss::Family::kOptisens)}.empty());

    CHECK(!app::isBaseRtcmSupportedForFamily(1033U, gnss::Family::kUblox));
    CHECK(app::isBaseRtcmSupportedForFamily(1230U, gnss::Family::kUblox));
    CHECK(std::string_view{app::baseRtcmFamilyTag(1230U, gnss::Family::kUblox)}.empty());

    CHECK(!app::isBaseRtcmSupportedForFamily(1033U, gnss::Family::kComnav));
    CHECK(!app::isBaseRtcmSupportedForFamily(1230U, gnss::Family::kComnav));
    CHECK(app::isBaseRtcmSupportedForFamily(1005U, gnss::Family::kComnav));
}

/** @brief 验证发送日志格式化对 ASCII 命令与二进制 UBX 十六进制的处理。 */
void testTxLogFormatting() noexcept
{
    std::array<char, 192U> buf{};
    const std::array<uint8_t, 12U> asciiCmd{'M', 'O', 'D', 'E', ' ',  'R',
                                            'O', 'V', 'E', 'R', '\r', '\n'};
    static_cast<void>(app::detail::formatGnssTxLog(asciiCmd, buf));
    CHECK(std::string_view{buf.data()} == "\"MODE ROVER\"");

    const std::array<uint8_t, 9U> ubx9{0xB5U, 0x62U, 0x06U, 0x8AU, 0x01U,
                                       0x00U, 0x00U, 0x91U, 0xF0U};
    static_cast<void>(app::detail::formatGnssTxLog(ubx9, buf));
    CHECK(std::string_view{buf.data()} == "UBX 0x06 0x8A, 9 bytes: B5 62 06 8A 01 00 00 91 F0");

    const std::array<uint8_t, 20U> ubx20{0xB5U, 0x62U, 0x06U, 0x8AU, 0x0CU, 0x00U, 0x01U,
                                         0x01U, 0x00U, 0x00U, 0x10U, 0x00U, 0x91U, 0x20U,
                                         0x01U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U};
    static_cast<void>(app::detail::formatGnssTxLog(ubx20, buf));
    CHECK(std::string_view{buf.data()} ==
          "UBX 0x06 0x8A, 20 bytes: B5 62 06 8A 0C 00 01 01 00 00 10 00 91 20 01 22 ...");
}

} // namespace

int main()
{
    testBaseConfigCodecs();
    testWorkModeTransitions();
    testBaseRtcmRing();
    testBaseRtcmFamilyFilter();
    testTxLogFormatting();
    return 0;
}
