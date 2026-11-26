// 设置服务公共类型（ADR-0018 §2.3/§2.4）：调用方、异步凭据、值/取值域、GNSS 能力快照与后端钩子。
// 纯 C++20，无 IDF/FreeRTOS 头；所有类型均可平凡拷贝、零初值落入 .bss，不分配堆、不阻塞。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "fw/Command.hpp"
#include "fw/Result.hpp"

namespace settings {

/** 凭据无效槽位标记（与 rtos::kInvalidSlot 一致）。 */
inline constexpr uint8_t kInvalidSlot = std::numeric_limits<uint8_t>::max();
/** 单个枚举键最多支持的可选值数量（定位频率与输出周期）。 */
inline constexpr size_t kMaxDomainValues = 12U;
/** 定位频率能力快照上限。 */
inline constexpr size_t kMaxRateOptions = 8U;
/** GNSS 网页可配置 NMEA 输出键数量（gga/gsa/gsv/gst/rmc）。 */
inline constexpr size_t kOutKeyCount = 5U;
/** SN 允许的最大字符数（1–16 个 [0-9A-Za-z-]）。 */
inline constexpr size_t kMaxSnBytes = 16U;
/** 蓝牙 MAC 字节数。 */
inline constexpr size_t kMacBytes = 6U;
/** 天线高二进制字节数（IEEE 754 binary64 LE）。 */
inline constexpr size_t kHeightBytes = 8U;
/** 手机 0x08 Nav SN+PN 组合字段字节数（16 B SN + 16 B PN）。 */
inline constexpr size_t kSnPnBytes = 32U;
/** 规范值与快照值的最大字节数（对齐 fw::kCommandArgBytes）。 */
inline constexpr size_t kMaxCanonicalBytes = fw::kCommandArgBytes;
/** 卫星跟踪（0x06 Survey）截止角最大值（度）与编码字节数。 */
inline constexpr int32_t kSatCutoffMaxDeg = 90;
inline constexpr size_t kSatTrackBytes = 6U;
/** GGA 质量门控最低卫星数闭区间范围。 */
inline constexpr int32_t kGgaGateMinSatsMin = 0;
inline constexpr int32_t kGgaGateMinSatsMax = 40;

/** 目标模块命令动词常量（由 main 侧 static_assert 保证与 GnssVerb/GnssSubVerb/StorageVerb 一致）。 */
inline constexpr uint8_t kVerbGnssSetPpp = 11U;
inline constexpr uint8_t kVerbGnssSetSatTrack = 12U;
inline constexpr uint8_t kVerbGnssSetRate = 14U;
inline constexpr uint8_t kVerbGnssSetOut = 15U;
inline constexpr uint8_t kVerbGnssSetGgaGate = 16U;
inline constexpr uint8_t kVerbStorageSaveSn = 0U;
inline constexpr uint8_t kVerbStorageWhitelistAdd = 3U;
inline constexpr uint8_t kVerbStorageWhitelistDel = 4U;
inline constexpr uint8_t kVerbStorageSetPairNext = 5U;
inline constexpr uint8_t kVerbStorageSetHeight = 6U;
inline constexpr uint8_t kVerbStorageSetSnPn = 7U;
/** 4G 配置动词：由 kStorage 持久化（与 main 的 StorageVerb 一致，SettingsBackend 以 static_assert 保证）。 */
inline constexpr uint8_t kVerbStorageSetNetEnable = 8U;
inline constexpr uint8_t kVerbStorageSetNetApn = 9U;
inline constexpr uint8_t kVerbStorageSetNetUser = 10U;
inline constexpr uint8_t kVerbStorageSetNetPass = 11U;
/** 4G 文本键（APN/用户名/密码）最大字符数；加 1 个终止位后仍 ≤ fw::kCommandArgBytes。 */
inline constexpr size_t kNetTextMaxBytes = 31U;

/** GGA 质量门控子命令标志位（与 app::kGgaGateFlag* 一致）。 */
inline constexpr uint8_t kGgaGateFlagEnabled = 1U << 0U;
inline constexpr uint8_t kGgaGateFlagMinSats = 1U << 1U;
inline constexpr uint8_t kGgaGateCmdArgLen = 3U;

/** 调用方身份（与 rtos::Caller 枚举值一一对应，ADR-0018 §2.2/§6 Q2）。 */
enum class Caller : uint8_t {
    kConsole = 0,
    kWeb,
    kPhone,
    kInternal,
    kMqtt, // 预留：仅电子围栏键开放，GNSS/门控/蓝牙/工厂键一律拒绝
};

/** 键的归属模块分类（由后端映射到对应执行器与 fw::ModuleId）。 */
enum class Owner : uint8_t {
    kNone = 0,
    kGnss,
    kStorage,
};

/** 异步设置凭据；seq == 0 或 slot == kInvalidSlot 表示无效。 */
struct Ticket final {
    uint32_t seq = 0U;
    uint8_t slot = kInvalidSlot;
    Owner owner = Owner::kNone;
};

/** 异步命令轮询结果（与 rtos::PollState 语义一致）。 */
enum class PollState : uint8_t {
    kPending = 0,
    kDone,
    kCancelled,
    kInvalid,
};

/** 设置服务同步返回错误码（与网页协议 §5 错误码一一映射）。 */
enum class Error : uint8_t {
    kOk = 0,
    kUnknownKey,
    kBadValue,
    kLocked,
    kBusy,
    kUnsupported,
    kFailed,
    kTimeout,
};

/** 键的值类型（ADR-0018 §2.3）。 */
enum class KeyKind : uint8_t {
    kEnum = 0,
    kIntRange,
    kBool,
    kMac,
    kSn,
    kSatTrack, // "<cutoffDeg>,<gps>,<glo>,<bds>,<gal>,<sbas>"
    kHeight,   // 16 位十六进制（8 B 小端 IEEE 754 binary64，0..10 m）
    kSnPn,     // 64 位十六进制（16 B SN + 16 B PN）
    kText,     // 0..31 个可见 ASCII（0x21..0x7E）文本，允许为空（用于 APN/用户名/密码）
};

/** 持久化策略（ADR-0018 §2.3）。 */
enum class PersistKind : uint8_t {
    kNone = 0,
    kNvs,
    kReceiverSave,
};

/** 访问权限位定义（ADR-0018 §2.3/§6 Q2）。 */
namespace access {
inline constexpr uint8_t kConsole = 1U << 0U;
inline constexpr uint8_t kWeb = 1U << 1U;
inline constexpr uint8_t kPhone = 1U << 2U;
inline constexpr uint8_t kInternal = 1U << 3U;
inline constexpr uint8_t kMqtt = 1U << 4U;
inline constexpr uint8_t kNeedsUnlock = 1U << 7U;
inline constexpr uint8_t kStandardAll = 0x0FU;   // console, web, phone, internal
inline constexpr uint8_t kLocalOnly = 0x0BU;     // console, web, internal
inline constexpr uint8_t kPhoneAndLocal = 0x0DU; // console, phone, internal（非网页键）
inline constexpr uint8_t kPhoneInternal = 0x0CU; // phone, internal
inline constexpr uint8_t kConsoleInternal =
    0x09U; // console, internal（不对网页/手机/MQTT 开放，如 4G 配置）
} // namespace access

/** 注册表中全部已知设置键的固定编号（顺序即网页与注册表遍历顺序）。 */
enum class KeyId : uint8_t {
    kGnssRate = 0,
    kGnssOutGga,
    kGnssOutGsa,
    kGnssOutGsv,
    kGnssOutGst,
    kGnssOutRmc,
    kGnssGgaGateEnabled,
    kGnssGgaGateMinSats,
    kGnssConstGps,
    kGnssConstBds,
    kGnssConstGlo,
    kGnssConstGal,
    kBtWlAdd,
    kBtWlDel,
    kBtPairNext,
    kFactorySn,
    kGnssPpp,
    kGnssSatTrack,
    kPhoneHeight,
    kPhoneSnPn,
    kCount,
    // 已发布编号不得移动：kCount(20) 保留，21 为 gnss.cutoff（Registry 内部常量）。
    // 4G 键只能追加在其后，显式取值保证既有 KeyId 数值（含 gnss.cutoff=21）不变。
    kNetEnable = 22,
    kNetApn = 23,
    kNetUser = 24,
    kNetPass = 25,
};

/** GNSS 输出能力表下标（对应 5 条网页 NMEA 输出键）。 */
enum class OutKeyIndex : uint8_t {
    kGga = 0,
    kGsa = 1,
    kGsv = 2,
    kGst = 3,
    kRmc = 4,
};

/** 星座能力掩码位（GnssCaps::constMask）。 */
namespace const_bit {
inline constexpr uint8_t kGps = 1U << 0U;
inline constexpr uint8_t kBds = 1U << 1U;
inline constexpr uint8_t kGlo = 1U << 2U;
inline constexpr uint8_t kGal = 1U << 3U;
inline constexpr uint8_t kSbas = 1U << 4U;
} // namespace const_bit

/** 单个 GNSS 输出语句在当前定位频率下的有效周期集合（ms，含 0=关闭）。 */
struct PeriodDomain final {
    std::array<uint16_t, kMaxDomainValues> values{};
    uint8_t count = 0U;
    bool supported = false;

