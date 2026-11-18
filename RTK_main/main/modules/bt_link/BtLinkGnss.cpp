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
#endif

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

} // namespace app
