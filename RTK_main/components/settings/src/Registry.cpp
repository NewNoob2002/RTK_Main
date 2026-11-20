// 设置键编译期注册表、值解析校验与命令编码器实现（ADR-0018 §2.3）。
// 纯函数，无全局可变状态，不阻塞、不分配堆，线程/ISR 安全。
#include "settings/Registry.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace settings {
namespace {

constexpr size_t kMacTextBytes = 17U; // "AA:BB:CC:DD:EE:FF"
constexpr int kHexLetterBase = 10;
constexpr uint32_t kDecimalBase = 10U;
constexpr int32_t kMsPerSecond = 1000;
constexpr int32_t kMaxSlowPeriodSec = 127;
constexpr size_t kOutMsgCount = 6U;
constexpr size_t kAppIdxGga = 0U;
constexpr size_t kAppIdxGsv = 1U;
constexpr size_t kAppIdxGst = 3U;
constexpr size_t kAppIdxRmc = 4U;
constexpr size_t kAppIdxGsa = 5U;

/**
 * @brief  判断字符是否为 ASCII 字母或数字
 * @param  c: 待判断字符
 * @retval true  '0'..'9'、'A'..'Z' 或 'a'..'z'
 * @retval false 其余字符
 */
bool isAsciiAlnum(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/**
 * @brief  把十六进制字符转为 0..15
 * @param  c: 待转换字符
 * @retval 0..15 合法十六进制字符
 * @retval -1    非十六进制字符
 */
int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + kHexLetterBase;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + kHexLetterBase;
    }
    return -1;
}

/**
 * @brief  按十进制严格解析 int32（不接受前导零、空格、正号与负零）
 * @param  text:  输入文本
 * @param[out] value: 输出数值；失败不写
 * @retval true  合法 int32
 * @retval false 空串、格式错误、溢出或负零
 */
bool parseStrictInt(std::string_view text, int32_t &value) noexcept
{
    if (text.empty()) {
        return false;
    }
    size_t pos = 0U;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        pos = 1U;
        if (pos == text.size()) {
            return false;
        }
    }
    if (text[pos] == '0' && (text.size() - pos) != 1U) {
        return false;
    }
    constexpr uint32_t kPositiveLimit = 2147483647U;
    constexpr uint32_t kNegativeLimit = 2147483648U;
    const uint32_t limit = negative ? kNegativeLimit : kPositiveLimit;
    uint32_t magnitude = 0U;
    for (; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (c < '0' || c > '9') {
            return false;
        }
        // 已验证 c 在 '0'..'9'，转换到 uint32_t 无损。
        const auto digit = static_cast<uint32_t>(c - '0');
        if (magnitude > (limit - digit) / kDecimalBase) {
            return false;
        }
        magnitude = magnitude * kDecimalBase + digit;
    }
    if (negative && magnitude == 0U) {
        return false;
    }
    if (!negative) {
        // 已检查 magnitude <= INT32_MAX，转有符号无损。
        value = static_cast<int32_t>(magnitude);
        return true;
    }
    if (magnitude == kNegativeLimit) {
        value = std::numeric_limits<int32_t>::min();
        return true;
    }
    // 已检查 magnitude < 2^31，转有符号取负无溢出。
    value = -static_cast<int32_t>(magnitude);
    return true;
}

/**
 * @brief  校验枚举键的值是否在当前允许集合内
 * @param  allowed: 允许的整数列表
 * @param  text:    文本输入
 * @param[out] out: 成功时写入规范整数；失败保持原内容
 * @retval Error::kOk       通过
 * @retval Error::kBadValue 格式错误或不在集合内
 */
Error validateEnum(std::span<const int32_t> allowed, std::string_view text,
                   CanonicalValue &out) noexcept
{
    int32_t number = 0;
    if (!parseStrictInt(text, number)) {
        return Error::kBadValue;
    }
    for (const int32_t candidate : allowed) {
        if (candidate == number) {
            out.kind = CanonicalKind::kInt;
            out.number = number;
            return Error::kOk;
        }
    }
    return Error::kBadValue;
}

/**
 * @brief  校验布尔键（"0" 或 "1"）
 * @param  text:    文本输入
 * @param[out] out: 成功时写入 0/1；失败保持原内容
 * @retval Error::kOk       通过
 * @retval Error::kBadValue 非 "0"/"1"
 */
