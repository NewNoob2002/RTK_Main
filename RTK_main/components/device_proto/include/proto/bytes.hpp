// 有界的小端字节读写器。所有协议编码都必须经由它们，不直接对缓冲区做下标拼装，
// 从而在类型层面排除 C 版 copy_field 那类越界读写（QA-A 发现 1）。
// 线程安全：否（每个实例只由一个任务使用）。ISR 安全：是（无阻塞、无分配）。
// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace proto {

// 写入器采用“粘滞错误”：任何一次写入越界后 ok() 永久为 false，之后的写入全部忽略，
// 已写入的字节保持不变。调用方在一串写入后只需检查一次 ok()。
/** 有界小端写指针；越界后置 !ok() 且不再写，实例单任务使用。 */
class ByteWriter {
public:
    explicit ByteWriter(std::span<uint8_t> buf) noexcept;

    void u8(uint8_t value) noexcept;
    void u16(uint16_t value) noexcept; // 小端
    void u32(uint32_t value) noexcept; // 小端
    void bytes(std::span<const uint8_t> data) noexcept;
    void zeros(size_t count) noexcept;
    // 写定宽字段：拷贝 min(text.size(), width) 字节，其余补 0；不要求 text 以 0 结尾。
    void fixed_str(std::string_view text, size_t width) noexcept;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] size_t size() const noexcept; // 已成功写入的字节数
    [[nodiscard]] std::span<const uint8_t> written() const noexcept;

private:
    std::span<uint8_t> buf_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// 读取器同样采用粘滞错误：越界读返回 0 并使 ok() 为 false。
/** 有界小端读指针；越界后置 !ok() 并返回默认值，实例单任务使用。 */
class ByteReader {
public:
    explicit ByteReader(std::span<const uint8_t> buf) noexcept;

    [[nodiscard]] uint8_t u8() noexcept;
    [[nodiscard]] uint16_t u16() noexcept; // 小端
    [[nodiscard]] uint32_t u32() noexcept; // 小端
    void skip(size_t count) noexcept;
    // 返回接下来 count 字节的视图；越界时返回空 span 并置错误。
    [[nodiscard]] std::span<const uint8_t> take(size_t count) noexcept;

    [[nodiscard]] bool ok() const noexcept;
    [[nodiscard]] size_t remaining() const noexcept;

private:
    std::span<const uint8_t> buf_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace proto
