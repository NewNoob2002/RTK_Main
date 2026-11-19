// GGA 质量门控实现（Q1）：解析 NMEA GGA 第 6（定位质量）与第 7（卫星数）字段并执行 1 坏即断 / 3 好放行迟滞。
// 线程安全：否（由单任务串行调用）；ISR 安全：否；不使用堆、不阻塞、无浮点。
#include "GgaQualityGate.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace gnss {
namespace {

constexpr uint8_t kMinFixQuality = 1U;
constexpr uint8_t kMaxFixQuality = 8U; // NMEA 0183 GGA 有效质量指示 1..8（0 为无解）
constexpr uint32_t kDecimalBase = 10U;
constexpr uint32_t kMaxParseValue = 1000U; // 卫星数与质量指示均远小于 1000，防乘法溢出
constexpr size_t kGgaQualityField = 6U;
constexpr size_t kGgaSatsField = 7U;

/**
 * @brief  从 NMEA 文本切片按逗号取下一个字段（遇到 '*' / '\r' / '\n' 终止）
 * @param  body: 去掉 '$' 与尾部的正文
 * @param[in,out] pos: 当前扫描位置，调用后推进到下一字段起点
 * @param[out] field: 本字段视图
 * @retval true 成功取得一个字段
 * @retval false 已到达正文末尾，没有更多字段
 */
bool nextField(std::span<const uint8_t> body, size_t &pos, std::span<const uint8_t> &field) noexcept
{
    if (pos > body.size()) {
        return false;
    }
    const size_t start = pos;
    while ((pos < body.size()) && (body[pos] != static_cast<uint8_t>(','))) {
        ++pos;
    }
    field = body.subspan(start, pos - start);
    if (pos < body.size()) {
        ++pos; // 跳过逗号，指向下一字段起点（末尾逗号允许产生空字段）
    } else {
        pos = body.size() + 1U; // 哨兵位置，标记后续不再有字段
    }
    return true;
}

/**
 * @brief  解析非空无符号十进制字段（允许 NMEA 标准前导零如 "08"，拒绝符号、空格与溢出）
 * @param  field: 字段字节视图
 * @param[out] out: 成功时写入解析结果
 * @retval true 解析成功
 * @retval false 字段为空、含非数字字符或超过上限
 */
bool parseNonEmptyUint(std::span<const uint8_t> field, uint32_t &out) noexcept
{
    if (field.empty()) {
        return false;
    }
    uint32_t value = 0U;
    for (const uint8_t ch : field) {
        if ((ch < static_cast<uint8_t>('0')) || (ch > static_cast<uint8_t>('9'))) {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(ch - static_cast<uint8_t>('0'));
        if (value > ((kMaxParseValue - digit) / kDecimalBase)) {
            return false;
        }
        value = (value * kDecimalBase) + digit;
    }
    out = value;
    return true;
}

/**
 * @brief  剥离 NMEA 帧头 '$' 与帧尾 '*hh'/'\r'/'\n'，并验证地址字段以 "GGA" 结尾
 * @param  frame: 原始帧字节
 * @param[out] body: 剥离后的正文切片
 * @retval true 具备 GGA 句型框架
 * @retval false 帧过短或不是 GGA 语句
 */
bool stripGgaBody(std::span<const uint8_t> frame, std::span<const uint8_t> &body) noexcept
{
    if (frame.empty()) {
        return false;
    }
    size_t start = 0U;
    if (frame[0] == static_cast<uint8_t>('$')) {
        start = 1U;
    }
    size_t end = start;
    while (end < frame.size()) {
        const uint8_t ch = frame[end];
        if ((ch == static_cast<uint8_t>('*')) || (ch == static_cast<uint8_t>('\r')) ||
            (ch == static_cast<uint8_t>('\n'))) {
            break;
        }
        ++end;
    }
    if (end <= start) {
        return false;
    }
    body = frame.subspan(start, end - start);
    return true;
}

/**
 * @brief  校验 NMEA 地址字段是否为 GGA 语句（如 GPGGA/GNGGA/BDGGA）
 * @param  addr: 字段 0 字节视图
 * @retval true 长度至少 3 且以 "GGA" 结尾
 * @retval false 非 GGA 地址
 */
bool isGgaAddress(std::span<const uint8_t> addr) noexcept
{
    constexpr size_t kSuffixLen = 3U;
    if (addr.size() < kSuffixLen) {
        return false;
    }
    const size_t base = addr.size() - kSuffixLen;
    return (addr[base] == static_cast<uint8_t>('G')) &&
           (addr[base + 1U] == static_cast<uint8_t>('G')) &&
           (addr[base + 2U] == static_cast<uint8_t>('A'));
}

/**
 * @brief  解析 GGA 的定位质量（字段 6）与参与解算卫星数（字段 7）
 * @param  frame: 原始 GGA 帧字节
 * @param[out] quality: 定位质量指示（仅返回 true 时有效）
 * @param[out] satsUsed: 使用卫星数（仅返回 true 时有效）
 * @retval true 两个字段均存在且为合法无符号十进制数
 * @retval false 非 GGA、字段缺失、为空或含非法字符
 */
bool parseGgaQualityAndSats(std::span<const uint8_t> frame, uint32_t &quality,
                            uint32_t &satsUsed) noexcept
{
    std::span<const uint8_t> body{};
    if (!stripGgaBody(frame, body)) {
        return false;
    }
    size_t pos = 0U;
    std::span<const uint8_t> field{};
    if (!nextField(body, pos, field) || !isGgaAddress(field)) {
        return false;
    }
    std::span<const uint8_t> qualityField{};
    std::span<const uint8_t> satsField{};
    for (size_t idx = 1U; idx <= kGgaSatsField; ++idx) {
        if (!nextField(body, pos, field)) {
            return false;
        }
        if (idx == kGgaQualityField) {
            qualityField = field;
        } else if (idx == kGgaSatsField) {
            satsField = field;
        }
    }
    return parseNonEmptyUint(qualityField, quality) && parseNonEmptyUint(satsField, satsUsed);
}

} // namespace

bool GgaQualityGate::setConfig(const GgaGateConfig &cfg) noexcept
{
    if (!GgaGateConfig::isValid(cfg)) {
        return false;
    }
    cfg_ = cfg;
    if (!cfg_.enabled) {
        goodStreak_ = 0U;
        if (blocked_) {
            blocked_ = false;
            ++gateTrans_;
        }
    } else if (blocked_) {
        // 阻断期间调整阈值，重置连续达标计数，按新阈值重新计数。
        goodStreak_ = 0U;
    }
    return true;
}

void GgaQualityGate::reset() noexcept
{
    blocked_ = false;
    goodStreak_ = 0U;
    ggaBlocked_ = 0U;
    gateTrans_ = 0U;
}

bool GgaQualityGate::evaluate(std::span<const uint8_t> ggaFrame) noexcept
{
    if (!cfg_.enabled) {
        return true;
    }
    uint32_t quality = 0U;
    uint32_t satsUsed = 0U;
    const bool parsed = parseGgaQualityAndSats(ggaFrame, quality, satsUsed);
    const bool good = parsed && (quality >= kMinFixQuality) && (quality <= kMaxFixQuality) &&
                      (satsUsed >= cfg_.minSats);
    if (!blocked_) {
        if (good) {
            return true;
        }
        // 放行态收到 1 条不达标 GGA 立即阻断，避免把坏 GGA 交给 caster。
        blocked_ = true;
        goodStreak_ = 0U;
        ++gateTrans_;
        ++ggaBlocked_;
        return false;
    }
    if (!good) {
        goodStreak_ = 0U;
        ++ggaBlocked_;
        return false;
    }
    ++goodStreak_;
    if (goodStreak_ >= GgaGateConfig::kReleaseStreak) {
        blocked_ = false;
        goodStreak_ = 0U;
        ++gateTrans_;
        return true;
    }
    ++ggaBlocked_;
    return false;
}

GgaGateStats GgaQualityGate::stats() const noexcept
{
    return {
        .enabled = cfg_.enabled,
        .blocked = blocked_,
        .minSats = cfg_.minSats,
        .ggaBlocked = ggaBlocked_,
        .gateTrans = gateTrans_,
    };
}

} // namespace gnss