Error validateBool(std::string_view text, CanonicalValue &out) noexcept
{
    if (text == "0" || text == "1") {
        out.kind = CanonicalKind::kInt;
        out.number = (text == "1") ? 1 : 0;
        return Error::kOk;
    }
    return Error::kBadValue;
}

/**
 * @brief  校验闭区间整数键
 * @param  minValue: 最小值（含）
 * @param  maxValue: 最大值（含）
 * @param  text:     文本输入
 * @param[out] out:  成功时写入规范整数；失败保持原内容
 * @retval Error::kOk       通过
 * @retval Error::kBadValue 格式错误或越界
 */
Error validateIntRange(int32_t minValue, int32_t maxValue, std::string_view text,
                       CanonicalValue &out) noexcept
{
    int32_t number = 0;
    if (!parseStrictInt(text, number) || number < minValue || number > maxValue) {
        return Error::kBadValue;
    }
    out.kind = CanonicalKind::kInt;
    out.number = number;
    return Error::kOk;
}

/**
 * @brief  校验并解析 MAC 文本（AA:BB:CC:DD:EE:FF，大小写不敏感）
 * @param  text:    文本输入
 * @param[out] out: 成功时写入 6 字节规范值；失败保持原内容
 * @retval Error::kOk       通过
 * @retval Error::kBadValue 格式或字符非法
 */
Error validateMac(std::string_view text, CanonicalValue &out) noexcept
{
    if (text.size() != kMacTextBytes) {
        return Error::kBadValue;
    }
    std::array<uint8_t, kMacBytes> bytes{};
    for (size_t i = 0U; i < kMacBytes; ++i) {
        const size_t base = i * 3U;
        const int highValue = hexValue(text[base]);
        const int lowValue = hexValue(text[base + 1U]);
        if (highValue < 0 || lowValue < 0) {
            return Error::kBadValue;
        }
        if ((i + 1U) < kMacBytes && text[base + 2U] != ':') {
            return Error::kBadValue;
        }
        // 半字节已验证为 0..15，拼成 0..255 后窄化到 uint8_t 安全。
        const auto high = static_cast<uint32_t>(highValue);
        const auto low = static_cast<uint32_t>(lowValue);
        bytes[i] = static_cast<uint8_t>((high << 4U) | low);
    }
    out.bytes.fill(0U);
    std::copy_n(bytes.begin(), kMacBytes, out.bytes.begin());
    out.kind = CanonicalKind::kBytes;
    out.size = static_cast<uint8_t>(kMacBytes);
    return Error::kOk;
}

/**
 * @brief  校验 SN 文本（1–16 个 [0-9A-Za-z-]）
 * @param  text:    文本输入
 * @param[out] out: 成功时写入字符字节串；失败保持原内容
 * @retval Error::kOk       通过
 * @retval Error::kBadValue 为空、超长或含非法字符
 */
Error validateSn(std::string_view text, CanonicalValue &out) noexcept
{
    if (text.empty() || text.size() > kMaxSnBytes) {
        return Error::kBadValue;
    }
    for (const char c : text) {
        if (!isAsciiAlnum(c) && c != '-') {
            return Error::kBadValue;
        }
    }
    out.bytes.fill(0U);
    for (size_t i = 0U; i < text.size(); ++i) {
        // 已检查全部字符为可见 ASCII，转 uint8_t 无损。
        out.bytes[i] = static_cast<uint8_t>(text[i]);
    }
    out.kind = CanonicalKind::kBytes;
    out.size = static_cast<uint8_t>(text.size());
    return Error::kOk;
}

/**
 * @brief  解析定长十六进制文本到目标字节切片（大小写不敏感）
 * @param  text: 十六进制文本（长度须等于 2 * dst.size()）
 * @param[out] dst: 目标字节切片
 * @retval true  全部字符合法并写入 dst
 * @retval false 长度不符或含非十六进制字符
 */
bool parseFixedHex(std::string_view text, std::span<uint8_t> dst) noexcept
{
    if (text.size() != (dst.size() * 2U)) {
        return false;
    }
    for (size_t i = 0U; i < dst.size(); ++i) {
        const int hi = hexValue(text[i * 2U]);
        const int lo = hexValue(text[(i * 2U) + 1U]);
        if ((hi < 0) || (lo < 0)) {
            return false;
        }
        // 半字节在 0..15 范围内，拼接后窄化到 uint8_t 安全。
        dst[i] =
            static_cast<uint8_t>((static_cast<uint32_t>(hi) << 4U) | static_cast<uint32_t>(lo));
    }
    return true;
}

