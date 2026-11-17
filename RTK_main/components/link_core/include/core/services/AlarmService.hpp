// 报警相关指令：0x40 状态查询（ALARM、type=QUERY）、0x41 心跳（ALARM、type=PUSH，结果放 reply.heartbeat）、
// 0x42 测试报警（PHONE）、0x43 电子围栏（PHONE），以及 0x40 主动推送帧的组帧。
// 线程安全：否（链路任务独占）；非 ISR、不阻塞。堆：不分配。冻结接口（ADR-0007 §3.3）。实现方不得修改。
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
    /**
     * @brief 只保存三个引用，不注册路由、不分配。
     * @param alarm 报警状态来源（自身可变，0x42 会写入）。
     * @param link 会话状态机，只读用于判断手机在线。
     * @param info 设备信息，用于状态帧中的定位质量与电量。
     * @note 启动期装配；三个对象须活到本对象析构。非 ISR。
     */
    AlarmService(AlarmSource &alarm, const LinkManager &link, const DeviceInfo &info) noexcept;
    AlarmService(const AlarmService &) = delete;
    AlarmService &operator=(const AlarmService &) = delete;
    AlarmService(AlarmService &&) = delete;
    AlarmService &operator=(AlarmService &&) = delete;
    ~AlarmService() = default;

    /**
     * @brief 组 0x40 主动上报帧（type=PUSH，sender=DEV）。
     * @param seq 帧序号，由调用方递增（LinkService 每轮推送 +1）。
     * @param nowMs 当前时间（ms），写入 uptimeMs。
     * @param[out] reply 复用缓冲；成功时写入完整帧并设置 len，失败时 len 为 0。
     * @retval true 已组帧。
     * @retval false 状态编码或组帧失败，应答缓冲内容不可用。
     * @note 每次都重新读取报警状态与手机在线标志；不分配、不阻塞、非 ISR 安全（含 traceAlarm）。
     */
    [[nodiscard]] bool buildStatusPush(uint16_t seq, uint32_t nowMs, Reply &reply) const noexcept;

    /** @brief 本服务负责的 0x40–0x43 静态路由表。
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
    AlarmSource &alarm_;
    const LinkManager &link_;
    const DeviceInfo &info_;
};

} // namespace core::services
