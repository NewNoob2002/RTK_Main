// 链路单一所有者；RX 调度保持 512 B 默认读缓冲、字节/20 ms 预算、每轮一个产品帧，RTCM 直推管道。
// 启动期由组合根初始化一次；poll 与命令只在 kLink 执行器任务；蓝牙 API 可阻塞，非 ISR 安全。
#include "modules/bt_link/BtLinkModule.hpp"
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
namespace {
constexpr uint32_t kRoundBudgetMs = 20;
#if RTK_GNSS_ENABLED
constexpr uint32_t kGnssFixStaleMs = 3000U;
constexpr uint32_t kGnssClockJitterMs = 10U;
constexpr size_t kMaxGnssStreamRecordsPerPoll = 16U;
constexpr size_t kMaxGnssFlushRecords = 64U;
#endif
constexpr size_t kAlarmArgBytes = 2;
// RTCM3 线路帧：1 B 前导 + 2 B 长度，载荷首 12 bit 为消息号，尾 3 B 为 CRC24Q。
constexpr size_t kMinRtcmMessageBytes = 8;
constexpr size_t kRtcmPayloadOffset = 3;
constexpr uint32_t kRtcmMsgShiftBits = 4;
constexpr uint32_t kHandleDisplayMask = 0xFFFFU;
} // namespace

void BtLinkModule::SessionSink::bind(BtLinkModule &owner, core::SessionHandle handle) noexcept
{
    owner_ = &owner;
    handle_ = handle;
    unhealthy_ = false;
}

void BtLinkModule::SessionSink::onFrame(const demux::Decoder &decoder, uint8_t index,
                                        std::span<const uint8_t> wire) noexcept
{
    const bool known = (index == kProductIndex && &decoder == &proto::demux_frame::kDecoder) ||
                       (index == kRtcmIndex && &decoder == &demux::rtcm3::kDecoder);
    if (!known || (owner_ == nullptr)) {
        unhealthy_ = true;
        FW_LOGE(fw::TraceCat::kProto,
                "session 0x%04" PRIx32
                " delivery callback invalid (decoder index %u, %u bytes), stream reset",
                handle_ & kHandleDisplayMask, static_cast<unsigned>(index),
                static_cast<unsigned>(wire.size()));
        return;
    }
    if (index == kProductIndex) {
        if (owner_->linkService().onData(handle_, wire, rtos::nowMs()) == 0U) {
            core::traceProto(core::ProtoTrace::kReplyError, handle_,
                             static_cast<uint32_t>(fw::Result::kBusy), 0U);
        }
        return;
    }
    owner_->handleRtcmFrame(handle_, wire);
}

void BtLinkModule::handleRtcmFrame(core::SessionHandle handle,
                                   std::span<const uint8_t> wire) noexcept
{
    if (link_manager_.role(handle) != core::Role::kPhone) {
        ++rtcm_snapshot_.rejectedRole;
        traceRtcm(RtcmTrace::kReject, handle, static_cast<uint32_t>(RtcmReject::kRole),
                  static_cast<uint32_t>(wire.size()));
        return;
    }
    ++rtcm_snapshot_.frames;
    // wire 已由 RTCM 解码器校验，长度 <= 1029 B，转换到 uint32_t 无损。
    rtcm_snapshot_.bytes += static_cast<uint32_t>(wire.size());
    rtcm_snapshot_.crc32 = proto::crc32(wire, rtcm_snapshot_.crc32);
    uint32_t msg = 0U;
    if (wire.size() >= kMinRtcmMessageBytes) {
        msg = (static_cast<uint32_t>(wire[kRtcmPayloadOffset]) << kRtcmMsgShiftBits) |
              (static_cast<uint32_t>(wire[kRtcmPayloadOffset + 1U]) >> kRtcmMsgShiftBits);
    }
    // 12-bit 消息号范围为 0..4095，转换到 uint16_t 无损。
    rtcm_snapshot_.lastMsg = static_cast<uint16_t>(msg);
    traceRtcm(RtcmTrace::kFrame, handle, msg, static_cast<uint32_t>(wire.size()));
#if RTK_GNSS_ENABLED
    if (ctx().rtcmPipe.push(wire)) {
        ++rtcm_snapshot_.pipeAcceptedFrames;
        rtcm_snapshot_.pipeAcceptedBytes += static_cast<uint32_t>(wire.size());
    } else {
        ++rtcm_snapshot_.pipeDroppedFrames;
        rtcm_snapshot_.pipeDroppedBytes += static_cast<uint32_t>(wire.size());
    }
#else
    ++rtcm_snapshot_.pipeAcceptedFrames;
    rtcm_snapshot_.pipeAcceptedBytes += static_cast<uint32_t>(wire.size());
#endif
}

