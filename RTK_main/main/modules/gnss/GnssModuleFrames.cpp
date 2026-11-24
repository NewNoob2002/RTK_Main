// GNSS 模块分流入口：DemuxSink 回调、整帧/拒绝分发与子对象访问器（ADR-0005、ADR-0010 §2.7）。
// 线程：init() 启动前单线程；start()/wait()/poll()/onCommand 仅 kIo 任务；keys/load/save 由 storage 与 kIo 调用。
// 均非 ISR；不分配堆。实时路径只用 GnssTrace；文本诊断经 FW_LOG*（ADR-0009 §2.12）：状态变化 I，逐帧/逐句 D。

#include "modules/gnss/GnssModule.hpp"
#include "modules/gnss/GnssModulePrivate.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/task.h"

#include "AppContext.hpp"
#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Rtcm3.hpp"
#include "demux/decoders/Ubx.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "fw/Status.hpp"
#include "fw/Log.hpp"
#include "fw/Tlog.hpp"
#include "gnss/Receiver.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/storage/StorageModule.hpp"
#include "rtos/EspStatus.hpp"

namespace app {

using detail::kLogTagBytes;
using detail::kRejectReasonBytes;
using detail::rejectCauseName;
using detail::traceGnssSub;

void GnssModule::DemuxSink::onFrame(const demux::Decoder &decoder, uint8_t /*index*/,
                                    std::span<const uint8_t> frame) noexcept
{
    if (owner_ != nullptr) {
        owner_->handleFrame(decoder, frame);
    }
}

void GnssModule::DemuxSink::onReject(const demux::Decoder &decoder, uint8_t /*index*/,
                                     std::span<const uint8_t> prefix, demux::RejectCause cause,
                                     bool last) noexcept
{
    if (owner_ != nullptr) {
        owner_->handleReject(decoder, prefix, cause, last);
    }
}

::gnss::Controller &GnssModule::controller() noexcept
{
    if (!controller_.has_value()) {
        std::abort();
    }
    return *controller_;
}

::gnss::NmeaParser &GnssModule::parser() noexcept
{
    if (!parser_.has_value()) {
        std::abort();
    }
    return *parser_;
}

demux::Demux &GnssModule::streamDemux() noexcept
{
    if (!demux_.has_value()) {
        std::abort();
    }
    return *demux_;
}

/**
 * @brief 处理来自 GNSS RX 侧的一条 RTCM3 帧，触发自测观察并推入基站转发环。
 * @param frame: 完整 RTCM3 线路帧字节。
 */
void GnssModule::handleRxRtcmFrame(std::span<const uint8_t> frame) noexcept
{
    constexpr size_t kMinRtcmMsgBytes = 5U;
    constexpr size_t kHeaderBytes = 3U;
    constexpr unsigned kRtcmMsgShift = 4U;
    uint16_t msgId = 0U;
    if (frame.size() >= kMinRtcmMsgBytes) {
        msgId = static_cast<uint16_t>(
            (static_cast<uint32_t>(frame[kHeaderBytes]) << kRtcmMsgShift) |
            (static_cast<uint32_t>(frame[kHeaderBytes + 1U]) >> kRtcmMsgShift));
    }
    if (work_mode_.has_value()) {
        auto &wm = *work_mode_;
        wm.onRtcmFrame(msgId);
        if (wm.shouldForwardRtcm() && ctx().data.phoneStreamReady.load(std::memory_order_acquire)) {
            static_cast<void>(ctx().data.baseRtcmRing.push(frame));
        }
    }
}

/**
 * @brief 更新 6 KiB 最新句缓存并按订阅掩码与降采样闸门向 App FanoutRing 推送语句。
 * @param decoder: 命中解码器。
 * @param frame: 完整帧字节。
 * @param now: 当前单调时间（ms）。
 */
void GnssModule::publishAppFrame(const demux::Decoder &decoder, std::span<const uint8_t> frame,
                                 uint32_t now) noexcept
{
    const ::gnss::OutputMsg cls = controller().receiver().classifyFrame(decoder, frame);
    if (cls == ::gnss::OutputMsg::kGsv) {
        if (!frame.empty() && (frame.size() <= proto::kMaxPayload)) {
            data::LatestGsv gsv{};
            std::copy(frame.begin(), frame.end(), gsv.bytes.begin());
            // 已校验 frame.size() <= proto::kMaxPayload(256)，缩窄到 uint16_t 无损。
            gsv.len = static_cast<uint16_t>(frame.size());
            ctx().data.gnssLatestGsv.write(gsv);
        }
        uint32_t activeMask = 0U;
        std::array<uint32_t, data::kGnssAppMsgCount> periodsMs{};
        ctx().data.gnssAppSub.readSubscription(activeMask, periodsMs);
        constexpr uint8_t kGsvType = static_cast<uint8_t>(::gnss::OutputMsg::kGsv);
        if ((activeMask & (1U << kGsvType)) != 0U) {
            static_cast<void>(ctx().data.gnssAppRing.write(kGsvType, frame));
        }
        return;
    }
    const auto idx = static_cast<size_t>(static_cast<uint8_t>(cls));
    if ((cls == ::gnss::OutputMsg::kNone) || (idx >= data::kGnssAppMsgCount)) {
        return;
    }
    if ((cls == ::gnss::OutputMsg::kGga) && gga_gate_.has_value()) {
        const bool pass = gga_gate_->evaluate(frame);
        publishGgaGateStatus();
        if (!pass) {
            // Q1：阻断期间直接丢弃 GGA，不更新最新帧缓存、不推入 App 环、不缓存不补发。
            return;
        }
    }
    const auto typeIdx = static_cast<uint8_t>(idx);
    if (!ctx().data.gnssLatest.update(typeIdx, frame)) {
        traceGnssSub(GnssSubTrace::kAppDrop, 2U, typeIdx);
    }
    uint32_t activeMask = 0U;
    std::array<uint32_t, data::kGnssAppMsgCount> periodsMs{};
    ctx().data.gnssAppSub.readSubscription(activeMask, periodsMs);
    if ((activeMask & (1U << typeIdx)) == 0U) {
        // 无订阅者：FanoutRing::write 只计 noSubscriber，不写入环形缓冲；闸门复位，重新订阅后首句立即推送。
        static_cast<void>(ctx().data.gnssAppRing.write(typeIdx, frame));
        push_gates_[typeIdx].reset();
        return;
    }
    const uint32_t period = periodsMs[typeIdx];
    auto &gate = push_gates_[typeIdx];
    if (!gate.due(now, period)) {
        ctx().data.gnssAppSub.addSkipped(typeIdx);
        return;
    }
    const fw::Result wres = ctx().data.gnssAppRing.write(typeIdx, frame);
    if (fw::isOk(wres)) {
        gate.onPushed(now, period);
    } else if (wres == fw::Result::kInvalidArg) {
        ctx().data.gnssAppSub.addRingOversize(typeIdx);
        traceGnssSub(GnssSubTrace::kAppDrop, 0U, typeIdx);
    }
}

void GnssModule::logValsetIssue(const char *kind, std::span<const uint32_t> keys) noexcept
{
    constexpr size_t kKeysTextBytes = 64U;
    std::array<char, kKeysTextBytes> keysText{};
    static_cast<void>(::gnss::ublox::formatValsetKeys(keys, keysText));
    FW_LOGW(fw::TraceCat::kGnss, "<ubx> VALSET %s (n=%lu keys=%s)", kind,
            static_cast<unsigned long>(keys.size()), keysText.data());
}

void GnssModule::handleUbxFrame(std::span<const uint8_t> frame, uint32_t nowMs) noexcept
{
    std::span<const uint32_t> keys{};
    const auto reply = valset_tracker_.onFrame(frame, nowMs, keys);
    if (reply == ::gnss::ublox::ValsetReplyKind::kNak) {
        logValsetIssue("NAK", keys);
        if (::gnss::ublox::containsNmeaHighPrecKeys(keys)) {
            FW_LOGW(fw::TraceCat::kGnss,
                    "<ubx> NMEA high-precision VALSET NAK, degrading to base port config");
        }
        return;
    }
    if ((reply == ::gnss::ublox::ValsetReplyKind::kAck) ||
        (reply == ::gnss::ublox::ValsetReplyKind::kUnmatched)) {
        return;
    }
    std::array<char, 16U> sn{};
    if (::gnss::ublox::parseSecUniqId(frame, sn) > 0U) {
        ctx().data.gnssBoardSn.write(sn);
        FW_LOGI(fw::TraceCat::kGnss, "<ubx> boardSn %s", sn.data());
    }
}

/** @brief 分发接收机帧。 @param decoder: 帧来源。 @param frame: 已校验的帧。 @note 仅 kIo。 */
void GnssModule::handleFrame(const demux::Decoder &decoder, std::span<const uint8_t> frame) noexcept
{
    // 文本日志不经中断路径：本函数只由 kIo 任务的 feed() 调用（ADR-0009 §2.11）。
    std::array<char, kLogTagBytes> tag{};
    controller().receiver().frameTag(decoder, frame, tag);
    FW_LOGD(fw::TraceCat::kGnss, "<rx> %s, %lu bytes", tag.data(),
            static_cast<unsigned long>(frame.size()));
    fw::tlogHex(fw::TraceCat::kGnss, frame);

    const uint32_t now = rtos::nowMs();
    // 读回窗口内先把这一行取走（只在 kIo 任务里写交接缓冲），再交给状态机计数与计时。
    if (controller().state() == ::gnss::State::kReadback) {
        appendReadbackLine(decoder, frame);
    }
    ppp_seq_.onFrame(controller().receiver(), decoder, frame, now);
    sat_seq_.onFrame(controller().receiver(), decoder, frame, now);
    controller().onFrame(decoder, frame, now);
    if (&decoder == &demux::nmea::kDecoder) {
        const auto res = parser().feed(frame, now);
        if ((res == ::gnss::NmeaResult::kFix) || (res == ::gnss::NmeaResult::kAux)) {
            ctx().data.gnssFix.write(parser().fix());
        } else if (res == ::gnss::NmeaResult::kSatsReady) {
            // GGA 触发卫星轮发布时已在同一次 feed() 中更新 fix_（G-A §4.1），须同步刷新定位槽。
            ctx().data.gnssFix.write(parser().fix());
            ctx().data.gnssSats.write(parser().sats());
        }
    } else if (&decoder == &demux::rtcm3::kDecoder) {
        handleRxRtcmFrame(frame);
    } else if (&decoder == &demux::ubx::kDecoder) {
        handleUbxFrame(frame, now);
    }
    // ADR-0011 §5：6 类可订阅语句同步更新 6 KiB 最新帧缓存与 App 扇出环。
    publishAppFrame(decoder, frame, now);
}

void GnssModule::handleReject(const demux::Decoder &decoder, std::span<const uint8_t> prefix,
                              demux::RejectCause cause, bool last) noexcept
{
    // 只在“该起点已无其它候选”时输出一行，避免多解码器并行尝试时刷屏（ADR-0009 §2.11）。
    if (!last || prefix.empty()) {
        return;
    }
    std::array<char, kRejectReasonBytes> reason{};
    const std::size_t len = (decoder.explain != nullptr) ? decoder.explain(prefix, reason) : 0U;
    const char *text = (len != 0U) ? reason.data() : rejectCauseName(cause);
    FW_LOGD(fw::TraceCat::kGnss, "<rx> %s rejected %lu bytes: %s", decoder.name,
            static_cast<unsigned long>(prefix.size()), text);
    fw::tlogHex(fw::TraceCat::kGnss, prefix);
}

} // namespace app
