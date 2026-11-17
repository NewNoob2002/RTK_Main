// 协议指令路由实现（ADR-0007 §3.5）：启动时把各服务登记的 Route 表展开到固定容量索引，
// 链路任务按 msgId 查找后调用对应 handler，替代旧 CommandDispatcher 的大 switch。
// 判定顺序与旧 dispatcher 一致：未登记 → kNoReply（先于角色检查）→ 角色不在 RoleMask → kDenied → handler。

#include "core/CommandRouter.hpp"
#include "core/TraceCodes.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace core {

fw::Status CommandRouter::init(std::span<const RouteTable> tables) noexcept
{
    if (inited_) {
        // 重复初始化按契约返回 kBusy，且不改动已有索引。
        return fw::Status::kBusy;
    }

    count_ = 0;
    fw::Status status = fw::Status::kOk;
    for (const RouteTable &table : tables) {
        if (!isOk(status)) {
            break;
        }
        if (table.self == nullptr) {
            // self 为空时 handler 无从取回服务对象，整体拒绝。
            status = fw::Status::kInvalidArg;
            break;
        }
        for (const Route &route : table.routes) {
            if ((route.handler == nullptr) || (route.roles == 0)) {
                status = fw::Status::kInvalidArg;
                break;
            }
            bool duplicated = false;
            for (size_t i = 0; i < count_; ++i) {
                if (entries_[i].msgId == route.msgId) {
                    duplicated = true;
                    break;
                }
            }
            if (duplicated) {
                // 同一 msgId 只能有一个 handler；重复登记在启动期就暴露。
                status = fw::Status::kInvalidArg;
                break;
            }
            if (count_ >= kMaxRoutes) {
                status = fw::Status::kNoSpace;
                break;
            }
            entries_[count_].msgId = route.msgId;
            entries_[count_].roles = route.roles;
            entries_[count_].handler = route.handler;
            entries_[count_].self = table.self;
            ++count_;
        }
    }

    if (!isOk(status)) {
        // 出错时路由器保持未初始化：handle 一律 reply.clear() + kNoReply。
        count_ = 0;
        return status;
    }
    inited_ = true;
    return status;
}

DispatchResult CommandRouter::handle(Role role, const proto::FrameView &frame, uint32_t nowMs,
                                     Reply &reply) noexcept
{
    reply.clear();
    if (!inited_) {
        return DispatchResult::kNoReply;
    }

    // Role 枚举值 < 8（Types.hpp），移位结果落在 u8 内。
    const RoleMask mask = roleBit(role);
    for (size_t i = 0; i < count_; ++i) {
        const Entry &entry = entries_[i];
        if (entry.msgId != frame.msgId) {
            continue;
        }
        if ((entry.roles & mask) == 0) {
            // 无路由：路由器不知道会话句柄，session 记 0。
            traceProto(ProtoTrace::kNoRoute, 0, static_cast<uint32_t>(frame.msgId),
                       static_cast<uint32_t>(role));
            return DispatchResult::kDenied;
        }
        return entry.handler(entry.self, frame, nowMs, reply);
    }
    // 未登记 ID（含 5.4 保留 ID）不回复，先于角色检查。
    traceProto(ProtoTrace::kNoRoute, 0, static_cast<uint32_t>(frame.msgId),
               static_cast<uint32_t>(role));
    return DispatchResult::kNoReply;
}

size_t CommandRouter::routeCount() const noexcept
{
    return count_;
}

} // namespace core