BtLinkModule::SessionEntry *BtLinkModule::sessionFind(core::SessionHandle handle) noexcept
{
    for (auto &entry : sessions_) {
        if (entry.used && entry.handle == handle) {
            return &entry;
        }
    }
    return nullptr;
}

void BtLinkModule::sessionForget(core::SessionHandle handle) noexcept
{
    auto *entry = sessionFind(handle);
    if (entry == nullptr) {
        return;
    }
    /* CLOSE 取消所有尚未路由的帧。abort 只补统计，其产出的尾帧也必须丢弃。 */
    if (entry->demuxReady && entry->demux.has_value()) {
        const auto error = entry->demux->abort();
        if (error != demux::Error::kOk) {
            FW_LOGE(fw::TraceCat::kProto, "stream demux abort failed (reentry)");
        }
    }
    /* 累计退役统计后复位会话，避免重用句柄携带半帧。 */
    if (entry->demux.has_value()) {
        const auto &stats = entry->demux->stats();
        retired_demux_.unknownBytes += stats.unknownBytes;
        retired_demux_.productRejects += stats.decoder[kProductIndex].rejects;
        retired_demux_.rtcmRejects += stats.decoder[kRtcmIndex].rejects;
    }
    entry->used = false;
    entry->handle = 0;
    entry->addr = {};
    entry->drainPending = false;
    entry->rxOffset = entry->rxSize = 0;
    entry->demuxReady = false;
    if (entry->sink.has_value()) {
        entry->sink->bind(*this, 0);
    }
}

void BtLinkModule::sessionRecord(core::SessionHandle handle, const core::BtAddr &addr) noexcept
{
    if (sessionFind(handle) != nullptr) {
        return;
    }
    /* 先登记地址和队列，再初始化本会话的分流器。 */
    for (auto &entry : sessions_) {
        if (entry.used) {
            continue;
        }
        entry.used = true;
        entry.handle = handle;
        entry.addr = addr;
        entry.drainPending = false;
        entry.rxOffset = entry.rxSize = 0;
        if (!entry.sink.has_value()) {
            entry.sink.emplace();
        }
        if (!entry.demux.has_value()) {
            entry.demux.emplace();
        }
        entry.sink->bind(*this, handle);
        entry.demuxReady = entry.demux->init(*entry.sink) == demux::Error::kOk;
        if (!entry.demuxReady) {
            FW_LOGW(fw::TraceCat::kProto,
                    "stream demux init failed, session 0x%04" PRIx32 " gets no parsing",
                    handle & kHandleDisplayMask);
        }
        return;
    }
    FW_LOGW(fw::TraceCat::kLink, "session map full, 0x%04" PRIx32 " not tracked",
            handle & kHandleDisplayMask);
}

void BtLinkModule::collectWhitelistDirty() noexcept
{
    if (linkService().consumeWhitelistDirty()) {
        ctx().storage.markWhitelistDirty(link_manager_.whitelist(), rtos::nowMs());
    }
}

void BtLinkModule::handleOpen(const spp_server_evt_t &evt, uint32_t nowMs) noexcept
{
    core::BtAddr addr{};
    for (size_t i = 0; i < core::kAddrLen; ++i) {
        addr.bytes[i] = evt.addr[i];
    }
    /* 会话元数据必须先于 LinkService 的 OPEN 可见。 */
    sessionRecord(evt.handle, addr);
    linkService().onOpen(evt.handle, addr, nowMs);
}

