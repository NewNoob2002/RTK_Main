// DeviceService 实现（ADR-0007 §3.3）：设备信息类指令 0x01/0x02/0x03/0x04/0x05/0x06/0x0D。
// 各分支自 command_dispatcher.cpp 逐字节搬迁，应答内容与旧 CommandDispatcher 一致。
//
// Generated-by: pi + workbuddy/deepseek-v4.1-flash
// Owner: UNKNOWN（待人工审查签字）

#include "rtk/core/services/device_service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/core/command_router.hpp"
#include "rtk/core/reply.hpp"
#include "rtk/proto/bytes.hpp"
#include "rtk/proto/frame.hpp"
#include "service_support.hpp"

namespace rtk::core::services {

namespace {

namespace proto = rtk::proto;
namespace msg_type = rtk::proto::msg_type;

// §4 命令 ID（本服务登记的 7 条）。
constexpr uint16_t kIdDevInfo = 0x0001;
constexpr uint16_t kIdDevState = 0x0002;
constexpr uint16_t kIdBattery = 0x0003;
constexpr uint16_t kIdGga = 0x0004;
constexpr uint16_t kIdSat = 0x0005;
constexpr uint16_t kIdSatTrack = 0x0006;
constexpr uint16_t kIdCom = 0x000D;

// 0x0D COM 口布局（§4）。
constexpr size_t kComOfsBaud = 4;
constexpr size_t kComOfsFlagHigh = 27;
constexpr uint16_t kComParam218 = 218;

// 0x03 电池/存储布局（§4）。
constexpr size_t kBatteryOfsTemp = 12;

// 0x01 设备信息：fw_ver 与 board 之间的保留区终点（§4）。
constexpr size_t kDevInfoBoardOfs = 56;

// 0x02 设备状态常量（§4）。
constexpr std::array<uint8_t, kDevStateLen> kDevStatePayload = {
    0x03, 0x01, 0x81, 0x83, 0x81, 0x00, 0x00, 0x00, 0x00, 0x81, 0x81, 0x81, 0x00, 0x01,
};

DispatchResult handle_dev_info(const DeviceInfo &info, const proto::FrameView &frame, Reply &reply) noexcept
{
    std::array<uint8_t, kDevInfoLen> payload{};
    proto::ByteWriter writer(payload);
    writer.fixed_str(info.product.view(), info.product.capacity());
    writer.fixed_str(info.sn.view(), info.sn.capacity());
    writer.fixed_str(info.hw_ver.view(), info.hw_ver.capacity());
    writer.fixed_str(info.fw_ver.view(), info.fw_ver.capacity());
    // §4：fw_ver 与 board 之间有 8 B 保留区。
    writer.zeros(kDevInfoBoardOfs - writer.size());
    writer.fixed_str(info.board.view(), info.board.capacity());
    writer.fixed_str(info.board_sn.view(), info.board_sn.capacity());
    writer.fixed_str(info.board_fw.view(), info.board_fw.capacity());
    writer.fixed_str(info.radio_type.view(), info.radio_type.capacity());
    writer.fixed_str(info.radio_fw.view(), info.radio_fw.capacity());
    // 字段总宽 120 B，剩余 16 B 保持 0（§4）。
    return detail::reply_build(reply, frame.msg_id, frame.type, payload);
}

DispatchResult handle_dev_state(const proto::FrameView &frame, Reply &reply) noexcept
{
    return detail::reply_build(reply, frame.msg_id, frame.type, kDevStatePayload);
}

DispatchResult handle_battery(const DeviceInfo &info, const proto::FrameView &frame, Reply &reply) noexcept
{
    std::array<uint8_t, kBatteryLen> payload{};
    proto::ByteWriter writer(payload);
    writer.u8(info.on_battery ? 1U : 0U);
    writer.u8(0);
    writer.u8(info.battery_pct);
    writer.zeros(kBatteryOfsTemp - writer.size());
    // int8_t 的补码位模式按协议直接写入（-5 → 0xFB）。
    writer.u8(static_cast<uint8_t>(info.battery_temp_c));
    writer.zeros(kBatteryLen - writer.size());
    return detail::reply_build(reply, frame.msg_id, frame.type, payload);
}

/** 0x04 GGA 与 0x05 卫星：本阶段占位，直接回成功。 */
DispatchResult handle_ok(const proto::FrameView &frame, Reply &reply) noexcept
{
    return detail::reply_ok(reply, frame.msg_id, frame.type);
}

DispatchResult handle_sat_track(const proto::FrameView &frame, Reply &reply) noexcept
{
    if (frame.type != msg_type::kQuery) {
        return detail::reply_ok(reply, frame.msg_id, frame.type);
    }
    const std::array<uint8_t, kSatTrackLen> payload{};
    return detail::reply_build(reply, frame.msg_id, frame.type, payload);
}

DispatchResult handle_com(const DeviceInfo &info, const proto::FrameView &frame, Reply &reply) noexcept
{
    // 5.4 行为：0x0D 设置不回复（§4）。
    if (frame.type != msg_type::kQuery) {
        return DispatchResult::kNoReply;
    }

    std::array<uint8_t, kComLen> payload{};
    proto::ByteWriter writer(payload);
    writer.u8(detail::kResultOk);
    writer.u8(detail::kResultOk);
    writer.zeros(kComOfsBaud - writer.size());
    writer.u32(info.com_baud);
    writer.u8(detail::kResultOk);
    writer.zeros(kComOfsFlagHigh - writer.size());
    writer.u8(detail::kResultOk);
    writer.u16(kComParam218);
    writer.zeros(kComLen - writer.size());
    return detail::reply_build(reply, frame.msg_id, frame.type, payload);
}

} // namespace

DeviceService::DeviceService() noexcept
{
    // 与旧 CommandDispatcher 构造函数相同的默认值（未列出的字段用 DeviceInfo 默认成员）。
    info_.product.assign("E1-Lite");
    info_.hw_ver.assign("V1.1.0");
    info_.board.assign("UM980");
    info_.radio_type.assign("5002D");
}

DeviceInfo &DeviceService::info() noexcept
{
    return info_;
}

const DeviceInfo &DeviceService::info() const noexcept
{
    return info_;
}

std::span<const Route> DeviceService::routes() noexcept
{
    // 冻结头没有暴露 info_ 的转发入口，而 handler 只拿到 void* self。局部类定义在成员函数内，
    // 按 [class.access] 与成员函数具有相同访问权限，因此这里的 static_cast 之后可以取到 info_；
    // 每个 self 都来自 table()（RouteTable{this, ...}），类型必然是本服务实例。
    struct Handlers {
        static DispatchResult dev_info(void *self, const proto::FrameView &frame, uint32_t now_ms,
                                       Reply &reply) noexcept
        {
            static_cast<void>(now_ms);
            return handle_dev_info(static_cast<DeviceService *>(self)->info(), frame, reply);
        }
        static DispatchResult dev_state(void *, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
        {
            static_cast<void>(now_ms);
            return handle_dev_state(frame, reply);
        }
        static DispatchResult battery(void *self, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
        {
            static_cast<void>(now_ms);
            return handle_battery(static_cast<DeviceService *>(self)->info(), frame, reply);
        }
        static DispatchResult placeholder(void *, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
        {
            static_cast<void>(now_ms);
            return handle_ok(frame, reply);
        }
        static DispatchResult sat_track(void *, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
        {
            static_cast<void>(now_ms);
            return handle_sat_track(frame, reply);
        }
        static DispatchResult com(void *self, const proto::FrameView &frame, uint32_t now_ms, Reply &reply) noexcept
        {
            static_cast<void>(now_ms);
            return handle_com(static_cast<DeviceService *>(self)->info(), frame, reply);
        }
    };

    static constexpr std::array<Route, 7> kRoutes = {{
        {kIdDevInfo, kRolePhone, &Handlers::dev_info},
        {kIdDevState, kRolePhone, &Handlers::dev_state},
        {kIdBattery, kRolePhone, &Handlers::battery},
        {kIdGga, kRolePhone, &Handlers::placeholder},
        {kIdSat, kRolePhone, &Handlers::placeholder},
        {kIdSatTrack, kRolePhone, &Handlers::sat_track},
        {kIdCom, kRolePhone, &Handlers::com},
    }};
    return kRoutes;
}

} // namespace rtk::core::services
