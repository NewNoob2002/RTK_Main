// 手机/报警器 SPP 帧编解码的 C++ 外观（冻结，ADR-0003）：只做 span/status/视图的类型安全转换，协议逻辑在
// DevProtoFrame.c。FrameParser 每会话一个实例、只由链路任务使用，非线程安全；无阻塞、无分配，ISR 安全。
// 不使用堆；线路格式见 docs/protocol/phone_protocol.md §1–§3。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/Status.hpp"
#include "DevProtoFrame.h"

namespace proto {

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

/**
 * @brief  一次解析成功的帧视图；payload 指向解析器内部缓冲，有效期到下次 feed
 * @note   线路上的 sender/type 可能为未定义值，故按原始字节保存
 */
struct FrameView {
    uint16_t msgId = 0;
    uint8_t sender = 0;
    uint8_t type = 0;
    uint8_t protocol = 0;
    uint8_t interval = 0;
    std::span<const uint8_t> payload{};
};

/**
 * @brief  计算标准 CRC-32（IEEE，反射多项式 0xEDB88320，zlib 兼容），可分段累计
 * @param  data: 待计算数据（只读）
 * @param  crc:  上一段数据的 CRC；首段用默认 0
 * @retval 累计后的 CRC-32；crc32(b, crc32(a)) == crc32(a || b)
 * @note   crc32("123456789") == 0xCBF43926；空数据返回 crc 本身；纯函数、ISR 安全
 */
[[nodiscard]] uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

/**
 * @brief  把请求 type 映射为应答 type
 * @param  reqType: 请求帧的 type 字节
 * @retval 0x01 请求为 QUERY
 * @retval 0x03 请求为 SET
 * @retval 0x05 其他值（含本身已是应答的 type）
 * @note   纯函数、无阻塞、ISR 安全
 */
[[nodiscard]] uint8_t responseType(uint8_t reqType) noexcept;

/**
 * @brief  组一帧：头（预留字节与 protocol/interval 置 0）+ payload + CRC（小端）
 * @param  out:     输出缓冲
 * @param  msgId:   消息号，写入偏移 4（u16 小端）
 * @param  sender:  sender 字段字节
 * @param  type:    type 字段字节
 * @param  payload: 负载数据
 * @param[out] outLen: 成功时写入帧总长 20 + payload.size() + 4；失败时不改动
 * @retval fw::Status::kOk         成功
 * @retval fw::Status::kInvalidArg payload.size() > kMaxPayload
 * @retval fw::Status::kNoSpace    out 容量不足（含空 span）
 * @note   失败时 out 内容未定义；无阻塞、无分配、ISR 安全
 */
[[nodiscard]] fw::Status buildFrame(std::span<uint8_t> out, uint16_t msgId, uint8_t sender,
                                    uint8_t type, std::span<const uint8_t> payload,
                                    size_t &outLen) noexcept;

/** 一次解析调用的结果：找到完整帧 / 需要更多字节 / 出错。 */
enum class ParseResult : uint8_t {
    kNone = 0,    // 需要更多字节
    kFrame,       // 一帧完整且 CRC 正确
    kCrcError,    // 一帧收完但 CRC 错误，已丢弃
    kLengthError, // 头长字节 != 0x14 或 payload 长度超限，已丢弃
};

/**
 * @brief  每会话独立的逐字节流式帧解析器（内部委托 C 实现）
 * @note   每会话一个实例、常驻，不得在每次收到数据时重建；只由链路任务使用。状态机规则：
 *         - 同步阶段遇到不匹配字节时，若该字节是 0xAA 则作为新帧起点，否则丢弃并重新寻找 0xAA。
 *         - 头长字节 != 0x14 或 payload 长度 > kMaxPayload：返回 kLengthError，lengthErrors()+1，
 *           回到寻找 0xAA（不回扫已收字节）。
 *         - 一帧收完：CRC 正确返回 kFrame 并填 frame；否则 kCrcError，crcErrors()+1。两者都回到寻找 0xAA。
 */
class FrameParser {
public:
    constexpr FrameParser() noexcept = default;

    /** @brief 丢弃半帧状态，保留错误计数；无阻塞、ISR 安全。 */
    void reset() noexcept;

    /**
     * @brief  喂入一个线路字节，凑满一帧时返回 kFrame 并填充 frame
     * @param  byte:  新收到的线路字节
     * @param[out] frame: 返回 kFrame 时填充；其他返回值不改动
     * @retval ParseResult::kNone        需要更多字节
     * @retval ParseResult::kFrame       一帧完整且 CRC 正确，frame 已填充
     * @retval ParseResult::kCrcError    一帧收完但 CRC 错误，已丢弃
     * @retval ParseResult::kLengthError 头长字节 != 0x14 或 payload 长度超限，已丢弃
     * @note   返回 kFrame 时 frame.payload 指向解析器内部缓冲，有效期到下次 feed；无阻塞、ISR 安全
     */
    [[nodiscard]] ParseResult feed(uint8_t byte, FrameView &frame) noexcept;

    /**
     * @brief  读取累计 CRC 错误帧数
     * @retval 自对象构造以来的 CRC 错误计数（reset 不清零）
     */
    [[nodiscard]] uint32_t crcErrors() const noexcept
    {
        return parser_.crc_errors;
    }
    /**
     * @brief  读取累计长度错误帧数
     * @retval 自对象构造以来的长度错误计数（reset 不清零）
     */
    [[nodiscard]] uint32_t lengthErrors() const noexcept
    {
        return parser_.len_errors;
    }

private:
    dev_proto_parser_t parser_{}; // 全零即初始状态（C 头文件契约）
};

// C++ 常量必须与 C 宏一致（两端共用同一线路格式）。
static_assert(kHeaderLen == DEV_PROTO_HEADER_LEN);
static_assert(kCrcLen == DEV_PROTO_CRC_LEN);
static_assert(kMaxPayload == DEV_PROTO_MAX_PAYLOAD);
static_assert(kMaxFrame == DEV_PROTO_MAX_FRAME);
static_assert(sender::kAlarm == DEV_PROTO_SENDER_ALARM);
static_assert(msg_type::kPush == DEV_PROTO_TYPE_PUSH);

} // namespace proto
