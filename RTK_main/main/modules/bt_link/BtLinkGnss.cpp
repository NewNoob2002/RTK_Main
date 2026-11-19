// 链路 GNSS 侧：RTCM 统计复位、固件/位置信息同步与 GNSS 流下行（ADR-0012 §2.7）。
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

#if RTK_GNSS_ENABLED
using detail::kGnssClockJitterMs;
using detail::kGnssFixStaleMs;
using detail::kMaxGnssFlushRecords;
using detail::kMaxGnssStreamRecordsPerPoll;

// PHONE 会话连接即推的默认 NMEA 掩码（P1 Rev B）：GGA/GST/RMC/GSA/GSV。
constexpr uint32_t kDefaultNmeaPushMask = (1U << static_cast<uint32_t>(::gnss::OutputMsg::kGga)) |
                                          (1U << static_cast<uint32_t>(::gnss::OutputMsg::kGst)) |
                                          (1U << static_cast<uint32_t>(::gnss::OutputMsg::kRmc)) |
                                          (1U << static_cast<uint32_t>(::gnss::OutputMsg::kGsa)) |
                                          (1U << static_cast<uint32_t>(::gnss::OutputMsg::kGsv));
#endif

void BtLinkModule::applyRtcmReset() noexcept
{
    uint32_t resetVersion = 0;
    const uint32_t reset = ctx().data.rtcmReset.read(resetVersion);
    if (resetVersion != reset_version_) {
        reset_version_ = resetVersion;
        retired_demux_ = {};
        rtcm_snapshot_ = {};
        rtcm_snapshot_.baseTxFrames = 0U;
        rtcm_snapshot_.baseTxBytes = 0U;
#if RTK_GNSS_ENABLED
        base_rtcm_tx_drop_ = 0U;
        base_rtcm_ring_drop_base_ = ctx().data.baseRtcmRing.dropFrames();
#endif
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
            gnss_supported_known_ = true;
            gnss_supported_mask_ = gnssSt.supportedMask;
        }
        if (gnssSt.fwVersion[0] != '\0') {
            deviceInfo.boardFw.assign(std::string_view{gnssSt.fwVersion.data()});
        }
        if (gnss_service_.has_value()) {
            gnss_service_->setSupportedMask(gnssSt.supportedMask);
        }
        if (gnss_default_push_) {
            applyGnssStreamMask();
        }
    }
}

uint32_t BtLinkModule::effectiveSupportedMask() const noexcept
{
    constexpr uint32_t kFallbackSupported = core::services::kGnssDefaultSupportedMask |
                                            (1U << static_cast<uint32_t>(::gnss::OutputMsg::kGsv));
    return gnss_supported_known_ ? gnss_supported_mask_ : kFallbackSupported;
}

void BtLinkModule::applyGnssStreamMask() noexcept
{
    const uint32_t defaultMask =
        gnss_default_push_ ? (kDefaultNmeaPushMask & effectiveSupportedMask()) : 0U;
    const uint32_t explicitMask = gnss_service_.has_value() ? gnss_service_->activeMask() : 0U;
    const uint32_t effectiveMask = defaultMask | explicitMask;
    if (gnss_reader_attached_ && (effectiveMask != gnss_applied_mask_)) {
        gnss_applied_mask_ = effectiveMask;
        static_cast<void>(ctx().data.gnssAppRing.setMask(gnss_reader_id_, effectiveMask));
        fw::trace(fw::TraceCat::kGnss, static_cast<uint8_t>(GnssSubTrace::kAppMask), 0U,
                  effectiveMask, (effectiveMask == 0U) ? 1U : 0U);
    }
    std::array<uint32_t, core::services::kGnssMsgCount> periodsMs{};
    if (gnss_service_.has_value()) {
        for (size_t i = 0U; i < core::services::kGnssMsgCount; ++i) {
            // 仅显式订阅的语句按其 interval 节流；默认直传语句周期为 0（沿用模块输出）。
            if ((explicitMask & (1U << i)) != 0U) {
                periodsMs[i] = gnss_service_->periodMs(i);
            }
        }
    }
    ctx().data.gnssAppSub.setSubscription(effectiveMask, periodsMs);
}

