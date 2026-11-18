// GNSS App 数据通路（ADR-0011）：6 KiB 最新帧缓存、统一输出配置与输出计划、跨任务订阅状态。
// 线程安全：GnssLatestCache 与 GnssAppSubState 通过注入的 LockOps 保护（DataCenter::mux）；classify/GnssOutConfig
// 为纯函数/值对象。ISR 安全：否。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/services/GnssService.hpp"
#include "fw/Lock.hpp"
#include "fw/Result.hpp"
#include "fw/Status.hpp"
#include "gnss/Receiver.hpp"

namespace app::data {

inline constexpr size_t kGnssAppMsgCount = core::services::kGnssMsgCount; // 6
inline constexpr size_t kGnssLatestTotalBytes = 6144U;                    // 4*256 + 1024 + 4096
inline constexpr size_t kGnssMaxQueryBytes = 256U;
inline constexpr uint8_t kGnssOutMaskAll = 0x3FU;

inline constexpr std::array<size_t, kGnssAppMsgCount> kGnssLatestCaps = {256U, 4096U, 1024U,
                                                                         256U, 256U,  256U};
inline constexpr std::array<size_t, kGnssAppMsgCount> kGnssLatestOffsets = {0U,    256U,  4352U,
                                                                            5376U, 5632U, 5888U};

/**
 * @brief 返回 typeIdx（0..5）对应的可读语句短名。线程/ISR 安全：是。
 * @param typeIdx: App 语句下标（0..5）
 * @retval 下标对应语句的静态短名；越界返回 ?
 */
[[nodiscard]] const char *gnssTypeName(size_t typeIdx) noexcept;

/**
 * @brief 返回协议 msgId 对应的 typeIdx（0..5）；未知返回 std::nullopt。线程/ISR 安全：是。
 * @param msgId: 协议消息 ID
 * @retval 已知消息的下标；未知消息返回 std::nullopt
 */
[[nodiscard]] std::optional<uint8_t> gnssTypeFromMsgId(uint16_t msgId) noexcept;

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
    uint32_t anchorMs = 0U;
    bool armed = false;

    /**
     * @brief periodMs=0 表示每条都推；返回 true 时调用者应写入，写成功后调用 onPushed()。
     * @param nowMs: 当前单调时间（毫秒）
     * @param periodMs: 周期（毫秒），0 表示每条放行
     * @retval true 首帧、无周期或已到放行窗口
     * @retval false 尚未到放行窗口
     * @note 仅 kIo 任务，非 ISR；成功写入后再调用 onPushed。
     */
    [[nodiscard]] bool due(uint32_t nowMs, uint32_t periodMs) const noexcept;
    /**
     * @brief 提交成功后推进订阅节流锚点
     * @param nowMs: 当前单调时间（毫秒）
     * @param periodMs: 周期（毫秒），0 表示每条放行
     * @note 仅 kIo 任务，非 ISR，不阻塞；只在写入成功后调用。
     */
    void onPushed(uint32_t nowMs, uint32_t periodMs) noexcept;
    /**
     * @brief 清除订阅节流锚点的启用状态
     * @note 仅 kIo 任务，非 ISR，不阻塞。
     */
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

    uint8_t version = 0U;
    uint8_t mask = 0U;
    std::array<int8_t, kGnssAppMsgCount> intervals{};

    /**
     * @brief 取得默认 GNSS 输出配置
     * @retval 版本 1、六类输出全开及默认周期的值对象
     * @note 纯函数，线程/ISR 安全。
     */
    [[nodiscard]] static constexpr GnssOutConfig defaults() noexcept
    {
        return {
            .version = kCurrentVersion,
            .mask = kGnssOutMaskAll,
            .intervals = {1, kDefaultSatsRate, 1, 1, 1, kDefaultGsaRate},
        };
    }

    /**
     * @brief 校验版本、保留掩码位与已启用语句的 interval 合法性。线程/ISR 安全：是。
     * @retval true 版本、保留位与已启用周期均合法
     * @retval false 任一校验不通过
     * @note 只读值对象；无并发修改时线程/ISR 安全。
     */
    [[nodiscard]] bool isValid() const noexcept;

    /**
     * @brief 设置或关闭一类输出语句
     * @param typeIdx: App 语句下标（0..5）
     * @param interval: 协议有符号周期，0 关闭
     * @retval kOk 已更新
     * @retval kInvalidArg 下标越界，配置不变
     * @note 只改值对象，不阻塞；调用方串行写入。
     */
    [[nodiscard]] fw::Result applyInterval(size_t typeIdx, int8_t interval) noexcept;

    /**
     * @brief 序列化到固定 8 B 缓冲。线程/ISR 安全：是。
     * @param[out] out: 输出缓冲或结果
     * @param[out] len: 成功置 8，失败保持原值
     * @retval kOk 已写入固定 8 字节并设置 len
     * @retval kInvalidArg 配置非法，输出不变
     * @retval kNoSpace 输出不足 8 字节，输出不变
     * @note 值对象操作，不分配、不阻塞；无并发修改时线程/ISR 安全。
     */
    [[nodiscard]] fw::Result encode(std::span<uint8_t> out, size_t &len) const noexcept;

