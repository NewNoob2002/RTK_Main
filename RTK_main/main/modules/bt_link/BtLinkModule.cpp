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

void BtLinkModule::SessionSink::on_frame(const demux::Decoder &decoder, uint8_t index,
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
        if (owner_->link_service().on_data(handle_, wire, rtos::nowMs()) == 0U) {
            core::trace_proto(core::ProtoTrace::kReplyError, handle_,
                              static_cast<uint32_t>(fw::Result::kBusy), 0U);
        }
        return;
    }
    owner_->handle_rtcm_frame(handle_, wire);
}

void BtLinkModule::handle_rtcm_frame(core::SessionHandle handle,
                                     std::span<const uint8_t> wire) noexcept
{
    if (link_manager_.role(handle) != core::Role::kPhone) {
        ++rtcm_snapshot_.rejected_role;
        trace_rtcm(RtcmTrace::kReject, handle, static_cast<uint32_t>(RtcmReject::kRole),
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
    rtcm_snapshot_.last_msg = static_cast<uint16_t>(msg);
    trace_rtcm(RtcmTrace::kFrame, handle, msg, static_cast<uint32_t>(wire.size()));
#if RTK_GNSS_ENABLED
    if (ctx().rtcm_pipe.push(wire)) {
        ++rtcm_snapshot_.pipe_accepted_frames;
        rtcm_snapshot_.pipe_accepted_bytes += static_cast<uint32_t>(wire.size());
    } else {
        ++rtcm_snapshot_.pipe_dropped_frames;
        rtcm_snapshot_.pipe_dropped_bytes += static_cast<uint32_t>(wire.size());
    }
#else
    ++rtcm_snapshot_.pipe_accepted_frames;
    rtcm_snapshot_.pipe_accepted_bytes += static_cast<uint32_t>(wire.size());
#endif
}

BtLinkModule::SessionEntry *BtLinkModule::session_find(core::SessionHandle handle) noexcept
{
    for (auto &entry : sessions_) {
        if (entry.used && entry.handle == handle) {
            return &entry;
        }
    }
    return nullptr;
}

void BtLinkModule::session_forget(core::SessionHandle handle) noexcept
{
    auto *entry = session_find(handle);
    if (entry == nullptr) {
        return;
    }
    /* CLOSE 取消所有尚未路由的帧。abort 只补统计，其产出的尾帧也必须丢弃。 */
    if (entry->demux_ready && entry->demux.has_value()) {
        const auto error = entry->demux->abort();
        if (error != demux::Error::kOk) {
            FW_LOGE(fw::TraceCat::kProto, "stream demux abort failed (reentry)");
        }
    }
    /* 累计退役统计后复位会话，避免重用句柄携带半帧。 */
    if (entry->demux.has_value()) {
        const auto &stats = entry->demux->stats();
        retired_demux_.unknown_bytes += stats.unknown_bytes;
        retired_demux_.product_rejects += stats.decoder[kProductIndex].rejects;
        retired_demux_.rtcm_rejects += stats.decoder[kRtcmIndex].rejects;
    }
    entry->used = false;
    entry->handle = 0;
    entry->addr = {};
    entry->drain_pending = false;
    entry->rx_offset = entry->rx_size = 0;
    entry->demux_ready = false;
    if (entry->sink.has_value()) {
        entry->sink->bind(*this, 0);
    }
}

void BtLinkModule::session_record(core::SessionHandle handle, const core::BtAddr &addr) noexcept
{
    if (session_find(handle) != nullptr) {
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
        entry.drain_pending = false;
        entry.rx_offset = entry.rx_size = 0;
        if (!entry.sink.has_value()) {
            entry.sink.emplace();
        }
        if (!entry.demux.has_value()) {
            entry.demux.emplace();
        }
        entry.sink->bind(*this, handle);
        entry.demux_ready = entry.demux->init(*entry.sink) == demux::Error::kOk;
        if (!entry.demux_ready) {
            FW_LOGW(fw::TraceCat::kProto,
                    "stream demux init failed, session 0x%04" PRIx32 " gets no parsing",
                    handle & kHandleDisplayMask);
        }
        return;
    }
    FW_LOGW(fw::TraceCat::kLink, "session map full, 0x%04" PRIx32 " not tracked",
            handle & kHandleDisplayMask);
}

void BtLinkModule::collect_whitelist_dirty() noexcept
{
    if (link_service().consume_whitelist_dirty()) {
        ctx().storage.mark_whitelist_dirty(link_manager_.whitelist(), rtos::nowMs());
    }
}

void BtLinkModule::handle_open(const spp_server_evt_t &evt, uint32_t now_ms) noexcept
{
    core::BtAddr addr{};
    for (size_t i = 0; i < core::kAddrLen; ++i) {
        addr.bytes[i] = evt.addr[i];
    }
    /* 会话元数据必须先于 LinkService 的 OPEN 可见。 */
    session_record(evt.handle, addr);
    link_service().on_open(evt.handle, addr, now_ms);
}

void BtLinkModule::handle_close(core::SessionHandle handle) noexcept
{
    const auto *entry = session_find(handle);
    const size_t unconsumed = (entry != nullptr && entry->rx_offset < entry->rx_size)
                                  ? (entry->rx_size - entry->rx_offset)
                                  : 0U;
    const bool was_paused = link_service().paused(handle);
    if (was_paused || (unconsumed > 0U)) {
        core::trace_link(core::LinkTrace::kCloseCancel, handle, static_cast<uint32_t>(unconsumed),
                         was_paused ? 1U : 0U);
    }
    link_service().on_close(handle);
    session_forget(handle);
}

void BtLinkModule::handle_data(core::SessionHandle handle) noexcept
{
    if (auto *entry = session_find(handle); entry != nullptr && entry->demux_ready) {
        entry->drain_pending = true;
    }
}

void BtLinkModule::handle_event(const spp_server_evt_t &evt, uint32_t now_ms) noexcept
{
    switch (evt.type) {
    case SPP_SERVER_EVT_OPEN:
        handle_open(evt, now_ms);
        break;
    case SPP_SERVER_EVT_CLOSE:
        handle_close(evt.handle);
        break;
    case SPP_SERVER_EVT_DATA:
        handle_data(evt.handle);
        break;
    case SPP_SERVER_EVT_TX_SPACE:
        link_service().resume(evt.handle, now_ms);
        break;
    case SPP_SERVER_EVT_RX_DROP:
        link_service().on_rx_dropped(evt.dropped);
        break;
    default:
        break;
    }
}

bool BtLinkModule::feed_one(SessionEntry &entry, size_t &budget, uint32_t round_start_ms) noexcept
{
    if (!entry.demux.has_value() || !entry.sink.has_value()) {
        entry.demux_ready = false;
        entry.drain_pending = false;
        return true;
    }
    auto &session_demux = *entry.demux;
    const auto &session_sink = *entry.sink;
    /* 只在预读耗尽时读取，空读结束此会话本轮处理。 */
    if (entry.rx_offset == entry.rx_size) {
        entry.rx_size = spp_server_read(entry.handle, entry.rx.data(), entry.rx.size());
        entry.rx_offset = 0;
        if (entry.rx_size == 0U) {
            entry.drain_pending = false;
            return false;
        }
    }
    // 非阻塞 read 也可能被抢占，过期时保留整个预读结果。
    if (rtos::nowMs() - round_start_ms >= kRoundBudgetMs) {
        return true;
    }
    /* 逐字节 feed 保留基线的帧投递和预算边界。 */
    const auto error =
        session_demux.feed(std::span<const uint8_t>{entry.rx}.subspan(entry.rx_offset, 1));
    ++entry.rx_offset;
    --budget;
    if (error != demux::Error::kOk || !session_sink.healthy()) {
        entry.demux_ready = false;
        entry.drain_pending = false;
        FW_LOGW(fw::TraceCat::kProto,
                "stream demux stopped (%s), further bytes on this session ignored",
                error != demux::Error::kOk ? "feed error" : "delivery queue invalid");
        return true;
    }
    return false;
}

bool BtLinkModule::drain_session(SessionEntry &entry, size_t index, size_t &budget,
                                 uint32_t round_start_ms) noexcept
{
    while (entry.used && entry.demux_ready && !link_service().paused(entry.handle) &&
           (entry.drain_pending || entry.rx_offset < entry.rx_size) && budget > 0U) {
        if (rtos::nowMs() - round_start_ms >= kRoundBudgetMs) {
            return true;
        }
        // 时间耗尽前不推进起点，避免跳过上轮让出的会话。
        rx_cursor_ = (index + 1U) % kMaxSessions;
        if (feed_one(entry, budget, round_start_ms)) {
            return true;
        }
    }
    return false;
}

void BtLinkModule::drain_rx(uint32_t round_start_ms) noexcept
{
    size_t budget = kRxBudgetBytes;
    const size_t first = rx_cursor_;
    /* 从上轮让出的会话开始，任一预算耗尽即结束整轮。 */
    for (size_t step = 0; step < kMaxSessions && budget > 0U; ++step) {
        const size_t index = (first + step) % kMaxSessions;
        if (drain_session(sessions_[index], index, budget, round_start_ms)) {
            return;
        }
    }
}

void BtLinkModule::sync_stream() noexcept
{
    if (stream_handle_ != 0 && link_manager_.role(stream_handle_) != core::Role::kPhone) {
#if RTK_GNSS_ENABLED
        reset_gnss_stream_session();
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

void BtLinkModule::publish_link(uint32_t now_ms) noexcept
{
    /* 按基线顺序发布链路、会话与报警状态。 */
    LinkStatus link{};
    link.policy = policy_from_config();
    link.pairing = link_manager_.is_pairing();
    link.snapshot_ms = now_ms;
    const auto list = link_manager_.whitelist();
    link.whitelist_count = std::min(list.size(), kMaxAlarms);
    std::copy_n(list.begin(), link.whitelist_count, link.whitelist.begin());
    ctx().data.link_status.write(link);
    Sessions sessions{};
    for (const auto &entry : sessions_) {
        const auto role = link_manager_.role(entry.handle);
        if (entry.used && role != core::Role::kNone && sessions.count < kMaxSessions) {
            sessions.entries[sessions.count++] = {true, entry.handle, role, entry.addr};
        }
    }
    ctx().data.sessions.write(sessions);
    if (alarm_.has_value()) {
        const AlarmState snapshot{alarm_->state(), alarm_->level(), alarm_->is_override()};
        ctx().data.alarm_state.write(snapshot);
    } else {
        ctx().data.alarm_state.write({});
    }
    /* 继续发布发送、心跳与分流统计，顺序不变。 */
    LinkStats stats{};
    static_cast<core::LinkStats &>(stats) = link_service().stats(); // 派生结构的基类赋值，无窄化。
    stats.parser_crc_errors = link_service().parser_crc_errors();
    stats.parser_length_errors = link_service().parser_length_errors();
    spp_server_tx_stats_t tx{};
    spp_server_tx_stats(&tx);
    stats.tx_accepted = tx.accepted;
    stats.tx_completed = tx.completed;
    stats.tx_cancelled = tx.cancelled;
    stats.tx_errors = tx.errors;
    stats.tx_cong = tx.cong;
    stats.tx_high_water = tx.high_water;
    stats.tx_paused = tx.paused;
    stats.tx_anon_write_fail = tx.anon_write_fail;
    stats.tx_stray_write = tx.stray_write;
    stats.tx_inflight = tx.inflight;
    spp_server_rx_stats_t rx{};
    spp_server_rx_stats(&rx);
    stats.evt_dropped = rx.evt_dropped;
    stats.rx_wake_calls = rx.wake_calls;
    for (size_t idx = 0U; idx < kMaxSessions; ++idx) {
        stats.rx_used[idx] = rx.sessions[idx].used;
        stats.rx_high_water[idx] = rx.sessions[idx].high_water;
        stats.rx_dropped_bytes[idx] = rx.sessions[idx].dropped_bytes;
        stats.rx_events[idx] = rx.sessions[idx].rx_events;
        stats.rx_drop_events[idx] = rx.sessions[idx].drop_events;
    }
    ctx().data.link_stats.write(stats);
    const auto heartbeat = link_service().last_heartbeat();
    ctx().data.heartbeat.write(
        {heartbeat.has_value(), heartbeat.value_or(proto::alarm::Heartbeat{})});
    DemuxStats demux = retired_demux_;
    for (auto &entry : sessions_) {
        if (entry.used && entry.demux.has_value()) {
            const auto &counts = entry.demux->stats();
            demux.unknown_bytes += counts.unknown_bytes;
            demux.product_rejects += counts.decoder[kProductIndex].rejects;
            demux.rtcm_rejects += counts.decoder[kRtcmIndex].rejects;
            /* 重同步与未知字节只在分流器内部累计；这里每轮取一次增量上报，
             * 避免在逐字节 feed 路径上读统计。 */
            if (counts.resyncs > entry.seen_resyncs) {
                trace_rtcm(RtcmTrace::kResync, entry.handle, counts.resyncs - entry.seen_resyncs);
                entry.seen_resyncs = counts.resyncs;
            }
            if (counts.unknown_bytes > entry.seen_unknown) {
                trace_rtcm(RtcmTrace::kUnknown, entry.handle,
                           counts.unknown_bytes - entry.seen_unknown);
                entry.seen_unknown = counts.unknown_bytes;
            }
        }
    }
    ctx().data.demux_stats.write(demux);
}

void BtLinkModule::apply_rtcm_reset() noexcept
{
    uint32_t reset_version = 0;
    const uint32_t reset = ctx().data.rtcm_reset.read(reset_version);
    if (reset_version != reset_version_) {
        reset_version_ = reset_version;
        retired_demux_ = {};
        rtcm_snapshot_ = {};
        rtcm_snapshot_.reset_epoch = reset;
        for (auto &entry : sessions_) {
            if (entry.demux.has_value()) {
                entry.demux->reset_stats();
            }
            entry.seen_resyncs = 0U;
            entry.seen_unknown = 0U;
        }
    }
}

#if RTK_GNSS_ENABLED
void BtLinkModule::sync_gnss_info(uint32_t now_ms) noexcept
{
    if (!device_service_.has_value()) {
        return;
    }
    auto &device_info = device_service_->info();
    if (!cached_gnss_fix_.has_value()) {
        cached_gnss_fix_.emplace();
    }
    auto &cached_fix = *cached_gnss_fix_;
    uint32_t fix_ver = 0;
    const auto fix = ctx().data.gnss_fix.read(fix_ver);
    if (fix_ver != gnss_fix_version_) {
        gnss_fix_version_ = fix_ver;
        cached_fix = fix;
    }
    const bool within_window = (now_ms - cached_fix.update_ms <= kGnssFixStaleMs) ||
                               (cached_fix.update_ms - now_ms <= kGnssClockJitterMs);
    const bool fix_fresh = cached_fix.valid && (cached_fix.update_ms != 0U) && within_window;
    const uint8_t fix_q = (fix_fresh && (cached_fix.quality != ::gnss::FixQuality::kUnknown))
                              ? static_cast<uint8_t>(cached_fix.quality)
                              : proto::alarm::kValueUnknown;
    device_info.fix_quality = fix_q;

    uint32_t status_ver = 0;
    const auto gnss_st = ctx().data.gnss_status.read(status_ver);
    if (status_ver != gnss_status_version_) {
        gnss_status_version_ = status_ver;
        device_info.gnss_running = gnss_st.enabled && (gnss_st.state == ::gnss::State::kRunning);
        if (gnss_st.model != ::gnss::Model::kUnknown) {
            device_info.board.assign(::gnss::model_name(gnss_st.model));
        }
        if (gnss_st.fw_version[0] != '\0') {
            device_info.board_fw.assign(std::string_view{gnss_st.fw_version.data()});
        }
        if (gnss_service_.has_value()) {
            gnss_service_->set_supported_mask(gnss_st.supported_mask);
        }
    }
}

core::services::GnssQueryResult BtLinkModule::on_gnss_query(void *ctx, uint16_t msg_id,
                                                            std::span<uint8_t> dst) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    const auto idx = core::services::GnssService::type_index_of(msg_id);
    if ((self == nullptr) || !idx.has_value()) {
        return {};
    }
    const auto type_idx = static_cast<uint8_t>(*idx);
    const auto res = self->ctx().data.gnss_latest.query(type_idx, dst);
    if (res.oversize) {
        // query_oversize 在 GnssService 内于本回调返回后自增，先累加 1 再写快照避免少计当前次。
        const uint32_t cur = self->gnss_service_.has_value()
                                 ? (self->gnss_service_->query_oversize_count() + 1U)
                                 : 1U;
        self->ctx().data.gnss_app_sub.set_query_oversize_total(cur);
        fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(GnssSubTrace::kAppDrop), 0U, 1U,
                  type_idx);
    }
    return res;
}

void BtLinkModule::on_gnss_mask(void *ctx, uint32_t mask) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if (self == nullptr) {
        return;
    }
    if (self->gnss_reader_attached_) {
        static_cast<void>(self->ctx().data.gnss_app_ring.setMask(self->gnss_reader_id_, mask));
    }
    std::array<uint32_t, core::services::kGnssMsgCount> periods_ms{};
    if (self->gnss_service_.has_value()) {
        for (size_t i = 0U; i < core::services::kGnssMsgCount; ++i) {
            periods_ms[i] = self->gnss_service_->period_ms(i);
        }
    }
    self->ctx().data.gnss_app_sub.set_subscription(mask, periods_ms);
    fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(GnssSubTrace::kAppMask), 0U, mask,
              (mask == 0U) ? 1U : 0U);
}

