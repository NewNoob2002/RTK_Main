// 网页配置可改键表与值校验（docs/web/ws-protocol.md §4）。
// 查表与校验为纯函数；由网页模块任务调用；不阻塞、非 ISR 安全、不分配堆。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "web_proto/Error.hpp"

namespace web {

/** 可改键的值类别；决定 validateValue 的解析方式。 */
enum class KeyKind : uint8_t { kEnum, kMac, kSn, kBool };

/** 一个可改键的协议定义；allowed 只用于 kEnum。 */
struct KeySpec final {
    std::string_view name;
    KeyKind kind = KeyKind::kEnum;
    std::span<const int32_t> allowed{};
    bool needsUnlock = false; // 需先 unlock（factory.sn）
    bool optional = false;    // v1 是否提供取决于 W0 的 CM680 星座验证
};

/** `gnss.rate` 允许的定位频率（Hz）。 */
inline constexpr std::array<int32_t, 5> kRateHz{1, 2, 5, 10, 20};
/** `gnss.out.*` 允许的语句周期（毫秒）；0 = 关闭。 */
inline constexpr std::array<int32_t, 6> kOutPeriodMs{0, 50, 100, 200, 500, 1000};
/** 布尔键的允许值（0/1）。 */
inline constexpr std::array<int32_t, 2> kBoolValues{0, 1};

/** 协议 §4 全部可改键；顺序即网页展示顺序。 */
inline constexpr std::array<KeySpec, 14> kKeyTable{{
    {"gnss.rate", KeyKind::kEnum, kRateHz, false, false},
    {"gnss.out.gga", KeyKind::kEnum, kOutPeriodMs, false, false},
    {"gnss.out.gsa", KeyKind::kEnum, kOutPeriodMs, false, false},
    {"gnss.out.gsv", KeyKind::kEnum, kOutPeriodMs, false, false},
    {"gnss.out.gst", KeyKind::kEnum, kOutPeriodMs, false, false},
    {"gnss.out.rmc", KeyKind::kEnum, kOutPeriodMs, false, false},
    {"gnss.const.gps", KeyKind::kBool, {}, false, true},
    {"gnss.const.bds", KeyKind::kBool, {}, false, true},
    {"gnss.const.glo", KeyKind::kBool, {}, false, true},
    {"gnss.const.gal", KeyKind::kBool, {}, false, true},
    {"bt.wl.add", KeyKind::kMac, {}, false, false},
    {"bt.wl.del", KeyKind::kMac, {}, false, false},
    {"bt.pair_next", KeyKind::kBool, {}, false, false},
    {"factory.sn", KeyKind::kSn, {}, true, false},
}};

/** SN 允许的最大字符数（协议 §4：1–16 个 `[0-9A-Za-z-]`）。 */
constexpr size_t kMaxSnBytes = 16;

/**
 * @brief  按协议键名查表。
 * @param  name: 键名，区分大小写。
 * @retval 找到时返回表项；未知键返回 nullptr
 * @note   纯查表，不阻塞；线程/ISR 安全。
 */
[[nodiscard]] const KeySpec *findKey(std::string_view name) noexcept;

/** 规范值的类型：整数（枚举/布尔）或字节串（MAC/SN）。 */
enum class ValueKind : uint8_t { kInt, kBytes };

/** 与键无关的规范值；由 validateValue 成功时填写。 */
struct CanonicalValue final {
    ValueKind kind = ValueKind::kInt;
    int32_t number = 0;
    std::array<uint8_t, kMaxSnBytes> bytes{};
    uint8_t size = 0;
};

/**
 * @brief  校验一个键的文本值并产出规范值。
 * @param  spec: 已经 findKey 得到的表项。
 * @param  text: 浏览器提供的字符串值（如 "10"、"AA:BB:CC:DD:EE:FF"）。
 * @param[out] out: 成功时填写规范值；失败时保持原内容（含 kind/number/size/bytes 全部字段）。
 * @retval kNone     校验通过
 * @retval kBadValue 格式或取值不在允许集合内
 * @note  纯函数，不阻塞、不分配堆；任意任务调用，纯函数故 ISR 中也可调用。
 */
[[nodiscard]] ErrorCode validateValue(const KeySpec &spec, std::string_view text,
                                      CanonicalValue &out) noexcept;

} // namespace web
