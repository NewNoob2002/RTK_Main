// 协议指令路由：各服务以 `static constexpr Route kRoutes[]` 登记 {msg_id, 允许的角色, handler}，
// 启动时由 init() 把 {self, routes} 展开到固定容量表。替代 CommandDispatcher 的大 switch。
// 与旧 dispatcher 逐字节等价（golden 测试）：
//   reply.clear() → 未登记 msg_id：kNoReply（先于角色检查；5.4 保留 ID 不登记即可）
//   → 角色不在 roles：kDenied → 否则调用 handler（type 等附加限制由 handler 自查，不满足返回 kDenied）。
// 线程安全：init 在启动前单线程调用；handle 只由链路任务调用。
// 冻结接口（ADR-0007 §3.3、§3.5）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/core/reply.hpp"
#include "rtk/core/types.hpp"
#include "rtk/proto/frame.hpp"
#include "rtk/status.hpp"

namespace rtk::core {

/** 允许角色的位图（每个 Role 占一位）。 */
using RoleMask = uint8_t;

[[nodiscard]] constexpr RoleMask role_bit(Role role) noexcept
{
    // Role 枚举值 < 8（types.hpp），移位结果落在 u8 内。
    return static_cast<RoleMask>(1U << static_cast<uint8_t>(role));
}

inline constexpr RoleMask kRolePhone = role_bit(Role::kPhone);
inline constexpr RoleMask kRoleAlarm = role_bit(Role::kAlarm);

/** handler 在链路任务中调用；reply 已 clear()。self 为登记时的服务对象。 */
using RouteHandler = DispatchResult (*)(void *self, const proto::FrameView &frame, uint32_t now_ms,
                                        Reply &reply) noexcept;

/** 一条路由：msg_id、允许角色位图与处理函数；表项静态存储，运行期只读。 */
struct Route {
    uint16_t msg_id = 0;
    RoleMask roles = 0;
    RouteHandler handler = nullptr;
};

/** 一个服务的路由表（{self, routes}）；启动期交给 init 展开，之后只读。 */
struct RouteTable {
    void *self = nullptr;
    std::span<const Route> routes;
};

inline constexpr size_t kMaxRoutes = 48;

/** msg_id → handler 的静态路由表；init 后只读查表，仅链路任务调用。 */
class CommandRouter {
public:
    constexpr CommandRouter() noexcept = default;
    CommandRouter(const CommandRouter &) = delete;
    CommandRouter &operator=(const CommandRouter &) = delete;
    CommandRouter(CommandRouter &&) = delete;
    CommandRouter &operator=(CommandRouter &&) = delete;
    ~CommandRouter() = default;

    /**
     * 展开全部路由表。错误：重复 msg_id、handler 为 nullptr、roles 为 0、self 为 nullptr → kInvalidArg；
     * 总数 > kMaxRoutes → kNoSpace；重复调用 → kBusy。出错时路由器保持未初始化（handle 一律 kNoReply）。
     */
    [[nodiscard]] Status init(std::span<const RouteTable> tables) noexcept;

    /** 见文件头的判定顺序。未 init → reply.clear() 后 kNoReply。 */
    [[nodiscard]] DispatchResult handle(Role role, const proto::FrameView &frame, uint32_t now_ms,
                                        Reply &reply) noexcept;

    [[nodiscard]] size_t route_count() const noexcept;

private:
    struct Entry {
        uint16_t msg_id = 0;
        RoleMask roles = 0;
        RouteHandler handler = nullptr;
        void *self = nullptr;
    };

    std::array<Entry, kMaxRoutes> entries_{};
    size_t count_ = 0;
    bool inited_ = false;
};

} // namespace rtk::core