void BtLinkModule::reset_gnss_stream_session() noexcept
{
    if (gnss_service_.has_value()) {
        gnss_service_->reset_session();
    }
    if (!gnss_reader_attached_) {
        return;
    }
    // 断连或切会话时清空旧读者的残余记录，避免重连后重放上一会话的句子（ADR-0011 §6.2）。
    for (size_t i = 0U; i < kMaxGnssFlushRecords; ++i) {
        fw::FanoutView view{};
        if (!fw::isOk(ctx().data.gnss_app_ring.peek(gnss_reader_id_, view))) {
            break;
        }
        static_cast<void>(ctx().data.gnss_app_ring.commit(view.lease));
    }
}

bool BtLinkModule::drain_gnss_stream() noexcept
{
    if (!gnss_reader_attached_ || (stream_handle_ == 0U) ||
        (link_manager_.role(stream_handle_) != core::Role::kPhone)) {
        return false;
    }
    size_t sent_count = 0U;
    for (; sent_count < kMaxGnssStreamRecordsPerPoll; ++sent_count) {
        fw::FanoutView view{};
        if (!fw::isOk(ctx().data.gnss_app_ring.peek(gnss_reader_id_, view))) {
            break;
        }
        const size_t total = view.first.size() + view.second.size();
        if ((total == 0U) || (total > kGnssStreamMaxRecord)) {
            static_cast<void>(ctx().data.gnss_app_ring.commit(view.lease));
            continue;
        }
        spp_server_tx_token_t token{};
        if (spp_server_stream_reserve(stream_handle_, total, &token) != ESP_OK) {
            static_cast<void>(ctx().data.gnss_app_ring.cancel(view.lease));
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
            static_cast<void>(ctx().data.gnss_app_ring.cancel(view.lease));
            return false;
        }
        static_cast<void>(ctx().data.gnss_app_ring.commit(view.lease));
        ctx().data.gnss_app_sub.add_tx_sent(view.type);
    }
    return sent_count == kMaxGnssStreamRecordsPerPoll;
}
#endif

