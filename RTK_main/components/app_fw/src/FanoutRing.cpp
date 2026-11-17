// 一写多读记录环实现（ADR-0007 §3.6-1）。纯 C++20，无 IDF/FreeRTOS 依赖。
// 只由 FanoutRing 的调用者在任务上下文调用；不阻塞、不分配堆；临界区由注入的锁界定。

#include "fw/FanoutRing.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include "FanoutRingRing.hpp"

namespace fw {

namespace {

constexpr size_t kByteMask = 0xFFU;
constexpr size_t kBitsPerByte = 8U;
constexpr size_t kMinRecordsInRing = 2U;

using detail::readHeaderLen;
using detail::ringPhys;
constexpr uint32_t kTypedHeaderBytes =
    kFanoutTypedHeaderBytes; // 固定为 3，索引加法必须按 u32 回绕。

/**
 * @brief  原地反转 buf[first, last) 区间。
 * @param  buf: 目标缓冲。
 * @param  first: 起始下标（含）。
 * @param  last: 结束下标（不含）。
 * @note   纯下标运算，无锁、无分配；供 ringRotate 的三次反转使用。
 */
void reverseSpan(std::span<uint8_t> buf, size_t first, size_t last) noexcept
{
    while (first < last) {
        --last;
        if (first < last) {
            const uint8_t tmp = buf[first];
            buf[first] = buf[last];
            buf[last] = tmp;
            ++first;
        }
    }
}

/**
 * @brief  把整段环内容右移 shift 字节（三次反转实现）。
 * @param  ring: 环存储。
 * @param  shift: 右移字节数；0、≥ 容量或容量 ≤ 1 时不动。
 * @note   仅在写者持锁、logical oldest 发生 u32 回绕需要重新对齐物理位置时调用。
 */
void rotateRingRight(std::span<uint8_t> ring, size_t shift) noexcept
{
    const size_t cap = ring.size();
    if ((cap <= 1U) || (shift == 0U) || (shift >= cap)) {
        return;
    }
    const size_t leftK = cap - shift;
    reverseSpan(ring, 0U, leftK);
    reverseSpan(ring, leftK, cap);
    reverseSpan(ring, 0U, cap);
}

/**
 * @brief  把 src 写到环的逻辑位置 pos，跨越存储末尾时折返。
 * @param  ring: 环存储。
 * @param  oldest: 逻辑位置 0 对应的物理下标。
 * @param  pos: 逻辑写入位置。
 * @param  src: 源字节；为空时直接返回。
 * @note   调用方持锁并保证写入区没有读者在使用；无分配、不阻塞。
 */
void ringWrite(std::span<uint8_t> ring, uint32_t oldest, uint32_t pos,
               std::span<const uint8_t> src) noexcept
{
    if (src.empty()) {
        return;
    }
    const size_t cap = ring.size();
    const size_t idx = ringPhys(oldest, cap, pos);
    const size_t first = std::min(src.size(), cap - idx);
    const std::span<const uint8_t> s0 = src.first(first);
    const std::span<uint8_t> d0 = ring.subspan(idx, first);
    std::copy(s0.begin(), s0.end(), d0.begin());
    if (first < src.size()) {
        const std::span<const uint8_t> s1 = src.subspan(first);
        std::copy(s1.begin(), s1.end(), ring.begin());
    }
}

/**
 * @brief  在记录起始位置写入 2 字节小端负载长度头。
 * @param  ring: 环存储。
 * @param  oldest: 逻辑位置 0 对应的物理下标。
 * @param  pos: 记录起始的逻辑位置。
 * @param  payloadLen: 负载长度，0..65535，直接决定头内容。
 * @note   调用方持锁；无分配。
 */
void writeHeaderLen(std::span<uint8_t> ring, uint32_t oldest, uint32_t pos,
                    uint16_t payloadLen) noexcept
{
    const std::array<uint8_t, kFanoutHeaderBytes> hdr{
        static_cast<uint8_t>(static_cast<size_t>(payloadLen) & kByteMask), // 低 8 位，范围 0..255
        static_cast<uint8_t>((static_cast<size_t>(payloadLen) >> kBitsPerByte) &
                             kByteMask), // 高 8 位，范围 0..255
    };
    ringWrite(ring, oldest, pos, hdr);
}

} // namespace

Result FanoutRing::init(std::span<uint8_t> storage, size_t maxRecord, const LockOps &lock) noexcept
{
    if (inited_) {
        return Result::kInvalidState;
    }
    if ((storage.data() == nullptr) || (maxRecord < 1U) ||
        (maxRecord > std::numeric_limits<uint16_t>::max())) {
        return Result::kInvalidArg;
    }
    const size_t minCap = kMinRecordsInRing * (maxRecord + kFanoutHeaderBytes);
    if ((storage.size() < minCap) ||
        (storage.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))) {
        return Result::kInvalidArg;
    }
    if ((lock.lock == nullptr) != (lock.unlock == nullptr)) {
        return Result::kInvalidArg;
    }