void BtLinkModule::onPhoneSessionOpen(core::SessionHandle handle) noexcept
{
    if (phone_session_handle_ != 0U) {
        resetGnssStreamSession();
    }
    phone_session_handle_ = handle;
    gnss_default_push_ = true;
    gnss_explicit_sub_ = false;
    applyGnssStreamMask();
}

void BtLinkModule::onPhoneProductFrame(core::SessionHandle handle,
                                       std::span<const uint8_t> wire) noexcept
{
    if ((handle != phone_session_handle_) || (wire.size() <= proto::kOfsSender) ||
        (link_manager_.role(handle) != core::Role::kPhone)) {
        return;
    }
    if ((wire[proto::kOfsSender] == proto::sender::kApp) && gnss_default_push_ &&
        !gnss_explicit_sub_) {
        gnss_default_push_ = false;
        applyGnssStreamMask();
    }
}

void BtLinkModule::recheckPhoneSessionRole(core::SessionHandle handle) noexcept
{
    if ((handle == phone_session_handle_) && (link_manager_.role(handle) != core::Role::kPhone)) {
        resetGnssStreamSession();
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
    if (mask != 0U) {
        self->gnss_explicit_sub_ = true;
    }
    self->applyGnssStreamMask();
}

void BtLinkModule::resetGnssStreamSession() noexcept
{
    phone_session_handle_ = 0U;
    gnss_default_push_ = false;
    gnss_explicit_sub_ = false;
    if (gnss_service_.has_value()) {
        gnss_service_->resetSession();
    }
    applyGnssStreamMask();
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

bool BtLinkModule::drainBaseRtcm() noexcept
{
    const uint32_t curRingDrop = ctx().data.baseRtcmRing.dropFrames();
    const uint32_t ringDropDelta =
        (curRingDrop >= base_rtcm_ring_drop_base_) ? (curRingDrop - base_rtcm_ring_drop_base_) : 0U;
    rtcm_snapshot_.baseTxDrop = ringDropDelta + base_rtcm_tx_drop_;
    if ((stream_handle_ == 0U) || (link_manager_.role(stream_handle_) != core::Role::kPhone)) {
        return false;
    }
    constexpr size_t kMaxBaseFramesPerPoll = 16U;
    size_t sentCount = 0U;
    for (; sentCount < kMaxBaseFramesPerPoll; ++sentCount) {
        size_t len = 0U;
        if (!ctx().data.baseRtcmRing.pop(base_rtcm_scratch_, len)) {
            break;
        }
        spp_server_tx_token_t token{};
        if (spp_server_stream_reserve(stream_handle_, len, &token) != ESP_OK) {
            ++base_rtcm_tx_drop_;
            continue;
        }
        if (spp_server_tx_commit(&token, base_rtcm_scratch_.data(), len) != ESP_OK) {
            spp_server_tx_cancel(&token);
            ++base_rtcm_tx_drop_;
            continue;
        }
        ++rtcm_snapshot_.baseTxFrames;
        rtcm_snapshot_.baseTxBytes += static_cast<uint32_t>(len);
    }
    const uint32_t postRingDrop = ctx().data.baseRtcmRing.dropFrames();
    const uint32_t postRingDelta = (postRingDrop >= base_rtcm_ring_drop_base_)
                                       ? (postRingDrop - base_rtcm_ring_drop_base_)
                                       : 0U;
    rtcm_snapshot_.baseTxDrop = postRingDelta + base_rtcm_tx_drop_;
    return sentCount == kMaxBaseFramesPerPoll;
}
#endif

} // namespace app
