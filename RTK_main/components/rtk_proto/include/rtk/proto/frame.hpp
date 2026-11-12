// 冻结接口（ADR-0003）。实现方不得修改。
// 手机/报警器 SPP 帧编解码的 C++ 外观：实现全部委托给 C 接口 rtk_proto_frame.h（报警器固件直接用 C 接口）。
// 本头只做类型安全包装（span、Status、FrameView），不重复实现协议逻辑。线路格式见 docs/protocol/phone_protocol.md
// §1–§3。 线程安全：否（FrameParser 每会话一个实例，只由链路任务使用）。ISR 安全：是。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/status.hpp"
#include "rtk_proto_frame.h"

namespace rtk::proto {

inline constexpr uint8_t kSyncA = 0xAA;
inline constexpr uint8_t kSyncB = 0x44;
inline constexpr uint8_t kSyncC = 0x18;
inline constexpr size_t kHeaderLen = 20;
inline constexpr size_t kCrcLen = 4;
inline constexpr size_t kMaxPayload = 256;
inline constexpr size_t kMaxFrame = kHeaderLen + kMaxPayload + kCrcLen;

// 头部字段偏移
inline constexpr size_t kOfsHeaderLen = 3;
inline constexpr size_t kOfsMsgId = 4;
inline constexpr size_t kOfsMsgLen = 12;
inline constexpr size_t kOfsSender = 16;
inline constexpr size_t kOfsType = 17;
inline constexpr size_t kOfsProtocol = 18;
inline constexpr size_t kOfsInterval = 19;

// 线路上的 sender / type 是原始字节，可能出现未定义值，因此 FrameView 中保存 uint8_t，
// 比较时用下列常量。
namespace sender {
inline constexpr uint8_t kApp = 0x00;
inline constexpr uint8_t kDevice = 0x01;
inline constexpr uint8_t kPanel = 0x02;
inline constexpr uint8_t kAlarm = 0x03;
} // namespace sender

namespace msg_type {
inline constexpr uint8_t kQuery = 0x00;
inline constexpr uint8_t kQueryRes = 0x01;
inline constexpr uint8_t kSet = 0x02;
inline constexpr uint8_t kSetRes = 0x03;
inline constexpr uint8_t kPush = 0x04;
inline constexpr uint8_t kErrReport = 0x05;
} // namespace msg_type

// 解析出的一帧。payload 指向解析器内部缓冲区，只在下一次 feed() 前有效。
struct FrameView {
    uint16_t msg_id = 0;
    uint8_t sender = 0;
    uint8_t type = 0;
    uint8_t protocol = 0;
    uint8_t interval = 0;
    std::span<const uint8_t> payload{};
};

/**
 * 标准 CRC-32（IEEE，反射多项式 0xEDB88320，zlib 兼容），可分段累计：
 * crc32(b, crc32(a)) == crc32(a || b)；crc32("123456789") == 0xCBF43926；空数据返回 crc 本身。
 */
[[nodiscard]] uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

/** 请求 type 映射到应答 type：0x00→0x01，0x02→0x03，其他→0x05。 */
[[nodiscard]] uint8_t response_type(uint8_t req_type) noexcept;

/**
 * 组一帧：头（预留字节、protocol、interval 置 0）+ payload + CRC（小端）。
 * 成功时 out_len = 20 + payload.size() + 4。
 * payload.size() > kMaxPayload → kInvalidArg；out 空间不足 → kNoSpace。失败时不写 out_len。
 */
[[nodiscard]] Status build_frame(std::span<uint8_t> out, uint16_t msg_id, uint8_t sender, uint8_t type,
                                 std::span<const uint8_t> payload, size_t &out_len) noexcept;

enum class ParseResult : uint8_t {
    kNone = 0,    // 需要更多字节
    kFrame,       // 一帧完整且 CRC 正确
    kCrcError,    // 一帧收完但 CRC 错误，已丢弃
    kLengthError, // 头长字节 != 0x14 或 payload 长度超限，已丢弃
};

// 逐字节流式解析器。每个会话一个实例，常驻，不得在每次收到数据时重建。
// 状态机规则：
// - 同步阶段遇到不匹配字节时，若该字节是 0xAA 则作为新帧起点，否则丢弃并重新寻找 0xAA。
// - 头长字节 != 0x14 或 payload 长度 > kMaxPayload：返回 kLengthError，length_errors()+1，
//   回到寻找 0xAA（不回扫已收字节）。
// - 一帧收完：CRC 正确返回 kFrame 并填 frame；否则 kCrcError，crc_errors()+1。两者都回到寻找 0xAA。
class FrameParser {
public:
    constexpr FrameParser() noexcept = default;

    /** 丢弃半帧状态，保留错误计数。 */
    void reset() noexcept;

    [[nodiscard]] ParseResult feed(uint8_t byte, FrameView &frame) noexcept;

    [[nodiscard]] uint32_t crc_errors() const noexcept
    {
        return parser_.crc_errors;
    }
    [[nodiscard]] uint32_t length_errors() const noexcept
    {
        return parser_.len_errors;
    }

private:
    rtk_proto_parser_t parser_{}; // 全零即初始状态（C 头文件契约）
};

// C++ 常量必须与 C 宏一致（两端共用同一线路格式）。
static_assert(kHeaderLen == RTK_PROTO_HEADER_LEN);
static_assert(kCrcLen == RTK_PROTO_CRC_LEN);
static_assert(kMaxPayload == RTK_PROTO_MAX_PAYLOAD);
static_assert(kMaxFrame == RTK_PROTO_MAX_FRAME);
static_assert(sender::kAlarm == RTK_PROTO_SENDER_ALARM);
static_assert(msg_type::kPush == RTK_PROTO_TYPE_PUSH);

} // namespace rtk::proto
