// GNSS 基站与工作模式 NVS 配置（gnss.base 组）及固定点解析工具（ADR-0014 §2.5/§2.6）。
// 线程：纯数据结构与编解码，无内部状态或锁；由 NVS 回调、kIo 与 console 解析共享，非 ISR。
// 堆与浮点：不使用堆、不使用浮点，全部坐标与精度按定点数编解码。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "def/GnssSubDef.hpp"
#include "gnss/Receiver.hpp"

namespace app {

/**
 * gnss.base 持久化配置：模式、自测门限、固定坐标与基站 RTCM 输出计划。
 * blob 字段全部按小端整数序列化，避免浮点与结构体对齐差异。
 */
struct GnssBaseConfig {
    static constexpr size_t kRtcmOutVersion = 1U;
    static constexpr size_t kRtcmOutHeaderBytes = 2U;
    static constexpr size_t kRtcmOutBlobBytes =
        kRtcmOutHeaderBytes + ::gnss::kBaseRtcmCount;              // 14 B
    static constexpr size_t kFixCoordBlobBytes = sizeof(int64_t);  // 8 B
    static constexpr size_t kFixHeightBlobBytes = sizeof(int32_t); // 4 B

    static constexpr uint16_t kMinSvyS = 5U;
    static constexpr uint16_t kMaxSvyS = 3600U;
    static constexpr uint16_t kDefaultSvyS = 20U;

    static constexpr uint16_t kMinSvyAccCm = 10U;
    static constexpr uint16_t kMaxSvyAccCm = 1000U;
    static constexpr uint16_t kDefaultSvyAccCm = 150U;

    static constexpr uint16_t kMinSvyToS = 15U;
    static constexpr uint16_t kMaxSvyToS = 3600U;
    static constexpr uint16_t kDefaultSvyToS = 120U;
    static constexpr uint16_t kMinSvyToMarginS = 10U;

    static constexpr int64_t kMaxLatE9 = 90000000000LL;
    static constexpr int64_t kMaxLonE9 = 180000000000LL;
    static constexpr int32_t kMinHeightMm = -1000000;
    static constexpr int32_t kMaxHeightMm = 100000000;

    static constexpr uint8_t kMsm4 = 4U;
    static constexpr uint8_t kMsm7 = 7U;

    WorkModeKind mode = WorkModeKind::kRover;
    BaseType baseType = BaseType::kSurvey;
    uint16_t svyS = kDefaultSvyS;
    uint16_t svyAccCm = kDefaultSvyAccCm;
    uint16_t svyToS = kDefaultSvyToS;
    int64_t fixLatE9 = 0;
    int64_t fixLonE9 = 0;
    int32_t fixHeightMm = 0;
    std::array<int8_t, ::gnss::kBaseRtcmCount> rtcmOut{};
    uint8_t msm = kMsm4;

    /**
     * @brief 返回出厂默认 gnss.base 配置（ADR-0014 §2.5/§2.6）。
     * @retval cfg: 默认配置（rover、survey 20s/150cm/120s、MSM4 缺省 RTCM 表）。
     */
    [[nodiscard]] static GnssBaseConfig defaults() noexcept;

    /**
     * @brief 生成指定 MSM 版本对应的默认 RTCM 输出率数组。
     * @param msm: 4 或 7。
     * @retval rates: 1005/1033/1230 及 MSM4 或 MSM7 置默认率，其余关闭。
     */
    [[nodiscard]] static std::array<int8_t, ::gnss::kBaseRtcmCount>
    defaultRtcmOut(uint8_t msm) noexcept;

    /**
     * @brief 在 MSM4（1074..1124）与 MSM7（1077..1127）之间切换，保留各星座开关。
     * @param newMsm: 目标 MSM 版本（4 或 7）。
     */
    void applyMsm(uint8_t newMsm) noexcept;

    /**
     * @brief 将单槽内部速率编码转换为周期毫秒（0 表示关闭）。
     * @param encoded: >0 为 Hz，0 为关，<0 为周期秒数相反数（如 -10 表示 0.1 Hz / 10 s）。
     * @retval periodMs: 周期（毫秒）。
     */
    [[nodiscard]] static uint32_t rateToPeriodMs(int8_t encoded) noexcept;

    /**
     * @brief 检查内部 RTCM 速率编码是否合法（0、1/2/5 或 -2/-5/-10）。
     * @param encoded: 待检查编码值。
     * @retval true: 合法。
     * @retval false: 超范围或不支持。
     */
    [[nodiscard]] static bool isValidRateCode(int8_t encoded) noexcept;

    /**
     * @brief 根据 RTCM3 报文号查找在 kBaseRtcmIds 中的槽位下标。
     * @param id: RTCM3 报文号（如 1005）。
     * @retval index: 下标（0..11），未知报文返回 std::nullopt。
     */
    [[nodiscard]] static std::optional<size_t> rtcmSlot(uint16_t id) noexcept;

    /**
     * @brief 导出 12 路基站 RTCM 周期表（毫秒，供 gnss::ModeRequest 使用）。
     * @retval periods: 每路 RTCM 周期（0 表示关闭）。
     */
    [[nodiscard]] std::array<uint32_t, ::gnss::kBaseRtcmCount> toPeriodsMs() const noexcept;

