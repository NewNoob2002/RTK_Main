// 链路单一所有者；RX 调度保持 512 B 默认读缓冲、字节/20 ms 预算、每轮一个产品帧，RTCM 直推管道。
// 启动期由组合根初始化一次；poll 与命令只在 kLink 执行器任务；蓝牙 API 可阻塞，非 ISR 安全。
#include "modules/bt_link/BtLinkModule.hpp"
#include "modules/bt_link/BtLinkModulePrivate.hpp"
#include "AppContext.hpp"

#include <algorithm>
#include <cstdlib>
#include <cinttypes>
#include <cstring>
#include "fw/Atomic.hpp"
#include "fw/Log.hpp"
#include "fw/Tlog.hpp"
#include "core/LinkService.hpp"
#include "esp_system.h"
#if RTK_GNSS_ENABLED
#include "def/GnssSubDef.hpp"
#include "PppSequence.hpp"
#endif
#include "proto/Frame.hpp"
#include "rtos/EspStatus.hpp"

// 回复信用按整帧预留；协议帧长一旦超过预留量，commit 会拒绝并永久占住该会话的预留，
// 使会话再也不能恢复。在此处绑定两者，避免只改一侧。
static_assert(proto::kMaxFrame <= SPP_SERVER_REPLY_RESERVE,
              "reply reserve must cover a full protocol frame");
static_assert(fw::kAtomicUsable<TaskHandle_t>, "wake_task_ must be lock-free");

