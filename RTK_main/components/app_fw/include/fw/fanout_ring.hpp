// 一写多读记录环（借鉴 SparkFun 的 GNSS 输出环形缓冲，改为单写者永不阻塞）：
// - 记录 = 2 字节小端长度前缀 + 负载，在环内可以跨越末尾回绕；负载长度 1..max_record。
// - 写满时覆盖最旧记录；落后读者下次 read() 时整条跳过被覆盖的记录并累计该读者的 dropped（按记录计）。
// - 每个读者有独立尾指针；读者通过 attach() 获得 id，从“当前最新位置”开始读（不回放历史）。
// - 位置用单调递增的 u32 字节计数（回绕安全：只比较差值，差值 < 2^31）。
// 线程模型：write() 只由一个写者任务调用；每个 reader id 只由一个读者任务调用；不同任务之间由注入的锁保护。
// 临界区内只做索引运算与一条记录的 memcpy（≤ max_record）。不可在 ISR 中调用。
// 冻结接口（ADR-0007 §3.6-1）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/lock.hpp"
#include "fw/result.hpp"

namespace fw {

inline constexpr size_t kFanoutMaxReaders = 4;
inline constexpr size_t kFanoutHeaderBytes = 2;

/** 单个读者的累计统计；读者 detach 时清零，重新 attach 从零开始。 */
struct FanoutReaderStats {
    uint32_t records = 0; // 成功读出的记录数
    uint32_t bytes = 0;   // 成功读出的负载字节数
    uint32_t dropped = 0; // 因被覆盖而跳过的记录数
};

/** 写者累计统计；init() 后清零。 */
struct FanoutWriterStats {
    uint32_t records = 0;     // 成功写入的记录数
    uint32_t bytes = 0;       // 成功写入的负载字节数
    uint32_t overwritten = 0; // 为腾出空间而淘汰的记录数
    uint32_t rejected = 0;    // 长度为 0 或超过 max_record 的写入
};

/**
 * 固定容量、单写多读、写者永不阻塞的记录环；存储由调用方提供（不分配堆）。
 * 生命周期：init() 前不持有存储，init() 后存储必须覆盖本对象生命周期；不可拷贝/移动。
 */
class FanoutRing {
public:
    constexpr FanoutRing() noexcept = default;
    FanoutRing(const FanoutRing &) = delete;
    FanoutRing &operator=(const FanoutRing &) = delete;
    FanoutRing(FanoutRing &&) = delete;
    FanoutRing &operator=(FanoutRing &&) = delete;
    ~FanoutRing() = default;

    /**
     * 启动前（单线程）调用。storage 由调用方提供且生命周期覆盖本对象；
     * 要求 max_record ≥ 1、max_record ≤ 65535、storage.size() ≥ 2 × (max_record + kFanoutHeaderBytes)，否则
     * kInvalidArg。 重复 init → kInvalidState。
     */
    [[nodiscard]] Result init(std::span<uint8_t> storage, size_t max_record, const LockOps &lock) noexcept;

    /** 登记一个读者，返回 [0, kFanoutMaxReaders) 的 id；满了 → kNoSpace；未 init → kInvalidState。 */
    [[nodiscard]] Result attach(uint8_t &reader_id) noexcept;
    /** 注销读者（id 可被再次 attach 复用）；无效 id → kInvalidArg。 */
    [[nodiscard]] Result detach(uint8_t reader_id) noexcept;

    /**
     * 写入一条记录（写者任务）。空间不够时淘汰最旧记录直到放得下，不等待任何读者。
     * len 为 0 或 > max_record → kInvalidArg 并计入 rejected；未 init → kInvalidState。
     */
    [[nodiscard]] Result write(std::span<const uint8_t> record) noexcept;

    /**
     * 读出该读者的下一条记录（读者任务）。
     * - 有记录：拷到 out，len = 负载长度，返回 kOk。
     * - out 放不下：kNoSpace，len = 需要的长度，尾指针不动。
     * - 没有新记录：kNotFound，len = 0。
     * - 读者曾被覆盖：先跳到最旧的可读记录并累加 dropped，再按上述规则返回。
     * 无效/未 attach 的 id → kInvalidArg。
     */
    [[nodiscard]] Result read(uint8_t reader_id, std::span<uint8_t> out, size_t &len) noexcept;

    [[nodiscard]] FanoutReaderStats reader_stats(uint8_t reader_id) const noexcept;
    [[nodiscard]] FanoutWriterStats writer_stats() const noexcept;

private:
    struct Reader {
        bool attached = false;
        uint32_t tail = 0; // 单调字节位置
        FanoutReaderStats stats{};
    };

    LockOps lock_{};
    uint8_t *buf_ = nullptr;
    size_t cap_ = 0;
    size_t max_record_ = 0;
    uint32_t head_ = 0;   // 下一条记录写入位置（单调）
    uint32_t oldest_ = 0; // 最旧有效记录起始位置（单调）
    std::array<Reader, kFanoutMaxReaders> readers_{};
    FanoutWriterStats wstats_{};
    bool inited_ = false;
};

} // namespace fw
