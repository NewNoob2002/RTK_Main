// 协议指令路由：各服务以 `static constexpr Route kRoutes[]` 登记 {msgId, 允许角色, handler}，init() 展开到定长表。
// 线程安全：init 于启动前单线程调用，handle 只由链路任务调用；非 ISR、不阻塞、不分配（表为定长数组）。
// 冻结接口（ADR-0007 §3.3、§3.5）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/Reply.hpp"
#include "core/Types.hpp"
#include "proto/Frame.hpp"
#include "fw/Status.hpp"

namespace core {

/** 允许角色的位图（每个 Role 占一位）。 */
using RoleMask = uint8_t;

/** @brief 把 Role 换算成路由角色位图。
 *  @param role 目标角色。
 *  @return 1U << role（Role 数值 < 8，结果落在 u8 内）。 */
[[nodiscard]] constexpr RoleMask roleBit(Role role) noexcept
{
    // Role 枚举值 < 8（Types.hpp），移位结果落在 u8 内。
    return static_cast<RoleMask>(1U << static_cast<uint8_t>(role));
}

inline constexpr RoleMask kRolePhone = roleBit(Role::kPhone);
inline constexpr RoleMask kRoleAlarm = roleBit(Role::kAlarm);

/** handler 在链路任务中调用；reply 已 clear()。self 为登记时的服务对象。 */
using RouteHandler = DispatchResult (*)(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                        Reply &reply) noexcept;

/** 一条路由：msgId、允许角色位图与处理函数；表项静态存储，运行期只读。 */
struct Route {
    uint16_t msgId = 0;
    RoleMask roles = 0;
    RouteHandler handler = nullptr;
};

/** 一个服务的路由表（{self, routes}）；启动期交给 init 展开，之后只读。 */
struct RouteTable {
    void *self = nullptr;
    std::span<const Route> routes;
};

inline constexpr size_t kMaxRoutes = 48;

/** msgId → handler 的静态路由表；init 后只读查表，仅链路任务调用。 */
class CommandRouter {
public:
    constexpr CommandRouter() noexcept = default;
    CommandRouter(const CommandRouter &) = delete;
    CommandRouter &operator=(const CommandRouter &) = delete;
    CommandRouter(CommandRouter &&) = delete;
    CommandRouter &operator=(CommandRouter &&) = delete;
    ~CommandRouter() = default;

    /**
     * @brief 展开全部路由表到定长表。出错时路由器保持未初始化（handle 一律 kNoReply）。
     * @param tables 各服务的 {self, routes} 列表；表项静态存储，调用后不再被读取。
     * @retval fw::Status::kOk 展开成功，可以 handle。
     * @retval fw::Status::kInvalidArg 重复 msgId、handler 为 nullptr、roles 为 0 或 self 为 nullptr。
     * @retval fw::Status::kNoSpace 总条数超过 kMaxRoutes。
     * @retval fw::Status::kBusy 重复调用（已 init）。
     * @note 仅启动阶段单线程调用一次，非 ISR、不阻塞、不分配。
     */
    [[nodiscard]] fw::Status init(std::span<const RouteTable> tables) noexcept;

    /**
     * @brief 按 msgId 与发送方角色分发一帧，结果写入 reply。
     * @param role 发送方角色。
     * @param frame 已通过 CRC/长度校验的帧视图。
     * @param nowMs 当前时间（ms），原样交给 handler。
     * @param[out] reply 复用应答缓冲；进入时先 clear()，handler 可写应答或副作用标志，失败时保持 clear 后状态。
     * @retval DispatchResult::kReply reply 中有应答帧，应发回同一会话。
     * @retval DispatchResult::kNoReply 未登记 msgId（先于角色检查；5.4 保留 ID 不登记即可）或按协议不回复。
     * @retval DispatchResult::kDenied 角色不在允许位图，或 handler 自查 type 等限制后拒绝。
     * @note 判定顺序（golden 测试逐字节固定）：
     *       reply.clear() → 未登记 msgId 则 kNoReply → 角色不符则 kDenied → 否则调用 handler。
     *       只允许链路任务调用，非 ISR、不阻塞。
     */
    [[nodiscard]] DispatchResult handle(Role role, const proto::FrameView &frame, uint32_t nowMs,
                                        Reply &reply) noexcept;

    /** @brief 已登记的路由条数。
     *  @return init 成功后的表项数；未 init 时为 0。 */
    [[nodiscard]] size_t routeCount() const noexcept;

private:
    struct Entry {
        uint16_t msgId = 0;
        RoleMask roles = 0;
        RouteHandler handler = nullptr;
        void *self = nullptr;
    };

    std::array<Entry, kMaxRoutes> entries_{};
    size_t count_ = 0;
    bool inited_ = false;
};

} // namespace core