core::LinkService &BtLinkModule::link_service() noexcept
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
    const auto system = ctx().data.system_status.read();
    if (!system.bt_ready) {
        return fw::Result::kInvalidState;
    }
    /* 从持久化快照恢复策略，再按依赖顺序原地构造服务。 */
    const auto link = ctx().data.link_status.read();
    link_manager_.init(link.policy, std::span{link.whitelist}.first(link.whitelist_count));
    auto &alarm = alarm_.emplace();
    auto &device_service = device_service_.emplace();
    device_service.info().sn.assign(system.sn.data());
    device_service.info().fw_ver.assign(kAppVersion);
    auto &config_service = config_service_.emplace();
    auto &alarm_service = alarm_service_.emplace(alarm, link_manager_, device_service.info());
    auto &pair_service = pair_service_.emplace(link_manager_);
#if RTK_GNSS_ENABLED
    cached_gnss_fix_.emplace();
    auto &gnss_service = gnss_service_.emplace();
    gnss_service.set_query_hook(&BtLinkModule::on_gnss_query, this);
    gnss_service.set_mask_hook(&BtLinkModule::on_gnss_mask, this);
    gnss_reader_attached_ = fw::isOk(ctx().data.gnss_app_ring.attach(0U, gnss_reader_id_));
    if (!gnss_reader_attached_) {
        FW_LOGE(fw::TraceCat::kLink, "<gnss> app ring attach failed");
        return fw::Result::kInvalidState;
    }
    /* 路由表校验成功后才构造 LinkService，失败由执行器隔离。 */
    const std::array<core::RouteTable, 5> tables = {device_service.table(), config_service.table(),
                                                    alarm_service.table(), pair_service.table(),
                                                    gnss_service.table()};