void BtLinkModule::handleClose(core::SessionHandle handle) noexcept
{
    const auto *entry = sessionFind(handle);
    const size_t unconsumed = (entry != nullptr && entry->rxOffset < entry->rxSize)
                                  ? (entry->rxSize - entry->rxOffset)
                                  : 0U;
    const bool wasPaused = linkService().paused(handle);
    if (wasPaused || (unconsumed > 0U)) {
        core::traceLink(core::LinkTrace::kCloseCancel, handle, static_cast<uint32_t>(unconsumed),
                        wasPaused ? 1U : 0U);
    }
    linkService().onClose(handle);
    sessionForget(handle);
}

void BtLinkModule::handleData(core::SessionHandle handle) noexcept
{
    if (auto *entry = sessionFind(handle); entry != nullptr && entry->demuxReady) {
        entry->drainPending = true;
    }
}

void BtLinkModule::handleEvent(const spp_server_evt_t &evt, uint32_t nowMs) noexcept
{
    switch (evt.type) {
    case SPP_SERVER_EVT_OPEN:
        handleOpen(evt, nowMs);
        break;
    case SPP_SERVER_EVT_CLOSE:
        handleClose(evt.handle);
        break;
    case SPP_SERVER_EVT_DATA:
        handleData(evt.handle);
        break;
    case SPP_SERVER_EVT_TX_SPACE:
        linkService().resume(evt.handle, nowMs);
        break;
    case SPP_SERVER_EVT_RX_DROP:
        linkService().onRxDropped(evt.dropped);
        break;
    default:
        break;
    }
}

bool BtLinkModule::feedOne(SessionEntry &entry, size_t &budget, uint32_t roundStartMs) noexcept
{
    if (!entry.demux.has_value() || !entry.sink.has_value()) {
        entry.demuxReady = false;
        entry.drainPending = false;
        return true;
    }
    auto &sessionDemux = *entry.demux;
    const auto &sessionSink = *entry.sink;
    /* 只在预读耗尽时读取，空读结束此会话本轮处理。 */
    if (entry.rxOffset == entry.rxSize) {
        entry.rxSize = spp_server_read(entry.handle, entry.rx.data(), entry.rx.size());
        entry.rxOffset = 0;
        if (entry.rxSize == 0U) {
            entry.drainPending = false;
            return false;
        }
    }
    // 非阻塞 read 也可能被抢占，过期时保留整个预读结果。
    if (rtos::nowMs() - roundStartMs >= kRoundBudgetMs) {
        return true;
    }
    /* 逐字节 feed 保留基线的帧投递和预算边界。 */
    const auto error =
        sessionDemux.feed(std::span<const uint8_t>{entry.rx}.subspan(entry.rxOffset, 1));
    ++entry.rxOffset;
    --budget;
    if (error != demux::Error::kOk || !sessionSink.healthy()) {
        entry.demuxReady = false;
        entry.drainPending = false;
        FW_LOGW(fw::TraceCat::kProto,
                "stream demux stopped (%s), further bytes on this session ignored",
                error != demux::Error::kOk ? "feed error" : "delivery queue invalid");
        return true;
    }
    return false;
}

bool BtLinkModule::drainSession(SessionEntry &entry, size_t index, size_t &budget,
                                uint32_t roundStartMs) noexcept
{
    while (entry.used && entry.demuxReady && !linkService().paused(entry.handle) &&
           (entry.drainPending || entry.rxOffset < entry.rxSize) && budget > 0U) {
        if (rtos::nowMs() - roundStartMs >= kRoundBudgetMs) {
            return true;
        }
        // 时间耗尽前不推进起点，避免跳过上轮让出的会话。
        rx_cursor_ = (index + 1U) % kMaxSessions;
        if (feedOne(entry, budget, roundStartMs)) {
            return true;
        }
    }
    return false;
}

