// 报警相关指令：0x40 状态查询（ALARM，type 必须为 QUERY）、0x41 心跳（ALARM，type 必须为 PUSH，结果放
// reply.heartbeat）、 0x42 测试报警（PHONE）、0x43 电子围栏（PHONE）；以及 0x40 主动推送帧的组帧。 行为与
// CommandDispatcher 对应分支、buildStatusPush 逐字节一致。线程安全：否（链路任务独占）。 冻结接口（ADR-0007
// §3.3）。实现方不得修改。
#pragma once

#include <cstdint>
#include <span>

#include "core/AlarmSource.hpp"
#include "core/CommandRouter.hpp"
#include "core/LinkManager.hpp"
#include "core/Reply.hpp"

namespace core::services {

/** 报警类指令服务（0x40–0x43）：状态推送与测试覆盖；仅链路任务调用，非 ISR。 */
class AlarmService {
public:
    /** 只保存引用。info 用于状态帧中的电池等字段（由 DeviceService 持有）。 */
    AlarmService(AlarmSource &alarm, const LinkManager &link, const DeviceInfo &info) noexcept;
    AlarmService(const AlarmService &) = delete;
    AlarmService &operator=(const AlarmService &) = delete;
    AlarmService(AlarmService &&) = delete;
    AlarmService &operator=(AlarmService &&) = delete;
    ~AlarmService() = default;

    /** 组 0x40 主动上报帧（type=PUSH，sender=DEV）；语义同 CommandDispatcher::buildStatusPush。 */
    [[nodiscard]] bool buildStatusPush(uint16_t seq, uint32_t nowMs, Reply &reply) const noexcept;

    [[nodiscard]] static std::span<const Route> routes() noexcept;
    [[nodiscard]] RouteTable table() noexcept
    {
        return RouteTable{this, routes()};
    }

private:
    AlarmSource &alarm_;
    const LinkManager &link_;
    const DeviceInfo &info_;
};

} // namespace core::services