/**
 * @brief  校验卫星跟踪设置（"<cutoffDeg>,<gps>,<glo>,<bds>,<gal>,<sbas>"）并核对星座能力掩码
 * @param  dom:  当前卫星跟踪取值域（mask 为允许启用的星座位掩码）
 * @param  text: 逗号分隔的 6 个整数
 * @param[out] out: 成功时写入 6 字节规范值；失败保持原内容
 * @retval Error::kOk       校验通过
 * @retval Error::kBadValue 格式错误、越界或尝试开启硬件不支持的星座
 */
Error validateSatTrack(const Domain &dom, std::string_view text, CanonicalValue &out) noexcept
{
    std::array<int32_t, kSatTrackBytes> fields{};
    std::string_view rest = text;
    for (size_t i = 0U; i < kSatTrackBytes; ++i) {
        const size_t sep = rest.find(',');
        const bool last = (i + 1U) == kSatTrackBytes;
        if (last ? (sep != std::string_view::npos) : (sep == std::string_view::npos)) {
            return Error::kBadValue;
        }
        const std::string_view part = last ? rest : rest.substr(0U, sep);
        if (!parseStrictInt(part, fields[i])) {
            return Error::kBadValue;
        }
        if (!last) {
            rest.remove_prefix(sep + 1U);
        }
    }
    if ((fields[0] < 0) || (fields[0] > kSatCutoffMaxDeg)) {
        return Error::kBadValue;
    }
    constexpr std::array<uint8_t, 5U> kBits{
        const_bit::kGps, const_bit::kGlo, const_bit::kBds, const_bit::kGal, const_bit::kSbas,
    };
    for (size_t i = 0U; i < kBits.size(); ++i) {
        const int32_t flag = fields[i + 1U];
        if ((flag != 0) && (flag != 1)) {
            return Error::kBadValue;
        }
        if ((flag == 1) && ((dom.mask & kBits[i]) == 0U)) {
            return Error::kBadValue;
        }
    }
    out.bytes.fill(0U);
    for (size_t i = 0U; i < kSatTrackBytes; ++i) {
        // 已验证 cutoff 在 0..90、星座在 0..1，窄化到 uint8_t 安全。
        out.bytes[i] = static_cast<uint8_t>(fields[i]);
    }
    out.kind = CanonicalKind::kBytes;
    out.size = static_cast<uint8_t>(kSatTrackBytes);
    return Error::kOk;
}

/**
 * @brief  校验手机天线高（16 位十六进制，小端 IEEE 754 binary64，范围 0..10 m）
 * @param  text: 16 位十六进制字符串
 * @param[out] out: 成功时写入 8 字节规范值；失败保持原内容
 * @retval Error::kOk       校验通过
 * @retval Error::kBadValue 长度、十六进制格式或数值越界/NaN/Inf
 */
Error validateHeight(std::string_view text, CanonicalValue &out) noexcept
{
    static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == kHeightBytes);
    std::array<uint8_t, kHeightBytes> raw{};
    if (!parseFixedHex(text, raw)) {
        return Error::kBadValue;
    }
    uint64_t bits = 0U;
    for (size_t i = 0U; i < kHeightBytes; ++i) {
        bits |= static_cast<uint64_t>(raw[i]) << (i * 8U);
    }
    constexpr double kMaxHeightMeters = 10.0;
    const auto meters = std::bit_cast<double>(bits);
    if (!(meters >= 0.0 && meters <= kMaxHeightMeters)) {
        return Error::kBadValue;
    }
    out.bytes.fill(0U);
    std::copy_n(raw.begin(), kHeightBytes, out.bytes.begin());
    out.kind = CanonicalKind::kBytes;
    out.size = static_cast<uint8_t>(kHeightBytes);
    return Error::kOk;
}

/**
 * @brief  校验手机 0x08 Nav SN+PN 组合字段（64 位十六进制，前 16 B 为 SN，后 16 B 为 PN）
 * @param  text: 64 位十六进制字符串
 * @param[out] out: 成功时写入 32 字节规范值；失败保持原内容
 * @retval Error::kOk       校验通过
 * @retval Error::kBadValue 格式错误、SN 为空或 SN/PN 含换行符
 */
