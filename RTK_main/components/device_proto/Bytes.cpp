// 有界小端字节读写器的实现；粘滞错误：越界后 ok() 永久为 false，已写字节不变，后续读写被忽略。
// 每个实例只由一个任务使用，非线程安全；无阻塞、无分配，ISR 安全。
// 不使用堆；多字节字段只经这里读写。

#include "proto/Bytes.hpp"

namespace proto {

ByteWriter::ByteWriter(std::span<uint8_t> buf) noexcept : buf_(buf) {}

void ByteWriter::u8(uint8_t value) noexcept
{
    if (!ok_) {
        return;
    }
    if (buf_.size() <= pos_) {
        ok_ = false;
        return;
    }
    buf_[pos_] = value;
    ++pos_;
}

void ByteWriter::u16(uint16_t value) noexcept
{
    if (!ok_) {
        return;
    }
    // 先整体检查宽度：契约要求失败时不得部分写入。
    if (2U > (buf_.size() - pos_)) {
        ok_ = false;
        return;
    }
    // 小端：先写低字节。
    for (size_t i = 0; i < 2U; ++i) {
        buf_[pos_ + i] = static_cast<uint8_t>(value >> (8U * i));
    }
    pos_ += 2U;
}

void ByteWriter::u32(uint32_t value) noexcept
{
    if (!ok_) {
        return;
    }
    // 先整体检查宽度：契约要求失败时不得部分写入。
    if (4U > (buf_.size() - pos_)) {
        ok_ = false;
        return;
    }
    // 小端：先写低字节。
    for (size_t i = 0; i < 4U; ++i) {
        buf_[pos_ + i] = static_cast<uint8_t>(value >> (8U * i));
    }
    pos_ += 4U;
}

void ByteWriter::bytes(std::span<const uint8_t> data) noexcept
{
    if (!ok_) {
        return;
    }
    if (data.size() > (buf_.size() - pos_)) {
        ok_ = false;
        return;
    }
    for (size_t i = 0; i < data.size(); ++i) {
        buf_[pos_ + i] = data[i];
    }
    pos_ += data.size();
}

void ByteWriter::zeros(size_t count) noexcept
{
    if (!ok_) {
        return;
    }
    if (count > (buf_.size() - pos_)) {
        ok_ = false;
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        buf_[pos_ + i] = 0;
    }
    pos_ += count;
}

void ByteWriter::fixedStr(std::string_view text, size_t width) noexcept
{
    if (!ok_) {
        return;
    }
    if (width > (buf_.size() - pos_)) {
        ok_ = false;
        return;
    }
    const size_t copyLen = (text.size() < width) ? text.size() : width;
    for (size_t i = 0; i < copyLen; ++i) {
        // char 的位模式按无符号字节写入（协议只传输字节，不解释字符集）。
        buf_[pos_ + i] = static_cast<uint8_t>(text[i]);
    }
    for (size_t i = copyLen; i < width; ++i) {
        buf_[pos_ + i] = 0;
    }
    pos_ += width;
}

bool ByteWriter::ok() const noexcept
{
    return ok_;
}

size_t ByteWriter::size() const noexcept
{
    return pos_;
}

std::span<const uint8_t> ByteWriter::written() const noexcept
{
    return {buf_.data(), pos_};
}

ByteReader::ByteReader(std::span<const uint8_t> buf) noexcept : buf_(buf) {}

/**
 * @brief  读一个字节
 * @retval 读到的字节；已越界时返回 0 并置 !ok()
 * @note   同名重载：定义处单独成块，避免与 ByteWriter::u8 混淆
 */
uint8_t ByteReader::u8() noexcept
{
    if (!ok_ || (pos_ >= buf_.size())) {
        ok_ = false;
        return 0;
    }
    const uint8_t value = buf_[pos_];
    ++pos_;
    return value;
}

/**
 * @brief  以小端读 u16
 * @retval 读到的值；越界部分按 0 计入并置 !ok()
 * @note   同名重载：定义处单独成块，避免与 ByteWriter::u16 混淆
 */
uint16_t ByteReader::u16() noexcept
{
    uint32_t value = 0;
    // 小端：第一个字节是低 8 位。越界时 u8() 返回 0 并置粘滞错误。
    for (size_t i = 0; i < 2U; ++i) {
        value |= static_cast<uint32_t>(u8()) << (8U * i);
    }
    return static_cast<uint16_t>(value);
}

/**
 * @brief  以小端读 u32
 * @retval 读到的值；越界部分按 0 计入并置 !ok()
 * @note   同名重载：定义处单独成块，避免与 ByteWriter::u32 混淆
 */
uint32_t ByteReader::u32() noexcept
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4U; ++i) {
        value |= static_cast<uint32_t>(u8()) << (8U * i);
    }
    return value;
}

void ByteReader::skip(size_t count) noexcept
{
    if (!ok_) {
        return;
    }
    if (count > (buf_.size() - pos_)) {
        ok_ = false;
        pos_ = buf_.size();
        return;
    }
    pos_ += count;
}

std::span<const uint8_t> ByteReader::take(size_t count) noexcept
{
    if (!ok_) {
        return {};
    }
    if (count > (buf_.size() - pos_)) {
        ok_ = false;
        pos_ = buf_.size();
        return {};
    }
    const std::span<const uint8_t> view = buf_.subspan(pos_, count);
    pos_ += count;
    return view;
}

bool ByteReader::ok() const noexcept
{
    return ok_;
}

size_t ByteReader::remaining() const noexcept
{
    return buf_.size() - pos_;
}

} // namespace proto
