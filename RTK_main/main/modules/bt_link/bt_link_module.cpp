// 链路单一所有者；RX 调度保持 256 B / 20 ms / 每轮一个产品帧与 64 B 跨轮预读。
// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "modules/bt_link/bt_link_module.hpp"
#include "app_context.hpp"

#include <algorithm>
#include <cstdlib>
#include <cinttypes>
#include <cstring>
#include "esp_log.h"
#include "rtk/core/link_service.hpp"
#include "rtk/os/esp_status.hpp"

namespace rtk::app {
namespace {
constexpr const char *kTag = "rtk_app";
constexpr size_t kRxBudgetBytes = 256;
constexpr uint32_t kRoundBudgetMs = 20;
constexpr size_t kAlarmArgBytes = 2;
} // namespace

void BtLinkModule::SessionSink::bind(BtLinkModule &owner, core::SessionHandle handle) noexcept
{
    owner_ = &owner;
    handle_ = handle;
    count_ = cursor_ = size_ = offset_ = 0;
    healthy_ = true;
}

void BtLinkModule::SessionSink::on_frame(const demux::Decoder &decoder, uint8_t index,
                                         std::span<const uint8_t> wire) noexcept
{
    const bool known = (index == kProductIndex && &decoder == &proto::demux_frame::kDecoder) ||
                       (index == kRtcmIndex && &decoder == &demux::rtcm3::kDecoder);
    if (!known || count_ == frames_.size() || wire.size() > bytes_.size() - size_) {
        healthy_ = false;
        ESP_LOGE(kTag, "demux delivery queue invalid, handle=0x%08" PRIx32, handle_);
        return;
    }
    /* 保存当前 feed 的完整帧，交给轮次预算逐个投递。 */
    std::memcpy(std::span{bytes_}.subspan(size_).data(), wire.data(), wire.size());
    // 已校验线路帧不超过 1029 B，收窄无损。
    frames_[count_++] = PendingFrame{static_cast<uint16_t>(wire.size()), index};
    size_ += wire.size();
}

bool BtLinkModule::SessionSink::dispatch_one() noexcept
{
    /* 先取出帧并推进游标，全部消费后立即复用队列。 */
    const PendingFrame info = frames_[cursor_++];
    const auto wire = std::span<const uint8_t>{bytes_}.subspan(offset_, info.size);
    offset_ += info.size;
    if (cursor_ == count_) {
        count_ = cursor_ = size_ = offset_ = 0;
    }
    /* 产品帧消耗本轮唯一预算，RTCM 仅收集手机角色的帧。 */
    if (info.index == kProductIndex) {
        owner_->link_service().on_data(handle_, wire, os::now_ms());
        return true;
    }
    if (owner_->link_manager_.role(handle_) != core::Role::kPhone) {
        ++owner_->rtcm_rejected_;
    } else if (wire.size() <= owner_->rtcm_batch_.size() - owner_->rtcm_size_) {
        std::memcpy(std::span{owner_->rtcm_batch_}.subspan(owner_->rtcm_size_).data(), wire.data(), wire.size());
        owner_->rtcm_size_ += wire.size();
    } else {
        // 两会话遗留帧 + 每轮 256 B 的界限被破坏；不允许静默覆盖。
        ESP_LOGE(kTag, "RTCM batch capacity violated");
        healthy_ = false;
    }
    return false;
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
    /* CLOSE 后完成统计；不再占用正常 RX 的产品帧预算。 */
    while (entry->sink.pending()) {
        static_cast<void>(entry->sink.dispatch_one());
    }
    if (entry->demux_ready) {
        const auto error = entry->demux.abort();
        if (error != demux::Error::kOk) {
            ESP_LOGE(kTag, "demux abort failed");
        }
        while (entry->sink.pending()) {
            static_cast<void>(entry->sink.dispatch_one());
        }
    }
    /* 累计退役统计后复位会话，避免重用句柄携带半帧。 */
    const auto &stats = entry->demux.stats();
    retired_demux_.unknown_bytes += stats.unknown_bytes;
    retired_demux_.product_rejects += stats.decoder[kProductIndex].rejects;
    retired_demux_.rtcm_rejects += stats.decoder[kRtcmIndex].rejects;
    entry->used = false;
    entry->handle = 0;
    entry->addr = {};
    entry->drain_pending = false;
    entry->rx_offset = entry->rx_size = 0;
    entry->demux_ready = false;
    entry->sink.bind(*this, 0);
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
        entry.sink.bind(*this, handle);
        entry.demux_ready = entry.demux.init(entry.sink) == demux::Error::kOk;
        if (!entry.demux_ready) {
            ESP_LOGE(kTag, "demux init failed");
        }
        return;
    }
    ESP_LOGE(kTag, "session map full");
}

void BtLinkModule::collect_whitelist_dirty() noexcept
{
    if (link_service().consume_whitelist_dirty()) {
        context_.storage.mark_whitelist_dirty(link_manager_.whitelist(), os::now_ms());
    }
}

