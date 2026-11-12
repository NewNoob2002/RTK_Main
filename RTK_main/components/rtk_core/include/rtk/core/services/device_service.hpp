// 冻结接口（ADR-0007 §3.3）。实现方不得修改。
// 设备信息类指令（PHONE）：0x01 设备信息、0x02 设备状态、0x03 电池/存储、0x04 GGA、0x05 卫星、0x06 卫星跟踪、0x0D COM 口。
// 行为与 CommandDispatcher 对应分支逐字节一致。线程安全：否（链路任务独占；info() 的写入也只在链路任务或启动前）。
//
// Generated-by: Claude Code + claude-opus-5-5
// Owner: UNKNOWN（待人工审查签字）
#pragma once

#include <span>

#include "rtk/core/command_router.hpp"
#include "rtk/core/reply.hpp"

namespace rtk::core::services {

class DeviceService {
public:
    /** 默认值与旧 dispatcher 相同：product="E1-Lite"，hw_ver="V1.1.0"，board="UM980"，radio_type="5002D"。 */
    DeviceService() noexcept;
    DeviceService(const DeviceService &) = delete;
    DeviceService &operator=(const DeviceService &) = delete;
    DeviceService(DeviceService &&) = delete;
    DeviceService &operator=(DeviceService &&) = delete;
    ~DeviceService() = default;

    [[nodiscard]] DeviceInfo &info() noexcept;
    [[nodiscard]] const DeviceInfo &info() const noexcept;

    [[nodiscard]] static std::span<const Route> routes() noexcept;
    [[nodiscard]] RouteTable table() noexcept
    {
        return RouteTable{this, routes()};
    }

private:
    DeviceInfo info_{};
};

} // namespace rtk::core::services
