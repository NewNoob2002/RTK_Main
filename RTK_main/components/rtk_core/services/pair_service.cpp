// PairService 实现（ADR-0007 §3.3）：配对/白名单指令 0x44（设置/查询/清除）。
// 各分支自 command_dispatcher.cpp 逐字节搬迁，应答内容与旧 CommandDispatcher 一致。
//
// Generated-by: pi + workbuddy/deepseek-v4.1-flash
// Owner: UNKNOWN（待人工审查签字）

#include "rtk/core/services/pair_service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/core/command_router.hpp"
#include "rtk/core/reply.hpp"
#include "rtk/core/types.hpp"
#include "rtk/proto/alarm_codec.hpp"
#include "rtk/proto/frame.hpp"
#include "service_support.hpp"

namespace rtk::core::services {

namespace {

namespace msg_type = rtk::proto::msg_type;

// 0x44 payload 的动作字节（§5，与 0x42 测试报警共用取值）。
constexpr uint8_t kPairActionOpen = 0x01;
constexpr uint8_t kPairActionClear = 0x00;

// 0x44 查询应答：1 字节数量 + n×6 字节 MAC。
constexpr size_t kPairOfsCount = 0;
constexpr size_t kPairOfsFirst = 1;
constexpr size_t kPairQueryMax = kPairOfsFirst + (kMaxAlarms * kAddrLen);

DispatchResult handle_pair(LinkManager &link, const rtk::proto::FrameView &frame, uint32_t now_ms,
                           Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.empty()) {
            return detail::reply_fail(reply, frame.msg_id, frame.type);
        }
        const uint8_t action = frame.payload[0];
        if (action == kPairActionOpen) {
            link.pair_open(now_ms);
            return detail::reply_ok(reply, frame.msg_id, frame.type);
        }
        if (action == kPairActionClear) {
            link.whitelist_clear();
            reply.whitelist_changed = true;
            return detail::reply_ok(reply, frame.msg_id, frame.type);
        }
        return detail::reply_fail(reply, frame.msg_id, frame.type);
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
    return detail::reply_build(reply, frame.msg_id, frame.type, std::span<const uint8_t>(payload).first(len));
}

} // namespace

PairService::PairService(LinkManager &link) noexcept : link_(link) {}

std::span<const Route> PairService::routes() noexcept
{
    // 冻结头没有暴露 link_，而 handler 只拿到 void* self。局部类定义在成员函数内，按 [class.access]
    // 与成员函数具有相同访问权限，因此 static_cast 之后可以取到私有引用；
    // 每个 self 都来自 table()（RouteTable{this, ...}），类型必然是本服务实例。
    struct Handlers {
        static DispatchResult pair(void *self, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
        {
            auto *const service = static_cast<PairService *>(self);
            return handle_pair(service->link_, frame, now_ms, reply);
        }
    };

    static constexpr std::array<Route, 1> kRoutes = {{
        {rtk::proto::alarm::kMsgPair, kRolePhone, &Handlers::pair},
    }};
    return kRoutes;
}

} // namespace rtk::core::services
