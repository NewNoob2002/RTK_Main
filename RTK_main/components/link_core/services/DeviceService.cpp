// DeviceService 实现（ADR-0007 §3.3）：设备信息类指令 0x01/0x02/0x03/0x04/0x05/0x06/0x0D。
// 各分支自 command_dispatcher.cpp 逐字节搬迁，应答内容与旧 CommandDispatcher 一致。

#include "core/services/DeviceService.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"
#include "proto/Bytes.hpp"
#include "proto/Frame.hpp"

namespace core::services {

namespace {

// 通用结果码（phone_protocol.md §3；沿用旧 CommandDispatcher 的取值）。
inline constexpr uint8_t kResultOk = 0x01;

/**
 * 组应答帧：sender 固定为 DEV，type 经 responseType 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 * 组帧失败时把 reply.len 置 0 并返回 kNoReply（与旧 dispatcher 一致）。
 */
inline DispatchResult replyBuild(Reply &reply, uint16_t msgId, uint8_t reqType,
                                 std::span<const uint8_t> payload) noexcept
{
    size_t len = 0;
    const fw::Status status = proto::buildFrame(reply.buf, msgId, proto::sender::kDevice,
                                                proto::responseType(reqType), payload, len);
    if (!isOk(status)) {
        reply.len = 0;
        return DispatchResult::kNoReply;
    }
    reply.len = len;
    return DispatchResult::kReply;
}

/** 结果码应答：§3 规定成功/失败都把同一字节写两遍。 */
inline DispatchResult replyResult(Reply &reply, uint16_t msgId, uint8_t reqType,
                                  uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return replyBuild(reply, msgId, reqType, payload);
}

/** 成功应答（0x01 0x01）。 */
inline DispatchResult replyOk(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultOk);
}

namespace msg_type = proto::msg_type;

// §4 命令 ID（本服务登记的 5 条；0x04/0x05 已移交 GnssService）。
constexpr uint16_t kIdDevInfo = 0x0001;
constexpr uint16_t kIdDevState = 0x0002;
constexpr uint16_t kIdBattery = 0x0003;
constexpr uint16_t kIdSatTrack = 0x0006;
constexpr uint16_t kIdCom = 0x000D;

// 0x0D COM 口布局（§4）。
constexpr size_t kComOfsBaud = 4;
constexpr size_t kComOfsFlagHigh = 27;
constexpr uint16_t kComParam218 = 218;

// 0x03 电池/存储布局（§4）。
constexpr size_t kBatteryOfsTemp = 12;

// 0x01 设备信息：fwVer 与 board 之间的保留区终点（§4）。
constexpr size_t kDevInfoBoardOfs = 56;

// 0x02 设备状态常量（§4）。
constexpr std::array<uint8_t, kDevStateLen> kDevStatePayload = {
    0x03, 0x01, 0x81, 0x83, 0x81, 0x00, 0x00, 0x00, 0x00, 0x81, 0x81, 0x81, 0x00, 0x01,
};

DispatchResult handleDevInfo(const DeviceInfo &info, const proto::FrameView &frame,
                             Reply &reply) noexcept
{
    std::array<uint8_t, kDevInfoLen> payload{};
    proto::ByteWriter writer(payload);
    writer.fixedStr(info.product.view(), info.product.capacity());
    writer.fixedStr(info.sn.view(), info.sn.capacity());
    writer.fixedStr(info.hwVer.view(), info.hwVer.capacity());
    writer.fixedStr(info.fwVer.view(), info.fwVer.capacity());
    // §4：fwVer 与 board 之间有 8 B 保留区。
    writer.zeros(kDevInfoBoardOfs - writer.size());
    writer.fixedStr(info.board.view(), info.board.capacity());
    writer.fixedStr(info.boardSn.view(), info.boardSn.capacity());
    writer.fixedStr(info.boardFw.view(), info.boardFw.capacity());
    writer.fixedStr(info.radioType.view(), info.radioType.capacity());
    writer.fixedStr(info.radioFw.view(), info.radioFw.capacity());
    // 字段总宽 120 B，剩余 16 B 保持 0（§4）。
    return replyBuild(reply, frame.msgId, frame.type, payload);
}

// 0x02 GNSS 板卡字节偏移与取值（survey_protocol_0530 功能信息 offset 2）。
constexpr size_t kDevStateGnssOfs = 2;
constexpr uint8_t kDevStateGnssRunning = 0x81;
constexpr uint8_t kDevStateGnssPresent = 0x01;

DispatchResult handleDevState(const DeviceInfo &info, const proto::FrameView &frame,
                              Reply &reply) noexcept
{
    auto payload = kDevStatePayload;
    payload[kDevStateGnssOfs] = info.gnssRunning ? kDevStateGnssRunning : kDevStateGnssPresent;
    return replyBuild(reply, frame.msgId, frame.type, payload);
}

DispatchResult handleBattery(const DeviceInfo &info, const proto::FrameView &frame,
                             Reply &reply) noexcept
{
    std::array<uint8_t, kBatteryLen> payload{};
    proto::ByteWriter writer(payload);
    writer.u8(info.onBattery ? 1U : 0U);
    writer.u8(0);
    writer.u8(info.batteryPct);
    writer.zeros(kBatteryOfsTemp - writer.size());
    // int8_t 的补码位模式按协议直接写入（-5 → 0xFB）。
    writer.u8(static_cast<uint8_t>(info.batteryTempC));
    writer.zeros(kBatteryLen - writer.size());
    return replyBuild(reply, frame.msgId, frame.type, payload);
}

DispatchResult handleSatTrack(const proto::FrameView &frame, Reply &reply) noexcept
{
    if (frame.type != msg_type::kQuery) {
        return replyOk(reply, frame.msgId, frame.type);
    }
    const std::array<uint8_t, kSatTrackLen> payload{};
    return replyBuild(reply, frame.msgId, frame.type, payload);
}

DispatchResult handleCom(const DeviceInfo &info, const proto::FrameView &frame,
                         Reply &reply) noexcept
{
    // 5.4 行为：0x0D 设置不回复（§4）。
    if (frame.type != msg_type::kQuery) {
        return DispatchResult::kNoReply;
    }

    std::array<uint8_t, kComLen> payload{};
    proto::ByteWriter writer(payload);
    writer.u8(kResultOk);
    writer.u8(kResultOk);
    writer.zeros(kComOfsBaud - writer.size());
    writer.u32(info.comBaud);
    writer.u8(kResultOk);
    writer.zeros(kComOfsFlagHigh - writer.size());
    writer.u8(kResultOk);
    writer.u16(kComParam218);
    writer.zeros(kComLen - writer.size());
    return replyBuild(reply, frame.msgId, frame.type, payload);
}

} // namespace