void BtLinkModule::drainRx(uint32_t roundStartMs) noexcept
{
    size_t budget = kRxBudgetBytes;
    const size_t first = rx_cursor_;
    /* 从上轮让出的会话开始，任一预算耗尽即结束整轮。 */
    for (size_t step = 0; step < kMaxSessions && budget > 0U; ++step) {
        const size_t index = (first + step) % kMaxSessions;
        if (drainSession(sessions_[index], index, budget, roundStartMs)) {
            return;
        }
    }
}

void BtLinkModule::syncStream() noexcept
{
    if (stream_handle_ != 0 && link_manager_.role(stream_handle_) != core::Role::kPhone) {
#if RTK_GNSS_ENABLED
        resetGnssStreamSession();
#endif
        const esp_err_t status = spp_server_stream_bind(stream_handle_, false);
        // CLOSE 已回收时 NOT_FOUND 等价于已解绑；其余失败保留句柄下轮重查。
        if (status != ESP_OK && status != ESP_ERR_NOT_FOUND) {
            return;
        }
        stream_handle_ = 0;
    }
    if (stream_handle_ == 0) {
        for (const auto &entry : sessions_) {
            if (entry.used && link_manager_.role(entry.handle) == core::Role::kPhone) {
                const esp_err_t status = spp_server_stream_bind(entry.handle, true);
                if (status == ESP_OK) {
                    stream_handle_ = entry.handle;
                }
                // 旧 generation 的 copy/API 未退出时容量不可用，下轮重查，不另建 stream 或同步回退。
                return;
            }
        }
    }
}

void BtLinkModule::publishLink(uint32_t nowMs) noexcept
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
    Sessions sessions{};
    for (const auto &entry : sessions_) {
        const auto role = link_manager_.role(entry.handle);
        if (entry.used && role != core::Role::kNone && sessions.count < kMaxSessions) {
            sessions.entries[sessions.count++] = {true, entry.handle, role, entry.addr};
        }
    }
    ctx().data.sessions.write(sessions);
    if (alarm_.has_value()) {
        const AlarmState snapshot{alarm_->state(), alarm_->level(), alarm_->isOverride()};
        ctx().data.alarmState.write(snapshot);
    } else {
        ctx().data.alarmState.write({});
    }
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
    DemuxStats demux = retired_demux_;
    for (auto &entry : sessions_) {
        if (entry.used && entry.demux.has_value()) {
            const auto &counts = entry.demux->stats();
            demux.unknownBytes += counts.unknownBytes;
            demux.productRejects += counts.decoder[kProductIndex].rejects;
            demux.rtcmRejects += counts.decoder[kRtcmIndex].rejects;
            /* 重同步与未知字节只在分流器内部累计；这里每轮取一次增量上报，
             * 避免在逐字节 feed 路径上读统计。 */
            if (counts.resyncs > entry.seenResyncs) {
                traceRtcm(RtcmTrace::kResync, entry.handle, counts.resyncs - entry.seenResyncs);
                entry.seenResyncs = counts.resyncs;
            }
            if (counts.unknownBytes > entry.seenUnknown) {
                traceRtcm(RtcmTrace::kUnknown, entry.handle,
                          counts.unknownBytes - entry.seenUnknown);
                entry.seenUnknown = counts.unknownBytes;
            }
        }
    }
    ctx().data.demuxStats.write(demux);
}

void BtLinkModule::applyRtcmReset() noexcept
{
    uint32_t resetVersion = 0;
    const uint32_t reset = ctx().data.rtcmReset.read(resetVersion);
    if (resetVersion != reset_version_) {
        reset_version_ = resetVersion;
        retired_demux_ = {};
        rtcm_snapshot_ = {};
        rtcm_snapshot_.resetEpoch = reset;
        for (auto &entry : sessions_) {
            if (entry.demux.has_value()) {
                entry.demux->resetStats();
            }
            entry.seenResyncs = 0U;
            entry.seenUnknown = 0U;
        }
    }
}

