// 一写多读记录环实现（ADR-0007 §3.6-1）。纯 C++20，无 IDF/FreeRTOS 依赖。
// 只由 FanoutRing 的调用者在任务上下文调用；不阻塞、不分配堆；临界区由注入的锁界定。

#include "fw/fanout_ring.hpp"

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

void reverse_span(std::span<uint8_t> buf, size_t first, size_t last) noexcept
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

void rotate_ring_right(std::span<uint8_t> ring, size_t shift) noexcept
{
    const size_t cap = ring.size();
    if ((cap <= 1U) || (shift == 0U) || (shift >= cap)) {
        return;
    }
    const size_t left_k = cap - shift;
    reverse_span(ring, 0U, left_k);
    reverse_span(ring, left_k, cap);
    reverse_span(ring, 0U, cap);
}

size_t ring_phys(uint32_t oldest, size_t cap, uint32_t pos) noexcept
{
    const size_t base = static_cast<size_t>(oldest) % cap; // uint32_t 扩宽至 size_t 无损（32/64 位平台）
    const size_t offset = static_cast<size_t>(pos - oldest);
    return (base + offset) % cap;
}

void ring_write(std::span<uint8_t> ring, uint32_t oldest, uint32_t pos, std::span<const uint8_t> src) noexcept
{
    if (src.empty()) {
        return;
    }
    const size_t cap = ring.size();
    const size_t idx = ring_phys(oldest, cap, pos);
    const size_t first = std::min(src.size(), cap - idx);
    const std::span<const uint8_t> s0 = src.first(first);
    const std::span<uint8_t> d0 = ring.subspan(idx, first);
    std::copy(s0.begin(), s0.end(), d0.begin());
    if (first < src.size()) {
        const std::span<const uint8_t> s1 = src.subspan(first);
        std::copy(s1.begin(), s1.end(), ring.begin());
    }
}

void ring_read(std::span<const uint8_t> ring, uint32_t oldest, uint32_t pos, std::span<uint8_t> dst) noexcept
{
    if (dst.empty()) {
        return;
    }
    const size_t cap = ring.size();
    const size_t idx = ring_phys(oldest, cap, pos);
    const size_t first = std::min(dst.size(), cap - idx);
    const std::span<const uint8_t> s0 = ring.subspan(idx, first);
    std::copy(s0.begin(), s0.end(), dst.begin());
    if (first < dst.size()) {
        const std::span<const uint8_t> s1 = ring.first(dst.size() - first);
        const std::span<uint8_t> d1 = dst.subspan(first);
        std::copy(s1.begin(), s1.end(), d1.begin());
    }
}

uint16_t read_header_len(std::span<const uint8_t> ring, uint32_t oldest, uint32_t pos) noexcept
{
    std::array<uint8_t, kFanoutHeaderBytes> hdr{};
    ring_read(ring, oldest, pos, hdr);
    const uint16_t lo = static_cast<uint16_t>(hdr[0]);
    const uint16_t hi = static_cast<uint16_t>(static_cast<uint16_t>(hdr[1]) << kBitsPerByte);
    return static_cast<uint16_t>(lo | hi); // hdr[0..1] 均为 uint8_t，结果在 0..65535
}

void write_header_len(std::span<uint8_t> ring, uint32_t oldest, uint32_t pos, uint16_t payload_len) noexcept
{
    const std::array<uint8_t, kFanoutHeaderBytes> hdr{
        static_cast<uint8_t>(static_cast<size_t>(payload_len) & kByteMask),                   // 低 8 位，范围 0..255
        static_cast<uint8_t>((static_cast<size_t>(payload_len) >> kBitsPerByte) & kByteMask), // 高 8 位，范围 0..255
    };
    ring_write(ring, oldest, pos, hdr);
}

} // namespace

Result FanoutRing::init(std::span<uint8_t> storage, size_t max_record, const LockOps &lock) noexcept
{
    if (inited_) {
        return Result::kInvalidState;
    }
    if ((storage.data() == nullptr) || (max_record < 1U) || (max_record > std::numeric_limits<uint16_t>::max())) {
        return Result::kInvalidArg;
    }
    const size_t min_cap = kMinRecordsInRing * (max_record + kFanoutHeaderBytes);
    if ((storage.size() < min_cap) || (storage.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))) {
        return Result::kInvalidArg;
    }
    if ((lock.lock == nullptr) != (lock.unlock == nullptr)) {
        return Result::kInvalidArg;
    }

    lock_ = lock;
    buf_ = storage.data();
    cap_ = storage.size();
    max_record_ = max_record;
    head_ = 0U;
    oldest_ = 0U;
    readers_.fill(Reader{});
    wstats_ = FanoutWriterStats{};
    inited_ = true;
    return Result::kOk;
}

