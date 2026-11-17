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
#if RTK_GNSS_ENABLED
#include "def/GnssSubDef.hpp"
#endif
#include "proto/Frame.hpp"
#include "rtos/EspStatus.hpp"

// 回复信用按整帧预留；协议帧长一旦超过预留量，commit 会拒绝并永久占住该会话的预留，
// 使会话再也不能恢复。在此处绑定两者，避免只改一侧。
static_assert(proto::kMaxFrame <= SPP_SERVER_REPLY_RESERVE,
              "reply reserve must cover a full protocol frame");
static_assert(fw::kAtomicUsable<TaskHandle_t>, "wake_task_ must be lock-free");

namespace app {

/**
 * @brief 发布链路策略/配对/白名单快照
 * @param nowMs: 本次快照时间
 * @note 仅 kLink 任务；非阻塞、不分配堆。
 */
void BtLinkModule::publishLinkStatus(uint32_t nowMs) noexcept
{

    /* 按基线顺序发布链路、会话与报警状态。 */
    LinkStatus link{};
    link.policy = policyFromConfig();
    link.pairing = link_manager_.isPairing();
    link.snapshotMs = nowMs;
    const auto list = link_manager_.whitelist();
    link.whitelistCount = std::min(list.size(), kMaxAlarms);
    std::copy_n(list.begin(), link.whitelistCount, link.whitelist.begin());
    ctx().data.linkStatus.write(link);
}

/**
 * @brief 发布当前会话表（句柄、角色、对端地址）
 * @note 仅 kLink 任务；只读会话表，非阻塞。
 */
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

/**
 * @brief 发布报警状态；无报警时写空快照
 * @note 仅 kLink 任务；非阻塞。
 */
void BtLinkModule::publishAlarmState() noexcept
{
    if (alarm_.has_value()) {
        const AlarmState snapshot{alarm_->state(), alarm_->level(), alarm_->isOverride()};
        ctx().data.alarmState.write(snapshot);
    } else {
        ctx().data.alarmState.write({});
    }
}

/**
 * @brief 发布发送、心跳与分流统计（顺序与基线一致）
 * @note 仅 kLink 任务；spp_server_*_stats 只读快照，非阻塞。
 */
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

/**
 * @brief 汇总各会话分流器统计并在增量变化时上报重同步/未知字节
 * @note 仅 kLink 任务；每轮取一次增量，避免在逐字节 feed 路径上读统计。
 */
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

/**
 * @brief 发布链路、会话、报警与统计快照（每轮一次，顺序固定）
 * @param nowMs: 本次快照时间
 * @note 仅 kLink 任务；非阻塞、非 ISR。
 */
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
 * @brief  构造链路服务并启动 SPP 服务端
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
    auto &alarm = alarm_.emplace();
    auto &deviceService = device_service_.emplace();
    deviceService.info().sn.assign(system.sn.data());
    deviceService.info().fwVer.assign(kAppVersion);
    auto &configService = config_service_.emplace();
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
    const std::array<core::RouteTable, 5> tables = {deviceService.table(), configService.table(),
                                                    alarmService.table(), pairService.table(),
                                                    gnssService.table()};
#else
    /* 路由表校验成功后才构造 LinkService，失败由执行器隔离。 */
    const std::array<core::RouteTable, 4> tables = {deviceService.table(), configService.table(),
                                                    alarmService.table(), pairService.table()};
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
            const TickType_t ticks = pdMS_TO_TICKS(maxMs);
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
#else
    const bool gnssMore = false;
#endif
    collectWhitelistDirty();
    publishLink(rtos::nowMs());
    bool pending = gnssMore;
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
        if (link_manager_.isPairing()) {
            result.status = fw::Result::kBusy;
        } else {
            link_manager_.pairOpen(rtos::nowMs());
        }
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
