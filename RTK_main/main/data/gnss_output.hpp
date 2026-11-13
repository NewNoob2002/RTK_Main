// GNSS App 数据通路（K4 / ADR-0011）：6 类语句识别、6 KiB 最新帧缓存、统一输出配置与跨任务订阅状态。
// 线程安全：GnssLatestCache 与 GnssAppSubState 通过注入的 LockOps 保护（DataCenter::mux）；classify/GnssOutConfig
// 为纯函数/值对象。ISR 安全：否。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/services/gnss_service.hpp"
#include "fw/fanout_ring.hpp"
#include "fw/lock.hpp"
#include "fw/status.hpp"

namespace app::data {

inline constexpr size_t kGnssAppMsgCount = core::services::kGnssMsgCount; // 6
inline constexpr size_t kGnssLatestTotalBytes = 6144U;                    // 4*256 + 1024 + 4096
inline constexpr size_t kGnssMaxQueryBytes = 256U;
inline constexpr uint8_t kGnssOutMaskAll = 0x3FU;

inline constexpr std::array<size_t, kGnssAppMsgCount> kGnssLatestCaps = {256U, 4096U, 1024U, 256U, 256U, 256U};
inline constexpr std::array<size_t, kGnssAppMsgCount> kGnssLatestOffsets = {0U, 256U, 4352U, 5376U, 5632U, 5888U};

/** 返回 type_idx（0..5）对应的可读语句短名。线程/ISR 安全：是。 */
[[nodiscard]] const char *gnss_type_name(size_t type_idx) noexcept;

/** 返回协议 msg_id 对应的 type_idx（0..5）；未知返回 std::nullopt。线程/ISR 安全：是。 */
[[nodiscard]] std::optional<uint8_t> gnss_type_from_msg_id(uint16_t msg_id) noexcept;

/**
 * 识别 GNSS 原始帧是否属于 6 类 App 可订阅语句（NMEA 忽略 talker；Unicore Hash 匹配 #SATSINFOA, / #HEADINGA,）。
 * 线程/ISR 安全：是（纯函数）。
 */
[[nodiscard]] std::optional<uint8_t> classify_gnss_sentence(std::span<const uint8_t> frame) noexcept;

/**
 * 订阅降采样闸门（每类语句一份，单写者：GNSS 模块 kIo 任务）。
 * 语句到达时刻有几十毫秒抖动：若按“now-上次 ≥ 周期”判断，源频率等于订阅频率时，
 * 早到 1 ms 就会被跳过，实测 1 Hz 订阅出现 2 s 间隔。因此：
 *  - 允许提前 slack = min(周期/4, 100 ms) 放行；
 *  - 放行后锚点按周期推进（而非取 now），平均速率不因 slack 偏高；
 *  - 落后超过 2 个周期（源停顿/首次）时锚点重置为 now，避免补发突发。
 */
struct GnssPushGate {
    static constexpr uint32_t kMaxSlackMs = 100U;
    uint32_t anchor_ms = 0U;
    bool armed = false;

    /** period_ms=0 表示每条都推；返回 true 时调用者应写入，写成功后调用 on_pushed()。 */
    [[nodiscard]] bool due(uint32_t now_ms, uint32_t period_ms) const noexcept;
    void on_pushed(uint32_t now_ms, uint32_t period_ms) noexcept;
    void reset() noexcept
    {
        armed = false;
    }
};

inline constexpr int8_t kDefaultSatsRate = -5;
inline constexpr int8_t kDefaultGsaRate = -3;

/**
 * 统一输出配置值对象（ADR-0011 §6.2 / ADR-0010 §2.10），用于 NVS `gnss/out` 持久化与控制台命令。
 * 序列化布局固定 8 B：[0]=version(1), [1]=mask(bit 0..5), [2..7]=int8_t intervals[0..5]。
 */
struct GnssOutConfig {
    static constexpr uint8_t kCurrentVersion = 1U;
    static constexpr size_t kBlobBytes = 8U;

    uint8_t version = kCurrentVersion;
    uint8_t mask = kGnssOutMaskAll;
    std::array<int8_t, kGnssAppMsgCount> intervals = {1, kDefaultSatsRate, 1, 1, 1, kDefaultGsaRate};

    [[nodiscard]] static constexpr GnssOutConfig defaults() noexcept
    {
        return GnssOutConfig{};
    }

    /** 校验版本、保留掩码位与已启用语句的 interval 合法性。线程/ISR 安全：是。 */
    [[nodiscard]] bool is_valid() const noexcept;

    /** 按统一规则设置或关闭某类语句（interval==0 关闭该类输出，!=0 开启并校验）。线程/ISR 安全：是。 */
    [[nodiscard]] fw::Result apply_interval(size_t type_idx, int8_t interval) noexcept;

    /** 序列化到固定 8 B 缓冲。线程/ISR 安全：是。 */
    [[nodiscard]] fw::Result encode(std::span<uint8_t> out, size_t &len) const noexcept;

