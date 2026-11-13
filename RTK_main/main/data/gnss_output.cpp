// GNSS App 数据通路辅助实现（K4 / ADR-0011）：语句分类、6 KiB 最新帧缓存、GnssOutConfig 编解码、订阅统计。
// 线程安全：GnssLatestCache 与 GnssAppSubState 使用注入锁；其余为纯函数。ISR 安全：否（带锁路径不可入 ISR）。

#include "data/gnss_output.hpp"

#include <algorithm>

#include <cstring>
#include <string_view>

namespace app::data {

namespace {

constexpr std::array<const char *, kGnssAppMsgCount> kTypeNames = {
    "GGA", "SATSINFOA", "HEADINGA", "GST", "RMC", "GSA",
};

constexpr std::array<uint16_t, kGnssAppMsgCount> kTypeMsgIds = {
    0x0004U, 0x0005U, 0x001FU, 0x0021U, 0x0022U, 0x0023U,
};

constexpr size_t kNmeaHeaderLen = 7U; // "$xxYYY,"
constexpr size_t kNmeaTalkerEnd = 3U; // "$xx"
constexpr size_t kNmeaTagLen = 3U;    // "GGA"/"GST"/"RMC"/"GSA"
constexpr size_t kNmeaCommaPos = 6U;

enum class SentenceType : uint8_t {
    kGga = 0,
    kSatsinfoa = 1,
    kHeadinga = 2,
    kGst = 3,
    kRmc = 4,
    kGsa = 5,
};

bool starts_with(std::span<const uint8_t> frame, std::string_view prefix) noexcept
{
    if (frame.size() < prefix.size()) {
        return false;
    }
    return std::memcmp(frame.data(), prefix.data(), prefix.size()) == 0;
}

bool nmea_sentence_is(std::span<const uint8_t> frame, std::string_view tag3) noexcept
{
    if ((frame.size() < kNmeaHeaderLen) || (frame[0] != static_cast<uint8_t>('$')) ||
        (frame[kNmeaCommaPos] != static_cast<uint8_t>(','))) {
        return false;
    }
    const auto tag_span = frame.subspan(kNmeaTalkerEnd, kNmeaTagLen);
    return std::memcmp(tag_span.data(), tag3.data(), kNmeaTagLen) == 0;
}

} // namespace

const char *gnss_type_name(size_t type_idx) noexcept
{
    if (type_idx >= kTypeNames.size()) {
        return "?";
    }
    return kTypeNames[type_idx];
}

std::optional<uint8_t> gnss_type_from_msg_id(uint16_t msg_id) noexcept
{
    for (size_t i = 0; i < kTypeMsgIds.size(); ++i) {
        if (kTypeMsgIds[i] == msg_id) {
            return static_cast<uint8_t>(i);
        }
    }
    return std::nullopt;
}

bool GnssPushGate::due(uint32_t now_ms, uint32_t period_ms) const noexcept
{
    if (!armed || (period_ms == 0U)) {
        return true;
    }
    // 锚点可能领先 now 至多 slack，按有符号差处理回绕。
    const auto elapsed = static_cast<int64_t>(static_cast<int32_t>(now_ms - anchor_ms));
    const uint32_t slack = std::min(period_ms / 4U, kMaxSlackMs);
    return (elapsed + static_cast<int64_t>(slack)) >= static_cast<int64_t>(period_ms);
}

void GnssPushGate::on_pushed(uint32_t now_ms, uint32_t period_ms) noexcept
{
    const auto elapsed = static_cast<int64_t>(static_cast<int32_t>(now_ms - anchor_ms));
    if (armed && (period_ms != 0U) && (elapsed < 2 * static_cast<int64_t>(period_ms))) {
        anchor_ms += period_ms;
    } else {
        anchor_ms = now_ms;
    }
    armed = true;
}

std::optional<uint8_t> classify_gnss_sentence(std::span<const uint8_t> frame) noexcept
{
    if (frame.empty()) {
        return std::nullopt;
    }
    if (frame[0] == static_cast<uint8_t>('$')) {
        if (nmea_sentence_is(frame, "GGA")) {
            return static_cast<uint8_t>(SentenceType::kGga);
        }
        if (nmea_sentence_is(frame, "GST")) {
            return static_cast<uint8_t>(SentenceType::kGst);
        }
        if (nmea_sentence_is(frame, "RMC")) {
            return static_cast<uint8_t>(SentenceType::kRmc);
        }
        if (nmea_sentence_is(frame, "GSA")) {
            return static_cast<uint8_t>(SentenceType::kGsa);
        }
        return std::nullopt;
    }
    if (frame[0] == static_cast<uint8_t>('#')) {
        if (starts_with(frame, "#SATSINFOA,")) {
            return static_cast<uint8_t>(SentenceType::kSatsinfoa);
        }
        if (starts_with(frame, "#HEADINGA,")) {
            return static_cast<uint8_t>(SentenceType::kHeadinga);
        }
    }
    return std::nullopt;
}

bool GnssOutConfig::is_valid() const noexcept
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

fw::Result GnssOutConfig::apply_interval(size_t type_idx, int8_t interval) noexcept
{
    if (type_idx >= kGnssAppMsgCount) {
        return fw::Result::kInvalidArg;
    }
    const auto bit = static_cast<uint8_t>(1U << type_idx);
    if (interval == 0) {
        mask = static_cast<uint8_t>(mask & ~bit);
        intervals[type_idx] = 0;
        return fw::Result::kOk;
    }
    mask = static_cast<uint8_t>(mask | bit);
    intervals[type_idx] = interval;
    return fw::Result::kOk;
}

fw::Result GnssOutConfig::encode(std::span<uint8_t> out, size_t &len) const noexcept
{
    if (!is_valid()) {
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
    if (!parsed.is_valid()) {
        return fw::Result::kInvalidArg;
    }
    out = parsed;
    return fw::Result::kOk;
}

bool GnssLatestCache::update(uint8_t type_idx, std::span<const uint8_t> frame) noexcept
{
    if ((type_idx >= kGnssAppMsgCount) || frame.empty()) {
        return false;
    }
    const size_t cap = kGnssLatestCaps[type_idx];
    const size_t ofs = kGnssLatestOffsets[type_idx];
    const fw::LockGuard guard(lock_);
    if (frame.size() > cap) {
        ++oversize_drops_[type_idx];
        return false;
    }
    const auto slot_dst = std::span<uint8_t>{buf_}.subspan(ofs, frame.size());
    std::memcpy(slot_dst.data(), frame.data(), frame.size());
    lengths_[type_idx] = static_cast<uint16_t>(frame.size());
    ++updates_[type_idx];
    return true;
}

core::services::GnssQueryResult GnssLatestCache::query(uint8_t type_idx, std::span<uint8_t> dst) noexcept
{
    if (type_idx >= kGnssAppMsgCount) {
        return {};
    }
    const size_t ofs = kGnssLatestOffsets[type_idx];
    const fw::LockGuard guard(lock_);
    const size_t len = lengths_[type_idx];
    if (len == 0U) {
        return {};
    }
    if ((len > kGnssMaxQueryBytes) || (len > dst.size())) {
        ++query_oversize_[type_idx];
        return {.ok = false, .oversize = true, .len = 0U};
    }
    const auto slot_src = std::span<const uint8_t>{buf_}.subspan(ofs, len);
    std::memcpy(dst.data(), slot_src.data(), len);
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
        out[i].oversize_drops = oversize_drops_[i];
        out[i].query_oversize = query_oversize_[i];
    }
    return out;
}

void GnssAppSubState::set_subscription(uint32_t active_mask,
                                       const std::array<uint32_t, kGnssAppMsgCount> &periods_ms) noexcept
{
    const fw::LockGuard guard(lock_);
    state_.active_mask = active_mask;
    state_.periods_ms = periods_ms;
}

void GnssAppSubState::read_subscription(uint32_t &active_mask,
                                        std::array<uint32_t, kGnssAppMsgCount> &periods_ms) const noexcept
{
    const fw::LockGuard guard(lock_);
    active_mask = state_.active_mask;
    periods_ms = state_.periods_ms;
}

void GnssAppSubState::add_tx_sent(uint8_t type_idx) noexcept
{
    if (type_idx >= kGnssAppMsgCount) {
        return;
    }
    const fw::LockGuard guard(lock_);
    ++state_.tx_sent[type_idx];
}

void GnssAppSubState::add_skipped(uint8_t type_idx) noexcept
{
    if (type_idx >= kGnssAppMsgCount) {
        return;
    }
    const fw::LockGuard guard(lock_);
    ++state_.skipped[type_idx];
}

void GnssAppSubState::add_ring_oversize(uint8_t type_idx) noexcept
{
    if (type_idx >= kGnssAppMsgCount) {
        return;
    }
    const fw::LockGuard guard(lock_);
    ++state_.ring_oversize[type_idx];
}

void GnssAppSubState::set_query_oversize_total(uint32_t total) noexcept
{
    const fw::LockGuard guard(lock_);
    state_.query_oversize_total = total;
}

GnssAppSubSnapshot GnssAppSubState::snapshot() const noexcept
{
    const fw::LockGuard guard(lock_);
    return state_;
}

} // namespace app::data