Error validateSnPn(std::string_view text, CanonicalValue &out) noexcept
{
    std::array<uint8_t, kSnPnBytes> raw{};
    if (!parseFixedHex(text, raw)) {
        return Error::kBadValue;
    }
    size_t snLen = 0U;
    while ((snLen < kMaxSnBytes) && (raw[snLen] != 0U)) {
        if ((raw[snLen] == '\r') || (raw[snLen] == '\n')) {
            return Error::kBadValue;
        }
        ++snLen;
    }
    if (snLen == 0U) {
        return Error::kBadValue;
    }
    for (size_t i = kMaxSnBytes; i < kSnPnBytes; ++i) {
        if (raw[i] == 0U) {
            break;
        }
        if ((raw[i] == '\r') || (raw[i] == '\n')) {
            return Error::kBadValue;
        }
    }
    std::copy_n(raw.begin(), kSnPnBytes, out.bytes.begin());
    out.kind = CanonicalKind::kBytes;
    out.size = static_cast<uint8_t>(kSnPnBytes);
    return Error::kOk;
}

// ---- 取值域查询回调 ----

/**
 * @brief  生成 `gnss.rate` 的动态取值域（来自 GNSS 能力快照）
 * @param  caps:        当前 GNSS 能力快照
 * @param  gnssEnabled: 固件是否启用 GNSS
 * @param[out] out:     取值域输出
 */
void domainGnssRate(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    out = Domain{.kind = KeyKind::kEnum};
    if (!gnssEnabled || !caps.valid || caps.navRateCount == 0U) {
        return;
    }
    const size_t count = std::min(static_cast<size_t>(caps.navRateCount), kMaxRateOptions);
    for (size_t i = 0U; i < count; ++i) {
        out.enumValues[i] = caps.navRatesHz[i];
    }
    out.enumCount = static_cast<uint8_t>(count);
    out.supported = true;
}

/**
 * @brief  按 OutKeyIndex 生成单条 `gnss.out.*` 的动态取值域
 * @tparam Idx: OutKeyIndex 下标
 * @param  caps:        当前 GNSS 能力快照
 * @param  gnssEnabled: 固件是否启用 GNSS
 * @param[out] out:     取值域输出
 */
template <OutKeyIndex Idx>
void domainGnssOut(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    out = Domain{.kind = KeyKind::kEnum};
    if (!gnssEnabled || !caps.valid) {
        return;
    }
    const PeriodDomain &src = caps.outPeriods[static_cast<size_t>(Idx)];
    if (!src.supported || src.count == 0U) {
        return;
    }
    const size_t count = std::min(static_cast<size_t>(src.count), kMaxDomainValues);
    for (size_t i = 0U; i < count; ++i) {
        out.enumValues[i] = src.values[i];
    }
    out.enumCount = static_cast<uint8_t>(count);
    out.supported = true;
}

/**
 * @brief  生成 `gnss.gga_gate.enabled` 的取值域
 * @param  caps:        未使用
 * @param  gnssEnabled: 固件是否启用 GNSS
 * @param[out] out:     取值域输出
 */
void domainGgaGateEnabled(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    static_cast<void>(caps);
    out = Domain{.kind = KeyKind::kBool, .supported = gnssEnabled};
}

/**
 * @brief  生成 `gnss.gga_gate.min_sats` 的闭区间取值域（0..40）
 * @param  caps:        未使用
 * @param  gnssEnabled: 固件是否启用 GNSS
 * @param[out] out:     取值域输出
 */
void domainGgaGateMinSats(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    static_cast<void>(caps);
    out = Domain{.kind = KeyKind::kIntRange,
                 .supported = gnssEnabled,
                 .minValue = kGgaGateMinSatsMin,
                 .maxValue = kGgaGateMinSatsMax};
}

/**
 * @brief  按星座能力位生成 `gnss.const.*` 的动态取值域（v1 暂不支持，返回 supported=false）
 * @tparam Bit: const_bit 位掩码
 * @param  caps:        当前 GNSS 能力快照
 * @param  gnssEnabled: 固件是否启用 GNSS
 * @param[out] out:     取值域输出
 */
template <uint8_t Bit>
void domainGnssConst(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    out = Domain{.kind = KeyKind::kBool,
                 .supported = gnssEnabled && caps.valid && ((caps.constMask & Bit) != 0U)};
}

/**
 * @brief  生成静态布尔键（`bt.pair_next`）取值域
 * @param  caps:        未使用
 * @param  gnssEnabled: 未使用
 * @param[out] out:     取值域输出
 */
void domainStaticBool(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    static_cast<void>(caps);
    static_cast<void>(gnssEnabled);
    out = Domain{.kind = KeyKind::kBool, .supported = true};
}