Result FanoutRing::attach(uint8_t &reader_id) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    const LockGuard guard(lock_);
    for (size_t i = 0U; i < kFanoutMaxReaders; ++i) {
        if (!readers_[i].attached) {
            readers_[i].attached = true;
            readers_[i].tail = head_;
            readers_[i].stats = FanoutReaderStats{};
            reader_id = static_cast<uint8_t>(i); // i < kFanoutMaxReaders (4)，收窄安全
            return Result::kOk;
        }
    }
    return Result::kNoSpace;
}

Result FanoutRing::detach(uint8_t reader_id) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (reader_id >= kFanoutMaxReaders) {
        return Result::kInvalidArg;
    }
    const LockGuard guard(lock_);
    if (!readers_[reader_id].attached) {
        return Result::kInvalidArg;
    }
    readers_[reader_id].attached = false;
    readers_[reader_id].tail = 0U;
    return Result::kOk;
}

Result FanoutRing::write(std::span<const uint8_t> record) noexcept
{
    if (!inited_) {
        return Result::kInvalidState;
    }
    const LockGuard guard(lock_);

    if (record.empty() || (record.size() > max_record_)) {
        ++wstats_.rejected;
        return Result::kInvalidArg;
    }

    const std::span<uint8_t> ring(buf_, cap_);
    const size_t need = kFanoutHeaderBytes + record.size();
    uint32_t cur_oldest = oldest_;
    while ((cap_ - static_cast<size_t>(head_ - cur_oldest)) < need) {
        const uint16_t victim_payload = read_header_len(ring, oldest_, cur_oldest);
        const uint32_t victim_total =
            static_cast<uint32_t>(kFanoutHeaderBytes + static_cast<size_t>(victim_payload)); // <= 65537
        const uint32_t next_oldest = cur_oldest + victim_total;

        for (Reader &reader : readers_) {
            if (reader.attached && (reader.tail == cur_oldest)) {
                ++reader.stats.dropped;
                reader.tail = next_oldest;
            }
        }
        cur_oldest = next_oldest;
        ++wstats_.overwritten;
    }

    if (cur_oldest < oldest_) {
        // oldest_ 发生 u32 回绕：若 cap_ 非 2^32 的约数，需把残留有效区旋转对齐到 new_oldest % cap_。
        if (cur_oldest != head_) {
            const size_t src_idx = ring_phys(oldest_, cap_, cur_oldest);
            const size_t dst_idx = static_cast<size_t>(cur_oldest) % cap_;
            if (src_idx != dst_idx) {
                const size_t shift = (dst_idx + cap_ - src_idx) % cap_;
                rotate_ring_right(ring, shift);
            }
        }
    }
    oldest_ = cur_oldest;

    // 此前已校验 record.size() <= max_record_ <= UINT16_MAX，收窄到 uint16_t 无损。
    write_header_len(ring, oldest_, head_, static_cast<uint16_t>(record.size()));
    const uint32_t payload_pos = head_ + static_cast<uint32_t>(kFanoutHeaderBytes);
    ring_write(ring, oldest_, payload_pos, record);

    head_ += static_cast<uint32_t>(need); // need <= max_record_ + 2 <= 65537
    ++wstats_.records;
    wstats_.bytes += static_cast<uint32_t>(record.size());
    return Result::kOk;
}

Result FanoutRing::read(uint8_t reader_id, std::span<uint8_t> out, size_t &len) noexcept
{
    len = 0U;
    if (!inited_) {
        return Result::kInvalidState;
    }
    if (reader_id >= kFanoutMaxReaders) {
        return Result::kInvalidArg;
    }
    const LockGuard guard(lock_);

    Reader &reader = readers_[reader_id];
    if (!reader.attached) {
        return Result::kInvalidArg;
    }
    if (reader.tail == head_) {
        return Result::kNotFound;
    }

    const std::span<const uint8_t> ring(buf_, cap_);
    const uint16_t payload_len = read_header_len(ring, oldest_, reader.tail);
    const size_t payload_sz = static_cast<size_t>(payload_len);
    len = payload_sz;
    if (out.size() < payload_sz) {
        return Result::kNoSpace;
    }

    const uint32_t payload_pos = reader.tail + static_cast<uint32_t>(kFanoutHeaderBytes);
    ring_read(ring, oldest_, payload_pos, out.first(payload_sz));

    reader.tail += static_cast<uint32_t>(kFanoutHeaderBytes + payload_sz);
    ++reader.stats.records;
    reader.stats.bytes += static_cast<uint32_t>(payload_sz);
    return Result::kOk;
}

FanoutReaderStats FanoutRing::reader_stats(uint8_t reader_id) const noexcept
{
    if ((!inited_) || (reader_id >= kFanoutMaxReaders)) {
        return FanoutReaderStats{};
    }
    const LockGuard guard(lock_);
    if (!readers_[reader_id].attached) {
        return FanoutReaderStats{};
    }
    return readers_[reader_id].stats;
}

FanoutWriterStats FanoutRing::writer_stats() const noexcept
{
    if (!inited_) {
        return FanoutWriterStats{};
    }
    const LockGuard guard(lock_);
    return wstats_;
}

} // namespace fw
