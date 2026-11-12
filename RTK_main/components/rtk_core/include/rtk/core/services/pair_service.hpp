// 冻结接口（ADR-0007 §3.3）。实现方不得修改。
// 配对/白名单指令（PHONE）：0x44（设置/查询/清除，成功修改时 reply.whitelist_changed = true）。
// 行为与 CommandDispatcher 对应分支逐字节一致。线程安全：否（链路任务独占）。
//
// Generated-by: Claude Code + claude-opus-5-5
// Owner: UNKNOWN（待人工审查签字）
#pragma once

#include <span>

#include "rtk/core/command_router.hpp"
#include "rtk/core/link_manager.hpp"
#include "rtk/core/reply.hpp"

namespace rtk::core::services {

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

} // namespace rtk::core::services