/**
 * @brief  生成静态 MAC 键（`bt.wl.add`/`bt.wl.del`）取值域
 * @param  caps:        未使用
 * @param  gnssEnabled: 未使用
 * @param[out] out:     取值域输出
 */
void domainStaticMac(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    static_cast<void>(caps);
    static_cast<void>(gnssEnabled);
    out = Domain{.kind = KeyKind::kMac,
                 .supported = true,
                 .minValue = static_cast<int32_t>(kMacTextBytes),
                 .maxValue = static_cast<int32_t>(kMacTextBytes)};
}

/**
 * @brief  生成静态 SN 键（`factory.sn`）取值域
 * @param  caps:        未使用
 * @param  gnssEnabled: 未使用
 * @param[out] out:     取值域输出
 */
void domainStaticSn(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    static_cast<void>(caps);
    static_cast<void>(gnssEnabled);
    out = Domain{.kind = KeyKind::kSn, .supported = true, .minValue = 1, .maxValue = kMaxSnBytes};
}

/**
 * @brief  生成 `gnss.ppp` 的动态取值域（由接收机型号能力决定，CM680 不支持）
 * @param  caps:        当前 GNSS 能力快照
 * @param  gnssEnabled: 固件是否启用 GNSS
 * @param[out] out:     取值域输出
 */
void domainGnssPpp(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    out =
        Domain{.kind = KeyKind::kBool, .supported = gnssEnabled && caps.valid && caps.pppSupported};
}

/**
 * @brief  生成 `gnss.sat_track`（0x06 截止角与星座开关）的动态取值域
 * @param  caps:        当前 GNSS 能力快照
 * @param  gnssEnabled: 固件是否启用 GNSS
 * @param[out] out:     取值域输出（mask 保存支持的星座掩码）
 */
void domainGnssSatTrack(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    const bool supported = gnssEnabled && caps.valid && (caps.constMask != 0U);
    out = Domain{.kind = KeyKind::kSatTrack,
                 .supported = supported,
                 .mask = supported ? caps.constMask : uint8_t{0U},
                 .minValue = 0,
                 .maxValue = kSatCutoffMaxDeg};
}

/**
 * @brief  生成 `phone.height` 的静态取值域
 * @param  caps:        未使用
 * @param  gnssEnabled: 未使用
 * @param[out] out:     取值域输出
 */
void domainStaticHeight(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    static_cast<void>(caps);
    static_cast<void>(gnssEnabled);
    out = Domain{.kind = KeyKind::kHeight, .supported = true};
}

/**
 * @brief  生成 `phone.sn_pn` 的静态取值域
 * @param  caps:        未使用
 * @param  gnssEnabled: 未使用
 * @param[out] out:     取值域输出
 */