    lock_ = lock;
    buf_ = storage.data();
    cap_ = storage.size();
    max_record_ = maxRecord;
    head_ = 0U;
    oldest_ = 0U;
    readers_.fill(Reader{});
    wstats_ = FanoutWriterStats{};
    inited_ = true;
    return Result::kOk;
}

Result FanoutRing::attach(uint8_t &readerId) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (typed_) {
        return Result::kNotSupported;
    }
    const LockGuard guard(lock_);
    for (size_t i = 0U; i < kFanoutMaxReaders; ++i) {
        if (!readers_[i].attached) {
            readers_[i].attached = true;
            readers_[i].tail = head_;
            readers_[i].stats = FanoutReaderStats{};
            readerId = static_cast<uint8_t>(i); // i < kFanoutMaxReaders (4)，收窄安全
            return Result::kOk;
        }
    }
    return Result::kNoSpace;
}

Result FanoutRing::detach(uint8_t readerId) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (readerId >= kFanoutMaxReaders) {
        return Result::kInvalidArg;
    }
    const LockGuard guard(lock_);
    if (!readers_[readerId].attached) {
        return Result::kInvalidArg;
    }
    if (readers_[readerId].pinned) {
        return Result::kBusy;
    }
    readers_[readerId].attached = false;
    readers_[readerId].tail = 0U;
    return Result::kOk;
}

/**
 * @brief  写入一条 legacy 记录，空间不足时淘汰最旧记录。
 * @param  record: 负载，长度须为 1..maxRecord。
 * @retval Result::kOk 写入成功
 * @retval Result::kInvalidArg 负载为空或超过 maxRecord，并计入 rejected
 * @retval Result::kInvalidState 尚未初始化
 * @retval Result::kNotSupported 环为 typed
 * @note   仅写者任务调用，非 ISR；持注入锁，永不等待读者，不分配堆。
 */
Result FanoutRing::write(std::span<const uint8_t> record) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (typed_) {
        return Result::kNotSupported;
    }
    const LockGuard guard(lock_);

    if (record.empty() || (record.size() > max_record_)) {
        ++wstats_.rejected;
        return Result::kInvalidArg;
    }

    const std::span<uint8_t> ring(buf_, cap_);
    const size_t need = kFanoutHeaderBytes + record.size();
    uint32_t curOldest = oldest_;
    while ((cap_ - static_cast<size_t>(head_ - curOldest)) < need) {
        const uint16_t victimPayload = readHeaderLen(ring, oldest_, curOldest);
        const uint32_t victimTotal = static_cast<uint32_t>(
            kFanoutHeaderBytes + static_cast<size_t>(victimPayload)); // <= 65537
        const uint32_t nextOldest = curOldest + victimTotal;

        for (Reader &reader : readers_) {
            if (reader.attached && (reader.tail == curOldest)) {
                ++reader.stats.dropped;
                reader.tail = nextOldest;
            }
        }
        curOldest = nextOldest;
        ++wstats_.overwritten;
    }

    if (curOldest < oldest_) {
        // oldest_ 发生 u32 回绕：若 cap_ 非 2^32 的约数，需把残留有效区旋转对齐到 new_oldest % cap_。
        if (curOldest != head_) {
            const size_t srcIdx = ringPhys(oldest_, cap_, curOldest);
            const size_t dstIdx = static_cast<size_t>(curOldest) % cap_;
            if (srcIdx != dstIdx) {
                const size_t shift = (dstIdx + cap_ - srcIdx) % cap_;
                rotateRingRight(ring, shift);
            }
        }
    }
    oldest_ = curOldest;

    // 此前已校验 record.size() <= max_record_ <= UINT16_MAX，收窄到 uint16_t 无损。
    writeHeaderLen(ring, oldest_, head_, static_cast<uint16_t>(record.size()));
    const uint32_t payloadPos = head_ + static_cast<uint32_t>(kFanoutHeaderBytes);
    ringWrite(ring, oldest_, payloadPos, record);

    head_ += static_cast<uint32_t>(need); // need <= max_record_ + 2 <= 65537
    ++wstats_.records;
    wstats_.bytes += static_cast<uint32_t>(record.size());
    return Result::kOk;
}

