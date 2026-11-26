// ADR-0022 第一阶段 EEPROM 字段及手机预留帧的解码。
// 纯计算，线程安全，不访问硬件或 NVS。
#include "modules/storage/LegacyEeprom.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>

#include "proto/Bytes.hpp"
#include "settings/Registry.hpp"

namespace app::store {
namespace {

struct Slot {
    size_t offset;
    size_t width;
};

// ADR-0022 §2.5：V2.7 (2280 B) 与 V3.1 (2560 B) 共享布局。
constexpr struct {
    Slot sn{200U, 16U};
    Slot pn{216U, 16U};
    std::array<size_t, 4U> constellation{300U, 301U, 302U, 303U}; // GPS/BDS/GLO/GAL
    std::array<size_t, settings::kOutKeyCount> output{304U, 305U, 306U, 307U, 308U};
    size_t cutoff = 310U;
    size_t sbas = 314U;
    Slot jsonHost{320U, 32U};
    Slot jsonPort{352U, 4U};
    size_t tbEnable = 356U;
    Slot tbClient{360U, 16U};
    Slot tbUser{376U, 16U};
    Slot tbPassword{392U, 16U};
    Slot tbHost{408U, 32U};
    Slot tbPort{440U, 4U};
    // APN/NTRIP（0–147、444–460）及围栏（512 起）由后续卡处理。
} kLayout{};

constexpr uint8_t kErasedByte = 0xFFU;
constexpr uint8_t kAsciiDelete = 0x7FU;
constexpr int32_t kSecondMs = 1000;

/**
 * @brief 提取有界文本；空串、擦除态或控制字符按未设置处理。
 * @param blob: 长度已确认的 EEPROM。
 * @param slot: 源字段偏移及宽度。
 * @retval EEPROM 内文本的视图，未设置时为空。
 */
std::string_view readText(std::span<const uint8_t> blob, Slot slot) noexcept
{
    const auto field = blob.subspan(slot.offset, slot.width);
    const auto end = std::ranges::find(field, 0U);
    const auto text = std::span{field.begin(), end};
    if (std::ranges::all_of(text, [](uint8_t byte) { return byte == kErasedByte; }) ||
        std::ranges::any_of(text,
                            [](uint8_t byte) { return byte < ' ' || byte == kAsciiDelete; })) {
        return {};
    }
    // char 可以查看 byte 缓冲的对象表示，长度已限制在字段内。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

/**
 * @brief 提取两个出厂身份字段，SN 走 factory.sn 的 Registry 校验。
 * @param blob: 长度已确认的 EEPROM。
 * @param[out] fields: 身份数据和统计。
 */
void decodeIdentity(std::span<const uint8_t> blob, LegacyFields &fields) noexcept
{
    const auto sn = readText(blob, kLayout.sn);
    if (sn.empty()) {
        ++fields.skipped;
    } else {
        const auto &spec = *settings::keyAt(settings::KeyId::kFactorySn);
        settings::Domain domain{};
        spec.domain({}, false, domain);
        settings::CanonicalValue value{};
        if (settings::validateValue(spec, domain, sn, value) == settings::Error::kOk) {
            std::memcpy(fields.sn.data(), value.bytes.data(), value.size);
        } else {
            ++fields.invalid;
        }
    }
    const auto pn = readText(blob, kLayout.pn);
    if (pn.empty()) {
        ++fields.skipped;
    } else if (std::ranges::any_of(pn, [](char ch) { return ch < ' ' || ch > '~'; })) {
        // C6 定长 PN 接口接受可打印 ASCII，包括空格。
        ++fields.invalid;
    } else {
        std::memcpy(fields.pn.data(), pn.data(), pn.size());
    }
}

/**
 * @brief 将旧布尔字段补入默认值并记录存在性。
 * @param byte: EEPROM 原值。 @param[out] value: 目标帧中的字段。
 * @param[in,out] count: 对应目标键的有效源字段数。
 * @param[in,out] skipped: 未设置字段数。
 */
void decodeFlag(uint8_t byte, uint8_t &value, uint8_t &count, uint8_t &skipped) noexcept
{
    if (byte <= 1U) {
        value = byte;
        ++count;
    } else {
        ++skipped;
    }
}

/**
 * @brief 解码星座、截止角和 SBAS 到默认卫星配置；探测后由现有能力掩码屏蔽不支持的 SBAS。
 * @param blob: 长度已确认的 EEPROM。 @param[out] fields: 卫星配置及统计。
 * @retval true 规范帧通过 Registry 卫星配置校验。
 */
bool decodeSatellite(std::span<const uint8_t> blob, LegacyFields &fields) noexcept
{
    auto &sat = fields.satellite;
    auto &count = fields.satelliteCount;
    const std::array flags{&sat.gps, &sat.bds, &sat.glo, &sat.gal};
    for (size_t i = 0U; i < flags.size(); ++i) {
        decodeFlag(blob[kLayout.constellation[i]], *flags[i], count, fields.skipped);
    }
    const int32_t cutoff =
        std::bit_cast<int32_t>(proto::ByteReader{blob.subspan(kLayout.cutoff)}.u32());
    if (cutoff <= 0) {
        ++fields.skipped;
    } else if (cutoff > settings::kSatCutoffMaxDeg) {
        ++fields.invalid;
    } else {
        // 截止角已限制为 1..90，可无损缩窄。
        sat.cutoffDeg = static_cast<uint8_t>(cutoff);
        ++count;
    }
    decodeFlag(blob[kLayout.sbas], sat.sbas, count, fields.skipped);
    if (count == 0U) {
        return true;
    }

    std::array<char, sizeof("90,1,1,1,1,1")> text{};
    const int len =
        std::snprintf(text.data(), text.size(), "%u,%u,%u,%u,%u,%u",
                      static_cast<unsigned>(sat.cutoffDeg), static_cast<unsigned>(sat.gps),
                      static_cast<unsigned>(sat.glo), static_cast<unsigned>(sat.bds),
                      static_cast<unsigned>(sat.gal), static_cast<unsigned>(sat.sbas));
    if (len < 0 || static_cast<size_t>(len) >= text.size()) {
        return false;
    }
    const auto &spec = *settings::keyAt(settings::KeyId::kGnssSatTrack);
    const settings::GnssCaps caps{.valid = true, .constMask = core::kSatCapAll};
    settings::Domain domain{};
    spec.domain(caps, true, domain);
    settings::CanonicalValue value{};
    // 已确认格式化成功，len 是缓冲内的非负字节数。
    return settings::validateValue(spec, domain, {text.data(), static_cast<size_t>(len)}, value) ==
           settings::Error::kOk;
}

/**
 * @brief 为正频率选择距 1000/Hz 最近的可编码、可与导航周期整除的正周期。
 * @param hz: 旧频率，1..127。
 * @param navPeriod: 导航轨迹周期，1000 ms 的正因子。
 * @param msg: 目标输出语句。
 * @retval 取整后周期；误差相同时取较长周期。
 */
int32_t nearestPeriod(uint8_t hz, int32_t navPeriod, gnss::OutputMsg msg) noexcept
{
    // 已支持的 GSA/GSV 最短正周期为 1000 ms；其余取探测前可编码的亚秒周期。
    if (msg == gnss::OutputMsg::kGsa || msg == gnss::OutputMsg::kGsv) {
        return kSecondMs;
    }
    constexpr std::array<int32_t, 4U> kSubSecondPeriods{50, 100, 200, 500};
    int32_t nearest = kSecondMs;
    int32_t error = kSecondMs * (hz - 1);
    for (const int32_t candidate : kSubSecondPeriods) {
        const int32_t diff = std::abs(candidate * hz - kSecondMs);
        if (candidate % navPeriod == 0 &&
            (diff < error || (diff == error && candidate > nearest))) {
            nearest = candidate;
            error = diff;
        }
    }
    return nearest;
}

/**
 * @brief 将五个旧频率贴到统一输出配置上，其他输出保留缺省。
 * @param blob: 长度已确认的 EEPROM。 @param navPeriod: 已校验的定位周期。
 * @param[out] fields: 输出配置及统计。 @retval true 所有选中周期都可编码。
 */
bool decodeOutput(std::span<const uint8_t> blob, int32_t navPeriod, LegacyFields &fields) noexcept
{
    constexpr std::array kMessages{gnss::OutputMsg::kGga, gnss::OutputMsg::kGsa,
                                   gnss::OutputMsg::kGsv, gnss::OutputMsg::kGst,
                                   gnss::OutputMsg::kRmc};
    for (size_t i = 0U; i < kMessages.size(); ++i) {
        const uint8_t raw = blob[kLayout.output[i]];
        if (raw == kErasedByte) {
            ++fields.skipped;
            continue;
        }
        const auto hz = std::bit_cast<int8_t>(raw);
        const auto msg = kMessages[i];
        // 正 int8_t 转换为 uint8_t 无损。旧固件把有符号负值当关闭。
        const int32_t period = hz > 0 ? nearestPeriod(static_cast<uint8_t>(hz), navPeriod, msg) : 0;
        if (hz > 0 && period * hz != kSecondMs) {
            ++fields.invalid;
        }
        const auto index = msg == gnss::OutputMsg::kGsv ? gnss::OutputMsg::kSatsInfo : msg;
        int8_t interval = 0;
        if (!settings::periodMsToInterval(period, interval) ||
            !fw::isOk(fields.output.applyInterval(static_cast<size_t>(index), interval))) {
            return false;
        }
        ++fields.outputCount;
    }
    return true;
}

/**
 * @brief 将旧文本写入已清零的等宽手机载荷字段。
 * @param blob: 长度已确认的 EEPROM。 @param slot: 源字段。
 * @param[out] frame: 目的载荷。 @param offset: 帧内字段位置。
 * @retval true 字段有值且已复制。
 */
bool copyText(std::span<const uint8_t> blob, Slot slot, std::span<uint8_t> frame,
              size_t offset) noexcept
{
    const auto text = readText(blob, slot);
    if (text.empty()) {
        return false;
    }
    std::memcpy(frame.subspan(offset, slot.width).data(), text.data(), text.size());
    return true;
}

/**
 * @brief 复制未擦除的 int32 小端字段，保留原有 4 字节表示。
 * @param blob: 长度已确认的 EEPROM。 @param slot: 源字段。
 * @param[out] frame: 目的载荷。 @param offset: 帧内字段位置。
 * @retval true 字段有值且已复制。
 */
bool copyPort(std::span<const uint8_t> blob, Slot slot, std::span<uint8_t> frame,
              size_t offset) noexcept
{
    const auto bytes = blob.subspan(slot.offset, slot.width);
    if (std::ranges::all_of(bytes, [](uint8_t byte) { return byte == kErasedByte; })) {
        return false;
    }
    std::ranges::copy(bytes, frame.subspan(offset, slot.width).begin());
    return true;
}

/**
 * @brief 构造 Nav 0x32/0x33 载荷，运行时状态和未设置字段保留为零。
 * @param blob: 长度已确认的 EEPROM。 @param[out] fields: 两个载荷及统计。
 */
void decodePhoneFrames(std::span<const uint8_t> blob, LegacyFields &fields) noexcept
{
    constexpr size_t kJsonPortOffset = 32U;
    constexpr size_t kTbClientOffset = 2U;
    constexpr size_t kTbUserOffset = kTbClientOffset + kLayout.tbClient.width;
    constexpr size_t kTbPasswordOffset = kTbUserOffset + kLayout.tbUser.width;
    constexpr size_t kTbHostOffset = kTbPasswordOffset + kLayout.tbPassword.width;
    constexpr size_t kTbPortOffset = kTbHostOffset + kLayout.tbHost.width;
    constexpr unsigned kPhoneFieldCountWithoutEnable = 7U;

    fields.jsonCount = copyText(blob, kLayout.jsonHost, fields.json, 0U) +
                       copyPort(blob, kLayout.jsonPort, fields.json, kJsonPortOffset);
    fields.thingsboardCount =
        copyText(blob, kLayout.tbClient, fields.thingsboard, kTbClientOffset) +
        copyText(blob, kLayout.tbUser, fields.thingsboard, kTbUserOffset) +
        copyText(blob, kLayout.tbPassword, fields.thingsboard, kTbPasswordOffset) +
        copyText(blob, kLayout.tbHost, fields.thingsboard, kTbHostOffset) +
        copyPort(blob, kLayout.tbPort, fields.thingsboard, kTbPortOffset);
    // 2 个 JSON 字段与 5 个 TB 载荷字段，差值最多 7。
    fields.skipped += static_cast<uint8_t>(kPhoneFieldCountWithoutEnable - fields.jsonCount -
                                           fields.thingsboardCount);
    decodeFlag(blob[kLayout.tbEnable], fields.thingsboard.front(), fields.thingsboardCount,
               fields.skipped);
}

} // namespace

std::optional<LegacyFields> decodeLegacyEeprom(std::span<const uint8_t> blob,
                                               uint8_t rateHz) noexcept
{
    if (!legacyEepromLength(blob.size()) || rateHz == 0U || kSecondMs % rateHz != 0) {
        return std::nullopt;
    }
    LegacyFields fields{};
    decodeIdentity(blob, fields);
    if (!decodeSatellite(blob, fields) || !decodeOutput(blob, kSecondMs / rateHz, fields)) {
        return std::nullopt;
    }
    decodePhoneFrames(blob, fields);
    return fields;
}

} // namespace app::store