void domainStaticSnPn(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept
{
    static_cast<void>(caps);
    static_cast<void>(gnssEnabled);
    out = Domain{.kind = KeyKind::kSnPn, .supported = true};
}

// ---- 命令编码器 ----

/**
 * @brief  编码 `gnss.rate` → `GnssVerb::kSetRate`（1 字节频率 Hz）
 * @param  val: 规范整数（1..255）
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 参数越界
 */
bool encodeGnssRate(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    if (val.kind != CanonicalKind::kInt || val.number <= 0 ||
        val.number > std::numeric_limits<uint8_t>::max()) {
        return false;
    }
    cmd.verb = kVerbGnssSetRate;
    cmd.argLen = 1U;
    // 已校验 1..255，窄化到 uint8_t 安全。
    cmd.args[0] = static_cast<uint8_t>(val.number);
    return true;
}

/**
 * @brief  编码单条 `gnss.out.*` → `GnssSubVerb::kSetOut`（单项补丁）
 * @tparam AppIdx: App 输出语句下标（0..5，gsv 对应 1）
 * @param  val: 规范周期毫秒
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 周期无法编码或下标非法
 */
template <size_t AppIdx> bool encodeGnssOut(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    static_assert(AppIdx < kOutMsgCount, "AppIdx out of range");
    if (val.kind != CanonicalKind::kInt) {
        return false;
    }
    int8_t interval = 0;
    if (!periodMsToInterval(val.number, interval)) {
        return false;
    }
    cmd.verb = kVerbGnssSetOut;
    cmd.argLen = 2U;
    // 模板下标限制在已有语句内，间隔按二补码传给归属模块。
    cmd.args[0] = static_cast<uint8_t>(AppIdx);
    cmd.args[1] = static_cast<uint8_t>(interval);
    return true;
}

/**
 * @brief  编码 `gnss.gga_gate.enabled` → `GnssSubVerb::kSetGgaGate`
 * @param  val: 0/1 布尔整数
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 参数类型错误
 */
bool encodeGgaGateEnabled(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    if (val.kind != CanonicalKind::kInt) {
        return false;
    }
    cmd.verb = kVerbGnssSetGgaGate;
    cmd.argLen = kGgaGateCmdArgLen;
    cmd.args[0] = kGgaGateFlagEnabled;
    cmd.args[1] = (val.number != 0) ? 1U : 0U;
    cmd.args[2] = 0U;
    return true;
}

/**
 * @brief  编码 `gnss.gga_gate.min_sats` → `GnssSubVerb::kSetGgaGate`
 * @param  val: 0..40 整数
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 越界
 */
bool encodeGgaGateMinSats(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    if (val.kind != CanonicalKind::kInt || val.number < kGgaGateMinSatsMin ||
        val.number > kGgaGateMinSatsMax) {
        return false;
    }
    cmd.verb = kVerbGnssSetGgaGate;
    cmd.argLen = kGgaGateCmdArgLen;
    cmd.args[0] = kGgaGateFlagMinSats;
    cmd.args[1] = 0U;
    // 已限制在 0..40，窄化到 uint8_t 安全。
    cmd.args[2] = static_cast<uint8_t>(val.number);
    return true;
}

/**
 * @brief  编码白名单增/删命令（6 字节 MAC）
 * @tparam Verb: kVerbStorageWhitelistAdd 或 kVerbStorageWhitelistDel
 * @param  val: 6 字节 MAC 规范值
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 长度错误
 */
template <uint8_t Verb> bool encodeStorageMac(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    if (val.kind != CanonicalKind::kBytes || val.size != kMacBytes) {
        return false;
    }
    cmd.verb = Verb;
    cmd.argLen = static_cast<uint8_t>(kMacBytes);
    std::copy_n(val.bytes.begin(), kMacBytes, cmd.args.begin());
    return true;
}

/**
 * @brief  编码 `bt.pair_next` → `StorageVerb::kSetPairNext`
 * @param  val: 0/1 布尔整数
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 参数类型错误
 */
bool encodePairNext(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    if (val.kind != CanonicalKind::kInt) {
        return false;
    }
    cmd.verb = kVerbStorageSetPairNext;
    cmd.argLen = 1U;
    cmd.args[0] = (val.number != 0) ? 1U : 0U;
    return true;
}

/**
 * @brief  编码 `factory.sn` → `StorageVerb::kSaveSn`
 * @param  val: 1..16 字节 SN 字符
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 长度非法
 */
bool encodeFactorySn(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    if (val.kind != CanonicalKind::kBytes || val.size == 0U || val.size > cmd.args.size()) {
        return false;
    }
    cmd.verb = kVerbStorageSaveSn;
    cmd.argLen = val.size;
    std::copy_n(val.bytes.begin(), val.size, cmd.args.begin());
    return true;
}

/**
 * @brief  编码 `gnss.ppp` → `GnssSubVerb::kApplyPpp`（1 字节 0/1）
 * @param  val: 0/1 布尔整数
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 参数类型错误
 */
bool encodeGnssPpp(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    if (val.kind != CanonicalKind::kInt) {
        return false;
    }
    cmd.verb = kVerbGnssSetPpp;
    cmd.argLen = 1U;
    cmd.args[0] = (val.number != 0) ? 1U : 0U;
    return true;
}

/**
 * @brief  编码定长字节类规范值到命令参数
 * @tparam Verb: 目标模块动词
 * @tparam ExpectedBytes: 期望字节长度
 * @param  val: 字节规范值
 * @param[out] cmd: 输出命令
 * @retval true  编码成功
 * @retval false 类型或长度不符
 */
template <uint8_t Verb, size_t ExpectedBytes>
bool encodeFixedBytes(const CanonicalValue &val, fw::Command &cmd) noexcept
{
    static_assert(ExpectedBytes <= fw::kCommandArgBytes, "payload exceeds fw::Command::args");
    if ((val.kind != CanonicalKind::kBytes) || (val.size != ExpectedBytes)) {
        return false;
    }
    cmd.verb = Verb;
    cmd.argLen = static_cast<uint8_t>(ExpectedBytes);
    std::copy_n(val.bytes.begin(), ExpectedBytes, cmd.args.begin());
    return true;
}

/** 全部已登记设置键的编译期注册表（下标与 KeyId 一一对应）。 */
constexpr std::array<KeySpec, static_cast<size_t>(KeyId::kCount)> kRegistry{{
    {KeyId::kGnssRate, "gnss.rate", KeyKind::kEnum, Owner::kGnss, kVerbGnssSetRate, &encodeGnssRate,
     &domainGnssRate, PersistKind::kNvs, access::kStandardAll, true},
    {KeyId::kGnssOutGga, "gnss.out.gga", KeyKind::kEnum, Owner::kGnss, kVerbGnssSetOut,
     &encodeGnssOut<kAppIdxGga>, &domainGnssOut<OutKeyIndex::kGga>, PersistKind::kNvs,
     access::kStandardAll, true},
    {KeyId::kGnssOutGsa, "gnss.out.gsa", KeyKind::kEnum, Owner::kGnss, kVerbGnssSetOut,
     &encodeGnssOut<kAppIdxGsa>, &domainGnssOut<OutKeyIndex::kGsa>, PersistKind::kNvs,
     access::kStandardAll, true},
    {KeyId::kGnssOutGsv, "gnss.out.gsv", KeyKind::kEnum, Owner::kGnss, kVerbGnssSetOut,
     &encodeGnssOut<kAppIdxGsv>, &domainGnssOut<OutKeyIndex::kGsv>, PersistKind::kNvs,
     access::kStandardAll, true},
    {KeyId::kGnssOutGst, "gnss.out.gst", KeyKind::kEnum, Owner::kGnss, kVerbGnssSetOut,
     &encodeGnssOut<kAppIdxGst>, &domainGnssOut<OutKeyIndex::kGst>, PersistKind::kNvs,
     access::kStandardAll, true},
    {KeyId::kGnssOutRmc, "gnss.out.rmc", KeyKind::kEnum, Owner::kGnss, kVerbGnssSetOut,
     &encodeGnssOut<kAppIdxRmc>, &domainGnssOut<OutKeyIndex::kRmc>, PersistKind::kNvs,
     access::kStandardAll, true},
    {KeyId::kGnssGgaGateEnabled, "gnss.gga_gate.enabled", KeyKind::kBool, Owner::kGnss,
     kVerbGnssSetGgaGate, &encodeGgaGateEnabled, &domainGgaGateEnabled, PersistKind::kNvs,
     access::kStandardAll, true},
    {KeyId::kGnssGgaGateMinSats, "gnss.gga_gate.min_sats", KeyKind::kIntRange, Owner::kGnss,
     kVerbGnssSetGgaGate, &encodeGgaGateMinSats, &domainGgaGateMinSats, PersistKind::kNvs,
     access::kStandardAll, true},
    {KeyId::kGnssConstGps, "gnss.const.gps", KeyKind::kBool, Owner::kGnss, 0U, nullptr,
     &domainGnssConst<const_bit::kGps>, PersistKind::kReceiverSave, access::kStandardAll, true},
    {KeyId::kGnssConstBds, "gnss.const.bds", KeyKind::kBool, Owner::kGnss, 0U, nullptr,
     &domainGnssConst<const_bit::kBds>, PersistKind::kReceiverSave, access::kStandardAll, true},
    {KeyId::kGnssConstGlo, "gnss.const.glo", KeyKind::kBool, Owner::kGnss, 0U, nullptr,
     &domainGnssConst<const_bit::kGlo>, PersistKind::kReceiverSave, access::kStandardAll, true},
    {KeyId::kGnssConstGal, "gnss.const.gal", KeyKind::kBool, Owner::kGnss, 0U, nullptr,
     &domainGnssConst<const_bit::kGal>, PersistKind::kReceiverSave, access::kStandardAll, true},
    {KeyId::kBtWlAdd, "bt.wl.add", KeyKind::kMac, Owner::kStorage, kVerbStorageWhitelistAdd,
     &encodeStorageMac<kVerbStorageWhitelistAdd>, &domainStaticMac, PersistKind::kNvs,
     access::kLocalOnly, false},
    {KeyId::kBtWlDel, "bt.wl.del", KeyKind::kMac, Owner::kStorage, kVerbStorageWhitelistDel,
     &encodeStorageMac<kVerbStorageWhitelistDel>, &domainStaticMac, PersistKind::kNvs,
     access::kLocalOnly, false},
    {KeyId::kBtPairNext, "bt.pair_next", KeyKind::kBool, Owner::kStorage, kVerbStorageSetPairNext,
     &encodePairNext, &domainStaticBool, PersistKind::kNvs, access::kLocalOnly, true},
    {KeyId::kFactorySn, "factory.sn", KeyKind::kSn, Owner::kStorage, kVerbStorageSaveSn,
     &encodeFactorySn, &domainStaticSn, PersistKind::kNvs,
     static_cast<uint8_t>(access::kLocalOnly | access::kNeedsUnlock), true},
    {KeyId::kGnssPpp, "gnss.ppp", KeyKind::kBool, Owner::kGnss, kVerbGnssSetPpp, &encodeGnssPpp,
     &domainGnssPpp, PersistKind::kNvs, access::kPhoneAndLocal, true},
    {KeyId::kGnssSatTrack, "gnss.sat_track", KeyKind::kSatTrack, Owner::kGnss, kVerbGnssSetSatTrack,
     &encodeFixedBytes<kVerbGnssSetSatTrack, kSatTrackBytes>, &domainGnssSatTrack,
     PersistKind::kNvs, access::kPhoneAndLocal, true},
    {KeyId::kPhoneHeight, "phone.height", KeyKind::kHeight, Owner::kStorage, kVerbStorageSetHeight,
     &encodeFixedBytes<kVerbStorageSetHeight, kHeightBytes>, &domainStaticHeight, PersistKind::kNvs,
     access::kPhoneAndLocal, true},
    {KeyId::kPhoneSnPn, "phone.sn_pn", KeyKind::kSnPn, Owner::kStorage, kVerbStorageSetSnPn,
     &encodeFixedBytes<kVerbStorageSetSnPn, kSnPnBytes>, &domainStaticSnPn, PersistKind::kNvs,
     access::kPhoneInternal, true},
}};

} // namespace