    /**
     * @brief 从字节串反序列化并校验；非法返回 kInvalidArg 且不改 out。线程/ISR 安全：是。
     * @param in: 输入字节视图
     * @param[out] out: 配置输出，失败不变
     * @retval kOk 解码并更新 out
     * @retval kInvalidArg 长度或配置非法，out 不变
     * @note 纯计算，不分配、不阻塞；无共享写入时线程/ISR 安全。
     */
    [[nodiscard]] static fw::Result decode(std::span<const uint8_t> in,
                                           GnssOutConfig &out) noexcept;
};

/**
 * @brief 判断某条输出是否跟随定位频率（`gnss rate`）下发
 * @param out: 输出配置
 * @param index: App 输出下标（与 gnss::OutputMsg 前 6 项一致）
 * @retval true GGA/RMC/GSA/GST 且取默认间隔：按定位频率下发，不按表内数值（ADR-0012 §2.4）
 * @retval false 按表内间隔下发
 * @note 纯函数，线程/ISR 安全；下发（makeOutputPlan）与控制台显示共用。
 */
[[nodiscard]] constexpr bool followsFixRate(const GnssOutConfig &out, size_t index) noexcept
{
    const bool locate = (index == static_cast<size_t>(gnss::OutputMsg::kGga)) ||
                        (index == static_cast<size_t>(gnss::OutputMsg::kRmc)) ||
                        (index == static_cast<size_t>(gnss::OutputMsg::kGsa)) ||
                        (index == static_cast<size_t>(gnss::OutputMsg::kGst));
    return locate && (index < kGnssAppMsgCount) &&
           (out.intervals[index] == GnssOutConfig::defaults().intervals[index]);
}

/**
 * @brief 按统一配置生成接收机输出计划
 * @param out: 有效的持久化输出配置；关闭掩码位对应周期 0
 * @param rateHz: 定位频率，0 按 1 Hz 处理
 * @param gsv: 是否输出 1 Hz GSV
 * @param port: 设备输出端口
 * @param caps: 接收机能力掩码
 * @retval 带端口、定位频率与七类语句周期的输出计划
 * @note 纯计算，线程/ISR 安全。默认 GGA/RMC/GSA/GST 跟定位频率；SATSINFOA 为 5 s；HEADINGA 仅有能力时为 1 s；GSV 由 gsv 决定。
 */
[[nodiscard]] gnss::OutputPlan makeOutputPlan(const GnssOutConfig &out, uint8_t rateHz, bool gsv,
                                              uint8_t port, uint32_t caps) noexcept;

struct GnssLatestSlotStats {
    uint16_t len = 0;
    uint16_t cap = 0;
    uint32_t updates = 0;
    uint32_t oversizeDrops = 0;
    uint32_t queryOversize = 0;
};

/** 6 KiB 最新语句缓存（4*256 + 1024 + 4096），供 interval==0 单次查询读取。 */
class GnssLatestCache {
public:
    constexpr GnssLatestCache() noexcept = default;
    ~GnssLatestCache() = default;
    GnssLatestCache(const GnssLatestCache &) = delete;
    GnssLatestCache &operator=(const GnssLatestCache &) = delete;

    /**
     * @brief 启动前绑定跨任务锁。线程安全：仅启动前。
     * @param lock: 跨任务同步操作表
     * @note 仅启动前单线程调用，不阻塞，非 ISR。
     */
    void bindLock(fw::LockOps lock) noexcept
    {
        lock_ = lock;
    }

    /**
     * @brief 绑定外供 6 KiB 缓冲并清零长度与统计；buf 须非空且 ≥ kGnssLatestTotalBytes。线程安全：仅启动前。
     * @param buf: 外供缓存，生命周期覆盖读写者
     * @retval kOk 缓存已绑定并清零统计
     * @retval kInvalidArg 缓冲为空或不足 6 KiB
     * @note 仅启动前单线程调用；内部使用已绑定锁，非 ISR。
     */
    [[nodiscard]] fw::Status init(std::span<uint8_t> buf) noexcept;

    /**
     * @brief 写入最新一条帧；超每类上限时丢弃并计 oversizeDrops，返回 false。线程安全：是（非 ISR）。
     * @param typeIdx: App 语句下标（0..5）
     * @param frame: 完整帧字节
     * @retval true 最新帧已更新
     * @retval false 下标非法、帧为空、缓存未绑定或帧超容量
     * @note 内部加锁，跨任务安全，非 ISR；超长帧计数并保持旧帧。
     */
    [[nodiscard]] bool update(uint8_t typeIdx, std::span<const uint8_t> frame) noexcept;

    /**
     * @brief 读取 <=256 B 最新帧到 dst；超 256 B 时设 oversize=true 并计 queryOversize。线程安全：是（非 ISR）。
     * @param typeIdx: App 语句下标（0..5）
     * @param[out] dst: 最新帧输出，失败不变
     * @retval ok=true 已复制最新帧
     * @retval oversize=true 帧超过 256 B 或目标容量，目标不变
     * @retval 空结果 下标非法、未初始化或无帧，目标不变
     * @note 内部加锁，跨任务安全，非 ISR。
     */
    [[nodiscard]] core::services::GnssQueryResult query(uint8_t typeIdx,
                                                        std::span<uint8_t> dst) noexcept;