#else
    /* 路由表校验成功后才构造 LinkService，失败由执行器隔离。 */
    const std::array<core::RouteTable, 4> tables = {device_service.table(), config_service.table(),
                                                    alarm_service.table(), pair_service.table()};
#endif
    const fw::Status route_status = router_.init(tables);
    if (!isOk(route_status)) {
        FW_LOGE(fw::TraceCat::kLink, "command router init failed: %s", toString(route_status));
        return fw::Result::kInvalidState;
    }
    transport_.emplace();
    if (!transport_->supports_credit()) {
        return fw::Result::kInvalidState;
    }
    service_.emplace(*transport_, link_manager_, router_, alarm_service);
    spp_server_set_rx_wake_hook(&BtLinkModule::on_rx_wake, this);
    return fw::Result::kOk;
}

void BtLinkModule::on_rx_wake(void *ctx) noexcept
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

bool BtLinkModule::poll_and_check_ready(uint32_t now_ms) noexcept
{
    spp_server_evt_t evt{};
    for (size_t i = 0; i < SPP_SERVER_EVENT_QUEUE_LEN; ++i) {
        if (!spp_server_poll_event(&evt, 0)) {
            break;
        }
        handle_event(evt, now_ms);
    }
    bool ready = false;
    for (auto &entry : sessions_) {
        if (!entry.used) {
            continue;
        }
        if (link_service().paused(entry.handle)) {
            if (transport_.has_value() && transport_->reply_ready(entry.handle)) {
                ready = true;
            }
            continue;
        }
        if (entry.demux_ready) {
            if (spp_server_rx_ready(entry.handle)) {
                entry.drain_pending = true;
            }
            if (entry.drain_pending || (entry.rx_offset < entry.rx_size)) {
                ready = true;
            }
        }
    }
    return ready;
}

