// GNSS App 数据通路辅助实现（ADR-0011）：6 KiB 最新帧缓存、GnssOutConfig 编解码、输出计划、订阅统计。
// 线程安全：GnssLatestCache 与 GnssAppSubState 使用注入锁；其余为纯函数。ISR 安全：否（带锁路径不可入 ISR）。

#include "data/GnssOutput.hpp"

#include <algorithm>
#include <cstring>

namespace app::data {

namespace {

constexpr std::array<const char *, kGnssAppMsgCount> kTypeNames = {
    "GGA", "SATSINFOA", "HEADINGA", "GST", "RMC", "GSA",
};

constexpr std::array<uint16_t, kGnssAppMsgCount> kTypeMsgIds = {
    0x0004U, 0x0005U, 0x001FU, 0x0021U, 0x0022U, 0x0023U,
};

// 输出周期的基本单位（ADR-0012 §2.4：GGA/RMC/GSA/GST 跟 rate，HEADINGA/GSV 默认 1 s）。
inline constexpr uint32_t kOneSecondMs = 1000U;

} // namespace

const char *gnssTypeName(size_t typeIdx) noexcept
{
    if (typeIdx >= kTypeNames.size()) {
        return "?";
    }
    return kTypeNames[typeIdx];
}

std::optional<uint8_t> gnssTypeFromMsgId(uint16_t msgId) noexcept
{
    for (size_t i = 0; i < kTypeMsgIds.size(); ++i) {
        if (kTypeMsgIds[i] == msgId) {
            return static_cast<uint8_t>(i);
        }
    }
    return std::nullopt;
}

bool GnssPushGate::due(uint32_t nowMs, uint32_t periodMs) const noexcept
{
    if (!armed || (periodMs == 0U)) {
        return true;
    }
    // 锚点可能领先 now 至多 slack，按有符号差处理回绕。
    const auto elapsed = static_cast<int64_t>(static_cast<int32_t>(nowMs - anchorMs));
    const uint32_t slack = std::min(periodMs / 4U, kMaxSlackMs);
    return (elapsed + static_cast<int64_t>(slack)) >= static_cast<int64_t>(periodMs);
}

void GnssPushGate::onPushed(uint32_t nowMs, uint32_t periodMs) noexcept
{
    const auto elapsed = static_cast<int64_t>(static_cast<int32_t>(nowMs - anchorMs));
    if (armed && (periodMs != 0U) && (elapsed < 2 * static_cast<int64_t>(periodMs))) {
        anchorMs += periodMs;
    } else {
        anchorMs = nowMs;
    }
    armed = true;
}

bool GnssOutConfig::isValid() const noexcept
{
    if ((version != kCurrentVersion) || ((mask & ~kGnssOutMaskAll) != 0U)) {
        return false;
    }
    for (size_t i = 0; i < kGnssAppMsgCount; ++i) {
        if (((mask & (1U << i)) != 0U) && (intervals[i] == 0)) {
            return false;
        }
    }
    return true;
}

fw::Result GnssOutConfig::applyInterval(size_t typeIdx, int8_t interval) noexcept
{
    if (typeIdx >= kGnssAppMsgCount) {
        return fw::Result::kInvalidArg;
    }
    const auto bit = static_cast<uint8_t>(1U << typeIdx);
    if (interval == 0) {
        mask = static_cast<uint8_t>(mask & ~bit);
        intervals[typeIdx] = 0;
        return fw::Result::kOk;
    }
    mask = static_cast<uint8_t>(mask | bit);
    intervals[typeIdx] = interval;
    return fw::Result::kOk;
}

fw::Result GnssOutConfig::encode(std::span<uint8_t> out, size_t &len) const noexcept
{
    if (!isValid()) {
        return fw::Result::kInvalidArg;
    }
    if (out.size() < kBlobBytes) {
        return fw::Result::kNoSpace;
    }
    out[0] = version;
    out[1] = mask;
    for (size_t i = 0; i < kGnssAppMsgCount; ++i) {
        out[2U + i] = static_cast<uint8_t>(intervals[i]);
    }
    len = kBlobBytes;
    return fw::Result::kOk;
}

fw::Result GnssOutConfig::decode(std::span<const uint8_t> in, GnssOutConfig &out) noexcept
{
    if (in.size() != kBlobBytes) {
        return fw::Result::kInvalidArg;
    }
    GnssOutConfig parsed{};
    parsed.version = in[0];
    parsed.mask = in[1];
    for (size_t i = 0; i < kGnssAppMsgCount; ++i) {
        // NVS 字节还原回有符号 interval（-128..127）。
        parsed.intervals[i] = static_cast<int8_t>(in[2U + i]);
    }
    if (!parsed.isValid()) {
        return fw::Result::kInvalidArg;
    }
    out = parsed;
    return fw::Result::kOk;
}

gnss::OutputPlan makeOutputPlan(const GnssOutConfig &out, uint8_t rateHz, bool gsv, uint8_t port,
                                uint32_t caps) noexcept
{
    gnss::OutputPlan plan{};
    const uint8_t hz = (rateHz == 0U) ? 1U : rateHz;
    const uint32_t locatePeriodMs = kOneSecondMs / static_cast<uint32_t>(hz);
    const GnssOutConfig dflt = GnssOutConfig::defaults();

    // 下标与 gnss::OutputMsg 一致（kGga=0 … kGsa=5），App 侧 6 项与驱动 7 项共用前缀。
    for (size_t i = 0; i < kGnssAppMsgCount; ++i) {
        if ((out.mask & (1U << i)) == 0U) {
            plan.periodMs[i] = 0U;
            continue;
        }
        if (out.intervals[i] != dflt.intervals[i]) {
            plan.periodMs[i] = core::services::GnssService::intervalToPeriodMs(out.intervals[i]);
            continue;
        }
        // 默认值：GGA/RMC/GSA/GST 跟定位频率（ADR-0012 §2.4）；SATSINFOA 按 gnss/out 默认 5 s
        // （用户 2026-11-16 决定，接收机基础脚本为 1 s）；HEADINGA 1 s 且仅双天线型号下发。
        const bool locate = (i == static_cast<size_t>(gnss::OutputMsg::kGga)) ||
                            (i == static_cast<size_t>(gnss::OutputMsg::kRmc)) ||
                            (i == static_cast<size_t>(gnss::OutputMsg::kGsa)) ||
                            (i == static_cast<size_t>(gnss::OutputMsg::kGst));
        if (locate) {
            plan.periodMs[i] = locatePeriodMs;
        } else if (i == static_cast<size_t>(gnss::OutputMsg::kHeading)) {
            plan.periodMs[i] = ((caps & gnss::cap::kHeading) != 0U) ? kOneSecondMs : 0U;
        } else {
            plan.periodMs[i] = core::services::GnssService::intervalToPeriodMs(out.intervals[i]);
        }
    }
    plan.periodMs[static_cast<size_t>(gnss::OutputMsg::kGsv)] = gsv ? kOneSecondMs : 0U;
    plan.port = port;
    plan.rateHz = hz;
    return plan;
}

/**
 * @brief 绑定外供 6 KiB 缓冲并清零长度与统计；buf 须非空且 ≥ kGnssLatestTotalBytes。线程安全：仅启动前。
 * @param buf: 外供缓存，生命周期覆盖读写者
 * @retval kOk 缓存已绑定并清零统计
 * @retval kInvalidArg 缓冲为空或不足 6 KiB
 * @note 仅启动前单线程调用；内部使用已绑定锁，非 ISR。
 */
fw::Status GnssLatestCache::init(std::span<uint8_t> buf) noexcept
{
    if ((buf.data() == nullptr) || (buf.size() < kGnssLatestTotalBytes)) {
        return fw::Status::kInvalidArg;
    }
    const fw::LockGuard guard(lock_);
    buf_ = buf.first(kGnssLatestTotalBytes);
    lengths_.fill(0U);
    updates_.fill(0U);
    oversize_drops_.fill(0U);
    query_oversize_.fill(0U);
    return fw::Status::kOk;
}

bool GnssLatestCache::update(uint8_t typeIdx, std::span<const uint8_t> frame) noexcept
{
    if ((typeIdx >= kGnssAppMsgCount) || frame.empty()) {
        return false;
    }
    const size_t cap = kGnssLatestCaps[typeIdx];
    const size_t ofs = kGnssLatestOffsets[typeIdx];
    const fw::LockGuard guard(lock_);
    if (buf_.size() < kGnssLatestTotalBytes) {
        return false;
    }
    if (frame.size() > cap) {
        ++oversize_drops_[typeIdx];
        return false;
    }
    const auto slotDst = buf_.subspan(ofs, frame.size());
    std::memcpy(slotDst.data(), frame.data(), frame.size());
    lengths_[typeIdx] = static_cast<uint16_t>(frame.size());
    ++updates_[typeIdx];
    return true;
}

core::services::GnssQueryResult GnssLatestCache::query(uint8_t typeIdx,
                                                       std::span<uint8_t> dst) noexcept
{
    if (typeIdx >= kGnssAppMsgCount) {
        return {};
    }
    const size_t ofs = kGnssLatestOffsets[typeIdx];
    const fw::LockGuard guard(lock_);
    if (buf_.size() < kGnssLatestTotalBytes) {
        return {};
    }
    const size_t len = lengths_[typeIdx];
    if (len == 0U) {
        return {};
    }
    if ((len > kGnssMaxQueryBytes) || (len > dst.size())) {
        ++query_oversize_[typeIdx];
        return {.ok = false, .oversize = true, .len = 0U};
    }
    const auto slotSrc = buf_.subspan(ofs, len);
    std::memcpy(dst.data(), slotSrc.data(), len);
    return {.ok = true, .oversize = false, .len = len};
}

std::array<GnssLatestSlotStats, kGnssAppMsgCount> GnssLatestCache::snapshot() const noexcept
{
    std::array<GnssLatestSlotStats, kGnssAppMsgCount> out{};
    const fw::LockGuard guard(lock_);
    for (size_t i = 0; i < kGnssAppMsgCount; ++i) {
        out[i].len = lengths_[i];
        out[i].cap = static_cast<uint16_t>(kGnssLatestCaps[i]);
        out[i].updates = updates_[i];
        out[i].oversizeDrops = oversize_drops_[i];
        out[i].queryOversize = query_oversize_[i];
    }
    return out;
}

void GnssAppSubState::setSubscription(
    uint32_t activeMask, const std::array<uint32_t, kGnssAppMsgCount> &periodsMs) noexcept
{
    const fw::LockGuard guard(lock_);
    state_.activeMask = activeMask;
    state_.periodsMs = periodsMs;
}

void GnssAppSubState::readSubscription(
    uint32_t &activeMask, std::array<uint32_t, kGnssAppMsgCount> &periodsMs) const noexcept
{
    const fw::LockGuard guard(lock_);
    activeMask = state_.activeMask;
    periodsMs = state_.periodsMs;
}

void GnssAppSubState::addTxSent(uint8_t typeIdx) noexcept
{
    if (typeIdx >= kGnssAppMsgCount) {
        return;
    }
    const fw::LockGuard guard(lock_);
    ++state_.txSent[typeIdx];
}

void GnssAppSubState::addSkipped(uint8_t typeIdx) noexcept
{
    if (typeIdx >= kGnssAppMsgCount) {
        return;
    }
    const fw::LockGuard guard(lock_);
    ++state_.skipped[typeIdx];
}

void GnssAppSubState::addRingOversize(uint8_t typeIdx) noexcept
{
    if (typeIdx >= kGnssAppMsgCount) {
        return;
    }
    const fw::LockGuard guard(lock_);
    ++state_.ringOversize[typeIdx];
}

void GnssAppSubState::setQueryOversizeTotal(uint32_t total) noexcept
{
    const fw::LockGuard guard(lock_);
    state_.queryOversizeTotal = total;
}

GnssAppSubSnapshot GnssAppSubState::snapshot() const noexcept
{
    const fw::LockGuard guard(lock_);
    return state_;
}

} // namespace app::data
