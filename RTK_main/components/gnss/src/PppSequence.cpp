// Unicore PPP 与 0x06 截止角/星座配置脚本，单步 ACK 失败时立即结束。
// 接收机任务串行调用，非 ISR，无阻塞和分配。
#include "PppSequence.hpp"

#include <array>

#include "../src/AsciiCmd.hpp"

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
        return true;
    case Model::kZedF9p:
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

/** @brief 启动 0x06 截止角与星座脚本。 @param cfg: 合法配置。 @retval true 已启动；false 截止角越界。 */
bool SatMaskSequence::start(const SatMaskConfig &cfg) noexcept
{
    if (cfg.cutoffDeg > SatMaskConfig::kMaxCutoffDeg) {
        active_ = false;
        return false;
    }
    cfg_ = cfg;
    step_ = 0U;
    active_ = true;
    sent_.clear();
    ack_ = Ack::kNone;
    return true;
}

bool SatMaskSequence::buildCurrentStep() noexcept
{
    sent_.clear();
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

std::optional<fw::Result> SatMaskSequence::poll(uint32_t nowMs,
                                                std::span<const uint8_t> &tx) noexcept
{
    tx = {};
    if (!active_) {
        return fw::Result::kInvalidState;
    }
    if (!sent_.view().empty()) {
        if ((ack_ == Ack::kNone) && ((nowMs - sent_ms_) < kReplyMs)) {
            return std::nullopt;
        }
        if (ack_ != Ack::kAck) {
            active_ = false;
            return (ack_ == Ack::kNack) ? fw::Result::kIoError : fw::Result::kTimeout;
        }
        ++step_;
        if (step_ >= kStepCount) {
            active_ = false;
            return fw::Result::kOk;
        }
    }
    if (!buildCurrentStep()) {
        active_ = false;
        return fw::Result::kNoSpace;
    }
    sent_ms_ = nowMs;
    ack_ = Ack::kNone;
    tx = sent_.view();
    return std::nullopt;
}

void SatMaskSequence::onFrame(const Receiver &receiver, const demux::Decoder &decoder,
                              std::span<const uint8_t> frame, uint32_t nowMs) noexcept
{
    if (active_ && !sent_.view().empty() && (ack_ == Ack::kNone) &&
        ((nowMs - sent_ms_) < kReplyMs)) {
        ack_ = receiver.classifyReply(decoder, frame, sent_.view());
    }
}

} // namespace gnss