void BtLinkModule::handle_open(const rtk_bt_spp_evt_t &evt, uint32_t now_ms) noexcept
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
    link_service().on_close(handle);
    session_forget(handle);
}

void BtLinkModule::handle_data(core::SessionHandle handle) noexcept
{
    if (auto *entry = session_find(handle); entry != nullptr && entry->demux_ready) {
        entry->drain_pending = true;
    }
}

void BtLinkModule::handle_event(const rtk_bt_spp_evt_t &evt, uint32_t now_ms) noexcept
{
    switch (evt.type) {
    case RTK_BT_SPP_EVT_OPEN:
        handle_open(evt, now_ms);
        break;
    case RTK_BT_SPP_EVT_CLOSE:
        handle_close(evt.handle);
        break;
    case RTK_BT_SPP_EVT_DATA:
        handle_data(evt.handle);
        break;
    case RTK_BT_SPP_EVT_RX_DROP:
        link_service().on_rx_dropped(evt.dropped);
        break;
    default:
        break;
    }
}

bool BtLinkModule::feed_one(SessionEntry &entry, size_t &budget, uint32_t round_start_ms) noexcept
{
    /* 只在预读耗尽时读取，空读结束此会话本轮处理。 */
    if (entry.rx_offset == entry.rx_size) {
        entry.rx_size = rtk_bt_spp_read(entry.handle, entry.rx.data(), entry.rx.size());
        entry.rx_offset = 0;
        if (entry.rx_size == 0U) {
            entry.drain_pending = false;
            return false;
        }
    }
    // 非阻塞 read 也可能被抢占，过期时保留整个预读结果。
    if (os::now_ms() - round_start_ms >= kRoundBudgetMs) {
        return true;
    }
    /* 逐字节 feed 保留基线的帧投递和预算边界。 */
    const auto error = entry.demux.feed(std::span<const uint8_t>{entry.rx}.subspan(entry.rx_offset, 1));
    ++entry.rx_offset;
    --budget;
    if (error != demux::Error::kOk || !entry.sink.healthy()) {
        entry.demux_ready = false;
        entry.drain_pending = false;
        ESP_LOGE(kTag, "demux feed failed");
        return true;
    }
    return false;
}

