// ConfigService 实现（ADR-0007 §3.3）：配置类指令 0x07 工作模式、0x14 电台、0x15 日志。
// 各分支自 command_dispatcher.cpp 逐字节搬迁，读写语义与旧 CommandDispatcher 一致。

#include "core/services/ConfigService.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"
#include "proto/Frame.hpp"

namespace core::services {

namespace {

// 通用结果码（phone_protocol.md §3；沿用旧 CommandDispatcher 的取值）。
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

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

/** 失败应答（0x02 0x02）。 */
inline DispatchResult replyFail(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultFail);
}

namespace msg_type = proto::msg_type;

// §4 命令 ID（本服务登记的 3 条）。
constexpr uint16_t kIdWorkMode = 0x0007;
constexpr uint16_t kIdRadio = 0x0014;
constexpr uint16_t kIdLog = 0x0015;

DispatchResult handleWorkMode(std::span<uint8_t> cfg, const proto::FrameView &frame,
                              Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.size() < kWorkModeSetLen) {
            return replyFail(reply, frame.msgId, frame.type);
        }
        for (size_t i = 0; i < kWorkModeSetLen; ++i) {
            cfg[i] = frame.payload[i];
        }
        return replyOk(reply, frame.msgId, frame.type);
    }
    return replyBuild(reply, frame.msgId, frame.type, cfg);
}

DispatchResult handleRadio(std::span<uint8_t> cfg, const proto::FrameView &frame,
                           Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.size() < kRadioSetMinLen) {
            return replyFail(reply, frame.msgId, frame.type);
        }
        const size_t count = (frame.payload.size() < kRadioLen) ? frame.payload.size() : kRadioLen;
        for (size_t i = 0; i < count; ++i) {
            cfg[i] = frame.payload[i];
        }
        // payload 未覆盖 [17] 时保持原值（§4）。
        return replyOk(reply, frame.msgId, frame.type);
    }
    return replyBuild(reply, frame.msgId, frame.type, cfg);
}

DispatchResult handleLog(std::span<uint8_t> cfg, const proto::FrameView &frame,
                         Reply &reply) noexcept
{
    if (frame.type == msg_type::kSet) {
        if (frame.payload.size() < kLogLen) {
            return replyFail(reply, frame.msgId, frame.type);
        }
        for (size_t i = 0; i < kLogLen; ++i) {
            cfg[i] = frame.payload[i];
        }
        return replyOk(reply, frame.msgId, frame.type);
    }
    return replyBuild(reply, frame.msgId, frame.type, cfg);
}

} // namespace

ConfigService::ConfigService() noexcept
{
    // 与旧 CommandDispatcher 构造函数相同：三块配置清零，电台模式字节取默认值。
    work_mode_.fill(0);
    radio_.fill(0);
    radio_[kRadioModeOffset] = kRadioModeDefault;
    log_.fill(0);
}

std::span<const uint8_t, kWorkModeLen> ConfigService::workMode() const noexcept
{
    return work_mode_;
}

std::span<const uint8_t, kRadioLen> ConfigService::radio() const noexcept
{
    return radio_;
}

std::span<const uint8_t, kLogLen> ConfigService::log() const noexcept
{
    return log_;
}

std::span<const Route> ConfigService::routes() noexcept
{
    // 冻结头只提供只读视图，SET 分支必须写回私有配置块。局部类定义在成员函数内，按 [class.access]
    // 与成员函数具有相同访问权限，因此 static_cast 之后可以拿到可变引用；
    // 每个 self 都来自 table()（RouteTable{this, ...}），类型必然是本服务实例。
    struct Handlers {
        static DispatchResult workMode(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                       Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            auto *const service = static_cast<ConfigService *>(self);
            return handleWorkMode(std::span<uint8_t>(service->work_mode_), frame, reply);
        }
        static DispatchResult radio(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                    Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            auto *const service = static_cast<ConfigService *>(self);
            return handleRadio(std::span<uint8_t>(service->radio_), frame, reply);
        }
        static DispatchResult log(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                  Reply &reply) noexcept
        {
            static_cast<void>(nowMs);
            auto *const service = static_cast<ConfigService *>(self);
            return handleLog(std::span<uint8_t>(service->log_), frame, reply);
        }
    };

    static constexpr std::array<Route, 3> kRoutes = {{
        {kIdWorkMode, kRolePhone, &Handlers::workMode},
        {kIdRadio, kRolePhone, &Handlers::radio},
        {kIdLog, kRolePhone, &Handlers::log},
    }};
    return kRoutes;
}

} // namespace core::services
