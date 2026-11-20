// 逐字段构造 WebSocket JSON 帧：根对象 + 一层嵌套、字符串数组、整数、bool。
// 内部暂存 1024 B，finish() 一次拷出，缓冲不足时不输出半截 JSON。
// 由网页模块任务调用；不阻塞，非 ISR 安全；本文件不分配堆。
#include "web_proto/Writer.hpp"

namespace web {
namespace {

constexpr std::string_view kHexDigits{"0123456789abcdef"};
constexpr size_t kMaxIntDigits = 11; // int32 十进制最多 10 位，加符号 11 位
constexpr int64_t kIntDecimalBase = 10;

} // namespace

void JsonWriter::clear() noexcept
{
    size_ = 0;
    memberCount_.fill(0);
    depth_ = 0;
    closed_ = false;
    failed_ = false;
}

bool JsonWriter::beginObject() noexcept
{
    if (failed_ || closed_ || depth_ != 0) {
        return fail();
    }
    if (!append('{')) {
        return false;
    }
    depth_ = 1;
    memberCount_[depth_] = 0;
    return true;
}

bool JsonWriter::beginNested(std::string_view key) noexcept
{
    if (depth_ == 0 || depth_ >= kMaxDepth) {
        return fail();
    }
    if (!memberPrefix(key)) {
        return false;
    }
    if (!append('{')) {
        return false;
    }
    ++depth_;
    memberCount_[depth_] = 0;
    return true;
}

bool JsonWriter::endNested() noexcept
{
    if (depth_ <= 1U) {
        return fail();
    }
    if (!append('}')) {
        return false;
    }
    --depth_;
    return true;
}

bool JsonWriter::endObject() noexcept
{
    if (depth_ != 1) {
        return fail();
    }
    if (!append('}')) {
        return false;
    }
    depth_ = 0;
    closed_ = true;
    return true;
}

bool JsonWriter::writeInt(std::string_view key, int32_t value) noexcept
{
    if (!memberPrefix(key)) {
        return false;
    }
    return appendInt(value);
}

bool JsonWriter::writeBool(std::string_view key, bool value) noexcept
{
    if (!memberPrefix(key)) {
        return false;
    }
    return appendRaw(value ? "true" : "false");
}

bool JsonWriter::writeString(std::string_view key, std::string_view value) noexcept
{
    if (!memberPrefix(key)) {
        return false;
    }
    return appendEscaped(value);
}

bool JsonWriter::writeStringArray(std::string_view key,
                                  std::span<const std::string_view> values) noexcept
{
    if (!memberPrefix(key)) {
        return false;
    }
    if (!append('[')) {
        return false;
    }
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0 && !append(',')) {
            return false;
        }
        if (!appendEscaped(values[i])) {
            return false;
        }
    }
    return append(']');
}

bool JsonWriter::writeIntArray(std::string_view key, std::span<const int32_t> values) noexcept
{
    if (!memberPrefix(key) || !append('[')) {
        return false;
    }
    for (size_t i = 0U; i < values.size(); ++i) {
        if ((i != 0U && !append(',')) || !appendInt(values[i])) {
            return false;
        }
    }
    return append(']');
}

size_t JsonWriter::finish(std::span<char> out) noexcept
{
    if (failed_ || !closed_ || size_ == 0 || out.size() < size_) {
        return 0;
    }
    // 先检查容量再拷贝：失败时调用方缓冲保持原样。
    for (size_t i = 0; i < size_; ++i) {
        out[i] = buffer_[i];
    }
    return size_;
}

bool JsonWriter::append(char c) noexcept
{
    if (failed_ || size_ >= buffer_.size()) {
        return fail();
    }
    buffer_[size_] = c;
    ++size_;
    return true;
}

bool JsonWriter::appendRaw(std::string_view text) noexcept
{
    for (const char c : text) {
        if (!append(c)) {
            return false;
        }
    }
    return true;
}

bool JsonWriter::appendEscaped(std::string_view text) noexcept
{
    if (!append('"')) {
        return false;
    }
    for (const char c : text) {
        if (!appendEscapedChar(c)) {
            return false;
        }
    }
    return append('"');
}

bool JsonWriter::appendEscapedChar(char c) noexcept
{
    switch (c) {
    case '"':
        return appendRaw("\\\"");
    case '\\':
        return appendRaw("\\\\");
    case '\b':
        return appendRaw("\\b");
    case '\f':
        return appendRaw("\\f");
    case '\n':
        return appendRaw("\\n");
    case '\r':
        return appendRaw("\\r");
    case '\t':
        return appendRaw("\\t");
    default:
        break;
    }
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20U) {
        return appendControlEscape(byte);
    }
    return append(c);
}

bool JsonWriter::appendControlEscape(unsigned char byte) noexcept
{
    // byte<0x20，拆成两个半字节后查表，索引 0..15 安全。
    const auto high = static_cast<size_t>(byte >> 4U);
    const auto low = static_cast<size_t>(byte & 0x0FU);
    const std::array<char, 6> escape{{'\\', 'u', '0', '0', kHexDigits[high], kHexDigits[low]}};
    return appendRaw(std::string_view(escape.data(), escape.size()));
}

bool JsonWriter::appendInt(int32_t value) noexcept
{
    std::array<char, kMaxIntDigits> digits{};
    int64_t magnitude = value;
    bool negative = false;
    if (magnitude < 0) {
        // int32 最小值取反会超出 int32，但 int64 可容纳；这里不窄化回 int32。
        negative = true;
        magnitude = -magnitude;
    }
    size_t count = 0;
    while (magnitude != 0) {
        // 余数 0..9，转 char 安全。
        digits[count] = static_cast<char>('0' + (magnitude % kIntDecimalBase));
        ++count;
        magnitude /= kIntDecimalBase;
    }
    if (count == 0) {
        digits[0] = '0';
        count = 1;
    }
    if (negative && !append('-')) {
        return false;
    }
    while (count > 0) {
        --count;
        if (!append(digits[count])) {
            return false;
        }
    }
    return true;
}

bool JsonWriter::memberPrefix(std::string_view key) noexcept
{
    if (failed_ || closed_ || depth_ == 0) {
        return fail();
    }
    if (memberCount_[depth_] > 0 && !append(',')) {
        return false;
    }
    if (!appendEscaped(key) || !append(':')) {
        return false;
    }
    ++memberCount_[depth_];
    return true;
}

bool JsonWriter::fail() noexcept
{
    failed_ = true;
    return false;
}

} // namespace web
