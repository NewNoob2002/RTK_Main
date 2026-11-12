// rtk_core 服务私有头（非冻结）：把应答组帧的公共部分集中在一处，供 services/*.cpp 共用。
// 只被 components/rtk_core/services/ 下的翻译单元包含，不进公共 include 目录。
// 语义自 command_dispatcher.cpp 的匿名命名空间原样迁出，保证应答字节不变。
// 线程安全：否（只在链路任务中调用，与 Reply 的所有权一致）。
//
// Generated-by: pi + workbuddy/deepseek-v4.1-flash
// Owner: UNKNOWN（待人工审查签字）
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/core/reply.hpp"
#include "rtk/proto/frame.hpp"
#include "rtk/status.hpp"

namespace rtk::core::services::detail {

// 通用结果码（phone_protocol.md §3；沿用旧 CommandDispatcher 的取值）。
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

/**
 * 组应答帧：sender 固定为 DEV，type 经 response_type 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 * 组帧失败时把 reply.len 置 0 并返回 kNoReply（与旧 dispatcher 一致）。
 */
inline DispatchResult reply_build(Reply &reply, uint16_t msg_id, uint8_t req_type,
                                  std::span<const uint8_t> payload) noexcept
{
    size_t len = 0;
    const Status status =
        proto::build_frame(reply.buf, msg_id, proto::sender::kDevice, proto::response_type(req_type), payload, len);
    if (!is_ok(status)) {
        reply.len = 0;
        return DispatchResult::kNoReply;
    }
    reply.len = len;
    return DispatchResult::kReply;
}

/** 结果码应答：§3 规定成功/失败都把同一字节写两遍。 */
inline DispatchResult reply_result(Reply &reply, uint16_t msg_id, uint8_t req_type, uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return reply_build(reply, msg_id, req_type, payload);
}

/** 成功应答（0x01 0x01）。 */
inline DispatchResult reply_ok(Reply &reply, uint16_t msg_id, uint8_t req_type) noexcept
{
    return reply_result(reply, msg_id, req_type, kResultOk);
}

/** 失败应答（0x02 0x02）。 */
inline DispatchResult reply_fail(Reply &reply, uint16_t msg_id, uint8_t req_type) noexcept
{
    return reply_result(reply, msg_id, req_type, kResultFail);
}

} // namespace rtk::core::services::detail
