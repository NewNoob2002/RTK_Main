// 设备信息类指令（PHONE）：0x01 设备信息、0x02 设备状态、0x03 电池/存储、0x06 卫星跟踪、0x0D COM 口。
// 线程安全：否（链路任务独占，info() 的写入也只在链路任务或启动前）；非 ISR、不阻塞。
// 堆：不分配（DeviceInfo 为成员，应答缓冲复用调用方）。冻结接口（ADR-0007 §3.3）。实现方不得修改。
#pragma once

#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"

namespace core::services {

/** 设备信息类指令服务（0x01–0x06/0x0D）：持有 DeviceInfo 副本；仅链路任务调用。 */
class DeviceService {
public:
    /** @brief 填入产品/硬件/板卡/电台型号默认值（product="E1-Lite"，hwVer="V1.1.0"，board="UM980"，
     *         radioType="5002D"），其余字段用 DeviceInfo 默认成员。
     *  @note 启动期调用；不分配（只写定长缓冲），非 ISR 安全。 */
    DeviceService() noexcept;
    DeviceService(const DeviceService &) = delete;
    DeviceService &operator=(const DeviceService &) = delete;
    DeviceService(DeviceService &&) = delete;
    DeviceService &operator=(DeviceService &&) = delete;
    ~DeviceService() = default;

    /** @brief 可写视图，供 GNSS/电池等采集方更新字段。
     *  @return 内部 DeviceInfo 的引用；本对象存活期间有效，调用方须自行保证互斥。 */
    [[nodiscard]] DeviceInfo &info() noexcept;
    /** @brief 只读视图，供组帧方使用。
     *  @return 内部 DeviceInfo 的常量引用；本对象存活期间有效。 */
    [[nodiscard]] const DeviceInfo &info() const noexcept;

    /** @brief 本服务负责的 0x01/0x02/0x03/0x06/0x0D 静态路由表。
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
    DeviceInfo info_{};
};

} // namespace core::services