#if RTK_GNSS_ENABLED
void BtLinkModule::syncGnssInfo(uint32_t nowMs) noexcept
{
    if (!device_service_.has_value()) {
        return;
    }
    auto &deviceInfo = device_service_->info();
    if (!cached_gnss_fix_.has_value()) {
        cached_gnss_fix_.emplace();
    }
    auto &cachedFix = *cached_gnss_fix_;
    uint32_t fixVer = 0;
    const auto fix = ctx().data.gnssFix.read(fixVer);
    if (fixVer != gnss_fix_version_) {
        gnss_fix_version_ = fixVer;
        cachedFix = fix;
    }
    const bool withinWindow = (nowMs - cachedFix.updateMs <= kGnssFixStaleMs) ||
                              (cachedFix.updateMs - nowMs <= kGnssClockJitterMs);
    const bool fixFresh = cachedFix.valid && (cachedFix.updateMs != 0U) && withinWindow;
    const uint8_t fixQ = (fixFresh && (cachedFix.quality != ::gnss::FixQuality::kUnknown))
                             ? static_cast<uint8_t>(cachedFix.quality)
                             : proto::alarm::kValueUnknown;
    deviceInfo.fixQuality = fixQ;

    uint32_t statusVer = 0;
    const auto gnssSt = ctx().data.gnssStatus.read(statusVer);
    if (statusVer != gnss_status_version_) {
        gnss_status_version_ = statusVer;
        deviceInfo.gnssRunning = gnssSt.enabled && (gnssSt.state == ::gnss::State::kRunning);
        if (gnssSt.model != ::gnss::Model::kUnknown) {
            deviceInfo.board.assign(::gnss::modelName(gnssSt.model));
        }
        if (gnssSt.fwVersion[0] != '\0') {
            deviceInfo.boardFw.assign(std::string_view{gnssSt.fwVersion.data()});
        }
        if (gnss_service_.has_value()) {
            gnss_service_->setSupportedMask(gnssSt.supportedMask);
        }
    }
}

core::services::GnssQueryResult BtLinkModule::onGnssQuery(void *ctx, uint16_t msgId,
                                                          std::span<uint8_t> dst) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    const auto idx = core::services::GnssService::typeIndexOf(msgId);
    if ((self == nullptr) || !idx.has_value()) {
        return {};
    }
    const auto typeIdx = static_cast<uint8_t>(*idx);
    const auto res = self->ctx().data.gnssLatest.query(typeIdx, dst);
    if (res.oversize) {
        // queryOversize 在 GnssService 内于本回调返回后自增，先累加 1 再写快照避免少计当前次。
        const uint32_t cur =
            self->gnss_service_.has_value() ? (self->gnss_service_->queryOversizeCount() + 1U) : 1U;
        self->ctx().data.gnssAppSub.setQueryOversizeTotal(cur);
        fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(GnssSubTrace::kAppDrop), 0U, 1U,
                  typeIdx);
    }
    return res;
}

void BtLinkModule::onGnssMask(void *ctx, uint32_t mask) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if (self == nullptr) {
        return;
    }
    if (self->gnss_reader_attached_) {
        static_cast<void>(self->ctx().data.gnssAppRing.setMask(self->gnss_reader_id_, mask));
    }
    std::array<uint32_t, core::services::kGnssMsgCount> periodsMs{};
    if (self->gnss_service_.has_value()) {
        for (size_t i = 0U; i < core::services::kGnssMsgCount; ++i) {
            periodsMs[i] = self->gnss_service_->periodMs(i);
        }
    }
    self->ctx().data.gnssAppSub.setSubscription(mask, periodsMs);
    fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(GnssSubTrace::kAppMask), 0U, mask,
              (mask == 0U) ? 1U : 0U);
}

void BtLinkModule::resetGnssStreamSession() noexcept
{
    if (gnss_service_.has_value()) {
        gnss_service_->resetSession();
    }
    if (!gnss_reader_attached_) {
        return;
    }
    // 断连或切会话时清空旧读者的残余记录，避免重连后重放上一会话的句子（ADR-0011 §6.2）。
    for (size_t i = 0U; i < kMaxGnssFlushRecords; ++i) {
        fw::FanoutView view{};
        if (!fw::isOk(ctx().data.gnssAppRing.peek(gnss_reader_id_, view))) {
            break;
        }
        static_cast<void>(ctx().data.gnssAppRing.commit(view.lease));
    }
}

