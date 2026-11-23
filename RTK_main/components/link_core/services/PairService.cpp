// PairService 实现（ADR-0007 §3.3）：配对/白名单指令 0x44（设置=开配对窗/清空白名单，查询=回条目）。
// 线程安全：否，只由链路任务串行调用；非 ISR、不阻塞。
// 堆：不分配（查询负载用栈上定长数组，应答复用调用方缓冲）。

#include "core/services/PairService.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"
#include "core/Types.hpp"
#include "../private_include/PairWindow.hpp"
#include "proto/AlarmCodec.hpp"
#include "proto/Frame.hpp"

namespace core::services {

namespace {

// 通用结果码（phone_protocol.md §3）。
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

/**
 * @brief 组应答帧：sender 固定为 DEV，type 经 responseType 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 * @param[out] reply 复用缓冲；成功时写入帧并设置 len，失败时 len 置 0。
 * @param msgId 请求的 msg_id，原样回填。
 * @param reqType 请求的 type，用于映射应答 type。
 * @param payload 应答负载。
 * @retval DispatchResult::kReply 已组帧。
 * @retval DispatchResult::kNoReply 组帧失败（缓冲不足等），reply.len 为 0。
 * @note 不分配、不加锁；非 ISR 可调（纯缓冲写入）。
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

/** @brief 结果码应答：§3 规定成功/失败都把同一字节写两遍。
 *  @param[out] reply 复用缓冲，语义同 replyBuild。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @param value 结果码（kResultOk/kResultFail）。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
inline DispatchResult replyResult(Reply &reply, uint16_t msgId, uint8_t reqType,
                                  uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return replyBuild(reply, msgId, reqType, payload);
}

/** @brief 成功应答（结果码 0x01 0x01）的快捷封装。
 *  @param[out] reply 复用缓冲。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
inline DispatchResult replyOk(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultOk);
}

/** @brief 失败应答（结果码 0x02 0x02）的快捷封装。
 *  @param[out] reply 复用缓冲。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
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

/** @brief 0x44 配对/白名单：SET 按动作开配对窗或清空白名单，其他 type 回当前白名单。
 *  @param[out] link 白名单状态机（开窗、清零、读取）。
 *  @param frame 请求帧（PHONE）；SET 时负载为空回失败，未知动作回失败。
 *  @param nowMs 当前时间（ms），作为配对窗起点。
 *  @param[out] reply 应答缓冲；清空白名单成功时置 whitelistChanged，供上层持久化。
 *  @retval DispatchResult::kReply 已组帧（成功或失败结果码，或白名单查询应答）。
 *  @retval DispatchResult::kNoReply 组帧失败。
 *  @note 查询应答为 1 字节数量 + n×6 字节 MAC，最多 kMaxAlarms 条。 */
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
            if (!admitWhitelistWrite(link)) {
                return replyFail(reply, frame.msgId, frame.type);
            }
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
