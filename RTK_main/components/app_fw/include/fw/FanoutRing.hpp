// 一写多读记录环：legacy 为 2B 长度头并覆盖整条；typed 为 3B 头、订阅与短租约。
// 单写者任务、每 id 单读者任务；注入锁保护元数据，非 ISR 安全，不分配堆。
// typed 先保留空闲区、锁外复制、最后发布；租约存续期间负载不覆盖、不搬动。
// legacy 临界区包含整条复制；typed 每次临界区仅处理一条记录头和至多 4 个读者。
// 冻结接口（ADR-0007 §3.6-1、ADR-0011 §5）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/Lock.hpp"
#include "fw/Result.hpp"

namespace fw {

inline constexpr size_t kFanoutMaxReaders = 4;
inline constexpr size_t kFanoutHeaderBytes = 2;
inline constexpr size_t kFanoutTypedHeaderBytes = 3;
using FanoutMask = uint32_t;

/** 仅在生成它的环上使用；commit/cancel 后禁止继续访问原 view，禁止跨等待持有。 */
struct FanoutLease {
    uint32_t generation = 0;
    uint32_t serial = 0;
    uint8_t readerId = 0;
};

/** 两段仅含负载；租约期间只读，长度和为整条记录长度。 */
struct FanoutView {
    FanoutLease lease{};
    uint8_t type = 0;
    std::span<const uint8_t> first{};
    std::span<const uint8_t> second{};
};

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
    uint32_t rejected = 0;    // 长度为 0 或超过 maxRecord 的写入
};

/** typed 读者统计；计数按 uint32_t 回绕，重新 attach 后清零。 */
struct FanoutTypedReaderStats {
    FanoutReaderStats base{};
    uint32_t filtered = 0;
    uint32_t pinnedDrop = 0;
};

/** typed 写者统计；base.rejected 包含参数错误与 oversize，不包含 pin 拒新。 */
struct FanoutTypedWriterStats {
    FanoutWriterStats base{};
    uint32_t noSubscriber = 0;
    uint32_t pinnedDrop = 0;
    uint32_t oversizeDrop = 0;
    size_t highWaterBytes = 0;
};

