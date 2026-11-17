// 手机/报警器 SPP 帧编解码的 C++ 外观（ADR-0003）。
// 协议逻辑全部在 DevProtoFrame.c；本文件只做类型安全包装：span → 指针+长度、
// dev_proto_err_t / dev_proto_parse_result_t → fw::Status / ParseResult、C 结构 → FrameView。
// 线程安全：否（FrameParser 每会话一个实例，只由链路任务使用）。ISR 安全：是。

#include "proto/Frame.hpp"

namespace proto {

namespace {

fw::Status from_c_err(dev_proto_err_t err) noexcept
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

ParseResult from_c_result(dev_proto_parse_result_t result) noexcept
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

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) noexcept
{
    return dev_proto_crc32(crc, data.data(), data.size());
}

uint8_t response_type(uint8_t req_type) noexcept
{
    return dev_proto_response_type(req_type);
}

fw::Status build_frame(std::span<uint8_t> out, uint16_t msg_id, uint8_t sender, uint8_t type,
                       std::span<const uint8_t> payload, size_t &out_len) noexcept
{
    if (payload.size() > kMaxPayload) {
        return fw::Status::kInvalidArg;
    }
    if (out.empty()) {
        /* 空 span 的 data() 可能为 NULL，C 接口会报 ARG；空间不足语义应是 kNoSpace。 */
        return fw::Status::kNoSpace;
    }

    size_t c_out_len = 0;
    /* payload.size() ≤ kMaxPayload(256)，uint16_t 窄化安全 */
    const dev_proto_err_t err =
        dev_proto_frame_build(out.data(), out.size(), msg_id, sender, type, payload.data(),
                              static_cast<uint16_t>(payload.size()), &c_out_len);
    if (err != DEV_PROTO_OK) {
        return from_c_err(err);
    }

    out_len = c_out_len;
    return fw::Status::kOk;
}

void FrameParser::reset() noexcept
{
    dev_proto_parser_reset(&parser_);
}

ParseResult FrameParser::feed(uint8_t byte, FrameView &frame) noexcept
{
    dev_proto_frame_t c_frame{};
    const ParseResult result = from_c_result(dev_proto_parser_feed(&parser_, byte, &c_frame));

    if (result != ParseResult::kFrame) {
        return result;
    }

    frame.msg_id = c_frame.msg_id;
    frame.sender = c_frame.sender;
    frame.type = c_frame.type;
    frame.protocol = c_frame.protocol;
    frame.interval = c_frame.interval;
    frame.payload = (c_frame.payload != nullptr)
                        ? std::span<const uint8_t>(c_frame.payload, c_frame.payload_len)
                        : std::span<const uint8_t>();
    return ParseResult::kFrame;
}

} // namespace proto