    /**
     * @brief 读取 6 类缓存与丢弃统计快照。线程安全：是（非 ISR）。
     * @retval 内部加锁复制的当前统计快照
     * @note 跨任务安全，内部加锁，非 ISR。
     */
    [[nodiscard]] std::array<GnssLatestSlotStats, kGnssAppMsgCount> snapshot() const noexcept;

private:
    fw::LockOps lock_{};
    std::span<uint8_t> buf_{};
    std::array<uint16_t, kGnssAppMsgCount> lengths_{};
    std::array<uint32_t, kGnssAppMsgCount> updates_{};
    std::array<uint32_t, kGnssAppMsgCount> oversize_drops_{};
    std::array<uint32_t, kGnssAppMsgCount> query_oversize_{};
};

struct GnssAppSubSnapshot {
    uint32_t activeMask = 0;
    std::array<uint32_t, kGnssAppMsgCount> periodsMs{};
    std::array<uint32_t, kGnssAppMsgCount> txSent{};
    std::array<uint32_t, kGnssAppMsgCount> skipped{};
    std::array<uint32_t, kGnssAppMsgCount> ringOversize{};
    uint32_t queryOversizeTotal = 0;
};

/** 跨任务（kLink 写订阅掩码/发包计数，kIo 读周期门限/写跳过计数）共享的 App 订阅与计数状态。 */
class GnssAppSubState {
public:
    constexpr GnssAppSubState() noexcept = default;
    ~GnssAppSubState() = default;
    GnssAppSubState(const GnssAppSubState &) = delete;
    GnssAppSubState &operator=(const GnssAppSubState &) = delete;

    /**
     * @brief 启动前绑定跨任务锁。线程安全：仅启动前。
     * @param lock: 跨任务同步操作表
     * @note 仅启动前单线程调用，不阻塞，非 ISR。
     */
    void bindLock(fw::LockOps lock) noexcept
    {
        lock_ = lock;
    }

    /**
     * @brief kLink 更新活跃订阅掩码与 6 路生效周期（ms）。线程安全：是（非 ISR）。
     * @param activeMask: 活跃订阅位掩码
     * @param periodsMs: 六类语句的实际周期（毫秒）
     * @note 仅 kLink 写入，内部加锁，非 ISR。
     */
    void setSubscription(uint32_t activeMask,
                         const std::array<uint32_t, kGnssAppMsgCount> &periodsMs) noexcept;

    /**
     * @brief kIo 读取当前掩码与生效周期。线程安全：是（非 ISR）。
     * @param activeMask: 当前订阅位掩码输出
     * @param periodsMs: 当前六类周期输出（毫秒）
     * @note 仅 kIo 读取，内部加锁，非 ISR。
     */
    void readSubscription(uint32_t &activeMask,
                          std::array<uint32_t, kGnssAppMsgCount> &periodsMs) const noexcept;

    /**
     * @brief kLink 记录一帧已提交至 SPP TX.stream。线程安全：是（非 ISR）。
     * @param typeIdx: App 语句下标（0..5）
     * @note 内部加锁，越界下标忽略；发送计数由 kLink、跳过与超长计数由 kIo 更新，非 ISR。
     */
    void addTxSent(uint8_t typeIdx) noexcept;

    /**
     * @brief kIo 记录一帧因无订阅或周期节流跳过入环。线程安全：是（非 ISR）。
     * @param typeIdx: App 语句下标（0..5）
     * @note 内部加锁，越界下标忽略；发送计数由 kLink、跳过与超长计数由 kIo 更新，非 ISR。
     */
    void addSkipped(uint8_t typeIdx) noexcept;

    /**
     * @brief kIo 记录一帧因超 App 环单条载荷上限（>cap-3）被 writer 丢弃。线程安全：是（非 ISR）。
     * @param typeIdx: App 语句下标（0..5）
     * @note 内部加锁，越界下标忽略；发送计数由 kLink、跳过与超长计数由 kIo 更新，非 ISR。
     */
    void addRingOversize(uint8_t typeIdx) noexcept;

    /**
     * @brief kLink 同步当前 queryOversize 累计值。线程安全：是（非 ISR）。
     * @param total: 查询超长累计值
     * @note 仅 kLink 写入，内部加锁，非 ISR。
     */
    void setQueryOversizeTotal(uint32_t total) noexcept;

    /**
     * @brief 读取完整快照，供 status / gnss status 展示。线程安全：是（非 ISR）。
     * @retval 内部加锁复制的当前统计快照
     * @note 跨任务安全，内部加锁，非 ISR。
     */
    [[nodiscard]] GnssAppSubSnapshot snapshot() const noexcept;

private:
    fw::LockOps lock_{};
    GnssAppSubSnapshot state_{};
};

} // namespace app::data
