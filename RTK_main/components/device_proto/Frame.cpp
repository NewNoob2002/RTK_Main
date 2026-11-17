// 手机/报警器 SPP 帧编解码的 C++ 外观（ADR-0003）：只做 span/status/视图转换，协议逻辑在 DevProtoFrame.c。
// FrameParser 每会话一个实例、只由链路任务使用，非线程安全；无阻塞、无分配，ISR 安全。
// 不使用堆。

#include "proto/Frame.hpp"

namespace proto {

namespace {

/**
 * @brief  把 C 层错误码映射为 fw::Status
 * @param  err: DevProtoFrame.h 的错误码
 * @retval fw::Status::kOk          DEV_PROTO_OK
 * @retval fw::Status::kInvalidArg  DEV_PROTO_ERR_ARG 或未知值
 * @retval fw::Status::kNoSpace     DEV_PROTO_ERR_SPACE
 * @note   纯函数、ISR 安全
 */
fw::Status fromCErr(dev_proto_err_t err) noexcept
{
    switch (err) {
    case DEV_PROTO_OK:
        return fw::Status::kOk;
    case DEV_PROTO_ERR_ARG:
        return fw::Status::kInvalidArg;
    case DEV_PROTO_ERR_SPACE:
        return fw::Status::kNoSpace;
    default:
        return fw::Status::kInvalidArg;
    }
}

/**
 * @brief  把 C 层解析结果映射为 ParseResult
 * @param  result: DevProtoFrame.h 的解析结果
 * @retval 与输入一一对应的 ParseResult；未知值按 kNone（需要更多字节）处理
 * @note   纯函数、ISR 安全
 */
ParseResult fromCResult(dev_proto_parse_result_t result) noexcept
{
    switch (result) {
    case DEV_PROTO_PARSE_NONE:
        return ParseResult::kNone;
    case DEV_PROTO_PARSE_FRAME:
        return ParseResult::kFrame;
    case DEV_PROTO_PARSE_CRC_ERR:
        return ParseResult::kCrcError;
    case DEV_PROTO_PARSE_LEN_ERR:
        return ParseResult::kLengthError;
    default:
        return ParseResult::kNone;
    }
}

} // namespace

// C 实现 dev_proto_crc32 的 C++ 外观（ADR-0003）：span → 指针 + 长度，供 link_core 跨组件调用。
uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) noexcept
{
    return dev_proto_crc32(crc, data.data(), data.size());
}

uint8_t responseType(uint8_t reqType) noexcept
{
    return dev_proto_response_type(reqType);
}

fw::Status buildFrame(std::span<uint8_t> out, uint16_t msgId, uint8_t sender, uint8_t type,
                      std::span<const uint8_t> payload, size_t &outLen) noexcept
{
    if (payload.size() > kMaxPayload) {
        return fw::Status::kInvalidArg;
    }
    if (out.empty()) {
        /* 空 span 的 data() 可能为 NULL，C 接口会报 ARG；空间不足语义应是 kNoSpace。 */
        return fw::Status::kNoSpace;
    }

    size_t cOutLen = 0;
    /* payload.size() ≤ kMaxPayload(256)，uint16_t 窄化安全 */
    const dev_proto_err_t err =
        dev_proto_frame_build(out.data(), out.size(), msgId, sender, type, payload.data(),
                              static_cast<uint16_t>(payload.size()), &cOutLen);
    if (err != DEV_PROTO_OK) {
        return fromCErr(err);
    }

    outLen = cOutLen;
    return fw::Status::kOk;
}

void FrameParser::reset() noexcept
{
    dev_proto_parser_reset(&parser_);
}

ParseResult FrameParser::feed(uint8_t byte, FrameView &frame) noexcept
{
    dev_proto_frame_t cFrame{};
    const ParseResult result = fromCResult(dev_proto_parser_feed(&parser_, byte, &cFrame));

    if (result != ParseResult::kFrame) {
        return result;
    }

    frame.msgId = cFrame.msg_id;
    frame.sender = cFrame.sender;
    frame.type = cFrame.type;
    frame.protocol = cFrame.protocol;
    frame.interval = cFrame.interval;
    frame.payload = (cFrame.payload != nullptr)
                        ? std::span<const uint8_t>(cFrame.payload, cFrame.payload_len)
                        : std::span<const uint8_t>();
    return ParseResult::kFrame;
}

} // namespace proto
