// Unicore PPP 与 0x06 截止角/星座配置脚本，单步 ACK 失败时立即结束。
// 接收机任务串行调用，非 ISR，无阻塞和分配。
#include "PppSequence.hpp"

#include <array>

#include "../src/AsciiCmd.hpp"
#include "UbloxExt.hpp"

namespace gnss {
namespace {

constexpr std::array<std::string_view, 5U> kEnable{
    "CONFIG SBAS DISABLE\r\n", "CONFIG SIGNALGROUP 2\r\n", "CONFIG PPP ENABLE E6-HAS\r\n",
    "CONFIG PPP CONVERGE 15 25\r\n", "SAVECONFIG\r\n"};
constexpr std::array<std::string_view, 2U> kDisable{"CONFIG PPP DISABLE\r\n", "SAVECONFIG\r\n"};

constexpr size_t kStepCutoff = 0U;
constexpr size_t kStepGps = 1U;
constexpr size_t kStepGlo = 2U;
constexpr size_t kStepBds = 3U;
constexpr size_t kStepGal = 4U;
constexpr size_t kStepSbas = 5U;

} // namespace

bool supportsPpp(Model model) noexcept
{
    switch (model) {
    case Model::kUm980:
    case Model::kUm981:
    case Model::kUm982:
        return true;
    case Model::kCm680:
    case Model::kZedF9p:
    case Model::kK803:
    case Model::kUnknown:
    default:
        return false;
    }
}

bool supportsSatMask(Model model) noexcept
{
    switch (model) {
    case Model::kUm980:
    case Model::kUm981:
    case Model::kUm982:
    case Model::kCm680:
    case Model::kZedF9p:
        return true;
    case Model::kK803:
    case Model::kUnknown:
    default:
        return false;
    }
}

void PppSequence::start(bool enabled) noexcept
{
    commands_ = enabled ? std::span<const std::string_view>{kEnable}
                        : std::span<const std::string_view>{kDisable};
    sent_.clear();
    ack_ = Ack::kNone;
}

std::optional<fw::Result> PppSequence::poll(uint32_t nowMs, std::span<const uint8_t> &tx) noexcept
{
    tx = {};
    if (!active()) {
        return fw::Result::kInvalidState;
    }
    if (!sent_.view().empty()) {
        if (ack_ == Ack::kNone && nowMs - sent_ms_ < kReplyMs) {
            return std::nullopt;
        }
        if (ack_ != Ack::kAck) {
            commands_ = {};
            return ack_ == Ack::kNack ? fw::Result::kIoError : fw::Result::kTimeout;
        }
        commands_ = commands_.subspan(1U);
        if (!active()) {
            return fw::Result::kOk;
        }
    }
    sent_.clear();
    if (!sent_.append(commands_.front())) {
        commands_ = {};
        return fw::Result::kNoSpace;
    }
    sent_ms_ = nowMs;
    ack_ = Ack::kNone;
    tx = sent_.view();
    return std::nullopt;
}

void PppSequence::onFrame(const Receiver &receiver, const demux::Decoder &decoder,
                          std::span<const uint8_t> frame, uint32_t nowMs) noexcept
{
    if (active() && !sent_.view().empty() && ack_ == Ack::kNone && nowMs - sent_ms_ < kReplyMs) {
        ack_ = receiver.classifyReply(decoder, frame, sent_.view());
    }
}

/**
 * @brief 开始 0x06 截止角/星座脚本。
 * @param cfg: 合法的截止角与星座配置。
 * @param model: 当前接收机型号。
 * @param includeSbas: F9P 是否包含 CFG-SIGNAL-SBAS_ENA 键。
 * @retval true 已启动脚本；false 参数非法、F9P 关闭 GPS 或型号不支持。
 */
bool SatMaskSequence::start(const SatMaskConfig &cfg, Model model, bool includeSbas) noexcept
{
    if ((cfg.cutoffDeg > SatMaskConfig::kMaxCutoffDeg) || !supportsSatMask(model)) {
        phase_ = Phase::kIdle;
        return false;
    }
    if ((model == Model::kZedF9p) && !cfg.gps) {
        phase_ = Phase::kIdle;
        return false;
    }
    cfg_ = cfg;
    model_ = model;
    include_sbas_ = includeSbas;
    step_ = 0U;
    phase_ = Phase::kSend;
    pending_result_ = fw::Result::kOk;
    sent_.clear();
    ack_ = Ack::kNone;
    return true;
}

bool SatMaskSequence::buildCurrentStep() noexcept
{
    sent_.clear();
    if (model_ == Model::kZedF9p) {
        return (step_ == 0U) && ublox::buildSatTrackValset(sent_, cfg_, include_sbas_);
    }
    switch (step_) {
    case kStepCutoff:
        return sent_.append("MASK ") && ascii::appendUint(sent_, cfg_.cutoffDeg) &&
               sent_.append("\r\n");
    case kStepGps:
        return sent_.append(cfg_.gps ? "UNMASK GPS\r\n" : "MASK GPS\r\n");
    case kStepGlo:
        return sent_.append(cfg_.glo ? "UNMASK GLO\r\n" : "MASK GLO\r\n");
    case kStepBds:
        return sent_.append(cfg_.bds ? "UNMASK BDS\r\n" : "MASK BDS\r\n");
    case kStepGal:
        return sent_.append(cfg_.gal ? "UNMASK GAL\r\n" : "MASK GAL\r\n");
    case kStepSbas:
        return sent_.append(cfg_.sbas ? "CONFIG SBAS ENABLE AUTO\r\n" : "CONFIG SBAS DISABLE\r\n");
    default:
        return false;
    }
}

/**
 * @brief 推进 kAwaitAck 阶段：等待应答、失败转重同步或推进到下一步/稳定期。
 * @param nowMs: 当前单调时钟。
 * @param[out] advanceToSend: 需立即构造并发送下一步时置 true。
 * @retval nullopt 仍在等待或进入稳定/重同步/下一步；结果则序列已结束。
 */
std::optional<fw::Result> SatMaskSequence::pollAwaitAck(uint32_t nowMs,
                                                        bool &advanceToSend) noexcept
{
    advanceToSend = false;
    if ((ack_ == Ack::kNone) && ((nowMs - sent_ms_) < kReplyMs)) {
        return std::nullopt;
    }
    if (ack_ != Ack::kAck) {
        const fw::Result err = (ack_ == Ack::kNack) ? fw::Result::kIoError : fw::Result::kTimeout;
        if ((model_ == Model::kZedF9p) && (err == fw::Result::kTimeout)) {
            phase_ = Phase::kResync;
            phase_ms_ = nowMs;
            pending_result_ = err;
            return std::nullopt;
        }
        phase_ = Phase::kIdle;
        return err;
    }
    ++step_;
    if (step_ >= totalSteps()) {
        if (model_ == Model::kZedF9p) {
            phase_ = Phase::kSettle;
            phase_ms_ = nowMs;
            return std::nullopt;
        }
        phase_ = Phase::kIdle;
        return fw::Result::kOk;
    }
    phase_ = Phase::kSend;
    advanceToSend = true;
    return std::nullopt;
}

std::optional<fw::Result> SatMaskSequence::poll(uint32_t nowMs,
                                                std::span<const uint8_t> &tx) noexcept
{
    tx = {};
    if (phase_ == Phase::kIdle) {
        return fw::Result::kInvalidState;
    }
    if (phase_ == Phase::kSettle) {
        if ((nowMs - phase_ms_) < kF9pSignalSettleMs) {
            return std::nullopt;
        }
        phase_ = Phase::kIdle;
        return fw::Result::kOk;
    }
    if (phase_ == Phase::kResync) {
        if ((nowMs - phase_ms_) < kF9pResyncMs) {
            return std::nullopt;
        }
        phase_ = Phase::kIdle;
        return pending_result_;
    }
    if (phase_ == Phase::kAwaitAck) {
        bool advanceToSend = false;
        const auto done = pollAwaitAck(nowMs, advanceToSend);
        if (!advanceToSend) {
            return done;
        }
    }
    if (!buildCurrentStep()) {
        phase_ = Phase::kIdle;
        return fw::Result::kNoSpace;
    }
    sent_ms_ = nowMs;
    ack_ = Ack::kNone;
    phase_ = Phase::kAwaitAck;
    tx = sent_.view();
    return std::nullopt;
}

/**
 * @brief 记录当前命令的匹配应答。
 * @param receiver: 当前接收机驱动。
 * @param decoder: 帧来源解码器。
 * @param frame: 已校验的接收机帧。
 * @param nowMs: 接收时钟。
 */
void SatMaskSequence::onFrame(const Receiver &receiver, const demux::Decoder &decoder,
                              std::span<const uint8_t> frame, uint32_t nowMs) noexcept
{
    if ((phase_ == Phase::kAwaitAck) && !sent_.view().empty() && (ack_ == Ack::kNone) &&
        ((nowMs - sent_ms_) < kReplyMs)) {
        ack_ = receiver.classifyReply(decoder, frame, sent_.view());
    }
}

} // namespace gnss