    friend constexpr bool operator==(const PeriodDomain &, const PeriodDomain &) = default;
};

/** GNSS 能力快照（由 kIo 探测到型号后及定位频率变化后写入 DataCenter::gnssCaps）。 */
struct GnssCaps final {
    bool valid = false;
    bool enabled = false;
    bool pppSupported = false;
    uint8_t navRateHz = 0U;
    std::array<uint8_t, kMaxRateOptions> navRatesHz{};
    uint8_t navRateCount = 0U;
    std::array<PeriodDomain, kOutKeyCount> outPeriods{};
    uint8_t constMask = 0U;

    /**
     * @brief  比较两份能力快照的内容是否一致
     * @param  other: 另一份能力快照
     * @retval true  全部能力字段完全一致
     * @retval false 至少一项能力字段不同
     * @note   纯函数，线程/ISR 安全。
     */
    [[nodiscard]] constexpr bool sameCaps(const GnssCaps &other) const noexcept
    {
        return (enabled == other.enabled) && (valid == other.valid) &&
               (pppSupported == other.pppSupported) && (navRateHz == other.navRateHz) &&
               (navRateCount == other.navRateCount) && (navRatesHz == other.navRatesHz) &&
               (outPeriods == other.outPeriods) && (constMask == other.constMask);
    }
};

/** 单个设置键的当前取值域描述（由 Service::domain 返回）。 */
struct Domain final {
    KeyKind kind = KeyKind::kEnum;
    bool supported = false;
    std::array<int32_t, kMaxDomainValues> enumValues{};
    uint8_t enumCount = 0U;
    uint8_t mask = 0U;
    int32_t minValue = 0;
    int32_t maxValue = 0;