DeviceService::DeviceService() noexcept
{
    // 与旧 CommandDispatcher 构造函数相同的默认值（未列出的字段用 DeviceInfo 默认成员）。
    info_.product.assign("E1-Lite");
    info_.hwVer.assign("V1.1.0");
    info_.board.assign("UM980");
    info_.radioType.assign("5002D");
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
        static DispatchResult devInfo(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                      Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            return handleDevInfo(static_cast<DeviceService *>(self)->info(), frame, reply);
        }
        static DispatchResult devState(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                       Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            return handleDevState(static_cast<DeviceService *>(self)->info(), frame, reply);
        }
        static DispatchResult battery(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                      Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            return handleBattery(static_cast<DeviceService *>(self)->info(), frame, reply);
        }
        static DispatchResult satTrack(void *, const proto::FrameView &frame, uint32_t nowMs,
                                       Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            return handleSatTrack(frame, reply);
        }
        static DispatchResult com(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                  Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            return handleCom(static_cast<DeviceService *>(self)->info(), frame, reply);
        }
    };

    static constexpr std::array<Route, 5> kRoutes = {{
        {kIdDevInfo, kRolePhone, &Handlers::devInfo},
        {kIdDevState, kRolePhone, &Handlers::devState},
        {kIdBattery, kRolePhone, &Handlers::battery},
        {kIdSatTrack, kRolePhone, &Handlers::satTrack},
        {kIdCom, kRolePhone, &Handlers::com},
    }};
    return kRoutes;
}

} // namespace core::services
