// GGA 质量门控（Q1）：按 GGA 定位质量与使用卫星数做阻断/迟滞放行，防止低质量 GGA 导致 NTRIP caster 封号。
// 线程安全：否（由 GNSS 接收任务单线程驱动；跨任务只读状态由 main 侧发布原子标志与快照）。ISR 安全：否（修改门控状态）。
// 堆与阻塞：不使用堆、不阻塞、无 IDF/FreeRTOS 依赖，可独立主机测试。
#pragma once

#include <cstdint>
#include <span>

namespace gnss {

/** GGA 质量门控配置参数（成员默认零初值以便静态对象落入 .bss；缺省值通过 defaults() 获取）。 */
struct GgaGateConfig final {
    static constexpr uint8_t kDefaultMinSats = 8U;
    static constexpr uint8_t kMaxMinSats = 40U;
    static constexpr uint8_t kReleaseStreak = 3U; // 阻断态需连续 3 条达标 GGA 才放行

    bool enabled = false;
    uint8_t minSats = 0U;

    /**
     * @brief  返回出厂默认门控配置（默认关闭、min_sats=8）
     * @retval 默认配置结构体
     * @note   纯常量函数，线程/ISR 安全
     */
    [[nodiscard]] static constexpr GgaGateConfig defaults() noexcept
    {
        return {.enabled = false, .minSats = kDefaultMinSats};
    }

    /**
     * @brief  校验门控配置是否合法（minSats <= 40）
     * @param  cfg: 待校验配置
     * @retval true 配置合法
     * @retval false minSats 超出 0..40 范围
     * @note   纯常量函数，线程/ISR 安全
     */
    [[nodiscard]] static constexpr bool isValid(const GgaGateConfig &cfg) noexcept
    {
        return cfg.minSats <= kMaxMinSats;
    }
};

/** GGA 质量门控运行态与累计计数快照。 */
struct GgaGateStats final {
    bool enabled = false;
    bool blocked = false;
    uint8_t minSats = 0U;
    uint32_t ggaBlocked = 0U; // 累计被拦截丢弃的 GGA 条数
    uint32_t gateTrans = 0U;  // 阻断/放行状态切换总次数
};

/**
 * GGA 质量门控状态机：仅作用于 GGA 语句，阻断期间不缓存不补发。
 * - 放行态：1 条不达标立即阻断。
 * - 阻断态：连续 3 条达标后放行并放行第 3 条真实 GGA。
 * - 关闭态：全部放行，不计拦截。
 */
class GgaQualityGate final {
public:
    constexpr GgaQualityGate() noexcept = default;

    /**
     * @brief  应用新的门控配置并立即生效
     * @param  cfg: 新配置（minSats 须在 0..40）
     * @retval true 已应用配置
     * @retval false 配置非法（原有配置与状态保持不变）
     * @note   单任务调用，不阻塞；关闭开关时若正处于阻断态则立即解除阻断并计一次切换。
     */
    [[nodiscard]] bool setConfig(const GgaGateConfig &cfg) noexcept;

    /**
     * @brief  清空运行期阻断态与统计计数（保留当前配置）
     * @note   单任务调用，不阻塞、不分配堆
     */
    void reset() noexcept;

    /**
     * @brief  判定单条原始 GGA 帧是否允许放行，并推进迟滞状态机与统计计数
     * @param  ggaFrame: 完整 NMEA GGA 帧字节（如 "$GNGGA,...*hh\r\n"）
     * @retval true 允许向蓝牙/NTRIP 出口推送该 GGA
     * @retval false 本条 GGA 被门控阻断，应直接丢弃
     * @note   字段为空、缺失或解析失败均按不达标处理；单任务调用，不阻塞。
     */
    [[nodiscard]] bool evaluate(std::span<const uint8_t> ggaFrame) noexcept;

    /**
     * @brief  查询当前门控是否允许输出 GGA（供蓝牙直传、0x04 query 与未来 4G NTRIP 共用）
     * @retval true 门控关闭或当前处于放行态
     * @retval false 门控开启且当前处于阻断态
     * @note   只读查询，不阻塞
     */
    [[nodiscard]] bool ggaAllowed() const noexcept
    {
        return !cfg_.enabled || !blocked_;
    }

    /**
     * @brief  读取当前生效的门控配置
     * @retval 配置常引用
     * @note   只读查询，不阻塞
     */
    [[nodiscard]] const GgaGateConfig &config() const noexcept
    {
        return cfg_;
    }

    /**
     * @brief  导出当前门控状态与累计计数快照
     * @retval GgaGateStats 快照
     * @note   只读查询，不阻塞
     */
    [[nodiscard]] GgaGateStats stats() const noexcept;

private:
    GgaGateConfig cfg_{};
    bool blocked_ = false;
    uint8_t goodStreak_ = 0U;
    uint32_t ggaBlocked_ = 0U;
    uint32_t gateTrans_ = 0U;
};

} // namespace gnss