void BtLinkModule::wait(uint32_t max_ms) noexcept
{
    round_start_ms_ = rtos::nowMs();
    if (!service_.has_value()) {
        return;
    }
    wake_task_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    // 先清旧边缘通知，再查队列与水平状态；若仍空闲则复查后阻塞。
    static_cast<void>(ulTaskNotifyTake(pdTRUE, 0U));
    if (!poll_and_check_ready(rtos::nowMs()) && (max_ms > 0U)) {
        if (!poll_and_check_ready(rtos::nowMs())) {
            const TickType_t ticks = pdMS_TO_TICKS(max_ms);
            const TickType_t wait_ticks = (ticks == 0U) ? 1U : ticks;
            static_cast<void>(ulTaskNotifyTake(pdTRUE, wait_ticks));
            static_cast<void>(poll_and_check_ready(rtos::nowMs()));
        }
    }
    round_start_ms_ = rtos::nowMs();
}

bool BtLinkModule::poll(uint32_t /*now_ms*/) noexcept
{
    /* 先消费复位和白名单变化，再按真实时钟推进 tick。 */
    apply_rtcm_reset();
    collect_whitelist_dirty();
    const uint32_t now = rtos::nowMs();
#if RTK_GNSS_ENABLED
    sync_gnss_info(now);
#endif
    if (now - last_tick_ms_ >= kTickPeriodMs) {
        last_tick_ms_ = now;
        link_service().tick(now);
    }
    for (const auto &entry : sessions_) {
        if (entry.used) {
            link_service().resume(entry.handle, now);
        }
    }
    /* 按预算接收并直推 RTCM，再绑定并排空 PHONE GNSS 流，最后发布链路统计。 */
    drain_rx(round_start_ms_);
    sync_stream();
#if RTK_GNSS_ENABLED
    const bool gnss_more = drain_gnss_stream();
#else
    const bool gnss_more = false;
#endif
    collect_whitelist_dirty();
    publish_link(rtos::nowMs());
    bool pending = gnss_more;
    for (const auto &entry : sessions_) {
        pending =
            pending || (entry.used && entry.demux_ready && !link_service().paused(entry.handle) &&
                        (entry.drain_pending || entry.rx_offset < entry.rx_size));
    }
    return pending;
}