    /**
     * @brief  返回枚举允许值的只读切片
     * @retval 指向 enumValues[0..enumCount) 的 span
     * @note   纯函数，线程/ISR 安全。
     */
    [[nodiscard]] constexpr std::span<const int32_t> values() const noexcept
    {
        return std::span<const int32_t>{enumValues.data(), enumCount};
    }
};

/** 规范解析值的底层存储类别。 */
enum class CanonicalKind : uint8_t {
    kInt = 0,
    kBytes,
};

/** 经键类型与取值域校验后的规范值。 */
struct CanonicalValue final {
    CanonicalKind kind = CanonicalKind::kInt;
    int32_t number = 0;
    std::array<uint8_t, kMaxCanonicalBytes> bytes{};
    uint8_t size = 0U;
};

/** Service::get 返回的快照值类别。 */
enum class ValueKind : uint8_t {
    kInt = 0,
    kBool,
    kString,
    kBytes,
};

/** Service::get 读出的当前设置值快照。 */
struct Value final {
    ValueKind kind = ValueKind::kInt;
    int32_t number = 0;
    bool boolVal = false;
    std::array<char, kMaxCanonicalBytes> text{};
    uint8_t textLen = 0U;

    /**
     * @brief  取得字符串值视图（仅 kind == ValueKind::kString 时有效）
     * @retval 指向 text[0..textLen) 的视图
     * @note   纯函数，线程/ISR 安全。
     */
    [[nodiscard]] constexpr std::string_view textView() const noexcept
    {
        return std::string_view{text.data(), textLen};
    }
};

/** 后端适配器操作表（由 main 绑定到 DataCenter 与 Executor::post/poll/cancel）。 */
struct Backend final {
    void *ctx = nullptr;
    fw::Result (*post)(void *ctx, Owner owner, const fw::Command &cmd, Caller caller,
                       Ticket &out) noexcept = nullptr;
    PollState (*poll)(void *ctx, const Ticket &t, fw::CommandResult &out) noexcept = nullptr;
    void (*cancel)(void *ctx, const Ticket &t) noexcept = nullptr;
    bool (*get)(void *ctx, KeyId id, Value &out) noexcept = nullptr;
    GnssCaps (*gnssCaps)(void *ctx) noexcept = nullptr;
    bool (*gnssEnabled)(void *ctx) noexcept = nullptr;
};

/**
 * @brief  把 Caller 转换为权限位掩码
 * @param  caller: 调用方身份
 * @retval 对应 access::k* 位；未知值返回 0
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t callerBit(Caller caller) noexcept
{
    switch (caller) {
    case Caller::kConsole:
        return access::kConsole;
    case Caller::kWeb:
        return access::kWeb;
    case Caller::kPhone:
        return access::kPhone;
    case Caller::kInternal:
        return access::kInternal;
    case Caller::kMqtt:
        return access::kMqtt;
    default:
        return 0U;
    }
}

/**
 * @brief  把 fw::Result 映射为 settings::Error
 * @param  res: 底层投递或命令结果
 * @retval 对应 settings::Error
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr Error fromResult(fw::Result res) noexcept
{
    switch (res) {
    case fw::Result::kOk:
        return Error::kOk;
    case fw::Result::kInvalidArg:
        return Error::kBadValue;
    case fw::Result::kBusy:
        return Error::kBusy;
    case fw::Result::kNotSupported:
        return Error::kUnsupported;
    case fw::Result::kTimeout:
        return Error::kTimeout;
    case fw::Result::kNoSpace:
    case fw::Result::kNotFound:
    case fw::Result::kIoError:
    case fw::Result::kInvalidState:
    default:
        return Error::kFailed;
    }
}

} // namespace settings