/**
 * 固定容量、单写多读、写者永不阻塞的记录环；存储由调用方提供（不分配堆）。
 * 生命周期：init() 前不持有存储，init() 后存储必须覆盖本对象生命周期；不可拷贝/移动。
 * 销毁前所有任务和租约必须静止，并丢弃全部 token；不允许携带旧 token 重建对象。
 * legacy/typed 专用接口混用返回 kNotSupported；detach 和 base stats 两种模式共用。
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
     * 要求 maxRecord ≥ 1、maxRecord ≤ 65535、storage.size() ≥ 2 × (maxRecord + kFanoutHeaderBytes)，否则
     * kInvalidArg。 重复 init → kInvalidState。
     */
    [[nodiscard]] Result init(std::span<uint8_t> storage, size_t maxRecord,
                              const LockOps &lock) noexcept;

    /** 登记一个读者，返回 [0, kFanoutMaxReaders) 的 id；满了 → kNoSpace；未 init → kInvalidState。 */
    [[nodiscard]] Result attach(uint8_t &readerId) noexcept;
    /** 注销读者（id 可被再次 attach 复用）；无效 id → kInvalidArg。 */
    [[nodiscard]] Result detach(uint8_t readerId) noexcept;

    /**
     * 写入一条记录（写者任务）。空间不够时淘汰最旧记录直到放得下，不等待任何读者。
     * len 为 0 或 > maxRecord → kInvalidArg 并计入 rejected；未 init → kInvalidState。
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
    [[nodiscard]] Result read(uint8_t readerId, std::span<uint8_t> out, size_t &len) noexcept;

    [[nodiscard]] FanoutReaderStats readerStats(uint8_t readerId) const noexcept;
    [[nodiscard]] FanoutWriterStats writerStats() const noexcept;

    /**
     * 启动前单线程、非 ISR；typed 容量为二次幂且 <2^31，maxRecord 为 1..65535。
     * 要求 maxRecord+3 <= cap，否则 kInvalidArg；重复初始化 kInvalidState。
     */
    [[nodiscard]] Result initTyped(std::span<uint8_t> storage, size_t maxRecord,
                                   const LockOps &lock) noexcept;
    /** 读者任务、非 ISR；mask=0 不接收，attach 从 head 开始；满 kNoSpace。
     * generation 到 UINT32_MAX 的槽永久退役，全部空闲槽退役时 kInvalidState。
     */
    [[nodiscard]] Result attach(FanoutMask mask, uint8_t &readerId) noexcept;
    /** 该读者任务、非 ISR；有租约 kBusy，否则修改 mask 并移至 head，不计历史丢弃。 */
    [[nodiscard]] Result setMask(uint8_t readerId, FanoutMask mask) noexcept;
    /** 单写者任务、非 ISR；type<32；无匹配订阅 kOk/noSubscriber++。
     * pin 阻挡时 kBusy，整条拒新且只计 writer/匹配读者 pinnedDrop；无等待。
     * 参数错误 kInvalidArg；超过 maxRecord 或容量额外计 oversizeDrop。
     * payload 不得与环存储重叠，调用期间源字节保持不变。
     */
    [[nodiscard]] Result write(uint8_t type, std::span<const uint8_t> payload) noexcept;
    /** 该读者任务、非 ISR；过滤未订阅记录；空 kNotFound，已有租约 kBusy。
     * serial 到 UINT32_MAX 后 kInvalidState，须 detach/attach 换 generation；绝不复用 token。
     * 失败时 view 清空；租约仅覆盖当次有界复制（BT 总量 <=4096B），不得跨 IO/等待。
     */
    [[nodiscard]] Result peek(uint8_t readerId, FanoutView &view) noexcept;
    /** 该读者任务、非 ISR；消费整条并释放 pin；无效/过期 token kInvalidArg，不移动 tail。 */
    [[nodiscard]] Result commit(const FanoutLease &lease) noexcept;
    /** 该读者任务、非 ISR；释放 pin 但不消费；过期 token kInvalidArg。 */
    [[nodiscard]] Result cancel(const FanoutLease &lease) noexcept;
    /** 任意任务、非 ISR；注入锁保护快照，无效 id/非 typed 返回空统计。 */
    [[nodiscard]] FanoutTypedReaderStats typedReaderStats(uint8_t readerId) const noexcept;
    /** 任意任务、非 ISR；注入锁保护快照，未 init/非 typed 返回空统计。 */
    [[nodiscard]] FanoutTypedWriterStats typedWriterStats() const noexcept;

private:
    // 仅供 host 边界测试访问计数器；不提供运行期重置入口。
    friend struct FanoutRingTestAccess;

    struct Reader {
        bool attached = false;
        uint32_t tail = 0; // 单调字节位置
        FanoutReaderStats stats{};
        FanoutMask mask = 0;
        uint32_t generation = 0;
        uint32_t serial = 0;
        uint32_t filtered = 0;
        uint32_t pinnedDrop = 0;
        bool pinned = false;
    };

    [[nodiscard]] Result beginTypedWrite(uint8_t type, size_t size) noexcept;
    void evictTyped() noexcept; // 调用者持锁，每次只淘汰一条记录。
    [[nodiscard]] uint16_t typedLength(uint32_t pos) const noexcept;
    [[nodiscard]] uint8_t typedType(uint32_t pos) const noexcept;
    [[nodiscard]] Result finishLease(const FanoutLease &lease, bool consume) noexcept;

    LockOps lock_{};
    uint8_t *buf_ = nullptr;
    size_t cap_ = 0;
    size_t max_record_ = 0;
    uint32_t head_ = 0;   // 下一条记录写入位置（单调）
    uint32_t oldest_ = 0; // 最旧有效记录起始位置（单调）
    std::array<Reader, kFanoutMaxReaders> readers_{};
    FanoutWriterStats wstats_{};
    bool inited_ = false;
    bool typed_ = false;
    size_t reserved_bytes_ = 0;             // 锁保护；[head, head+reserved) 直到最终发布都不可见。
    FanoutTypedWriterStats typed_wstats_{}; // base 快照取自 wstats_，不重复更新。
};

} // namespace fw