void BtLinkModule::set_alarm(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!alarm_.has_value()) {
        alarm_.emplace();
    }
    if (cmd.argLen != kAlarmArgBytes || !alarm_->set_override(cmd.args[0], cmd.args[1])) {
        result.status = fw::Result::kInvalidArg;
    }
}

void BtLinkModule::clear_whitelist(fw::CommandResult &result) noexcept
{
    /* 清除内存白名单并同步持久化，回执携带原后端错误码。 */
    link_manager_.whitelist_clear();
    persist_whitelist(result);
}

void BtLinkModule::persist_whitelist(fw::CommandResult &result) noexcept
{
    /* 控制台改动立即落盘（不走 HELLO 学习的延迟合并），回执携带原后端错误码。 */
    ctx().storage.mark_whitelist_dirty(link_manager_.whitelist(), rtos::nowMs());
    result.detail = ctx().storage.flush_whitelist(rtos::nowMs());
    if (result.detail != ESP_OK) {
        result.status = fw::Result::kIoError;
    }
}

void BtLinkModule::edit_whitelist(const fw::Command &cmd, bool add,
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
        add ? link_manager_.whitelist_add(addr) : link_manager_.whitelist_remove(addr);
    switch (edit) {
    case core::WhitelistEdit::kDone:
        persist_whitelist(result);
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

void BtLinkModule::on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept
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
        set_alarm(cmd, result);
        break;
    case LinkVerb::kAlarmClear:
        if (!alarm_.has_value()) {
            alarm_.emplace();
        }
        alarm_->clear_override();
        break;
    case LinkVerb::kPair:
        if (link_manager_.is_pairing()) {
            result.status = fw::Result::kBusy;
        } else {
            link_manager_.pair_open(rtos::nowMs());
        }
        break;
    case LinkVerb::kWhitelistClear:
        clear_whitelist(result);
        break;
    case LinkVerb::kWhitelistAdd:
        edit_whitelist(cmd, true, result);
        break;
    case LinkVerb::kWhitelistDel:
        edit_whitelist(cmd, false, result);
        break;
    default:
        result.status = fw::Result::kInvalidArg;
        break;
    }
}
} // namespace app
