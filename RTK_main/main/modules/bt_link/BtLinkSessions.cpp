// 链路会话侧：分流的 Sink 回调、会话表、白名单编辑与 RX 排空（ADR-0007 §5、ADR-0011）。
// 启动期由组合根初始化一次；poll 与命令只在 kLink 执行器任务；蓝牙 API 可阻塞，非 ISR 安全。
#include "modules/bt_link/BtLinkModule.hpp"
#include "modules/bt_link/BtLinkModulePrivate.hpp"
#include "AppContext.hpp"
#include "app/PowerRequests.hpp"

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

using detail::kAlarmArgBytes;
using detail::kHandleDisplayMask;
using detail::kMinRtcmMessageBytes;
using detail::kRoundBudgetMs;
using detail::kRtcmMsgShiftBits;
using detail::kRtcmPayloadOffset;

using detail::kAlarmArgBytes;
using detail::kHandleDisplayMask;
using detail::kMinRtcmMessageBytes;
using detail::kRoundBudgetMs;
using detail::kRtcmMsgShiftBits;
using detail::kRtcmPayloadOffset;

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
#if RTK_GNSS_ENABLED
        owner_->onPhoneProductFrame(handle_, wire);
#endif
        if (owner_->linkService().onData(handle_, wire, rtos::nowMs()) == 0U) {
            core::traceProto(core::ProtoTrace::kReplyError, handle_,
                             static_cast<uint32_t>(fw::Result::kBusy), 0U);
        }
#if RTK_GNSS_ENABLED
        owner_->recheckPhoneSessionRole(handle_);
#endif
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
#if RTK_GNSS_ENABLED
    if (ctx().data.gnssWorkMode.read().active) {
        ++rtcm_snapshot_.rejectedMode;
        traceRtcm(RtcmTrace::kReject, handle, static_cast<uint32_t>(RtcmReject::kMode),
                  static_cast<uint32_t>(wire.size()));
        return;
    }
#endif
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

void BtLinkModule::initLinkManager() noexcept
{
    const auto link = ctx().data.linkStatus.read();
    link_manager_.init(link.policy, std::span{link.whitelist}.first(link.whitelistCount));
    core::bindPairWindow(link_manager_, &pair_window_);
    pair_window_.bindWhitelistAdmission(&admitWhitelistChange, this);
}

bool BtLinkModule::admitWhitelistChange(void *context) noexcept
{
    if (!power::s_power_requests.beginSetting()) {
        return false;
    }
    // 由本模块绑定 this，kLink 在同轮交给 Storage 后结束登记。
    auto &self = *static_cast<BtLinkModule *>(context);
    ++self.whitelistAdmissions_;
    return true;
}

void BtLinkModule::collectWhitelistDirty() noexcept
{
    if (linkService().consumeWhitelistDirty()) {
        ctx().storage.markWhitelistDirty(link_manager_.whitelist(), rtos::nowMs());
    }
    while (whitelistAdmissions_ != 0U) {
        --whitelistAdmissions_;
        power::s_power_requests.endSetting();
    }
}

void BtLinkModule::handleOpen(const spp_server_evt_t &evt, uint32_t nowMs) noexcept
{
    if (powerPaused_) {
        const auto err = spp_server_disconnect(evt.handle);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            FW_LOGW(fw::TraceCat::kLink, "power BT late open disconnect failed: %s",
                    esp_err_to_name(err));
        }
        return;
    }
    core::BtAddr addr{};
    for (size_t i = 0; i < core::kAddrLen; ++i) {
        addr.bytes[i] = evt.addr[i];
    }
    /* 会话元数据必须先于 LinkService 的 OPEN 可见。 */
    sessionRecord(evt.handle, addr);
    linkService().onOpen(evt.handle, addr, nowMs);
#if RTK_GNSS_ENABLED
    if (link_manager_.role(evt.handle) == core::Role::kPhone) {
        onPhoneSessionOpen(evt.handle);
    }
#endif
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
#if RTK_GNSS_ENABLED
    if (handle == phone_session_handle_) {
        resetGnssStreamSession();
    }
#endif
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
        ctx().data.phoneStreamReady.store(false, std::memory_order_release);
        ctx().data.baseRtcmRing.clearFrames();
        if (phone_session_handle_ == stream_handle_) {
            resetGnssStreamSession();
        }
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
#if RTK_GNSS_ENABLED
                    ctx().data.phoneStreamReady.store(true, std::memory_order_release);
#endif
                }
                // 旧 generation 的 copy/API 未退出时容量不可用，下轮重查，不另建 stream 或同步回退。
                return;
            }
        }
    }
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

} // namespace app