bool BtLinkModule::drain_session(SessionEntry &entry, size_t index, size_t &budget, uint32_t round_start_ms) noexcept
{
    while (entry.used && entry.demux_ready && (entry.drain_pending || entry.sink.pending()) && budget > 0U) {
        if (os::now_ms() - round_start_ms >= kRoundBudgetMs) {
            return true;
        }
        // 时间耗尽前不推进起点，避免跳过上轮让出的会话。
        rx_cursor_ = (index + 1U) % kMaxSessions;
        if (entry.sink.pending()) {
            if (entry.sink.dispatch_one()) {
                return true;
            }
            continue;
        }
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
    context_.data.link_status.write(link);
    Sessions sessions{};
    for (const auto &entry : sessions_) {
        const auto role = link_manager_.role(entry.handle);
        if (entry.used && role != core::Role::kNone && sessions.count < kMaxSessions) {
            sessions.entries[sessions.count++] = {true, entry.handle, role, entry.addr};
        }
    }
    context_.data.sessions.write(sessions);
    context_.data.alarm_state.write({alarm_.state(), alarm_.level(), alarm_.is_override()});
    /* 继续发布发送、心跳与分流统计，顺序不变。 */
    LinkStats stats{};
    static_cast<core::LinkStats &>(stats) = link_service().stats(); // 派生结构的基类赋值，无窄化。
    stats.parser_crc_errors = link_service().parser_crc_errors();
    stats.parser_length_errors = link_service().parser_length_errors();
    const AsyncTxStats tx = async_tx_.stats();
    stats.tx_sent = tx.sent;
    stats.tx_failed = tx.failed;
    stats.tx_queue_full = tx.queue_full;
    stats.tx_max_depth = tx.max_depth;
    stats.tx_stack_free = tx.stack_free;
    context_.data.link_stats.write(stats);
    const auto heartbeat = link_service().last_heartbeat();
    context_.data.heartbeat.write({heartbeat.has_value(), heartbeat.value_or(proto::alarm::Heartbeat{})});
    DemuxStats demux = retired_demux_;
    for (const auto &entry : sessions_) {
        if (entry.used) {
            const auto &counts = entry.demux.stats();
            demux.unknown_bytes += counts.unknown_bytes;
            demux.product_rejects += counts.decoder[kProductIndex].rejects;
            demux.rtcm_rejects += counts.decoder[kRtcmIndex].rejects;
        }
    }
    context_.data.demux_stats.write(demux);
}

void BtLinkModule::apply_rtcm_reset() noexcept
{
    uint32_t reset_version = 0;
    const uint32_t reset = context_.data.rtcm_reset.read(reset_version);
    if (reset_version != reset_version_) {
        reset_version_ = reset_version;
        retired_demux_ = {};
        rtcm_size_ = 0;
        rtcm_rejected_ = 0;
        for (auto &entry : sessions_) {
            entry.demux.reset_stats();
        }
    }
    // 载荷仅登记世代；实际变化由 Slot version 判定。
    static_cast<void>(reset);
}

core::LinkService &BtLinkModule::link_service() noexcept
{
    // 执行器只在 init 成功后调用业务回调；SessionSink 同样仅在该生命周期内分发。
    if (!service_.has_value()) {
        std::abort();
    }
    return *service_;
}

fw::Result BtLinkModule::init() noexcept
{
    const auto system = context_.data.system_status.read();
    if (!system.bt_ready) {
        return fw::Result::kInvalidState;
    }
    /* 从持久化快照恢复策略，再按依赖顺序原地构造服务。 */
    const auto link = context_.data.link_status.read();
    link_manager_.init(link.policy, std::span{link.whitelist}.first(link.whitelist_count));
    device_service_.emplace();
    device_service_->info().sn.assign(system.sn.data());
    device_service_->info().fw_ver.assign(kAppVersion);
    config_service_.emplace();
    alarm_service_.emplace(alarm_, link_manager_, device_service_->info());
    pair_service_.emplace(link_manager_);
    /* 路由表校验成功后才构造 LinkService，失败由执行器隔离。 */
    const std::array<core::RouteTable, 4> tables = {device_service_->table(), config_service_->table(),
                                                    alarm_service_->table(), pair_service_->table()};
    const Status route_status = router_.init(tables);
    if (!is_ok(route_status)) {
        ESP_LOGE(kTag, "command router init failed: %s", to_string(route_status));
        return fw::Result::kInvalidState;
    }
    service_.emplace(async_tx_, link_manager_, router_, *alarm_service_);
    return fw::Result::kOk;
}

Status BtLinkModule::start_tx() noexcept
{
    // 与 BT 驱动发送任务同核同优先级：两者都是“等事件 → 短暂处理”，不抢链路核。
    constexpr uint32_t kTxPriority = 5;
    constexpr BaseType_t kTxCore = 0;
    return async_tx_.start(kTxPriority, kTxCore);
}

void BtLinkModule::wait(uint32_t max_ms) noexcept
{
    /* 上轮 RTCM 已由 rtcm.poll 消费，此时才复用批次。 */
    rtcm_size_ = 0;
    rtcm_rejected_ = 0;
    round_start_ms_ = os::now_ms();
    if (!service_.has_value()) {
        return;
    }
    /* 先等首个事件，再按原队列上限处理已排队的事件。 */
    rtk_bt_spp_evt_t evt{};
    if (rtk_bt_spp_poll_event(&evt, max_ms)) {
        handle_event(evt, os::now_ms());
        for (size_t i = 1; i < RTK_BT_SPP_EVENT_QUEUE_LEN; ++i) {
            if (!rtk_bt_spp_poll_event(&evt, 0)) {
                break;
            }
            handle_event(evt, os::now_ms());
        }
    }
    round_start_ms_ = os::now_ms();
}

bool BtLinkModule::poll(uint32_t /*now_ms*/) noexcept
{
    /* 先消费复位和白名单变化，再按真实时钟推进 tick。 */
    apply_rtcm_reset();
    collect_whitelist_dirty();
    const uint32_t now = os::now_ms();
    if (now - last_tick_ms_ >= kTickPeriodMs) {
        last_tick_ms_ = now;
        link_service().tick(now);
    }
    /* 按预算接收并保留原快照、RTCM 批次的发布顺序。 */
    drain_rx(round_start_ms_);
    collect_whitelist_dirty();
    publish_link(os::now_ms());
    context_.data.rtcm_batch.write({std::span<const uint8_t>{rtcm_batch_}.first(rtcm_size_), rtcm_rejected_});
    bool pending = false;
    for (const auto &entry : sessions_) {
        pending = pending || (entry.used && entry.demux_ready &&
                              (entry.drain_pending || entry.sink.pending() || entry.rx_offset < entry.rx_size));
    }
    return pending;
}

void BtLinkModule::set_alarm(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (cmd.arg_len != kAlarmArgBytes || !alarm_.set_override(cmd.args[0], cmd.args[1])) {
        result.status = fw::Result::kInvalidArg;
    }
}

void BtLinkModule::clear_whitelist(fw::CommandResult &result) noexcept
{
    /* 清除内存白名单并同步持久化，回执携带原后端错误码。 */
    link_manager_.whitelist_clear();
    context_.storage.mark_whitelist_dirty(link_manager_.whitelist(), os::now_ms());
    result.detail = context_.storage.flush_whitelist(os::now_ms());
    if (result.detail != ESP_OK) {
        result.status = fw::Result::kIoError;
    }
}

void BtLinkModule::on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    result.status = fw::Result::kOk;
    if (cmd.arg_len > cmd.args.size()) {
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
        alarm_.clear_override();
        break;
    case LinkVerb::kPair:
        link_manager_.pair_open(os::now_ms());
        break;
    case LinkVerb::kWhitelistClear:
        clear_whitelist(result);
        break;
    default:
        result.status = fw::Result::kInvalidArg;
        break;
    }
}
} // namespace rtk::app
