// 配对/白名单指令（PHONE）：0x44（设置=开配对窗/清空白名单，查询=回白名单条目）。
// 线程安全：否（链路任务独占）；非 ISR、不阻塞。堆：不分配。
// 冻结接口（ADR-0007 §3.3）。实现方不得修改。
#pragma once

#include <span>

#include "core/CommandRouter.hpp"
#include "core/LinkManager.hpp"
#include "core/Reply.hpp"

namespace core::services {

/** 配对/白名单指令服务（0x44）：改写 LinkManager 的白名单；仅链路任务调用。 */
class PairService {
public:
    /** @brief 只保存 LinkManager 引用。
     *  @param link 白名单状态机，须活到本对象析构（0x44 会直接改写它）。
     *  @note 启动期装配；不分配、非 ISR。 */
    explicit PairService(LinkManager &link) noexcept;
    PairService(const PairService &) = delete;
    PairService &operator=(const PairService &) = delete;
    PairService(PairService &&) = delete;
    PairService &operator=(PairService &&) = delete;
    ~PairService() = default;

    /** @brief 本服务负责的 0x44 静态路由表。
     *  @return 指向静态数组的视图，内容只读且与实例无关；不得修改。 */
    [[nodiscard]] static std::span<const Route> routes() noexcept;
    /** @brief 把静态路由表与 this 绑定，供 CommandRouter::init 展开。
     *  @return RouteTable{this, routes()}；本对象须存活到路由器不再使用该表为止。
     *  @note 不分配、无副作用；启动期调用。 */
    [[nodiscard]] RouteTable table() noexcept
    {
        return RouteTable{this, routes()};
    }

private:
    LinkManager &link_;
};

} // namespace core::services