    /**
     * @brief 将 rtcmOut 编码为 NVS 14 字节 blob（version + reserved + int8_t[12]）。
     * @param out: 目标缓冲。
     * @param len: 输出实际长度（14）。
     * @retval true: 编码成功。
     * @retval false: 缓冲不足。
     */
    [[nodiscard]] bool encodeRtcmOut(std::span<uint8_t> out, size_t &len) const noexcept;

    /**
     * @brief 从 NVS blob 解码 rtcmOut（严格校验长度、版本号与逐项编码）。
     * @param in: 输入 blob。
     * @retval true: 解码并写入当前对象。
     * @retval false: 格式错误（当前对象不部分修改）。
     */
    [[nodiscard]] bool decodeRtcmOut(std::span<const uint8_t> in) noexcept;
};

/** @brief 编码 64 位小端有符号整数（fix_lat / fix_lon）。 */
void encodeI64Le(int64_t val, std::span<uint8_t> out) noexcept;
/** @brief 解码 64 位小端有符号整数（fix_lat / fix_lon）。 */
[[nodiscard]] int64_t decodeI64Le(std::span<const uint8_t> in) noexcept;
/** @brief 编码 32 位小端有符号整数（fix_h_mm）。 */
void encodeI32Le(int32_t val, std::span<uint8_t> out) noexcept;
/** @brief 解码 32 位小端有符号整数（fix_h_mm）。 */
[[nodiscard]] int32_t decodeI32Le(std::span<const uint8_t> in) noexcept;

/**
 * @brief 解析十进制定点数（无浮点，允许正负号与可选小数点，超出 fracDigits 位截断或四舍五入）。
 * @param text: 输入文本。
 * @param fracDigits: 目标小数位数（如 9 表示 1e-9，3 表示 mm，2 表示 cm）。
 * @param outVal: 输出按 10^fracDigits 放大后的有符号整数。
 * @retval true: 解析成功且未溢出。
 * @retval false: 空串、非法字符或数值溢出。
 */
[[nodiscard]] bool parseScaledDecimal(std::string_view text, uint8_t fracDigits,
                                      int64_t &outVal) noexcept;

/**
 * @brief 解析工作模式关键字（rover/base/smartbase）。
 * @param text: 输入词。
 * @retval mode: 解析结果；未知词返回 std::nullopt。
 */
[[nodiscard]] std::optional<WorkModeKind> parseWorkMode(std::string_view text) noexcept;

/**
 * @brief 返回工作模式短名。
 * @param mode: 模式枚举。
 * @retval 静态可读名称。
 */
[[nodiscard]] const char *workModeName(WorkModeKind mode) noexcept;

/**
 * @brief 返回基站坐标来源短名（survey/fixed）。
 * @param type: 类型枚举。
 * @retval 静态可读名称。
 */
[[nodiscard]] const char *baseTypeName(BaseType type) noexcept;

/**
 * @brief 返回工作模式状态机状态短名。
 * @param state: 状态枚举。
 * @retval 静态可读名称。
 */
[[nodiscard]] const char *workModeStateName(WorkModeState state) noexcept;

/**
 * @brief 返回工作模式错误原因短名。
 * @param err: 错误枚举。
 * @retval 静态可读名称。
 */
[[nodiscard]] const char *workModeErrName(WorkModeErr err) noexcept;

/**
 * @brief 解析 `gnss out rtcm` 速率参数（off/0/0.1/0.2/0.5/1/2/5）。
 * @param text: 速率文本。
 * @param outCode: 输出内部编码。
 * @retval true: 解析成功。
 * @retval false: 非支持速率文本。
 */
[[nodiscard]] bool parseRtcmRateArg(std::string_view text, int8_t &outCode) noexcept;

/**
 * @brief 格式化内部 RTCM 速率编码为文本（如 "off"、"0.1 Hz"、"1 Hz"）。
 * @param code: 内部编码。
 * @param buf: 输出缓冲。
 * @retval 指向 buf.data() 的 NUL 结尾字符串。
 */
[[nodiscard]] const char *formatRtcmRate(int8_t code, std::span<char> buf) noexcept;

/**
 * @brief 格式化带符号定点数为十进制文本（无浮点）。
 * @param val: 放大后数值。
 * @param decimals: 小数位数（3 或 9）。
 * @param buf: 输出缓冲。
 * @retval 指向 buf.data() 的 NUL 结尾字符串。
 */
[[nodiscard]] const char *formatScaledInt(int64_t val, uint8_t decimals,
                                          std::span<char> buf) noexcept;

/**
 * @brief 判断 RTCM3 基站报文是否受当前接收机族支持（1033 仅 Unicore，1230 仅 u-blox）。
 * @param id: RTCM3 报文号。
 * @param family: 已探测接收机族；kUnknown 时两者均可选。
 * @retval true: 支持或未探测到接收机族；false: 已探测族不支持。
 */
[[nodiscard]] bool isBaseRtcmSupportedForFamily(uint16_t id, ::gnss::Family family) noexcept;

/**
 * @brief 未探测到接收机族时返回族专用 RTCM3 报文后缀（" (Unicore)"/" (u-blox)"），其余返回 ""。
 * @param id: RTCM3 报文号。
 * @param family: 当前接收机族。
 * @retval 静态说明后缀串。
 */
[[nodiscard]] const char *baseRtcmFamilyTag(uint16_t id, ::gnss::Family family) noexcept;

} // namespace app
