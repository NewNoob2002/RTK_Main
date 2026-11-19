// 处理手机 Nav/Survey 兼容指令（0x06 Survey、0x08..0x13、0x1D..0x1E、0x24..0x29、0x32..0x33、0x60..0x62）。
// 仅链路任务串行调用，非 ISR，不阻塞，不分配堆。
#include "PhoneCompatService.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "proto/Bytes.hpp"

namespace core {

bool validAntennaHeight(std::span<const uint8_t, kAntennaHeightBytes> bytes) noexcept
{
    static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == kAntennaHeightBytes);
    proto::ByteReader reader{bytes};
    const uint64_t low = reader.u32();
    const uint64_t high = reader.u32();
    const auto height = std::bit_cast<double>((high << 32U) | low);
    constexpr double kMaxHeightMeters = 10.0;
    // 两个有界比较同时排除 NaN 和无穷大。
    return height >= 0.0 && height <= kMaxHeightMeters;
}

namespace services {
namespace {

constexpr uint16_t kSnPnId = 0x08U;
constexpr uint16_t kGsmInfoId = 0x09U;
constexpr uint16_t kGsvId = 0x0AU;
constexpr uint16_t kMountpointId = 0x11U;
constexpr uint16_t kNtripId = 0x12U;
constexpr uint16_t kReserved13Id = 0x13U;
constexpr uint16_t kOtaId = 0x1DU;
constexpr uint16_t kResetId = 0x1EU;
constexpr uint16_t kHeightId = 0x24U;
constexpr uint16_t kInsId = 0x25U;
constexpr uint16_t kAlignId = 0x26U;
constexpr uint16_t kHandNetId = 0x27U;
constexpr uint16_t kBaseInfoId = 0x29U;
constexpr uint16_t kJsonServerId = 0x32U;
constexpr uint16_t kFenceListId = 0x60U;
constexpr uint16_t kFenceDetailId = 0x61U;
constexpr uint16_t kFenceEditId = 0x62U;
// TODO(Q1): Configure app 的 GGA 质量门控配置消息等待 app 团队定义消息 ID 与负载格式后再接入。

constexpr size_t kSatOfsCutoff = 1U;
constexpr size_t kSatOfsGps = 4U;
constexpr size_t kSatOfsGlo = 5U;
constexpr size_t kSatOfsBds = 6U;
constexpr size_t kSatOfsGal = 7U;
constexpr size_t kSatOfsSbas = 8U;
constexpr size_t kSatMinSetLen = 9U;
constexpr size_t kSnPnReplyLen = 48U;
constexpr size_t kGsmReplyLen = 96U;
constexpr size_t kMountpointReplyLen = 4U;
constexpr size_t kNtripMinSetLen = 109U;
constexpr size_t kReserved13ReplyLen = 132U;
constexpr size_t kResetMinSetLen = 4U;
constexpr size_t kResetOfsFactory = 0U;
constexpr size_t kResetOfsReboot = 3U;
constexpr size_t kHeightReplyLen = 16U;
constexpr size_t kAlignReplyLen = 4U;
constexpr size_t kJsonServerMinSetLen = 38U;
constexpr size_t kPppReplyLen = 4U;
constexpr size_t kFenceListReplyLen = 1U;
constexpr size_t kRouteCount = 18U;

constexpr std::array<uint8_t, 2U> kOk{1U, 1U};
constexpr std::array<uint8_t, 2U> kFail{2U, 2U};
constexpr std::array<uint8_t, 2U> kZero2{};

/**
 * @brief 按手机协议生成应答帧。
 * @param msgId: 应答消息号。
 * @param type: 应答类型。
 * @param payload: 应答负载。
 * @param[out] reply: 应答缓冲。
 * @retval DispatchResult::kReply 组帧成功。
 * @retval DispatchResult::kNoReply 组帧失败。
 */
DispatchResult buildMsg(uint16_t msgId, uint8_t type, std::span<const uint8_t> payload,
                        Reply &reply) noexcept
{
    return fw::isOk(proto::buildFrame(reply.buf, msgId, proto::sender::kDevice, type, payload,
                                      reply.len))
               ? DispatchResult::kReply
               : DispatchResult::kNoReply;
}

/**
 * @brief 按原请求消息号生成应答帧。
 * @param frame: 原请求。
 * @param type: 应答类型。
 * @param payload: 应答负载。
 * @param[out] reply: 应答缓冲。
 * @retval 路由应答结果。
 */
DispatchResult build(const proto::FrameView &frame, uint8_t type, std::span<const uint8_t> payload,
                     Reply &reply) noexcept
{
    return buildMsg(frame.msgId, type, payload, reply);
}

/**
 * @brief 从路由 self 恢复 PhoneCompatService 引用。
 * @param self: 路由表绑定的服务指针。
 * @retval 服务引用。
 */
PhoneCompatService &serviceOf(void *self) noexcept
{
    // RouteTable 与钩子的 self 始终指向 PhoneCompatService 实例。
    return *static_cast<PhoneCompatService *>(self);
}

/**
 * @brief 轮询延迟的 PPP 应答并在完成时组帧。
 * @param ctx: PhoneCompatService 实例指针。
 * @param nowMs: 当前单调时钟（未用）。
 * @param[out] reply: 应答缓冲。
 * @retval true 已完成并组帧。
 * @retval false 仍在等待 kIo。
 */
bool pollPppDeferred(void *ctx, uint32_t nowMs, Reply &reply) noexcept
{
    static_cast<void>(nowMs);
    auto &service = serviceOf(ctx);
    if (service.ops.pppResult == nullptr) {
        service.pppWaiting = false;
        static_cast<void>(buildMsg(kPhonePppId, proto::msg_type::kSetRes, kFail, reply));
        return true;
    }
    const auto done = service.ops.pppResult(service.ops.ctx);
    if (!done.has_value()) {
        return false;
    }
    service.pppWaiting = false;
    static_cast<void>(buildMsg(kPhonePppId, proto::msg_type::kSetRes, *done ? kOk : kFail, reply));
    return true;
}

/**
 * @brief 取消挂起的 PPP 延迟应答状态。
 * @param ctx: PhoneCompatService 实例指针。
 */
void cancelPppDeferred(void *ctx) noexcept
{
    serviceOf(ctx).pppWaiting = false;
}

/**
 * @brief 轮询延迟的 0x06 卫星配置应答并在完成时组帧。
 * @param ctx: PhoneCompatService 实例指针。
 * @param nowMs: 当前单调时钟（未用）。
 * @param[out] reply: 应答缓冲。
 * @retval true 已完成并组帧。
 * @retval false 仍在等待 kIo。
 */
bool pollSatDeferred(void *ctx, uint32_t nowMs, Reply &reply) noexcept
{
    static_cast<void>(nowMs);
    auto &service = serviceOf(ctx);
    if (service.ops.satConfigResult == nullptr) {
        service.satWaiting = false;
        static_cast<void>(buildMsg(kPhoneSatTrackId, proto::msg_type::kSetRes, kFail, reply));
        return true;
    }
    const auto done = service.ops.satConfigResult(service.ops.ctx);
    if (!done.has_value()) {
        return false;
    }
    service.satWaiting = false;
    static_cast<void>(
        buildMsg(kPhoneSatTrackId, proto::msg_type::kSetRes, *done ? kOk : kFail, reply));
    return true;
}

/**
 * @brief 取消挂起的 0x06 延迟应答状态。
 * @param ctx: PhoneCompatService 实例指针。
 */
void cancelSatDeferred(void *ctx) noexcept
{
    serviceOf(ctx).satWaiting = false;
}

/**
 * @brief 0x08 Nav SN/PN 查询与设置（48 B 负载：[0..15]SN [16..31]PN）。
 * @param self: PhoneCompatService 实例。
 * @param frame: 请求帧。
 * @param nowMs: 去抖时间戳（ms）。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleSnPn(void *self, const proto::FrameView &frame, uint32_t nowMs,
                          Reply &reply) noexcept
{
    if (appKindFromSender(frame.sender) != AppKind::kNav) {
        return DispatchResult::kNoReply;
    }
    const auto &ops = serviceOf(self).ops;
    if (frame.type == proto::msg_type::kQuery) {
        std::array<uint8_t, kSnPnReplyLen> payload{};
        if (ops.readSnPn != nullptr) {
            const auto snPn = ops.readSnPn(ops.ctx);
            std::copy(snPn.begin(), snPn.end(), payload.begin());
        }
        return build(frame, proto::msg_type::kQueryRes, payload, reply);
    }
    if (frame.type != proto::msg_type::kSet) {
        return DispatchResult::kDenied;
    }
    if ((frame.payload.size() < kPhoneSnPnBytes) || (ops.setSnPn == nullptr)) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    PhoneSnPn snPn{};
    std::copy_n(frame.payload.begin(), snPn.size(), snPn.begin());
    const bool ok = ops.setSnPn(ops.ctx, snPn, nowMs);
    return build(frame, proto::msg_type::kSetRes, ok ? kOk : kFail, reply);
}

/**
 * @brief 0x09 4G 信息占位（query 96 B 全 0；set 回 01 01）。
 * @param frame: 请求帧。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleGsmInfo(void *, const proto::FrameView &frame, uint32_t, Reply &reply) noexcept
{
    if (frame.type == proto::msg_type::kQuery) {
        const std::array<uint8_t, kGsmReplyLen> payload{};
        return build(frame, proto::msg_type::kQueryRes, payload, reply);
    }
    if (frame.type == proto::msg_type::kSet) {
        return build(frame, proto::msg_type::kSetRes, kOk, reply);
    }
    return DispatchResult::kDenied;
}

/**
 * @brief 0x0A Nav GSV 查询（返回最近 GSV 原文，无缓存回 02 02）。
 * @param self: PhoneCompatService 实例。
 * @param frame: 请求帧。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleGsv(void *self, const proto::FrameView &frame, uint32_t, Reply &reply) noexcept
{
    if (appKindFromSender(frame.sender) != AppKind::kNav) {
        return DispatchResult::kNoReply;
    }
    if (frame.type != proto::msg_type::kQuery) {
        return DispatchResult::kDenied;
    }
    const auto &ops = serviceOf(self).ops;
    std::array<uint8_t, proto::kMaxPayload> buf{};
    const size_t len = (ops.readLatestGsv != nullptr) ? ops.readLatestGsv(ops.ctx, buf) : 0U;
    if ((len == 0U) || (len > buf.size())) {
        return build(frame, proto::msg_type::kQueryRes, kFail, reply);
    }
    return build(frame, proto::msg_type::kQueryRes, std::span<const uint8_t>{buf.data(), len},
                 reply);
}

/**
 * @brief 0x11 Nav 挂载点列表占位（4 B：[0]=1 [1]=0）。
 * @param frame: 请求帧。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleMountpoints(void *, const proto::FrameView &frame, uint32_t,
                                 Reply &reply) noexcept
{
    if (appKindFromSender(frame.sender) != AppKind::kNav) {
        return DispatchResult::kNoReply;
    }
    if (frame.type != proto::msg_type::kQuery) {
        return DispatchResult::kDenied;
    }
    constexpr std::array<uint8_t, kMountpointReplyLen> kEmptyList{1U, 0U, 0U, 0U};
    return build(frame, proto::msg_type::kQueryRes, kEmptyList, reply);
}

/**
 * @brief 0x12 NTRIP 配置（112 B RAM+NVS 存储占位，不连接 4G）。
 * @param self: PhoneCompatService 实例。
 * @param frame: 请求帧。
 * @param nowMs: 去抖时间戳（ms）。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleNtrip(void *self, const proto::FrameView &frame, uint32_t nowMs,
                           Reply &reply) noexcept
{
    const auto &ops = serviceOf(self).ops;
    if (frame.type == proto::msg_type::kQuery) {
        const PhoneNtripConfig cfg =
            (ops.readNtrip != nullptr) ? ops.readNtrip(ops.ctx) : PhoneNtripConfig{};
        return build(frame, proto::msg_type::kQueryRes, cfg, reply);
    }
    if (frame.type != proto::msg_type::kSet) {
        return DispatchResult::kDenied;
    }
    if ((frame.payload.size() < kNtripMinSetLen) || (ops.setNtrip == nullptr)) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    PhoneNtripConfig cfg{};
    const size_t copyLen = std::min(frame.payload.size(), cfg.size());
    std::copy_n(frame.payload.begin(), copyLen, cfg.begin());
    const bool ok = ops.setNtrip(ops.ctx, cfg, nowMs);
    return build(frame, proto::msg_type::kSetRes, ok ? kOk : kFail, reply);
}

/**
 * @brief 0x13 占位（query-res 132 B 全 0）。
 * @param frame: 请求帧。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleReserved13(void *, const proto::FrameView &frame, uint32_t,
                                Reply &reply) noexcept
{
    const std::array<uint8_t, kReserved13ReplyLen> payload{};
    return build(frame, proto::msg_type::kQueryRes, payload, reply);
}

/**
 * @brief 0x1D Nav OTA 占位（固定回 set-res 02 02）。
 * @param frame: 请求帧。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleOta(void *, const proto::FrameView &frame, uint32_t, Reply &reply) noexcept
{
    if (appKindFromSender(frame.sender) != AppKind::kNav) {
        return DispatchResult::kNoReply;
    }
    return build(frame, proto::msg_type::kSetRes, kFail, reply);
}

/**
 * @brief 0x1E Nav 恢复出厂与重启（先回 01 01，延迟 >=200 ms 执行）。
 * @param self: PhoneCompatService 实例。
 * @param frame: 请求帧。
 * @param nowMs: 当前单调时钟（ms）。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleReset(void *self, const proto::FrameView &frame, uint32_t nowMs,
                           Reply &reply) noexcept
{
    if (appKindFromSender(frame.sender) != AppKind::kNav) {
        return DispatchResult::kNoReply;
    }
    if (frame.type != proto::msg_type::kSet) {
        return DispatchResult::kDenied;
    }
    if (frame.payload.size() < kResetMinSetLen) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    const bool factoryReset = frame.payload[kResetOfsFactory] != 0U;
    const bool reboot = frame.payload[kResetOfsReboot] != 0U;
    if (!factoryReset && !reboot) {
        return build(frame, proto::msg_type::kSetRes, kOk, reply);
    }
    const auto &ops = serviceOf(self).ops;
    const bool scheduled =
        (ops.scheduleReset != nullptr) && ops.scheduleReset(ops.ctx, factoryReset, reboot, nowMs);
    return build(frame, proto::msg_type::kSetRes, scheduled ? kOk : kFail, reply);
}

/**
 * @brief 0x24 回读或登记新的天线高。
 * @param self: PhoneCompatService 实例。
 * @param frame: 手机请求。
 * @param nowMs: 去抖起点。
 * @param[out] reply: 应答缓冲。
 * @retval 路由应答结果。
 */
DispatchResult handleHeight(void *self, const proto::FrameView &frame, uint32_t nowMs,
                            Reply &reply) noexcept
{
    const auto &ops = serviceOf(self).ops;
    if (frame.type == proto::msg_type::kQuery) {
        std::array<uint8_t, kHeightReplyLen> payload{};
        if (ops.readHeight != nullptr) {
            const auto value = ops.readHeight(ops.ctx);
            std::copy(value.begin(), value.end(), payload.begin());
        }
        return build(frame, proto::msg_type::kQueryRes, payload, reply);
    }
    if (frame.type != proto::msg_type::kSet) {
        return DispatchResult::kDenied;
    }
    AntennaHeight value{};
    if ((frame.payload.size() < value.size()) || (ops.setHeight == nullptr)) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    std::copy_n(frame.payload.begin(), value.size(), value.begin());
    const bool ok = validAntennaHeight(value) && ops.setHeight(ops.ctx, value, nowMs);
    return build(frame, proto::msg_type::kSetRes, ok ? kOk : kFail, reply);
}

/**
 * @brief 0x25 惯导数据与 0x27 手簿网络占位（set-res 2 B 全 0）。
 * @param frame: 手机请求。
 * @param[out] reply: 应答缓冲。
 * @retval 路由应答结果。
 */
DispatchResult handleSetZero(void *, const proto::FrameView &frame, uint32_t, Reply &reply) noexcept
{
    return build(frame, proto::msg_type::kSetRes, kZero2, reply);
}

/**
 * @brief 0x26 对准开关占位（query 4 B 全 0；set 回 01 01）。
 * @param frame: 手机请求。
 * @param[out] reply: 应答缓冲。
 * @retval 路由应答结果。
 */
DispatchResult handleAlign(void *, const proto::FrameView &frame, uint32_t, Reply &reply) noexcept
{
    if (frame.type == proto::msg_type::kQuery) {
        const std::array<uint8_t, kAlignReplyLen> payload{};
        return build(frame, proto::msg_type::kQueryRes, payload, reply);
    }
    if (frame.type == proto::msg_type::kSet) {
        return build(frame, proto::msg_type::kSetRes, kOk, reply);
    }
    return DispatchResult::kDenied;
}

/**
 * @brief 0x29 BASEINFO 占位（set-res 01 01）。
 * @param frame: 手机请求。
 * @param[out] reply: 应答缓冲。
 * @retval 路由应答结果。
 */
DispatchResult handleBaseInfo(void *, const proto::FrameView &frame, uint32_t,
                              Reply &reply) noexcept
{
    return build(frame, proto::msg_type::kSetRes, kOk, reply);
}

/**
 * @brief 0x32 Nav JSON 服务器配置（42 B RAM+NVS 存储占位）。
 * @param self: PhoneCompatService 实例。
 * @param frame: 请求帧。
 * @param nowMs: 去抖时间戳（ms）。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleJsonServer(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                Reply &reply) noexcept
{
    if (appKindFromSender(frame.sender) != AppKind::kNav) {
        return DispatchResult::kNoReply;
    }
    const auto &ops = serviceOf(self).ops;
    if (frame.type == proto::msg_type::kQuery) {
        const PhoneJsonServerConfig cfg =
            (ops.readJsonServer != nullptr) ? ops.readJsonServer(ops.ctx) : PhoneJsonServerConfig{};
        return build(frame, proto::msg_type::kQueryRes, cfg, reply);
    }
    if (frame.type != proto::msg_type::kSet) {
        return DispatchResult::kDenied;
    }
    if ((frame.payload.size() < kJsonServerMinSetLen) || (ops.setJsonServer == nullptr)) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    PhoneJsonServerConfig cfg{};
    const size_t copyLen = std::min(frame.payload.size(), cfg.size());
    std::copy_n(frame.payload.begin(), copyLen, cfg.begin());
    const bool ok = ops.setJsonServer(ops.ctx, cfg, nowMs);
    return build(frame, proto::msg_type::kSetRes, ok ? kOk : kFail, reply);
}

/**
 * @brief 0x33 Nav 分支：Thingsboard 配置（86 B RAM+NVS 存储占位）。
 * @param ops: 回调表。
 * @param frame: 请求帧。
 * @param nowMs: 去抖时间戳（ms）。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleThingsboard(const PhoneCompatOps &ops, const proto::FrameView &frame,
                                 uint32_t nowMs, Reply &reply) noexcept
{
    if (frame.type == proto::msg_type::kQuery) {
        const PhoneThingsboardConfig cfg = (ops.readThingsboard != nullptr)
                                               ? ops.readThingsboard(ops.ctx)
                                               : PhoneThingsboardConfig{};
        return build(frame, proto::msg_type::kQueryRes, cfg, reply);
    }
    if (frame.type != proto::msg_type::kSet) {
        return DispatchResult::kDenied;
    }
    if ((frame.payload.size() < kPhoneThingsboardBytes) || (ops.setThingsboard == nullptr)) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    PhoneThingsboardConfig cfg{};
    std::copy_n(frame.payload.begin(), cfg.size(), cfg.begin());
    const bool ok = ops.setThingsboard(ops.ctx, cfg, nowMs);
    return build(frame, proto::msg_type::kSetRes, ok ? kOk : kFail, reply);
}

/**
 * @brief 0x33 Survey 分支：PPP 查询与延迟设置（不支持时直接回 02 02，不下发命令）。
 * @param service: PhoneCompatService 实例。
 * @param frame: 请求帧。
 * @param[out] reply: 应答缓冲或延迟应答钩子。
 * @retval 路由分发结果。
 */
DispatchResult handleSurveyPpp(PhoneCompatService &service, const proto::FrameView &frame,
                               Reply &reply) noexcept
{
    const auto &ops = service.ops;
    const bool supported = (ops.pppSupported == nullptr) || ops.pppSupported(ops.ctx);
    if (frame.type == proto::msg_type::kQuery) {
        const bool enabled = supported && (ops.readPpp != nullptr) && ops.readPpp(ops.ctx);
        const std::array<uint8_t, kPppReplyLen> payload{uint8_t{enabled}};
        return build(frame, proto::msg_type::kQueryRes, payload, reply);
    }
    if (frame.type != proto::msg_type::kSet) {
        return DispatchResult::kDenied;
    }
    if (!supported || frame.payload.empty() || (frame.payload.front() > 1U) || service.pppWaiting ||
        (ops.startPpp == nullptr) || !ops.startPpp(ops.ctx, frame.payload.front() != 0U)) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    service.pppWaiting = true;
    reply.deferred = DeferredHook{&service, &pollPppDeferred, &cancelPppDeferred};
    return DispatchResult::kDeferred;
}

/**
 * @brief 0x33 入口：Nav 走 Thingsboard，Survey 走 PPP。
 * @param self: PhoneCompatService 实例。
 * @param frame: 请求帧。
 * @param nowMs: 当前单调时钟（ms）。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handlePppOrThingsboard(void *self, const proto::FrameView &frame, uint32_t nowMs,
                                      Reply &reply) noexcept
{
    auto &service = serviceOf(self);
    if (appKindFromSender(frame.sender) == AppKind::kNav) {
        return handleThingsboard(service.ops, frame, nowMs, reply);
    }
    return handleSurveyPpp(service, frame, reply);
}

/**
 * @brief 0x60 Nav 电子围栏列表占位（query 1 B：[0]=0）。
 * @param frame: 请求帧。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleFenceList(void *, const proto::FrameView &frame, uint32_t,
                               Reply &reply) noexcept
{
    if (appKindFromSender(frame.sender) != AppKind::kNav) {
        return DispatchResult::kNoReply;
    }
    if (frame.type != proto::msg_type::kQuery) {
        return DispatchResult::kDenied;
    }
    const std::array<uint8_t, kFenceListReplyLen> payload{0U};
    return build(frame, proto::msg_type::kQueryRes, payload, reply);
}

/**
 * @brief 0x61/0x62 Nav 电子围栏详情与编辑占位（query/set 均回 02 02）。
 * @param frame: 请求帧。
 * @param[out] reply: 应答缓冲。
 * @retval 路由分发结果。
 */
DispatchResult handleFenceUnsupported(void *, const proto::FrameView &frame, uint32_t,
                                      Reply &reply) noexcept
{
    if (appKindFromSender(frame.sender) != AppKind::kNav) {
        return DispatchResult::kNoReply;
    }
    if (frame.type == proto::msg_type::kQuery) {
        return build(frame, proto::msg_type::kQueryRes, kFail, reply);
    }
    if (frame.type == proto::msg_type::kSet) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    return DispatchResult::kDenied;
}

} // namespace

DispatchResult PhoneCompatService::handleSurveySatTrack(void *self, const proto::FrameView &frame,
                                                        uint32_t, Reply &reply) noexcept
{
    auto &service = serviceOf(self);
    const auto &ops = service.ops;
    if (frame.type == proto::msg_type::kQuery) {
        const SatTrackConfig cfg = (ops.readSatConfig != nullptr) ? ops.readSatConfig(ops.ctx)
                                                                  : SatTrackConfig::defaults();
        std::array<uint8_t, kSatTrackLen> payload{};
        payload[kSatOfsCutoff] = cfg.cutoffDeg;
        payload[kSatOfsGps] = cfg.gps;
        payload[kSatOfsGlo] = cfg.glo;
        payload[kSatOfsBds] = cfg.bds;
        payload[kSatOfsGal] = cfg.gal;
        payload[kSatOfsSbas] = cfg.sbas;
        return build(frame, proto::msg_type::kQueryRes, payload, reply);
    }
    if (frame.type != proto::msg_type::kSet) {
        return DispatchResult::kDenied;
    }
    if (frame.payload.size() < kSatMinSetLen) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    const SatTrackConfig cfg{
        .cutoffDeg = frame.payload[kSatOfsCutoff],
        .gps = frame.payload[kSatOfsGps],
        .glo = frame.payload[kSatOfsGlo],
        .bds = frame.payload[kSatOfsBds],
        .gal = frame.payload[kSatOfsGal],
        .sbas = frame.payload[kSatOfsSbas],
    };
    if (!validSatTrackConfig(cfg) || service.satWaiting || (ops.startSatConfig == nullptr) ||
        !ops.startSatConfig(ops.ctx, cfg)) {
        return build(frame, proto::msg_type::kSetRes, kFail, reply);
    }
    service.satWaiting = true;
    reply.deferred = DeferredHook{&service, &pollSatDeferred, &cancelSatDeferred};
    return DispatchResult::kDeferred;
}

RouteTable PhoneCompatService::table() noexcept
{
    static constexpr std::array<Route, kRouteCount> kRoutes{{
        {kSnPnId, kRolePhone, &handleSnPn},
        {kGsmInfoId, kRolePhone, &handleGsmInfo},
        {kGsvId, kRolePhone, &handleGsv},
        {kMountpointId, kRolePhone, &handleMountpoints},
        {kNtripId, kRolePhone, &handleNtrip},
        {kReserved13Id, kRolePhone, &handleReserved13},
        {kOtaId, kRolePhone, &handleOta},
        {kResetId, kRolePhone, &handleReset},
        {kHeightId, kRolePhone, &handleHeight},
        {kInsId, kRolePhone, &handleSetZero},
        {kAlignId, kRolePhone, &handleAlign},
        {kHandNetId, kRolePhone, &handleSetZero},
        {kBaseInfoId, kRolePhone, &handleBaseInfo},
        {kJsonServerId, kRolePhone, &handleJsonServer},
        {kPhonePppId, kRolePhone, &handlePppOrThingsboard},
        {kFenceListId, kRolePhone, &handleFenceList},
        {kFenceDetailId, kRolePhone, &handleFenceUnsupported},
        {kFenceEditId, kRolePhone, &handleFenceUnsupported},
    }};
    return {this, kRoutes};
}

} // namespace services
} // namespace core
