// 一写多读记录环实现（ADR-0007 §3.6-1）。纯 C++20，无 IDF/FreeRTOS 依赖。
// 只由 FanoutRing 的调用者在任务上下文调用；不阻塞、不分配堆；临界区由注入的锁界定。

#include "fw/FanoutRing.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace fw {

namespace {

constexpr size_t kByteMask = 0xFFU;
constexpr size_t kBitsPerByte = 8U;
constexpr size_t kMinRecordsInRing = 2U;
constexpr uint32_t kTypedHeaderBytes =
    kFanoutTypedHeaderBytes; // 固定为 3，索引加法必须按 u32 回绕。

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

size_t ringPhys(uint32_t oldest, size_t cap, uint32_t pos) noexcept
{
    const size_t base =
        static_cast<size_t>(oldest) % cap; // uint32_t 扩宽至 size_t 无损（32/64 位平台）
    const size_t offset = static_cast<size_t>(pos - oldest);
    return (base + offset) % cap;
}

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

void ringRead(std::span<const uint8_t> ring, uint32_t oldest, uint32_t pos,
              std::span<uint8_t> dst) noexcept
{
    if (dst.empty()) {
        return;
    }
    const size_t cap = ring.size();
    const size_t idx = ringPhys(oldest, cap, pos);
    const size_t first = std::min(dst.size(), cap - idx);
    const std::span<const uint8_t> s0 = ring.subspan(idx, first);
    std::copy(s0.begin(), s0.end(), dst.begin());
    if (first < dst.size()) {
        const std::span<const uint8_t> s1 = ring.first(dst.size() - first);
        const std::span<uint8_t> d1 = dst.subspan(first);
        std::copy(s1.begin(), s1.end(), d1.begin());
    }
}

uint16_t readHeaderLen(std::span<const uint8_t> ring, uint32_t oldest, uint32_t pos) noexcept
{
    std::array<uint8_t, kFanoutHeaderBytes> hdr{};
    ringRead(ring, oldest, pos, hdr);
    const uint16_t lo = static_cast<uint16_t>(hdr[0]);
    const uint16_t hi = static_cast<uint16_t>(static_cast<uint16_t>(hdr[1]) << kBitsPerByte);
    return static_cast<uint16_t>(lo | hi); // hdr[0..1] 均为 uint8_t，结果在 0..65535
}

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

Result FanoutRing::read(uint8_t readerId, std::span<uint8_t> out, size_t &len) noexcept
{
    len = 0U;
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (typed_) {
        return Result::kNotSupported;
    }
    if (readerId >= kFanoutMaxReaders) {
        return Result::kInvalidArg;
    }
    const LockGuard guard(lock_);

    Reader &reader = readers_[readerId];
    if (!reader.attached) {
        return Result::kInvalidArg;
    }
    if (reader.tail == head_) {
        return Result::kNotFound;
    }

    const std::span<const uint8_t> ring(buf_, cap_);
    const uint16_t payloadLen = readHeaderLen(ring, oldest_, reader.tail);
    const size_t payloadSz = static_cast<size_t>(payloadLen);
    len = payloadSz;
    if (out.size() < payloadSz) {
        return Result::kNoSpace;
    }

    const uint32_t payloadPos = reader.tail + static_cast<uint32_t>(kFanoutHeaderBytes);
    ringRead(ring, oldest_, payloadPos, out.first(payloadSz));

    reader.tail += static_cast<uint32_t>(kFanoutHeaderBytes + payloadSz);
    ++reader.stats.records;
    reader.stats.bytes += static_cast<uint32_t>(payloadSz);
    return Result::kOk;
}

FanoutReaderStats FanoutRing::readerStats(uint8_t readerId) const noexcept
{
    if ((!inited_) || (readerId >= kFanoutMaxReaders)) {
        return FanoutReaderStats{};
    }
    const LockGuard guard(lock_);
    if (!readers_[readerId].attached) {
        return FanoutReaderStats{};
    }
    return readers_[readerId].stats;
}

FanoutWriterStats FanoutRing::writerStats() const noexcept
{
    if (!inited_) {
        return FanoutWriterStats{};
    }
    const LockGuard guard(lock_);
    return wstats_;
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

// 类型化记录长度：把内部缓冲包装成 span 交给 readHeaderLen（类型适配，非纯转发）。
uint16_t FanoutRing::typedLength(uint32_t pos) const noexcept
{
    return readHeaderLen({buf_, cap_}, 0U, pos);
}

uint8_t FanoutRing::typedType(uint32_t pos) const noexcept
{
    // cap 是 2 的幂且 <2^31；低位掩码在 u32 回绕前后保持同一物理位置。
    return std::span<const uint8_t>(buf_, cap_)[(size_t{pos} + kFanoutHeaderBytes) & (cap_ - 1U)];
}

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

FanoutTypedReaderStats FanoutRing::typedReaderStats(uint8_t readerId) const noexcept
{
    if (!inited_ || !typed_) {
        return {};
    }
    const LockGuard guard(lock_);
    if ((readerId >= readers_.size()) || !readers_[readerId].attached) {
        return {};
    }
    const Reader &reader = readers_[readerId];
    return {reader.stats, reader.filtered, reader.pinnedDrop};
}

FanoutTypedWriterStats FanoutRing::typedWriterStats() const noexcept
{
    if (!inited_ || !typed_) {
        return {};
    }
    const LockGuard guard(lock_);
    FanoutTypedWriterStats stats = typed_wstats_;
    stats.base = wstats_;
    return stats;
}

} // namespace fw