std::span<const KeySpec> keyTable() noexcept
{
    return kRegistry;
}

const KeySpec *findKey(std::string_view name) noexcept
{
    for (const KeySpec &spec : kRegistry) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

const KeySpec *keyAt(KeyId id) noexcept
{
    const auto idx = static_cast<size_t>(id);
    if (idx >= kRegistry.size()) {
        return nullptr;
    }
    return &kRegistry[idx];
}

Error validateValue(const KeySpec &spec, const Domain &dom, std::string_view text,
                    CanonicalValue &out) noexcept
{
    if (!dom.supported) {
        return Error::kUnsupported;
    }
    switch (spec.type) {
    case KeyKind::kEnum:
        return validateEnum(dom.values(), text, out);
    case KeyKind::kBool:
        return validateBool(text, out);
    case KeyKind::kIntRange:
        return validateIntRange(dom.minValue, dom.maxValue, text, out);
    case KeyKind::kMac:
        return validateMac(text, out);
    case KeyKind::kSn:
        return validateSn(text, out);
    case KeyKind::kSatTrack:
        return validateSatTrack(dom, text, out);
    case KeyKind::kHeight:
        return validateHeight(text, out);
    case KeyKind::kSnPn:
        return validateSnPn(text, out);
    default:
        return Error::kBadValue;
    }
}

bool periodMsToInterval(int32_t periodMs, int8_t &interval) noexcept
{
    constexpr int32_t kPeriod20Hz = 50;
    constexpr int32_t kPeriod10Hz = 100;
    constexpr int32_t kPeriod5Hz = 200;
    constexpr int32_t kPeriod2Hz = 500;
    constexpr int8_t kInterval20Hz = 20;
    constexpr int8_t kInterval10Hz = 10;
    constexpr int8_t kInterval5Hz = 5;
    constexpr int8_t kInterval2Hz = 2;
    if (periodMs < 0) {
        return false;
    }
    switch (periodMs) {
    case 0:
        interval = 0;
        return true;
    case kPeriod20Hz:
        interval = kInterval20Hz;
        return true;
    case kPeriod10Hz:
        interval = kInterval10Hz;
        return true;
    case kPeriod5Hz:
        interval = kInterval5Hz;
        return true;
    case kPeriod2Hz:
        interval = kInterval2Hz;
        return true;
    default:
        break;
    }
    if ((periodMs >= kMsPerSecond) && ((periodMs % kMsPerSecond) == 0)) {
        const int32_t sec = periodMs / kMsPerSecond;
        if ((sec >= 1) && (sec <= kMaxSlowPeriodSec)) {
            // sec 落在 1..127，取负后在 int8_t [-127, -1] 范围内，窄化安全。
            interval = static_cast<int8_t>(-sec);
            return true;
        }
    }
    return false;
}

} // namespace settings