namespace app {
namespace {

constexpr uint32_t kPhoneResetDelayMs = 250U; // >= 200 ms，确保 0x1E 应答发出后再执行复位
constexpr uint32_t kPhoneResetPollMs = 50U;

/** @brief 读取手机天线高（0x24）。 @param ctx: AppContext 指针。 @retval 8 B 小端字节。 */
core::AntennaHeight phoneReadHeight(const void *ctx) noexcept
{
    return static_cast<const AppContext *>(ctx)->storage.phoneHeight();
}

/** @brief 登记手机天线高（0x24）。 @param ctx: AppContext 指针。 @param height: 8 B 小端字节。 @param nowMs: 去抖起点。 @retval true 已登记。 */
bool phoneSetHeight(void *ctx, const core::AntennaHeight &height, uint32_t nowMs) noexcept
{
    return static_cast<AppContext *>(ctx)->storage.setPhoneHeight(height, nowMs);
}

/** @brief 查询当前接收机是否支持 PPP（0x33 Survey）。 @param ctx: AppContext 指针。 @retval true 支持。 */
bool phonePppSupported(const void *ctx) noexcept
{
#if RTK_GNSS_ENABLED
    const auto status = static_cast<const AppContext *>(ctx)->data.gnssStatus.read();
    return status.enabled && ::gnss::supportsPpp(status.model);
#else
    static_cast<void>(ctx);
    return false;
#endif
}

/** @brief 读取当前 PPP 开关（不支持时恒返回 false）。 @param ctx: AppContext 指针。 @retval PPP 状态。 */
bool phoneReadPpp(const void *ctx) noexcept
{
    if (!phonePppSupported(ctx)) {
        return false;
    }
    return static_cast<const AppContext *>(ctx)->data.phonePppEnabled.read().value_or(false);
}

#if RTK_GNSS_ENABLED
/** @brief 投递 PPP 开关命令到 kIo（不支持时直接返回 false，不发命令）。 @param ctx: AppContext 指针。 @param enabled: 开关值。 @retval true 已投递。 */
bool phoneStartPpp(void *ctx, bool enabled) noexcept
{
    if (!phonePppSupported(ctx)) {
        return false;
    }
    auto &data = static_cast<AppContext *>(ctx)->data;
    const auto previous = data.phonePppCommand.read().seq;
    if (previous != data.phonePppResult.read().seq) {
        return false;
    }
    fw::Command command{};
    command.target = id(ModuleKind::kGnss);
    command.verb = static_cast<uint8_t>(GnssSubVerb::kPhonePpp);
    command.seq = previous == UINT32_MAX ? 1U : previous + 1U;
    command.argLen = 1U;
    command.args[0] = enabled ? 1U : 0U;
    data.phonePppCommand.write(command);
    return true;
}

/** @brief 轮询 kIo 的 PPP 执行结果。 @param ctx: AppContext 指针。 @retval 完成时返回成败，否则为空。 */
std::optional<bool> phonePppResult(const void *ctx) noexcept
{
    const auto &data = static_cast<const AppContext *>(ctx)->data;
    const auto result = data.phonePppResult.read();
    if (result.seq != data.phonePppCommand.read().seq) {
        return std::nullopt;
    }
    return fw::isOk(result.status);
}

/** @brief 投递 0x06 截止角/星座配置到 kIo。 @param ctx: AppContext 指针。 @param cfg: 合法配置。 @retval true 已投递。 */
bool phoneStartSatConfig(void *ctx, const core::SatTrackConfig &cfg) noexcept
{
    auto &data = static_cast<AppContext *>(ctx)->data;
    const auto status = data.gnssStatus.read();
    const auto previous = data.phoneSatCommand.read().seq;
    if ((previous != data.phoneSatResult.read().seq) || !status.enabled ||
        (status.family != ::gnss::Family::kUnicore)) {
        return false;
    }
    fw::Command command{};
    command.target = id(ModuleKind::kGnss);
    command.verb = static_cast<uint8_t>(GnssSubVerb::kPhoneSatCfg);
    command.seq = previous == UINT32_MAX ? 1U : previous + 1U;
    const auto encoded = core::encodeSatTrackConfig(cfg);
    // encoded.size() == 6 <= fw::kCommandArgBytes(32)，缩窄安全。
    command.argLen = static_cast<uint8_t>(encoded.size());
    std::copy(encoded.begin(), encoded.end(), command.args.begin());
    data.phoneSatCommand.write(command);
    return true;
}

/** @brief 轮询 kIo 的 0x06 卫星配置执行结果。 @param ctx: AppContext 指针。 @retval 完成时返回成败，否则为空。 */
std::optional<bool> phoneSatConfigResult(const void *ctx) noexcept
{
    const auto &data = static_cast<const AppContext *>(ctx)->data;
    const auto result = data.phoneSatResult.read();
    if (result.seq != data.phoneSatCommand.read().seq) {
        return std::nullopt;
    }
    return fw::isOk(result.status);
}

/** @brief 读取最近单句 GSV 原文（0x0A Nav）。 @param ctx: AppContext 指针。 @param[out] dst: 输出缓冲。 @retval 有效字节数。 */
size_t phoneReadLatestGsv(const void *ctx, std::span<uint8_t> dst) noexcept
{
    const auto gsv = static_cast<const AppContext *>(ctx)->data.gnssLatestGsv.read();
    if ((gsv.len == 0U) || (gsv.len > dst.size()) || (gsv.len > gsv.bytes.size())) {
        return 0U;
    }
    std::copy_n(gsv.bytes.begin(), gsv.len, dst.begin());
    return gsv.len;
}
#endif

/** @brief 读取已保存或默认的 0x06 卫星配置。 @param ctx: AppContext 指针。 @retval 卫星配置。 */
core::SatTrackConfig phoneReadSatConfig(const void *ctx) noexcept
{
    return static_cast<const AppContext *>(ctx)->data.phoneSatConfig.read().value_or(
        core::SatTrackConfig::defaults());
}

/** @brief 读取 0x08 Nav SN/PN（SN 未写 NVS 时回退当前派生 SN）。 @param ctx: AppContext 指针。 @retval 32 B 组合字段。 */
core::PhoneSnPn phoneReadSnPn(const void *ctx) noexcept
{
    const auto &context = *static_cast<const AppContext *>(ctx);
    auto out = context.storage.phoneSnPn();
    if (out[0U] == 0U) {
        const auto sn = context.bt_link.deviceSn();
        const size_t copyLen = std::min(sn.size(), core::kPhoneSnBytes);
        std::memcpy(out.data(), sn.data(), copyLen);
    }
    return out;
}

/** @brief 保存 0x08 Nav SN/PN 并同步内存 DeviceInfo.sn。 @param ctx: AppContext 指针。 @param snPn: 32 B 组合字段。 @param nowMs: 去抖起点。 @retval true 已保存。 */
bool phoneSetSnPn(void *ctx, const core::PhoneSnPn &snPn, uint32_t nowMs) noexcept
{
    auto &context = *static_cast<AppContext *>(ctx);
    if (!context.storage.setPhoneSnPn(snPn, nowMs)) {
        return false;
    }
    size_t snLen = 0U;
    while ((snLen < core::kPhoneSnBytes) && (snPn[snLen] != 0U)) {
        ++snLen;
    }
    // 仅在 kLink 上更新，按字节视图构造有效 SN 字符串。
    std::array<char, core::kPhoneSnBytes> snChars{};
    for (size_t i = 0U; i < snLen; ++i) {
        snChars[i] = static_cast<char>(snPn[i]);
    }
    context.bt_link.updateDeviceSn(std::string_view{snChars.data(), snLen});
    return true;
}

/** @brief 读取 0x12 NTRIP 配置。 @param ctx: AppContext 指针。 @retval 112 B 配置块。 */
core::PhoneNtripConfig phoneReadNtrip(const void *ctx) noexcept
{
    return static_cast<const AppContext *>(ctx)->storage.phoneNtrip();
}

/** @brief 保存 0x12 NTRIP 配置。 @param ctx: AppContext 指针。 @param cfg: 112 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 */
bool phoneSetNtrip(void *ctx, const core::PhoneNtripConfig &cfg, uint32_t nowMs) noexcept
{
    return static_cast<AppContext *>(ctx)->storage.setPhoneNtrip(cfg, nowMs);
}

/** @brief 读取 0x32 JSON 服务器配置。 @param ctx: AppContext 指针。 @retval 42 B 配置块。 */
core::PhoneJsonServerConfig phoneReadJsonServer(const void *ctx) noexcept
{
    return static_cast<const AppContext *>(ctx)->storage.phoneJsonServer();
}

/** @brief 保存 0x32 JSON 服务器配置。 @param ctx: AppContext 指针。 @param cfg: 42 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 */
bool phoneSetJsonServer(void *ctx, const core::PhoneJsonServerConfig &cfg, uint32_t nowMs) noexcept
{
    return static_cast<AppContext *>(ctx)->storage.setPhoneJsonServer(cfg, nowMs);
}

/** @brief 读取 0x33 Nav Thingsboard 配置。 @param ctx: AppContext 指针。 @retval 86 B 配置块。 */
core::PhoneThingsboardConfig phoneReadThingsboard(const void *ctx) noexcept
{
    return static_cast<const AppContext *>(ctx)->storage.phoneThingsboard();
}

/** @brief 保存 0x33 Nav Thingsboard 配置。 @param ctx: AppContext 指针。 @param cfg: 86 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 */
bool phoneSetThingsboard(void *ctx, const core::PhoneThingsboardConfig &cfg,
                         uint32_t nowMs) noexcept
{
    return static_cast<AppContext *>(ctx)->storage.setPhoneThingsboard(cfg, nowMs);
}

/** @brief 登记 0x1E 延迟恢复出厂或重启。 @param ctx: AppContext 指针。 @param factoryReset: 是否擦除用户 NVS。 @param reboot: 是否重启。 @param nowMs: 当前时钟。 @retval true 已受理。 */
bool phoneScheduleReset(void *ctx, bool factoryReset, bool reboot, uint32_t nowMs) noexcept
{
    return static_cast<AppContext *>(ctx)->bt_link.schedulePhoneReset(factoryReset, reboot, nowMs);
}

/** @brief 绑定手机设置到应用存储、GNSS 与复位逻辑。 @param context: 同寿命上下文。 @retval 手机服务回调。 */
core::services::PhoneCompatOps phoneCompatOps(AppContext &context) noexcept
{
    return {
        .ctx = &context,
        .readHeight = &phoneReadHeight,
        .setHeight = &phoneSetHeight,
        .pppSupported = &phonePppSupported,
        .readPpp = &phoneReadPpp,
#if RTK_GNSS_ENABLED
        .startPpp = &phoneStartPpp,
        .pppResult = &phonePppResult,
#endif
        .readSatConfig = &phoneReadSatConfig,
#if RTK_GNSS_ENABLED
        .startSatConfig = &phoneStartSatConfig,
        .satConfigResult = &phoneSatConfigResult,
#endif
        .readSnPn = &phoneReadSnPn,
        .setSnPn = &phoneSetSnPn,
#if RTK_GNSS_ENABLED
        .readLatestGsv = &phoneReadLatestGsv,
#endif
        .readNtrip = &phoneReadNtrip,
        .setNtrip = &phoneSetNtrip,
        .readJsonServer = &phoneReadJsonServer,
        .setJsonServer = &phoneSetJsonServer,
        .readThingsboard = &phoneReadThingsboard,
        .setThingsboard = &phoneSetThingsboard,
        .scheduleReset = &phoneScheduleReset,
    };
}

} // namespace

std::string_view BtLinkModule::deviceSn() const noexcept
{
    return device_service_.has_value() ? device_service_->info().sn.view() : std::string_view{};
}

void BtLinkModule::updateDeviceSn(std::string_view sn) noexcept
{
    if (device_service_.has_value()) {
        device_service_->info().sn.assign(sn);
    }
}

bool BtLinkModule::schedulePhoneReset(bool factoryReset, bool reboot, uint32_t nowMs) noexcept
{
    if (pending_reset_ != PendingReset::kNone) {
        return false;
    }
    if (factoryReset) {
        pending_reset_ = PendingReset::kFactory;
    } else if (reboot) {
        pending_reset_ = PendingReset::kReboot;
    } else {
        return true;
    }
    pending_reset_at_ms_ = nowMs + kPhoneResetDelayMs;
    return true;
}

void BtLinkModule::pollPhoneReset(uint32_t nowMs) noexcept
{
    if (pending_reset_ == PendingReset::kNone) {
        return;
    }
    // 有符号差值判断到期，容忍 32 位毫秒回绕。
    if (static_cast<int32_t>(nowMs - pending_reset_at_ms_) < 0) {
        return;
    }
    const PendingReset action = pending_reset_;
    pending_reset_ = PendingReset::kNone;
    if (action == PendingReset::kFactory) {
        fw::Command cmd{};
        cmd.target = id(ModuleKind::kStorage);
        cmd.verb = StorageModule::kEraseUser;
        fw::CommandResult result{};
        ctx().storage.onCommand(cmd, result);
        if (!fw::isOk(result.status)) {
            FW_LOGW(fw::TraceCat::kStore, "Phone factory reset failed (%s)",
                    fw::toString(result.status));
            return;
        }
        FW_LOGI(fw::TraceCat::kStore, "Factory reset done, restarting");
    } else {
        FW_LOGI(fw::TraceCat::kSys, "Phone reboot requested, restarting");
    }
    esp_restart();
}

void BtLinkModule::publishLinkStatus(uint32_t nowMs) noexcept
{

    /* 按基线顺序发布链路、会话与报警状态。 */
    LinkStatus link{};
    link.policy = policyFromConfig();
    link.pairing = pairStatus(nowMs).remainingMs != 0U;
    link.snapshotMs = nowMs;
    const auto list = link_manager_.whitelist();
    link.whitelistCount = std::min(list.size(), kMaxAlarms);
    std::copy_n(list.begin(), link.whitelistCount, link.whitelist.begin());
    ctx().data.linkStatus.write(link);
}

void BtLinkModule::publishSessions() noexcept
{
    Sessions sessions{};
    for (const auto &entry : sessions_) {
        const auto role = link_manager_.role(entry.handle);
        if (entry.used && role != core::Role::kNone && sessions.count < kMaxSessions) {
            sessions.entries[sessions.count++] = {true, entry.handle, role, entry.addr};
        }
    }
    ctx().data.sessions.write(sessions);
}

void BtLinkModule::publishAlarmState() noexcept
{
    if (alarm_.has_value()) {
        const AlarmState snapshot{alarm_->state(), alarm_->level(), alarm_->isOverride()};
        ctx().data.alarmState.write(snapshot);
    } else {
        ctx().data.alarmState.write({});
    }
}

void BtLinkModule::publishStats() noexcept
{
    /* 继续发布发送、心跳与分流统计，顺序不变。 */
    LinkStats stats{};
    static_cast<core::LinkStats &>(stats) = linkService().stats(); // 派生结构的基类赋值，无窄化。
    stats.parserCrcErrors = linkService().parserCrcErrors();
    stats.parserLengthErrors = linkService().parserLengthErrors();
    spp_server_tx_stats_t tx{};
    spp_server_tx_stats(&tx);
    stats.txAccepted = tx.accepted;
    stats.txCompleted = tx.completed;
    stats.txCancelled = tx.cancelled;
    stats.txErrors = tx.errors;
    stats.txCong = tx.cong;
    stats.txHighWater = tx.high_water;
    stats.txPaused = tx.paused;
    stats.txAnonWriteFail = tx.anon_write_fail;
    stats.txStrayWrite = tx.stray_write;
    stats.txInflight = tx.inflight;
    spp_server_rx_stats_t rx{};
    spp_server_rx_stats(&rx);
    stats.evtDropped = rx.evt_dropped;
    stats.rxWakeCalls = rx.wake_calls;
    for (size_t idx = 0U; idx < kMaxSessions; ++idx) {
        stats.rxUsed[idx] = rx.sessions[idx].used;
        stats.rxHighWater[idx] = rx.sessions[idx].high_water;
        stats.rxDroppedBytes[idx] = rx.sessions[idx].dropped_bytes;
        stats.rxEvents[idx] = rx.sessions[idx].rx_events;
        stats.rxDropEvents[idx] = rx.sessions[idx].drop_events;
    }
    ctx().data.linkStats.write(stats);
    const auto heartbeat = linkService().lastHeartbeat();
    ctx().data.heartbeat.write(
        {heartbeat.has_value(), heartbeat.value_or(proto::alarm::Heartbeat{})});
}

void BtLinkModule::publishDemuxStats() noexcept
{
    DemuxStats demux = retired_demux_;
    for (auto &entry : sessions_) {
        if (!entry.used || !entry.demux.has_value()) {
            continue;
        }
        const auto &counts = entry.demux->stats();
        demux.unknownBytes += counts.unknownBytes;
        demux.productRejects += counts.decoder[kProductIndex].rejects;
        demux.rtcmRejects += counts.decoder[kRtcmIndex].rejects;
        if (counts.resyncs > entry.seenResyncs) {
            traceRtcm(RtcmTrace::kResync, entry.handle, counts.resyncs - entry.seenResyncs);
            entry.seenResyncs = counts.resyncs;
        }
        if (counts.unknownBytes > entry.seenUnknown) {
            traceRtcm(RtcmTrace::kUnknown, entry.handle, counts.unknownBytes - entry.seenUnknown);
            entry.seenUnknown = counts.unknownBytes;
        }
    }
    ctx().data.demuxStats.write(demux);
}

void BtLinkModule::publishLink(uint32_t nowMs) noexcept
{
    /* 按基线顺序发布链路、会话与报警状态。 */
    publishLinkStatus(nowMs);
    publishSessions();
    publishAlarmState();
    /* 继续发布发送、心跳与分流统计，顺序不变。 */
    publishStats();
    publishDemuxStats();
}

#if RTK_GNSS_ENABLED

#endif

core::LinkService &BtLinkModule::linkService() noexcept
{
    // 执行器只在 init 成功后调用业务回调；SessionSink 同样仅在该生命周期内分发。
    if (!service_.has_value()) {
        std::abort();
    }
    return *service_;
}

/**
 * @brief  原地构造手机与报警器链路服务并安装接收唤醒钩子
 * @retval fw::Result::kOk 成功；其他为失败原因（已记 E 级日志）
 * @note   启动前单线程，非 ISR，不分配堆。
 */
fw::Result BtLinkModule::init() noexcept
{
    if (context_ == nullptr) {
        return fw::Result::kInvalidState;
    }
    const auto system = ctx().data.systemStatus.read();
    if (!system.btReady) {
        return fw::Result::kInvalidState;
    }
    /* 从持久化快照恢复策略，再按依赖顺序原地构造服务。 */
    const auto link = ctx().data.linkStatus.read();
    link_manager_.init(link.policy, std::span{link.whitelist}.first(link.whitelistCount));
    core::bindPairWindow(link_manager_, &pair_window_);
    auto &alarm = alarm_.emplace();
    auto &deviceService = device_service_.emplace();
    deviceService.info().sn.assign(system.sn.data());
    deviceService.info().fwVer.assign(kAppVersion);
    deviceService.setSatTrackSurveyHook(&core::services::PhoneCompatService::handleSurveySatTrack,
                                        &phone_compat_);
    auto &configService = config_service_.emplace();
    phone_compat_.ops = phoneCompatOps(ctx());
    auto &alarmService = alarm_service_.emplace(alarm, link_manager_, deviceService.info());
    auto &pairService = pair_service_.emplace(link_manager_);
#if RTK_GNSS_ENABLED
    cached_gnss_fix_.emplace();
    auto &gnssService = gnss_service_.emplace();
    gnssService.setQueryHook(&BtLinkModule::onGnssQuery, this);
    gnssService.setMaskHook(&BtLinkModule::onGnssMask, this);
    gnss_reader_attached_ = fw::isOk(ctx().data.gnssAppRing.attach(0U, gnss_reader_id_));
    if (!gnss_reader_attached_) {
        FW_LOGE(fw::TraceCat::kLink, "<gnss> app ring attach failed");
        return fw::Result::kInvalidState;
    }
    /* 路由表校验成功后才构造 LinkService，失败由执行器隔离。 */
    const std::array<core::RouteTable, 6> tables = {deviceService.table(), configService.table(),
                                                    alarmService.table(),  pairService.table(),
                                                    gnssService.table(),   phone_compat_.table()};
#else
    /* 路由表校验成功后才构造 LinkService，失败由执行器隔离。 */
    const std::array<core::RouteTable, 5> tables = {deviceService.table(), configService.table(),
                                                    alarmService.table(), pairService.table(),
                                                    phone_compat_.table()};
#endif
    const fw::Status routeStatus = router_.init(tables);
    if (!isOk(routeStatus)) {
        FW_LOGE(fw::TraceCat::kLink, "command router init failed: %s", toString(routeStatus));
        return fw::Result::kInvalidState;
    }
    transport_.emplace();
    if (!transport_->supportsCredit()) {
        return fw::Result::kInvalidState;
    }
    service_.emplace(*transport_, link_manager_, router_, alarmService);
    spp_server_set_rx_wake_hook(&BtLinkModule::onRxWake, this);
    return fw::Result::kOk;
}

void BtLinkModule::onRxWake(void *ctx) noexcept
{
    const auto *self = static_cast<BtLinkModule *>(ctx);
    if (self == nullptr) {
        return;
    }
    const TaskHandle_t task = self->wake_task_.load(std::memory_order_acquire);
    if (task != nullptr) {
        static_cast<void>(xTaskNotifyGive(task));
    }
}

bool BtLinkModule::pollAndCheckReady(uint32_t nowMs) noexcept
{
    spp_server_evt_t evt{};
    for (size_t i = 0; i < SPP_SERVER_EVENT_QUEUE_LEN; ++i) {
        if (!spp_server_poll_event(&evt, 0)) {
            break;
        }
        handleEvent(evt, nowMs);
    }
    bool ready = false;
    for (auto &entry : sessions_) {
        if (!entry.used) {
            continue;
        }
        if (linkService().paused(entry.handle)) {
            if (transport_.has_value() && transport_->replyReady(entry.handle)) {
                ready = true;
            }
            continue;
        }
        if (entry.demuxReady) {
            if (spp_server_rx_ready(entry.handle)) {
                entry.drainPending = true;
            }
            if (entry.drainPending || (entry.rxOffset < entry.rxSize)) {
                ready = true;
            }
        }
    }
    return ready;
}

void BtLinkModule::wait(uint32_t maxMs) noexcept
{
    round_start_ms_ = rtos::nowMs();
    if (!service_.has_value()) {
        return;
    }
    wake_task_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    // 先清旧边缘通知，再查队列与水平状态；若仍空闲则复查后阻塞。
    static_cast<void>(ulTaskNotifyTake(pdTRUE, 0U));
    if (!pollAndCheckReady(rtos::nowMs()) && (maxMs > 0U)) {
        if (!pollAndCheckReady(rtos::nowMs())) {
            const uint32_t boundedMs = (pending_reset_ != PendingReset::kNone)
                                           ? std::min(maxMs, kPhoneResetPollMs)
                                           : maxMs;
            const TickType_t ticks = pdMS_TO_TICKS(boundedMs);
            const TickType_t waitTicks = (ticks == 0U) ? 1U : ticks;
            static_cast<void>(ulTaskNotifyTake(pdTRUE, waitTicks));
            static_cast<void>(pollAndCheckReady(rtos::nowMs()));
        }
    }
    round_start_ms_ = rtos::nowMs();
}

bool BtLinkModule::poll(uint32_t /*nowMs*/) noexcept
{
    /* 先消费复位和白名单变化，再按真实时钟推进 tick。 */
    applyRtcmReset();
    collectWhitelistDirty();
    const uint32_t now = rtos::nowMs();
#if RTK_GNSS_ENABLED
    syncGnssInfo(now);
#endif
    if (now - last_tick_ms_ >= kTickPeriodMs) {
        last_tick_ms_ = now;
        linkService().tick(now);
    } else {
        linkService().pollDeferred(now);
    }
    for (const auto &entry : sessions_) {
        if (entry.used) {
            linkService().resume(entry.handle, now);
        }
    }
    /* 按预算接收并直推 RTCM，再绑定并排空 PHONE GNSS 流，最后发布链路统计。 */
    drainRx(round_start_ms_);
    syncStream();
#if RTK_GNSS_ENABLED
    const bool gnssMore = drainGnssStream();
    const bool baseMore = drainBaseRtcm();
#else
    const bool gnssMore = false;
    const bool baseMore = false;
#endif
    collectWhitelistDirty();
    publishLink(rtos::nowMs());
    pollPhoneReset(rtos::nowMs());
    bool pending = gnssMore || baseMore;
    for (const auto &entry : sessions_) {
        pending =
            pending || (entry.used && entry.demuxReady && !linkService().paused(entry.handle) &&
                        (entry.drainPending || entry.rxOffset < entry.rxSize));
    }
    return pending;
}

void BtLinkModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    result.status = fw::Result::kOk;
    if (cmd.argLen > cmd.args.size()) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    /* 校验长度后按动词调用具名处理函数。 */
    // verb 来源为 uint8_t，LinkVerb 底层类型相同；未知值由 default 拒绝。
    switch (static_cast<LinkVerb>(cmd.verb)) {
    case LinkVerb::kAlarmSet:
        setAlarm(cmd, result);
        break;
    case LinkVerb::kAlarmClear:
        if (!alarm_.has_value()) {
            alarm_.emplace();
        }
        alarm_->clearOverride();
        break;
    case LinkVerb::kPair:
        openPairing(cmd, result);
        break;
    case LinkVerb::kPairClose:
        core::closePairWindow(pair_window_, rtos::nowMs());
        break;
    case LinkVerb::kWhitelistClear:
        clearWhitelist(result);
        break;
    case LinkVerb::kWhitelistAdd:
        editWhitelist(cmd, true, result);
        break;
    case LinkVerb::kWhitelistDel:
        editWhitelist(cmd, false, result);
        break;
    default:
        result.status = fw::Result::kInvalidArg;
        break;
    }
}
} // namespace app
