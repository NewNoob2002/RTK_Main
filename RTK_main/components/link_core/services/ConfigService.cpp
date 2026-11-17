// ConfigService 实现（ADR-0007 §3.3）：配置类指令 0x07 工作模式、0x14 电台、0x15 日志。
// 线程安全：否，只由链路任务串行调用；非 ISR、不阻塞。
// 堆：不分配（配置是定长成员，应答复用调用方的 Reply 缓冲）。

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

// 通用结果码（phone_protocol.md §3：0x01 成功，0x02 失败）。
inline constexpr uint8_t kResultOk = 0x01;
inline constexpr uint8_t kResultFail = 0x02;

/**
 * @brief 组应答帧：sender 固定为 DEV，type 经 responseType 映射（0x00→0x01，0x02→0x03，其他→0x05）。
 * @param[out] reply 复用缓冲；成功时写入帧并设置 len，失败时 len 置 0。
 * @param msgId 请求的 msg_id，原样回填。
 * @param reqType 请求的 type，用于映射应答 type。
 * @param payload 应答负载。
 * @retval DispatchResult::kReply 已组帧。
 * @retval DispatchResult::kNoReply 组帧失败（缓冲不足等），reply.len 为 0。
 * @note 不分配、不加锁；非 ISR 可调（纯缓冲写入）。
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

/** @brief 结果码应答：§3 规定成功/失败都把同一字节写两遍。
 *  @param[out] reply 复用缓冲，语义同 replyBuild。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @param value 结果码（kResultOk/kResultFail）。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
inline DispatchResult replyResult(Reply &reply, uint16_t msgId, uint8_t reqType,
                                  uint8_t value) noexcept
{
    const std::array<uint8_t, 2> payload = {value, value};
    return replyBuild(reply, msgId, reqType, payload);
}

/** @brief 成功应答（结果码 0x01 0x01）的快捷封装。
 *  @param[out] reply 复用缓冲。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
inline DispatchResult replyOk(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultOk);
}

/** @brief 失败应答（结果码 0x02 0x02）的快捷封装。
 *  @param[out] reply 复用缓冲。
 *  @param msgId 请求的 msg_id。
 *  @param reqType 请求的 type。
 *  @retval DispatchResult::kReply 已组帧。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
inline DispatchResult replyFail(Reply &reply, uint16_t msgId, uint8_t reqType) noexcept
{
    return replyResult(reply, msgId, reqType, kResultFail);
}

namespace msg_type = proto::msg_type;

// §4 命令 ID（本服务登记的 3 条）。
constexpr uint16_t kIdWorkMode = 0x0007;
constexpr uint16_t kIdRadio = 0x0014;
constexpr uint16_t kIdLog = 0x0015;

/** @brief 0x07 工作模式：SET 覆盖前 kWorkModeSetLen 字节，其他 type 回读当前值。
 *  @param[out] cfg 服务持有的工作模式数组视图（长度必须 ≥ kWorkModeLen）。
 *  @param frame 请求帧；SET 时负载不足 kWorkModeSetLen 一律回失败。
 *  @param[out] reply 应答缓冲。
 *  @retval DispatchResult::kReply 已组成功应答或读取应答。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
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

/** @brief 0x14 电台：SET 覆盖负载覆盖到的字节，其他 type 回读当前值。
 *  @param[out] cfg 服务持有的电台配置数组视图（长度必须 ≥ kRadioLen）。
 *  @param frame 请求帧；SET 时负载不足 kRadioSetMinLen 回失败。
 *  @param[out] reply 应答缓冲。
 *  @retval DispatchResult::kReply 已组成功应答或读取应答。
 *  @retval DispatchResult::kNoReply 组帧失败。
 *  @note SET 只写 min(负载长度, kRadioLen) 字节，未覆盖的尾部（如 [17]）保持原值。 */
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

/** @brief 0x15 日志：SET 要求负载恰好覆盖 kLogLen 字节并整体覆盖，其他 type 回读当前值。
 *  @param[out] cfg 服务持有的日志配置数组视图（长度必须 ≥ kLogLen）。
 *  @param frame 请求帧；SET 时负载不足 kLogLen 回失败。
 *  @param[out] reply 应答缓冲。
 *  @retval DispatchResult::kReply 已组成功应答或读取应答。
 *  @retval DispatchResult::kNoReply 组帧失败。 */
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
    // 三块配置清零，电台模式字节取默认值。
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