Result FanoutRing::initTyped(std::span<uint8_t> storage, size_t maxRecord,
                             const LockOps &lock) noexcept
{
    if (inited_) {
        return Result::kInvalidState;
    }
    if ((storage.data() == nullptr) || (maxRecord == 0U) ||
        (maxRecord > std::numeric_limits<uint16_t>::max()) ||
        (storage.size() < maxRecord + kFanoutTypedHeaderBytes) ||
        (storage.size() > std::numeric_limits<int32_t>::max()) ||
        ((storage.size() & (storage.size() - 1U)) != 0U) ||
        ((lock.lock == nullptr) != (lock.unlock == nullptr))) {
        return Result::kInvalidArg;
    }
    lock_ = lock;
    buf_ = storage.data();
    cap_ = storage.size();
    max_record_ = maxRecord;
    typed_ = true;
    inited_ = true;
    return Result::kOk;
}

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
Result FanoutRing::attach(FanoutMask mask, uint8_t &readerId) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (!typed_) {
        return Result::kNotSupported;
    }
    const LockGuard guard(lock_);
    bool retired = false;
    for (size_t i = 0U; i < readers_.size(); ++i) {
        Reader &reader = readers_[i];
        if (reader.attached) {
            continue;
        }
        if (reader.generation == std::numeric_limits<uint32_t>::max()) {
            retired = true;
            continue;
        }
        const uint32_t generation = reader.generation + 1U;
        reader = Reader{};
        reader.attached = true;
        reader.tail = head_;
        reader.mask = mask;
        reader.generation = generation;
        readerId = static_cast<uint8_t>(i); // i < 4，收窄安全。
        return Result::kOk;
    }
    return retired ? Result::kInvalidState : Result::kNoSpace;
}

Result FanoutRing::setMask(uint8_t readerId, FanoutMask mask) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (!typed_) {
        return Result::kNotSupported;
    }
    const LockGuard guard(lock_);
    if ((readerId >= readers_.size()) || !readers_[readerId].attached) {
        return Result::kInvalidArg;
    }
    Reader &reader = readers_[readerId];
    if (reader.pinned) {
        return Result::kBusy;
    }
    reader.mask = mask;
    reader.tail = head_;
    return Result::kOk;
}

/**
 * @brief  淘汰最旧的一条 typed 记录，并把正停在该记录上的读者前移。
 * @note   调用方必须持注入锁；订阅该记录的读者计 dropped，未订阅的计 filtered；不分配、不阻塞。
 */
void FanoutRing::evictTyped() noexcept
{
    const uint32_t next = oldest_ + kTypedHeaderBytes + typedLength(oldest_);
    const FanoutMask bit = FanoutMask{1U} << typedType(oldest_);
    for (Reader &reader : readers_) {
        if (reader.attached && (reader.tail == oldest_)) {
            if ((reader.mask & bit) != 0U) {
                ++reader.stats.dropped;
            } else {
                ++reader.filtered;
            }
            reader.tail = next;
        }
    }
    oldest_ = next;
    ++wstats_.overwritten;
}

