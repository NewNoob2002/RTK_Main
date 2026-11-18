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

/** @brief 测试 BaseRtcmRing 整帧边界、溢出整帧丢弃与清零语义。 */
void testBaseRtcmRing() noexcept
{
    app::data::BaseRtcmRing ring{};
    const std::array<uint8_t, 512U> frame{0xD3U, 0x01U, 0xFDU};
    std::array<uint8_t, app::data::BaseRtcmRing::kMaxFrameBytes> out{};
    size_t outLen = 0U;

    // 4096 B 容量最多容纳 7 条 514 B（2 + 512）记录 = 3598 B，第 8 条触发整帧丢弃并计数。
    for (size_t i = 0U; i < 7U; ++i) {
        CHECK(ring.push(frame));
    }
    CHECK(!ring.push(frame));
    CHECK(ring.dropFrames() == 1U);

    CHECK(ring.pop(out, outLen));
    CHECK((outLen == 512U) && (out[0U] == 0xD3U));
    CHECK(ring.push(frame)); // 释放空间后可继续推入整帧
    ring.clearFrames();
    CHECK(!ring.pop(out, outLen));
    CHECK(ring.dropFrames() == 1U);
}

} // namespace

int main()
{
    testBaseConfigCodecs();
    testWorkModeTransitions();
    testBaseRtcmRing();
    return 0;
}
