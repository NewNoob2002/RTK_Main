// 手机/报警器 SPP 帧编解码的 C++ 外观（ADR-0003）。
// 协议逻辑全部在 rtk_proto_frame.c；本文件只做类型安全包装：span → 指针+长度、
// rtk_proto_err_t / rtk_proto_parse_result_t → Status / ParseResult、C 结构 → FrameView。
// 线程安全：否（FrameParser 每会话一个实例，只由链路任务使用）。ISR 安全：是。

#include "rtk/proto/frame.hpp"

namespace rtk::proto {

namespace {

Status from_c_err(rtk_proto_err_t err) noexcept
{
    switch (err) {
    case RTK_PROTO_OK:
        return Status::kOk;
    case RTK_PROTO_ERR_ARG:
        return Status::kInvalidArg;
    case RTK_PROTO_ERR_SPACE:
        return Status::kNoSpace;
    default:
        return Status::kInvalidArg;
    }
}

ParseResult from_c_result(rtk_proto_parse_result_t result) noexcept
{
    switch (result) {
    case RTK_PROTO_PARSE_NONE:
        return ParseResult::kNone;
    case RTK_PROTO_PARSE_FRAME:
        return ParseResult::kFrame;
    case RTK_PROTO_PARSE_CRC_ERR:
        return ParseResult::kCrcError;
    case RTK_PROTO_PARSE_LEN_ERR:
        return ParseResult::kLengthError;
    default:
        return ParseResult::kNone;
    }
}

} // namespace

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) noexcept
{
    return rtk_proto_crc32(crc, data.data(), data.size());
}

uint8_t response_type(uint8_t req_type) noexcept
{
    return rtk_proto_response_type(req_type);
}

Status build_frame(std::span<uint8_t> out, uint16_t msg_id, uint8_t sender, uint8_t type,
                   std::span<const uint8_t> payload, size_t &out_len) noexcept
{
    if (payload.size() > kMaxPayload) {
        return Status::kInvalidArg;
    }
    if (out.empty()) {
        /* 空 span 的 data() 可能为 NULL，C 接口会报 ARG；空间不足语义应是 kNoSpace。 */
        return Status::kNoSpace;
    }

    size_t c_out_len = 0;
    /* payload.size() ≤ kMaxPayload(256)，uint16_t 窄化安全 */
    const rtk_proto_err_t err = rtk_proto_frame_build(out.data(), out.size(), msg_id, sender, type, payload.data(),
                                                      static_cast<uint16_t>(payload.size()), &c_out_len);
    if (err != RTK_PROTO_OK) {
        return from_c_err(err);
    }

    out_len = c_out_len;
    return Status::kOk;
}

void FrameParser::reset() noexcept
{
    rtk_proto_parser_reset(&parser_);
}

ParseResult FrameParser::feed(uint8_t byte, FrameView &frame) noexcept
{
    rtk_proto_frame_t c_frame{};
    const ParseResult result = from_c_result(rtk_proto_parser_feed(&parser_, byte, &c_frame));

    if (result != ParseResult::kFrame) {
        return result;
    }

    frame.msg_id = c_frame.msg_id;
    frame.sender = c_frame.sender;
    frame.type = c_frame.type;
    frame.protocol = c_frame.protocol;
    frame.interval = c_frame.interval;
    frame.payload = (c_frame.payload != nullptr) ? std::span<const uint8_t>(c_frame.payload, c_frame.payload_len)
                                                 : std::span<const uint8_t>();
    return ParseResult::kFrame;
}

} // namespace rtk::proto
