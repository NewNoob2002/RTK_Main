// 一写多读记录环：legacy 2B 长度头覆盖整条，typed 3B 头加订阅与短租约（ADR-0007 §3.6-1、ADR-0011 §5）。
// 单写者任务 + 每 id 单读者任务；注入锁只保护元数据，非 ISR 安全，不分配堆。
// legacy 临界区含整条复制，typed 临界区只处理记录头和至多 4 个读者；租约期内负载不覆盖、不搬动。
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
     * @brief  初始化 legacy 环，绑定调用方提供的存储与锁。
     * @param  storage: 环存储，长度须 ≥ 2 × (maxRecord + kFanoutHeaderBytes)，且 ≤ INT32_MAX。
     * @param  maxRecord: 单条负载上限，1..65535。
     * @param  lock: 元数据锁；lock/unlock 须同为 nullptr 或同为非 nullptr。
     * @retval Result::kOk 初始化成功
     * @retval Result::kInvalidArg 存储过小或过大、maxRecord 越界、锁表不成对
     * @retval Result::kInvalidState 已经初始化过
     * @note   仅启动前单线程调用，非 ISR；持注入锁，不阻塞、不分配堆。
     */
    [[nodiscard]] Result init(std::span<uint8_t> storage, size_t maxRecord,
                              const LockOps &lock) noexcept;

    /**
     * @brief  登记一个 legacy 读者并分配 id。
     * @param[out] readerId: 成功时写入 [0, kFanoutMaxReaders)；失败时不修改。
     * @retval Result::kOk 登记成功
     * @retval Result::kNoSpace 读者槽已满
     * @retval Result::kInvalidState 尚未初始化
     * @note   任务上下文（非 ISR）；持注入锁，不阻塞。
     */
    [[nodiscard]] Result attach(uint8_t &readerId) noexcept;
    /**
     * @brief  注销读者，释放 id 供再次 attach。
     * @param  readerId: 要注销的读者 id。
     * @retval Result::kOk 注销成功
     * @retval Result::kInvalidArg id 越界或该 id 未 attach
     * @retval Result::kBusy 该读者正持有租约
     * @note   任务上下文（非 ISR）；持注入锁，不阻塞。
     */
    [[nodiscard]] Result detach(uint8_t readerId) noexcept;

    /**
     * @brief  写入一条 legacy 记录，空间不够时淘汰最旧记录。
     * @param  record: 负载，长度须为 1..maxRecord。
     * @retval Result::kOk 写入成功
     * @retval Result::kInvalidArg 为空或超过 maxRecord，并计入 rejected
     * @retval Result::kInvalidState 尚未初始化
     * @note   仅写者任务调用，非 ISR；永不等待读者，不分配堆。
     */
    [[nodiscard]] Result write(std::span<const uint8_t> record) noexcept;

    /**
     * @brief  取出该读者的下一条 legacy 记录。
     * @param  readerId: 本读者 id。
     * @param[out] out: 接收负载的缓冲；放不下时不写入。
     * @param[out] len: 成功时为负载长度；kNoSpace 时为所需长度；kNotFound 时为 0。
     * @retval Result::kOk 读出并拷入 out
     * @retval Result::kNoSpace out 放不下，读者尾指针不动
     * @retval Result::kNotFound 没有新记录
     * @retval Result::kInvalidArg id 越界或未 attach
     * @retval Result::kInvalidState 尚未初始化
     * @retval Result::kNotSupported 环为 typed
     * @note   每个 id 只允许其读者任务调用，非 ISR；被覆盖时先跳到最旧可读记录并累加 dropped。
     */
    [[nodiscard]] Result read(uint8_t readerId, std::span<uint8_t> out, size_t &len) noexcept;

    [[nodiscard]] FanoutReaderStats readerStats(uint8_t readerId) const noexcept;
    [[nodiscard]] FanoutWriterStats writerStats() const noexcept;

    /**
     * @brief  初始化 typed 环，绑定调用方提供的存储与锁。
     * @param  storage: 环存储，容量须为 2 的幂且 < 2^31，并要求 ≥ maxRecord + kFanoutTypedHeaderBytes。
     * @param  maxRecord: 单条负载上限，1..65535。
     * @param  lock: 元数据锁；lock/unlock 须同为 nullptr 或同为非 nullptr。
     * @retval Result::kOk 初始化成功
     * @retval Result::kInvalidArg 容量非 2 的幂、过小或超过 2^31，maxRecord 越界，锁表不成对
     * @retval Result::kInvalidState 已经初始化过
     * @note   仅启动前单线程调用，非 ISR；持注入锁，不阻塞、不分配堆。
     */
    [[nodiscard]] Result initTyped(std::span<uint8_t> storage, size_t maxRecord,
                                   const LockOps &lock) noexcept;
    /**
     * @brief  登记一个 typed 读者，从当前 head 开始接收订阅记录。
     * @param  mask: 订阅位图，bit n 表示接收 type n；0 表示不接收任何记录。
     * @param[out] readerId: 成功时写入 [0, kFanoutMaxReaders)；失败时不修改。
     * @retval Result::kOk 登记成功
     * @retval Result::kNoSpace 读者槽已满
     * @retval Result::kNotSupported 环为 legacy
     * @retval Result::kInvalidState 尚未初始化，或 generation 耗尽的槽已全部退役
     * @note   读者任务调用，非 ISR；持注入锁，不阻塞。
     */
    [[nodiscard]] Result attach(FanoutMask mask, uint8_t &readerId) noexcept;
    /**
     * @brief  修改 typed 读者的订阅位图，并把读者移到当前 head。
     * @param  readerId: 本读者 id。
     * @param  mask: 新订阅位图（bit n = type n），0 表示暂停接收。
     * @retval Result::kOk 修改成功
     * @retval Result::kInvalidArg id 越界或未 attach
     * @retval Result::kNotSupported 环为 legacy
     * @retval Result::kBusy 该读者正持有租约
     * @note   该读者任务调用，非 ISR；跳过的旧记录不计 dropped。
     */
    [[nodiscard]] Result setMask(uint8_t readerId, FanoutMask mask) noexcept;
    /**
     * @brief  以 type 为标记写入一条 typed 记录。
     * @param  type: 记录类型，0..31，用于匹配读者 mask。
     * @param  payload: 负载，长度 1..maxRecord；不得与环存储重叠，调用期间源字节保持不变。
     * @retval Result::kOk 写入成功；无订阅者时同样返回 kOk，仅累加 noSubscriber
     * @retval Result::kInvalidArg type 越界、负载为空或超过 maxRecord
     * @retval Result::kNotSupported 环为 legacy
     * @retval Result::kInvalidState 尚未 initTyped
     * @retval Result::kBusy 有读者 pin 住待写区，整条拒新并计入 pinnedDrop
     * @note   仅写者任务调用，非 ISR；永不等待读者，不分配堆。
     */
    [[nodiscard]] Result write(uint8_t type, std::span<const uint8_t> payload) noexcept;
    /**
     * @brief  查看 typed 读者的下一条订阅记录并 pin 住。
     * @param  readerId: 本读者 id。
     * @param[out] view: 成功时写入两段负载视图并置 pin；失败时清空。
     * @retval Result::kOk 得到视图，须 commit 或 cancel 释放
     * @retval Result::kNotFound 暂无匹配记录
     * @retval Result::kInvalidArg id 越界或未 attach
     * @retval Result::kNotSupported 环为 legacy
     * @retval Result::kBusy 该读者已持有租约
     * @retval Result::kInvalidState 尚未初始化，或 serial 已耗尽
     * @note   该读者任务调用，非 ISR；租约仅覆盖当次有界复制，不得跨 IO/等待。
     */
    [[nodiscard]] Result peek(uint8_t readerId, FanoutView &view) noexcept;
    /**
     * @brief  提交租约：消费整条记录并释放 pin。
     * @param  lease: peek 返回的租约。
     * @retval Result::kOk 提交成功，读者 tail 前移
     * @retval Result::kInvalidArg 租约 token 无效或过期
     * @retval Result::kNotSupported 环为 legacy
     * @retval Result::kInvalidState 尚未初始化
     * @note   该读者任务调用，非 ISR；失败时不移动 tail。
     */
    [[nodiscard]] Result commit(const FanoutLease &lease) noexcept;
    /**
     * @brief  回滚租约：释放 pin 但不消费记录。
     * @param  lease: peek 返回的租约。
     * @retval Result::kOk 释放成功，记录仍可再次 peek
     * @retval Result::kInvalidArg 租约 token 无效或过期
     * @retval Result::kNotSupported 环为 legacy
     * @retval Result::kInvalidState 尚未初始化
     * @note   该读者任务调用，非 ISR。
     */
    [[nodiscard]] Result cancel(const FanoutLease &lease) noexcept;
    /**
     * @brief  取 typed 读者统计快照。
     * @param  readerId: 要查询的读者 id。
     * @retval 累计统计；id 越界、未 attach 或环为 legacy 时返回全零
     * @note   任意任务、非 ISR；持注入锁，不阻塞。
     */
    [[nodiscard]] FanoutTypedReaderStats typedReaderStats(uint8_t readerId) const noexcept;
    /**
     * @brief  取 typed 写者统计快照。
     * @retval 累计统计；未初始化或环为 legacy 时返回全零
     * @note   任意任务、非 ISR；持注入锁，不阻塞。
     */
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
