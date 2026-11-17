// NMEA 解析的内部声明（组件私有头，不进公共接口）：定点解析原语与解析实现类。
// 线程/调用约束：与 gnss::NmeaParser 相同——单任务顺序调用、不可在 ISR 中使用、不可阻塞。
// 堆：不使用。浮点：不使用（十进制定点解析，ADR-0010 §2.5）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gnss/NmeaParser.hpp"
#include "gnss/Types.hpp"

namespace gnss {

namespace detail {

// 十进制定点解析结果；“空字段”与“格式错误”必须区分：前者保持未知，后者整句拒绝。
enum class NumResult : uint8_t { kEmpty, kOk, kError };

// 地址字段 = 2 字母 talker + 3 字母消息名。
inline constexpr size_t kMinAddrLen = 5U;
// GSA 的 PRN 槽数；mergeUsed 的入参长度由它决定。
inline constexpr size_t kGsaPrnCount = 12U;
// used_/pending_ 的卫星键 = system << 8 | prn。
inline constexpr uint32_t kSatKeySystemShift = 8U;

// 以逗号切分语句主体（不含 '$' 与 "*hh\r\n"）。返回的每个字段不含分隔符。
struct FieldIter {
    std::span<const uint8_t> body;
    size_t pos = 0;

    /** @brief 取下一个逗号分隔字段。
     * @param out: 输出字段（不含分隔符）
     * @retval true 表示还有字段
     */
    bool next(std::span<const uint8_t> &out) noexcept
    {
        if (pos > body.size()) {
            return false;
        }
        size_t comma = pos;
        while (comma < body.size() && body[comma] != static_cast<uint8_t>(',')) {
            ++comma;
        }
        out = body.subspan(pos, comma - pos);
        pos = (comma < body.size()) ? (comma + 1) : (body.size() + 1);
        return true;
    }
};

/** @brief 把 used_/pending_ 的键拼成 uint16。
 * @param system: 星座
 * @param prn: 卫星编号
 * @retval 键值（system << 8 | prn）
 */
uint16_t satKey(Constellation system, uint8_t prn) noexcept;

/** @brief 计算 10^n（int64）。
 * @param n: 指数（调用方保证 n 很小）
 * @retval 10 的 n 次幂
 */
int64_t pow10(uint8_t n) noexcept;

/** @brief 四舍五入（远离 0）的整数除法，用于定点缩放，避免浮点。
 * @param a: 被除数
 * @param b: 除数（必须为正）
 * @retval a/b 的四舍五入结果
 */
int64_t roundDiv(int64_t a, int64_t b) noexcept;

/** @brief 定标点数的分数位换算。
 * @param value: 带 fromFrac 位小数的整数表示
 * @param fromFrac: 当前小数位数
 * @param toFrac: 目标小数位数
 * @retval 带 toFrac 位小数的整数表示
 */
int64_t rescale(int64_t value, uint8_t fromFrac, uint8_t toFrac) noexcept;

/** @brief 判断字段是否等于单个字符。
 * @param field: 待判字段
 * @param c: 期望字符
 * @retval true 表示长度 1 且字符相同
 */
bool fieldEquals(std::span<const uint8_t> field, char c) noexcept;

/** @brief 解析 [+-]?digits[.digits]，返回 10^frac_digits 定点值。
 * @param field: 单个 NMEA 字段
 * @param maxFrac: 允许的最大小数位数
 * @param value: 输出定点值
 * @param frac: 输出实际小数位数
 * @retval kEmpty 空字段；kOk 成功；kError 格式错误
 */
NumResult parseFixed(std::span<const uint8_t> field, uint8_t maxFrac, int64_t &value,
                     uint8_t &frac) noexcept;

/** @brief 解析无符号十进制整数。
 * @param field: 单个 NMEA 字段
 * @param value: 输出值
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseUint(std::span<const uint8_t> field, uint32_t &value) noexcept;

/** @brief hhmmss.sss → UtcTime 的时分与秒内毫秒。
 * @param field: 时间字段
 * @param utc: 输出的 UTC 时间
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseTime(std::span<const uint8_t> field, UtcTime &utc) noexcept;

/** @brief ddmmyy → UtcTime 的年月日。
 * @param field: 日期字段
 * @param utc: 输出的 UTC 时间
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseDate(std::span<const uint8_t> field, UtcTime &utc) noexcept;

/** @brief ddmm.mmm → 1e-9 度。
 * @param field: 经纬度字段
 * @param isLongitude: true 表示经度（上限 180°）
 * @param e9: 输出的 1e-9 度值
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseAngle(std::span<const uint8_t> field, bool isLongitude, int64_t &e9) noexcept;

/** @brief DOP 字段 → ×100；空字段/超范围按未知（0xFFFF）。
 * @param field: DOP 字段
 * @param out: 输出的 ×100 值
 * @retval kEmpty 空字段；kOk 成功；kError 格式错误
 */
NumResult parseDop(std::span<const uint8_t> field, uint16_t &out) noexcept;

/** @brief 毫米字段（高度/水准面差距）→ int32 mm。
 * @param field: 数值字段
 * @param out: 输出的毫米值
 * @retval kEmpty 空字段；kOk 成功；kError 格式或范围错误
 */
NumResult parseMm(std::span<const uint8_t> field, int32_t &out) noexcept;

/** @brief 差分龄期字段 → 0.1 s。
 * @param field: 龄期字段
 * @param out: 输出的 0.1 s 值
 * @retval kEmpty 空字段；kOk 成功；kError 格式错误
 */
NumResult parseDiffAge(std::span<const uint8_t> field, uint16_t &out) noexcept;

/** @brief 星座 → gsv_done_ 索引（越界归到 kOther）。
 * @param c: 星座
 * @retval 数组索引
 */
size_t constellationIndex(Constellation c) noexcept;

/** @brief 由 talker 与可选 System ID 字段确定星座。
 * @param addr: 地址字段
 * @param idField: System ID 字段（可为空）
 * @param system: 输出的星座
 * @retval false 表示 System ID 存在但格式错误
 */
bool resolveSystem(std::span<const uint8_t> addr, std::span<const uint8_t> idField,
                   Constellation &system) noexcept;

} // namespace detail

// 解析细节通过 friend 访问 NmeaParser 的私有状态；对外只暴露 feed()。
struct NmeaImpl {
    static NmeaResult feed(NmeaParser &parser, std::span<const uint8_t> sentence,
                           uint32_t nowMs) noexcept;
    static NmeaResult parseGga(NmeaParser &parser, std::span<const uint8_t> body,
                               uint32_t nowMs) noexcept;
    static NmeaResult parseRmc(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static NmeaResult parseGsa(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static NmeaResult parseGsv(NmeaParser &parser, std::span<const uint8_t> body) noexcept;
    static bool publish(NmeaParser &parser, uint32_t nowMs) noexcept;
    static void mergeUsed(NmeaParser &parser, Constellation system,
                          const std::array<uint8_t, detail::kGsaPrnCount> &prns,
                          size_t prnCount) noexcept;
    static void clearField(SatInfo &sat) noexcept;
};

} // namespace gnss
