// NMEA 0183 语句解析：GGA/RMC/GSA/GSV → gnss::Fix / gnss::SatTable；输入是 demux::nmea 已校验的完整语句，Talker 不区分，空字段保持“未知”。
// 线程安全：否（一个实例由一个任务使用）。ISR 安全：否。
// 堆与阻塞：不使用堆、不使用浮点、不阻塞（小数字段按十进制定点解析，ADR-0010 §2.5）。冻结接口（ADR-0010 §3）实现方不得修改；变更须先修订 ADR-0010。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gnss/Types.hpp"

namespace gnss {

/** 单条语句的处理结果。 */
enum class NmeaResult : uint8_t {
    kIgnored,   // 不关心的语句类型（如 VTG、ZDA）
    kFix,       // GGA 已更新 fix()
    kAux,       // RMC/GSA 已更新 fix() 的辅助字段（不改 updateMs）
    kSatsPart,  // GSV 收到一部分，尚未成轮
    kSatsReady, // 一轮 GSV（所有系统）收齐，sats() 已整体替换
    kMalformed, // 字段数量/格式错误，未改变任何状态
};

/** 语句统计。 */
struct NmeaStats {
    uint32_t gga = 0;
    uint32_t rmc = 0;
    uint32_t gsa = 0;
    uint32_t gsv = 0;
    uint32_t ignored = 0;
    uint32_t malformed = 0;
};

class NmeaParser {
public:
    constexpr NmeaParser() noexcept = default;
    ~NmeaParser() = default;
    NmeaParser(const NmeaParser &) = delete;
    NmeaParser &operator=(const NmeaParser &) = delete;

    /**
     * @brief  处理一条完整 NMEA 语句，必要时更新定位解与卫星表
     * @param  sentence: 完整语句（含 $ 与 \r\n），通常是 demux::nmea 的输出
     * @param  nowMs: 当前时间，写入 GGA 的 Fix::updateMs 与成轮的 SatTable::updateMs
     * @retval kIgnored 不关心的语句类型（如 VTG、ZDA）
     * @retval kFix GGA 已更新 fix()
     * @retval kAux RMC/GSA 已更新 fix() 的辅助字段（不改 updateMs）
     * @retval kSatsPart GSV 收到一部分，尚未成轮
     * @retval kSatsReady 一轮 GSV 收齐，sats() 已整体替换
     * @retval kMalformed 字段数量/格式错误，未改变任何状态
     * @note   GSA used 标记：同一历元内收到的所有 GSA 的 PRN 并集，在下一次 kSatsReady 时合并进卫星表。
     *         GSV 成轮判定：一个系统的“总条数”语句收齐算该系统完成；遇到 GGA（新历元）时把已完成的
     *         系统发布。非线程安全，不阻塞。
     */
    NmeaResult feed(std::span<const uint8_t> sentence, uint32_t nowMs) noexcept;

    /**
     * @brief  最近一次 GGA 得到的定位解
     * @retval 定位解引用（未收到 GGA 时 valid=false）
     */
    [[nodiscard]] const Fix &fix() const noexcept
    {
        return fix_;
    }
    /**
     * @brief  最近一轮收齐的卫星表
     * @retval 已发布卫星表引用（尚未成轮时 count=0）
     */
    [[nodiscard]] const SatTable &sats() const noexcept
    {
        return sats_;
    }
    /**
     * @brief  解析统计（各类语句计数）
     * @retval 统计引用，随 feed() 更新
     */
    [[nodiscard]] const NmeaStats &stats() const noexcept
    {
        return stats_;
    }
    /**
     * @brief  清空定位与卫星状态（模块掉电、重新探测时）
     * @note   不清统计；非线程安全
     */
    void reset() noexcept;

private:
    Fix fix_{};
    SatTable sats_{};    // 已发布
    SatTable pending_{}; // 本历元累积中
    std::array<uint8_t, static_cast<size_t>(Constellation::kCount)>
        gsv_done_{};                        // 每系统：0 未完成，1 已收齐
    std::array<uint16_t, kMaxSats> used_{}; // GSA 本历元使用的卫星键（system<<8|prn），0 = 空
    uint8_t used_count_ = 0;
    NmeaStats stats_{};

    friend struct NmeaImpl; // 实现细节在 src/NmeaParser.cpp
};

} // namespace gnss
