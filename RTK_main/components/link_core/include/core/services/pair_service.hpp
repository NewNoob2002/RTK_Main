// 配对/白名单指令（PHONE）：0x44（设置/查询/清除，成功修改时 reply.whitelist_changed = true）。
// 行为与 CommandDispatcher 对应分支逐字节一致。线程安全：否（链路任务独占）。
// 冻结接口（ADR-0007 §3.3）。实现方不得修改。
#pragma once

#include <span>

#include "core/command_router.hpp"
#include "core/link_manager.hpp"
#include "core/reply.hpp"

namespace core::services {

/** 配对/白名单指令服务（0x44）：改写 LinkManager 的白名单；仅链路任务调用。 */
class PairService {
public:
    explicit PairService(LinkManager &link) noexcept;
    PairService(const PairService &) = delete;
    PairService &operator=(const PairService &) = delete;
    PairService(PairService &&) = delete;
    PairService &operator=(PairService &&) = delete;
    ~PairService() = default;

    [[nodiscard]] static std::span<const Route> routes() noexcept;
    [[nodiscard]] RouteTable table() noexcept
    {
        return RouteTable{this, routes()};
    }

private:
    LinkManager &link_;
};

} // namespace core::services