    /** 从字节串反序列化并校验；非法返回 kInvalidArg 且不改 out。线程/ISR 安全：是。 */
    [[nodiscard]] static fw::Result decode(std::span<const uint8_t> in, GnssOutConfig &out) noexcept;
};

struct GnssLatestSlotStats {
    uint16_t len = 0;
    uint16_t cap = 0;
    uint32_t updates = 0;
    uint32_t oversize_drops = 0;
    uint32_t query_oversize = 0;
};

/** 6 KiB 最新语句缓存（4*256 + 1024 + 4096），供 interval==0 单次查询读取。 */
class GnssLatestCache {
public:
    constexpr GnssLatestCache() noexcept = default;
    ~GnssLatestCache() = default;
    GnssLatestCache(const GnssLatestCache &) = delete;
    GnssLatestCache &operator=(const GnssLatestCache &) = delete;

    /** 启动前绑定跨任务锁。线程安全：仅启动前。 */
    void bind_lock(fw::LockOps lock) noexcept
    {
        lock_ = lock;
    }

    /** 写入最新一条帧；超每类上限时丢弃并计 oversize_drops，返回 false。线程安全：是（非 ISR）。 */
    [[nodiscard]] bool update(uint8_t type_idx, std::span<const uint8_t> frame) noexcept;

    /** 读取 <=256 B 最新帧到 dst；超 256 B 时设 oversize=true 并计 query_oversize。线程安全：是（非 ISR）。 */
    [[nodiscard]] core::services::GnssQueryResult query(uint8_t type_idx, std::span<uint8_t> dst) noexcept;

    /** 读取 6 类缓存与丢弃统计快照。线程安全：是（非 ISR）。 */
    [[nodiscard]] std::array<GnssLatestSlotStats, kGnssAppMsgCount> snapshot() const noexcept;

private:
    fw::LockOps lock_{};
    std::array<uint8_t, kGnssLatestTotalBytes> buf_{};
    std::array<uint16_t, kGnssAppMsgCount> lengths_{};
    std::array<uint32_t, kGnssAppMsgCount> updates_{};
    std::array<uint32_t, kGnssAppMsgCount> oversize_drops_{};
    std::array<uint32_t, kGnssAppMsgCount> query_oversize_{};
};

struct GnssAppSubSnapshot {
    uint32_t active_mask = 0;
    std::array<uint32_t, kGnssAppMsgCount> periods_ms{};
    std::array<uint32_t, kGnssAppMsgCount> tx_sent{};
    std::array<uint32_t, kGnssAppMsgCount> skipped{};
    std::array<uint32_t, kGnssAppMsgCount> ring_oversize{};
    uint32_t query_oversize_total = 0;
};

/** 跨任务（kLink 写订阅掩码/发包计数，kIo 读周期门限/写跳过计数）共享的 App 订阅与计数状态。 */
class GnssAppSubState {
public:
    constexpr GnssAppSubState() noexcept = default;
    ~GnssAppSubState() = default;
    GnssAppSubState(const GnssAppSubState &) = delete;
    GnssAppSubState &operator=(const GnssAppSubState &) = delete;

    /** 启动前绑定跨任务锁。线程安全：仅启动前。 */
    void bind_lock(fw::LockOps lock) noexcept
    {
        lock_ = lock;
    }

    /** kLink 更新活跃订阅掩码与 6 路生效周期（ms）。线程安全：是（非 ISR）。 */
    void set_subscription(uint32_t active_mask, const std::array<uint32_t, kGnssAppMsgCount> &periods_ms) noexcept;

    /** kIo 读取当前掩码与生效周期。线程安全：是（非 ISR）。 */
    void read_subscription(uint32_t &active_mask, std::array<uint32_t, kGnssAppMsgCount> &periods_ms) const noexcept;

    /** kLink 记录一帧已提交至 SPP TX.stream。线程安全：是（非 ISR）。 */
    void add_tx_sent(uint8_t type_idx) noexcept;

    /** kIo 记录一帧因无订阅或周期节流跳过入环。线程安全：是（非 ISR）。 */
    void add_skipped(uint8_t type_idx) noexcept;

    /** kIo 记录一帧因超 App 环单条载荷上限（>cap-3）被 writer 丢弃。线程安全：是（非 ISR）。 */
    void add_ring_oversize(uint8_t type_idx) noexcept;

    /** kLink 同步当前 query_oversize 累计值。线程安全：是（非 ISR）。 */
    void set_query_oversize_total(uint32_t total) noexcept;

    /** 读取完整快照，供 status / gnss status 展示。线程安全：是（非 ISR）。 */
    [[nodiscard]] GnssAppSubSnapshot snapshot() const noexcept;

private:
    fw::LockOps lock_{};
    GnssAppSubSnapshot state_{};
};

} // namespace app::data
