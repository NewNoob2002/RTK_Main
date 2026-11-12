// 链路单一所有者；RX 调度保持 256 B / 20 ms / 每轮一个产品帧与 64 B 跨轮预读。
// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "bt_link_module.hpp"
#include "storage_module.hpp"
#include "app_version.hpp"
#include "async_tx.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <new>
#include "demux/decoders/rtcm3.hpp"
#include "demux/static_demux.hpp"
#include "esp_log.h"
#include "rtk/bt/spp_transport.hpp"
#include "rtk/core/alarm_source.hpp"
#include "rtk/core/command_router.hpp"
#include "rtk/core/link_manager.hpp"
#include "rtk/core/services/alarm_service.hpp"
#include "rtk/core/services/config_service.hpp"
#include "rtk/core/services/device_service.hpp"
#include "rtk/core/services/pair_service.hpp"
#include "rtk/os/esp_status.hpp"
#include "rtk/proto/demux_frame.hpp"
#include "rtk_bt_spp.h"

namespace rtk::app {
constinit BtLinkModule s_bt_link_module{};
namespace {
constexpr const char *kTag = "rtk_app";
constexpr size_t kRxChunkBytes = 64;
constexpr size_t kRxBudgetBytes = 256;
constexpr uint32_t kRoundBudgetMs = 20;
using SessionDemux = demux::StaticDemux<proto::demux_frame::kDecoder, demux::rtcm3::kDecoder>;
constexpr uint8_t kProductIndex = SessionDemux::index_of<proto::demux_frame::kDecoder>();
constexpr uint8_t kRtcmIndex = SessionDemux::index_of<demux::rtcm3::kDecoder>();

class SessionSink final : public demux::Sink {
public:
    constexpr SessionSink() noexcept = default;
    // 仅 kLink，非 ISR；OPEN 时复位元数据。
    void bind(core::SessionHandle handle) noexcept
    {
        handle_ = handle;
        count_ = cursor_ = size_ = offset_ = 0;
        healthy_ = true;
    }
    void on_frame(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> wire) noexcept override;
    [[nodiscard]] bool dispatch_one() noexcept;
    [[nodiscard]] bool pending() const noexcept
    {
        return cursor_ < count_;
    }
    [[nodiscard]] bool healthy() const noexcept
    {
        return healthy_;
    }

private:
    struct PendingFrame {
        uint16_t size = 0;
        uint8_t index = 0;
    };
    static constexpr size_t kMinWireFrame = 6;
    std::array<uint8_t, SessionDemux::kBufSize> bytes_{};
    std::array<PendingFrame, SessionDemux::kBufSize / kMinWireFrame> frames_{};
    core::SessionHandle handle_ = 0;
    size_t count_ = 0;
    size_t cursor_ = 0;
    size_t size_ = 0;
    size_t offset_ = 0;
    bool healthy_ = true;
};
struct SessionEntry {
    bool used = false;
    core::SessionHandle handle = 0;
    core::BtAddr addr{};
    bool drain_pending = false;
    std::array<uint8_t, kRxChunkBytes> rx{};
    size_t rx_offset = 0;
    size_t rx_size = 0;
    SessionSink sink{};
    SessionDemux demux{};
    bool demux_ready = false;
};
constinit DemuxStats s_retired_demux{};
constinit core::LinkManager s_link_manager{};
constinit core::AlarmSource s_alarm{};
constinit bt::SppTransport s_transport{};
// LinkService 只看到异步装饰器：回复入队即返回，由 rtk_tx 任务等待 WRITE_EVT（J5 §1）。
constinit AsyncTxTransport s_async_tx{s_transport};
using ServiceStorage = std::array<std::byte, sizeof(core::LinkService)>;
alignas(core::LinkService) constinit ServiceStorage s_service_storage{};

// 四个服务的构造函数不是 constexpr（冻结头未提供），因此与 LinkService 一样用静态存储 + init 时
// placement new，避免依赖动态初始化顺序；生命周期覆盖整个运行期。
template <typename T> struct Storage {
    alignas(T) std::array<std::byte, sizeof(T)> bytes{};
};
constinit Storage<core::services::DeviceService> s_device_storage{};
constinit Storage<core::services::ConfigService> s_config_storage{};
constinit Storage<core::services::AlarmService> s_alarm_storage{};
constinit Storage<core::services::PairService> s_pair_storage{};
constinit core::services::DeviceService *s_device_service = nullptr;
constinit core::services::ConfigService *s_config_service = nullptr;
constinit core::services::AlarmService *s_alarm_service = nullptr;
constinit core::services::PairService *s_pair_service = nullptr;
// 路由器默认构造是 constexpr，且 init 只调用一次，直接静态构造。
constinit core::CommandRouter s_router{};
constinit core::LinkService *s_service = nullptr;
constinit std::array<SessionEntry, kMaxSessions> s_sessions{};
constinit size_t s_rx_cursor = 0;
constinit std::array<uint8_t, kRtcmBatchBytes> s_rtcm_batch{};
constinit size_t s_rtcm_size = 0;
constinit uint32_t s_rtcm_rejected = 0;

void SessionSink::on_frame(const demux::Decoder &decoder, uint8_t index, std::span<const uint8_t> wire) noexcept
{
    const bool known = (index == kProductIndex && &decoder == &proto::demux_frame::kDecoder) ||
                       (index == kRtcmIndex && &decoder == &demux::rtcm3::kDecoder);
    if (!known || count_ == frames_.size() || wire.size() > bytes_.size() - size_) {
        healthy_ = false;
        ESP_LOGE(kTag, "demux delivery queue invalid, handle=0x%08" PRIx32, handle_);
        return;
    }
    std::memcpy(std::span{bytes_}.subspan(size_).data(), wire.data(), wire.size());
    // 已校验线路帧不超过 1029 B，收窄无损。
    frames_[count_++] = PendingFrame{static_cast<uint16_t>(wire.size()), index};
    size_ += wire.size();
}

bool SessionSink::dispatch_one() noexcept
{
    const PendingFrame info = frames_[cursor_++];
    const auto wire = std::span<const uint8_t>{bytes_}.subspan(offset_, info.size);
    offset_ += info.size;
    if (cursor_ == count_) {
        count_ = cursor_ = size_ = offset_ = 0;
    }
    if (info.index == kProductIndex) {
        s_service->on_data(handle_, wire, os::now_ms());
        return true;
    }
    if (s_link_manager.role(handle_) != core::Role::kPhone) {
        ++s_rtcm_rejected;
    } else if (wire.size() <= s_rtcm_batch.size() - s_rtcm_size) {
        std::memcpy(std::span{s_rtcm_batch}.subspan(s_rtcm_size).data(), wire.data(), wire.size());
        s_rtcm_size += wire.size();
    } else {
        // 两会话遗留帧 + 每轮 256 B 的界限被破坏；不允许静默覆盖。
        ESP_LOGE(kTag, "RTCM batch capacity violated");
        healthy_ = false;
    }
    return false;
}

SessionEntry *session_find(core::SessionHandle handle) noexcept
{
    for (auto &entry : s_sessions) {
        if (entry.used && entry.handle == handle) {
            return &entry;
        }
    }
    return nullptr;
}

void session_forget(core::SessionHandle handle) noexcept
{
    auto *entry = session_find(handle);
    if (entry == nullptr) {
        return;
    }
    while (entry->sink.pending()) {
        // CLOSE 后仅完成统计，不再占用正常 RX 产品帧预算。
        static_cast<void>(entry->sink.dispatch_one());
    }
    if (entry->demux_ready) {
        const auto error = entry->demux.abort();
        if (error != demux::Error::kOk) {
            ESP_LOGE(kTag, "demux abort failed");
        }
        while (entry->sink.pending()) {
            // 同上，实际分发副作用仍必须执行。
            static_cast<void>(entry->sink.dispatch_one());
        }
    }
    const auto &stats = entry->demux.stats();
    s_retired_demux.unknown_bytes += stats.unknown_bytes;
    s_retired_demux.product_rejects += stats.decoder[kProductIndex].rejects;
    s_retired_demux.rtcm_rejects += stats.decoder[kRtcmIndex].rejects;
    entry->used = false;
    entry->handle = 0;
    entry->addr = {};
    entry->drain_pending = false;
    entry->rx_offset = entry->rx_size = 0;
    entry->demux_ready = false;
    entry->sink.bind(0);
}

void session_record(core::SessionHandle handle, const core::BtAddr &addr) noexcept
{
    if (session_find(handle) != nullptr) {
        return;
    }
    for (auto &entry : s_sessions) {
        if (!entry.used) {
            entry.used = true;
            entry.handle = handle;
            entry.addr = addr;
            entry.drain_pending = false;
            entry.rx_offset = entry.rx_size = 0;
            entry.sink.bind(handle);
            entry.demux_ready = entry.demux.init(entry.sink) == demux::Error::kOk;
            if (!entry.demux_ready) {
                ESP_LOGE(kTag, "demux init failed");
            }
            return;
        }
    }
    ESP_LOGE(kTag, "session map full");
}
void collect_whitelist_dirty() noexcept
{
    if (s_service->consume_whitelist_dirty()) {
        mark_whitelist_dirty(s_link_manager.whitelist(), os::now_ms());
    }
}
void handle_event(const rtk_bt_spp_evt_t &evt, uint32_t now_ms) noexcept
{
    switch (evt.type) {
    case RTK_BT_SPP_EVT_OPEN: {
        core::BtAddr addr{};
        for (size_t i = 0; i < core::kAddrLen; ++i) {
            addr.bytes[i] = evt.addr[i];
        }
        session_record(evt.handle, addr);
        s_service->on_open(evt.handle, addr, now_ms);
        break;
    }
    case RTK_BT_SPP_EVT_CLOSE:
        s_service->on_close(evt.handle);
        session_forget(evt.handle);
        break;
    case RTK_BT_SPP_EVT_DATA:
        if (auto *entry = session_find(evt.handle); entry != nullptr && entry->demux_ready) {
            entry->drain_pending = true;
        }
        break;
    case RTK_BT_SPP_EVT_RX_DROP:
        s_service->on_rx_dropped(evt.dropped);
        break;
    default:
        break;
    }
}
void drain_rx(uint32_t round_start_ms) noexcept
{
    size_t budget = kRxBudgetBytes;
    const size_t first = s_rx_cursor;
    for (size_t step = 0; step < kMaxSessions && budget > 0U; ++step) {
        const size_t index = (first + step) % kMaxSessions;
        auto &entry = s_sessions[index];
        while (entry.used && entry.demux_ready && (entry.drain_pending || entry.sink.pending()) && budget > 0U) {
            if (os::now_ms() - round_start_ms >= kRoundBudgetMs) {
                return;
            }
            // 时间耗尽前不推进起点，避免跳过上轮让出的会话。
            s_rx_cursor = (index + 1U) % kMaxSessions;
            if (entry.sink.pending()) {
                if (entry.sink.dispatch_one()) {
                    return;
                }
                continue;
            }
            if (entry.rx_offset == entry.rx_size) {
                entry.rx_size = rtk_bt_spp_read(entry.handle, entry.rx.data(), entry.rx.size());
                entry.rx_offset = 0;
                if (entry.rx_size == 0U) {
                    entry.drain_pending = false;
                    break;
                }
            }
            // 非阻塞 read 也可能被抢占，过期时保留整个预读结果。
            if (os::now_ms() - round_start_ms >= kRoundBudgetMs) {
                return;
            }
            const auto error = entry.demux.feed(std::span<const uint8_t>{entry.rx}.subspan(entry.rx_offset, 1));
            ++entry.rx_offset;
            --budget;
            if (error != demux::Error::kOk || !entry.sink.healthy()) {
                entry.demux_ready = false;
                entry.drain_pending = false;
                ESP_LOGE(kTag, "demux feed failed");
                return;
            }
        }
    }
}
void publish_link(uint32_t now_ms) noexcept
{
    LinkStatus link{};
    link.policy = policy_from_config();
    link.pairing = s_link_manager.is_pairing();
    link.snapshot_ms = now_ms;
    const auto list = s_link_manager.whitelist();
    link.whitelist_count = std::min(list.size(), kMaxAlarms);
    std::copy_n(list.begin(), link.whitelist_count, link.whitelist.begin());
    data::kLinkStatus.write(link);
    Sessions sessions{};
    for (const auto &entry : s_sessions) {
        const auto role = s_link_manager.role(entry.handle);
        if (entry.used && role != core::Role::kNone && sessions.count < kMaxSessions) {
            sessions.entries[sessions.count++] = {true, entry.handle, role, entry.addr};
        }
    }
    data::kSessions.write(sessions);
    data::kAlarmState.write({s_alarm.state(), s_alarm.level(), s_alarm.is_override()});
    LinkStats stats{};
    static_cast<core::LinkStats &>(stats) = s_service->stats(); // 派生结构的基类赋值，无窄化。
    stats.parser_crc_errors = s_service->parser_crc_errors();
    stats.parser_length_errors = s_service->parser_length_errors();
    const AsyncTxStats tx = s_async_tx.stats();
    stats.tx_sent = tx.sent;
    stats.tx_failed = tx.failed;
    stats.tx_queue_full = tx.queue_full;
    stats.tx_max_depth = tx.max_depth;
    stats.tx_stack_free = tx.stack_free;
    data::kLinkStats.write(stats);
    const auto heartbeat = s_service->last_heartbeat();
    data::kHeartbeat.write({heartbeat.has_value(), heartbeat.value_or(proto::alarm::Heartbeat{})});
    DemuxStats demux = s_retired_demux;
    for (const auto &entry : s_sessions) {
        if (entry.used) {
            const auto &counts = entry.demux.stats();
            demux.unknown_bytes += counts.unknown_bytes;
            demux.product_rejects += counts.decoder[kProductIndex].rejects;
            demux.rtcm_rejects += counts.decoder[kRtcmIndex].rejects;
        }
    }
    data::kDemuxStats.write(demux);
}
void apply_rtcm_reset(BtLinkModule &module) noexcept
{
    uint32_t reset_version = 0;
    const uint32_t reset = data::kRtcmReset.read(reset_version);
    if (reset_version != module.reset_version) {
        module.reset_version = reset_version;
        s_retired_demux = {};
        s_rtcm_size = 0;
        s_rtcm_rejected = 0;
        for (auto &entry : s_sessions) {
            entry.demux.reset_stats();
        }
    }
    // 载荷仅登记世代；实际变化由 Slot version 判定。
    static_cast<void>(reset);
}
} // namespace

::app::Result bt_link_init(void * /*self*/) noexcept
{
    const auto system = data::kSystemStatus.read();
    if (!system.bt_ready) {
        return ::app::Result::kInvalidState;
    }
    const auto link = data::kLinkStatus.read();
    s_link_manager.init(link.policy, std::span{link.whitelist}.first(link.whitelist_count));
    // DeviceService 必须先于 AlarmService 构造：后者持有前者的 DeviceInfo 引用。
    s_device_service = new (s_device_storage.bytes.data()) core::services::DeviceService();
    s_device_service->info().sn.assign(system.sn.data());
    s_device_service->info().fw_ver.assign(kAppVersion);
    s_config_service = new (s_config_storage.bytes.data()) core::services::ConfigService();
    s_alarm_service = new (s_alarm_storage.bytes.data())
        core::services::AlarmService(s_alarm, s_link_manager, s_device_service->info());
    s_pair_service = new (s_pair_storage.bytes.data()) core::services::PairService(s_link_manager);

    const std::array<core::RouteTable, 4> tables = {s_device_service->table(), s_config_service->table(),
                                                    s_alarm_service->table(), s_pair_service->table()};
    const Status route_status = s_router.init(tables);
    if (!is_ok(route_status)) {
        // msg_id 重复/表非法/容量不足属程序错误：与其它 init 失败一样隔离本模块并留日志。
        ESP_LOGE(kTag, "command router init failed: %s", to_string(route_status));
        return ::app::Result::kInvalidState;
    }
    s_service =
        new (s_service_storage.data()) core::LinkService(s_async_tx, s_link_manager, s_router, *s_alarm_service);
    return ::app::Result::kOk;
}

Status bt_link_start_tx() noexcept
{
    // 与 BT 驱动发送任务同核同优先级：两者都是“等事件 → 短暂处理”，不抢链路核。
    constexpr uint32_t kTxPriority = 5;
    constexpr BaseType_t kTxCore = 0;
    return s_async_tx.start(kTxPriority, kTxCore);
}

void bt_link_wait(void * /*ctx*/, uint32_t max_ms) noexcept
{
    // 上轮 RTCM 已由 rtcm.poll 消费，此时才可复用其静态批次存储。
    s_rtcm_size = 0;
    s_rtcm_rejected = 0;
    s_bt_link_module.round_start_ms = os::now_ms();
    if (s_service == nullptr) {
        return;
    }
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
    s_bt_link_module.round_start_ms = os::now_ms();
}

bool bt_link_poll(void *self, uint32_t /*now_ms*/) noexcept
{
    // ModuleOps 固定绑定 BtLinkModule，且只在 kLink 调用。
    auto &module = *static_cast<BtLinkModule *>(self);
    apply_rtcm_reset(module);
    collect_whitelist_dirty();
    const uint32_t now = os::now_ms();
    if (now - module.last_tick_ms >= kTickPeriodMs) {
        module.last_tick_ms = now;
        s_service->tick(now);
    }
    drain_rx(module.round_start_ms);
    collect_whitelist_dirty();
    publish_link(os::now_ms());
    data::kRtcmBatch.write({std::span<const uint8_t>{s_rtcm_batch}.first(s_rtcm_size), s_rtcm_rejected});
    bool pending = false;
    for (const auto &entry : s_sessions) {
        pending = pending || (entry.used && entry.demux_ready &&
                              (entry.drain_pending || entry.sink.pending() || entry.rx_offset < entry.rx_size));
    }
    return pending;
}

void bt_link_command(void * /*self*/, const ::app::Command &cmd, ::app::CommandResult &result) noexcept
{
    result.status = ::app::Result::kOk;
    if (cmd.arg_len > cmd.args.size()) {
        result.status = ::app::Result::kInvalidArg;
        return;
    }
    // verb 来源为 uint8_t，LinkVerb 底层类型相同；未知值由 default 拒绝。
    switch (static_cast<LinkVerb>(cmd.verb)) {
    case LinkVerb::kAlarmSet:
        if (cmd.arg_len != 2U || !s_alarm.set_override(cmd.args[0], cmd.args[1])) {
            result.status = ::app::Result::kInvalidArg;
        }
        break;
    case LinkVerb::kAlarmClear:
        s_alarm.clear_override();
        break;
    case LinkVerb::kPair:
        s_link_manager.pair_open(os::now_ms());
        break;
    case LinkVerb::kWhitelistClear:
        s_link_manager.whitelist_clear();
        mark_whitelist_dirty(s_link_manager.whitelist(), os::now_ms());
        result.detail = flush_whitelist(os::now_ms());
        if (result.detail != ESP_OK) {
            result.status = ::app::Result::kIoError;
        }
        break;
    default:
        result.status = ::app::Result::kInvalidArg;
        break;
    }
}
} // namespace rtk::app
