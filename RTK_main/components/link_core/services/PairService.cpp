// PairService 实现（ADR-0007 §3.3）：配对/白名单指令 0x44（设置/查询/清除）。
// 各分支自 command_dispatcher.cpp 逐字节搬迁，应答内容与旧 CommandDispatcher 一致。

#include "core/services/PairService.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"
#include "core/Types.hpp"
#include "proto/AlarmCodec.hpp"
#include "proto/Frame.hpp"

namespace core::services {

namespace {

// 通用结果码（phone_protocol.md §3；沿用旧 CommandDispatcher 的取值）。
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

/**
 * 组应答帧：sender 固定为 DEV，type 经 responseType 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 * 组帧失败时把 reply.len 置 0 并返回 kNoReply（与旧 dispatcher 一致）。
 */
inline DispatchResult replyBuild(Reply &reply, uint16_t msgId, uint8_t reqType,
                                 std::span<const uint8_t> payload) noexcept
{
    size_t len = 0;
    const fw::Status status = proto::buildFrame(reply.buf, msgId, proto::sender::kDevice,
                                                proto::responseType(reqType), payload, len);
    if (!isOk(status)) {
        reply.len = 0;
        return DispatchResult::kNoReply;
    }
    reply.len = len;
    return DispatchResult::kReply;
}

/** 结果码应答：§3 规定成功/失败都把同一字节写两遍。 */
inline DispatchResult replyResult(Reply &reply, uint16_t msgId, uint8_t reqType,
                                  uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return replyBuild(reply, msgId, reqType, payload);
}

/** 成功应答（0x01 0x01）。 */
inline DispatchResult replyOk(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultOk);
}

/** 失败应答（0x02 0x02）。 */
inline DispatchResult replyFail(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultFail);
}

namespace msg_type = proto::msg_type;

// 0x44 payload 的动作字节（§5，与 0x42 测试报警共用取值）。
constexpr uint8_t kPairActionOpen = 0x01;
constexpr uint8_t kPairActionClear = 0x00;

// 0x44 查询应答：1 字节数量 + n×6 字节 MAC。
constexpr size_t kPairOfsCount = 0;
constexpr size_t kPairOfsFirst = 1;
constexpr size_t kPairQueryMax = kPairOfsFirst + (kMaxAlarms * kAddrLen);

DispatchResult handlePair(LinkManager &link, const proto::FrameView &frame, uint32_t nowMs,
                          Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.empty()) {
            return replyFail(reply, frame.msgId, frame.type);
        }
        const uint8_t action = frame.payload[0];
        if (action == kPairActionOpen) {
            link.pairOpen(nowMs);
            return replyOk(reply, frame.msgId, frame.type);
        }
        if (action == kPairActionClear) {
            link.whitelistClear();
            reply.whitelistChanged = true;
            return replyOk(reply, frame.msgId, frame.type);
        }
        return replyFail(reply, frame.msgId, frame.type);
    }

    std::array<uint8_t, kPairQueryMax> payload{};
    const std::span<const BtAddr> list = link.whitelist();
    const size_t count = (list.size() < kMaxAlarms) ? list.size() : kMaxAlarms;
    // count ≤ kMaxAlarms(4)，uint8_t 截断安全。
    payload[kPairOfsCount] = static_cast<uint8_t>(count);
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < kAddrLen; ++j) {
            payload[kPairOfsFirst + (i * kAddrLen) + j] = list[i].bytes[j];
        }
    }
    const size_t len = kPairOfsFirst + (count * kAddrLen);
    return replyBuild(reply, frame.msgId, frame.type, std::span<const uint8_t>(payload).first(len));
}

} // namespace

PairService::PairService(LinkManager &link) noexcept : link_(link) {}

std::span<const Route> PairService::routes() noexcept
{
    // 冻结头没有暴露 link_，而 handler 只拿到 void* self。局部类定义在成员函数内，按 [class.access]
    // 与成员函数具有相同访问权限，因此 static_cast 之后可以取到私有引用；
    // 每个 self 都来自 table()（RouteTable{this, ...}），类型必然是本服务实例。
    struct Handlers {
        static DispatchResult pair(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                   Reply &reply) noexcept
        {
            auto *const service = static_cast<PairService *>(self);
            return handlePair(service->link_, frame, nowMs, reply);
        }
    };

    static constexpr std::array<Route, 1> kRoutes = {{
        {proto::alarm::kMsgPair, kRolePhone, &Handlers::pair},
    }};
    return kRoutes;
}

} // namespace core::services