/**
 * @brief  校验参数并预留一条 typed 记录的空间。
 * @param  type: 记录类型，须小于掩码位宽 32。
 * @param  size: 负载长度，须为 1..maxRecord。
 * @retval Result::kOk 已预留 size + 3 字节，调用方可以开始复制
 * @retval Result::kInvalidArg type 越界、长度为 0 或超过 maxRecord（计入 rejected/oversizeDrop）
 * @retval Result::kNotFound 没有读者订阅该类型（调用方转成 kOk 并计入 noSubscriber）
 * @retval Result::kBusy 有读者 pin 住待淘汰区，整条拒新并计入 pinnedDrop
 * @note   调用方须已确认环为 typed 且已初始化；内部短暂持锁，不阻塞、不分配堆。
 */
Result FanoutRing::beginTypedWrite(uint8_t type, size_t size) noexcept
{
    const LockGuard guard(lock_);
    if ((type >= std::numeric_limits<FanoutMask>::digits) || (size == 0U) || (size > max_record_)) {
        ++wstats_.rejected;
        if ((size > max_record_) || (size > cap_ - kFanoutTypedHeaderBytes)) {
            ++typed_wstats_.oversizeDrop;
        }
        return Result::kInvalidArg;
    }
    const FanoutMask bit = FanoutMask{1U} << type;
    bool subscribed = false;
    for (const Reader &reader : readers_) {
        subscribed = subscribed || (reader.attached && ((reader.mask & bit) != 0U));
    }
    if (!subscribed) {
        ++typed_wstats_.noSubscriber;
        return Result::kNotFound; // 内部哨兵，公开 write 返回 kOk。
    }
    const size_t need = size + kFanoutTypedHeaderBytes;
    for (const Reader &reader : readers_) {
        // 减法差值 <=cap；改写区间若触及任一 pin，保留前即整条拒绝，不淘汰旧记录。
        if (reader.pinned && (need > cap_ - (head_ - reader.tail))) {
            ++typed_wstats_.pinnedDrop;
            for (Reader &matching : readers_) {
                if (matching.attached && ((matching.mask & bit) != 0U)) {
                    ++matching.pinnedDrop;
                }
            }
            return Result::kBusy;
        }
    }
    reserved_bytes_ = need;
    return Result::kOk;
}

/**
 * @brief  以 type 为标记写入一条 typed 记录。
 * @param  type: 记录类型，0..31，用于匹配读者 mask。
 * @param  payload: 负载，长度 1..maxRecord；不得与环存储重叠。
 * @retval Result::kOk 写入成功；无订阅者时同样返回 kOk，仅累加 noSubscriber
 * @retval Result::kInvalidArg type 越界、负载为空或超过 maxRecord
 * @retval Result::kNotSupported 环为 legacy
 * @retval Result::kInvalidState 尚未 initTyped
 * @retval Result::kBusy 有读者 pin 住待写区，整条拒新并计入 pinnedDrop
 * @note   仅写者任务调用，非 ISR；逐条淘汰时释放锁，永不等待读者，不分配堆。
 */
Result FanoutRing::write(uint8_t type, std::span<const uint8_t> payload) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (!typed_) {
        return Result::kNotSupported;
    }
    const Result result = beginTypedWrite(type, payload.size());
    if (result != Result::kOk) {
        return (result == Result::kNotFound) ? Result::kOk : result;
    }
    // reservation 阻止 peek 在待淘汰区新建 pin；逐条释放锁，避免长记录淘汰许多小记录时长持锁。
    for (;;) {
        const LockGuard guard(lock_);
        if (reserved_bytes_ <= cap_ - (head_ - oldest_)) {
            break;
        }
        evictTyped();
    }
    // 单写者拥有保留区，head 未发布；读者只能看到旧记录，且无 pin/读者再能引用保留区。
    const std::span<uint8_t> ring(buf_, cap_);
    writeHeaderLen(ring, 0U, head_, static_cast<uint16_t>(payload.size())); // 已验证 <=65535。
    ring[(size_t{head_} + kFanoutHeaderBytes) & (cap_ - 1U)] = type;
    ringWrite(ring, 0U, head_ + kTypedHeaderBytes, payload);
    {
        const LockGuard guard(lock_);
        head_ += static_cast<uint32_t>(reserved_bytes_); // <=65535+3，收窄安全。
        reserved_bytes_ = 0U;
        ++wstats_.records;
        wstats_.bytes += static_cast<uint32_t>(payload.size()); // <=65535，收窄安全。
        typed_wstats_.highWaterBytes =
            std::max(typed_wstats_.highWaterBytes, size_t{head_ - oldest_});
    }
    return Result::kOk;
}