bool BtLinkModule::drainGnssStream() noexcept
{
    if (!gnss_reader_attached_ || (stream_handle_ == 0U) ||
        (link_manager_.role(stream_handle_) != core::Role::kPhone)) {
        return false;
    }
    size_t sentCount = 0U;
    for (; sentCount < kMaxGnssStreamRecordsPerPoll; ++sentCount) {
        fw::FanoutView view{};
        if (!fw::isOk(ctx().data.gnssAppRing.peek(gnss_reader_id_, view))) {
            break;
        }
        const size_t total = view.first.size() + view.second.size();
        if ((total == 0U) || (total > kGnssStreamMaxRecord)) {
            static_cast<void>(ctx().data.gnssAppRing.commit(view.lease));
            continue;
        }
        spp_server_tx_token_t token{};
        if (spp_server_stream_reserve(stream_handle_, total, &token) != ESP_OK) {
            static_cast<void>(ctx().data.gnssAppRing.cancel(view.lease));
            return false;
        }
        // 源记录跨环尾时两段直接拼进 TX.stream，无需中转缓冲。
        const esp_err_t committed =
            view.second.empty()
                ? spp_server_tx_commit(&token, view.first.data(), total)
                : spp_server_tx_commit2(&token, view.first.data(), view.first.size(),
                                        view.second.data(), view.second.size());
        if (committed != ESP_OK) {
            spp_server_tx_cancel(&token);
            static_cast<void>(ctx().data.gnssAppRing.cancel(view.lease));
            return false;
        }
        static_cast<void>(ctx().data.gnssAppRing.commit(view.lease));
        ctx().data.gnssAppSub.addTxSent(view.type);
    }
    return sentCount == kMaxGnssStreamRecordsPerPoll;
}
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

void BtLinkModule::setAlarm(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!alarm_.has_value()) {
        alarm_.emplace();
    }
    if (cmd.argLen != kAlarmArgBytes || !alarm_->setOverride(cmd.args[0], cmd.args[1])) {
        result.status = fw::Result::kInvalidArg;
    }
}

void BtLinkModule::clearWhitelist(fw::CommandResult &result) noexcept
{
    /* 清除内存白名单并同步持久化，回执携带原后端错误码。 */
    link_manager_.whitelistClear();
    persistWhitelist(result);
}

void BtLinkModule::persistWhitelist(fw::CommandResult &result) noexcept
{
    /* 控制台改动立即落盘（不走 HELLO 学习的延迟合并），回执携带原后端错误码。 */
    ctx().storage.markWhitelistDirty(link_manager_.whitelist(), rtos::nowMs());
    result.detail = ctx().storage.flushWhitelist(rtos::nowMs());
    if (result.detail != ESP_OK) {
        result.status = fw::Result::kIoError;
    }
}

void BtLinkModule::editWhitelist(const fw::Command &cmd, bool add,
                                 fw::CommandResult &result) noexcept
{
    /* args = 6 字节 MAC；仅在白名单实际变化时持久化。 */
    if (cmd.argLen != core::kAddrLen) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    core::BtAddr addr{};
    std::copy_n(cmd.args.begin(), core::kAddrLen, addr.bytes.begin());
    const core::WhitelistEdit edit =
        add ? link_manager_.whitelistAdd(addr) : link_manager_.whitelistRemove(addr);
    switch (edit) {
    case core::WhitelistEdit::kDone:
        persistWhitelist(result);
        break;
    case core::WhitelistEdit::kUnchanged:
        // add：已存在；del：不存在。由 CLI 按动词翻译成可读错误。
        result.status = add ? fw::Result::kInvalidState : fw::Result::kNotFound;
        break;
    case core::WhitelistEdit::kFull:
        result.status = fw::Result::kNoSpace;
        break;
    default:
        result.status = fw::Result::kInvalidArg;
        break;
    }
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