Result FanoutRing::peek(uint8_t readerId, FanoutView &view) noexcept
{
    view = FanoutView{};
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (!typed_) {
        return Result::kNotSupported;
    }
    for (;;) {
        const LockGuard guard(lock_);
        if ((readerId >= readers_.size()) || !readers_[readerId].attached) {
            return Result::kInvalidArg;
        }
        Reader &reader = readers_[readerId];
        if (reader.pinned) {
            return Result::kBusy;
        }
        if (reader.tail == head_) {
            return Result::kNotFound;
        }
        if (reserved_bytes_ > cap_ - (head_ - reader.tail)) {
            // 协助淘汰 reservation 已声明将覆盖的前缀；不允许新 pin 抢占写者的保留区。
            evictTyped();
            continue;
        }
        const size_t length = typedLength(reader.tail);
        const uint8_t type = typedType(reader.tail);
        if ((reader.mask & (FanoutMask{1U} << type)) == 0U) {
            reader.tail += static_cast<uint32_t>(length + kFanoutTypedHeaderBytes); // <=65538。
            ++reader.filtered;
            continue;
        }
        if (reader.serial == std::numeric_limits<uint32_t>::max()) {
            return Result::kInvalidState;
        }
        ++reader.serial;
        reader.pinned = true;
        const uint32_t pos = reader.tail + kTypedHeaderBytes;
        const size_t idx = size_t{pos} & (cap_ - 1U);
        const size_t first = std::min(length, cap_ - idx);
        const std::span<const uint8_t> storage(buf_, cap_);
        view = {{reader.generation, reader.serial, readerId},
                type,
                storage.subspan(idx, first),
                storage.first(length - first)};
        return Result::kOk;
    }
}

/**
 * @brief  校验租约，并按 consume 决定是否消费该记录。
 * @param  lease: peek 返回的租约。
 * @param  consume: true 前移 tail 并累加 records/bytes；false 只释放 pin。
 * @retval Result::kOk 成功
 * @retval Result::kInvalidArg 读者未 attach、未 pin，或 generation/serial 不匹配
 * @retval Result::kInvalidState 尚未初始化
 * @retval Result::kNotSupported 环为 legacy
 * @note   持注入锁，不阻塞、不分配堆；失败时不移动 tail。
 */
Result FanoutRing::finishLease(const FanoutLease &lease, bool consume) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (!typed_) {
        return Result::kNotSupported;
    }
    const LockGuard guard(lock_);
    if (lease.readerId >= readers_.size()) {
        return Result::kInvalidArg;
    }
    Reader &reader = readers_[lease.readerId];
    if (!reader.attached || !reader.pinned || (reader.generation != lease.generation) ||
        (reader.serial != lease.serial)) {
        return Result::kInvalidArg;
    }
    if (consume) {
        const uint32_t length = typedLength(reader.tail);
        reader.tail += length + kTypedHeaderBytes;
        ++reader.stats.records;
        reader.stats.bytes += length;
    }
    reader.pinned = false;
    return Result::kOk;
}

// 租约提交（ADR-0013 §2.3 点名保留）：常量 true 表达落账语义，调用方无需记布尔含义。
Result FanoutRing::commit(const FanoutLease &lease) noexcept
{
    return finishLease(lease, true);
}

// 租约回滚（ADR-0013 §2.3 点名保留）：常量 false 表达回滚语义，与 commit 成对。
Result FanoutRing::cancel(const FanoutLease &lease) noexcept
{
    return finishLease(lease, false);
}

} // namespace fw
