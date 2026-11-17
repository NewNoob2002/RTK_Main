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

/**
 * @brief 返回 GNSS 控制器引用
 * @retval 控制器引用（生命周期由模块持有）
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
::gnss::Controller &GnssModule::controller() noexcept
{
    if (!controller_.has_value()) {
        std::abort();
    }
    return *controller_;
}

/**
 * @brief 返回 NMEA 解析器引用
 * @retval 解析器引用（生命周期由模块持有）
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
::gnss::NmeaParser &GnssModule::parser() noexcept
{
    if (!parser_.has_value()) {
        std::abort();
    }
    return *parser_;
}

/**
 * @brief 返回 GNSS 字节流解复用器引用
 * @retval 解复用器引用（生命周期由模块持有）
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
demux::Demux &GnssModule::streamDemux() noexcept
{
    if (!demux_.has_value()) {
        std::abort();
    }
    return *demux_;
}

/**
 * @brief  处理分流器交付的一帧：喂控制器、应用输出，并记 D 级帧日志
 * @param  decoder: 识别出该帧的解码器
 * @param  frame: 完整帧字节
 */
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
    controller().onFrame(decoder, frame, now);
    if (&decoder == &demux::nmea::kDecoder) {
        const auto res = parser().feed(frame, now);
        if (res == ::gnss::NmeaResult::kFix) {
            ctx().data.gnssFix.write(parser().fix());
        } else if (res == ::gnss::NmeaResult::kSatsReady) {
            // GGA 触发卫星轮发布时已在同一次 feed() 中更新 fix_（G-A §4.1），须同步刷新定位槽。
            ctx().data.gnssFix.write(parser().fix());
            ctx().data.gnssSats.write(parser().sats());
        }
    }

    // ADR-0011 §5：6 类可订阅语句同步更新 6 KiB 最新帧缓存，并按会话掩码与周期写入 App typed FanoutRing。
    {
        // 分类交给驱动（ADR-0012 §2.6）：基类覆盖 5 类 NMEA，Unicore 另加 #SATSINFOA/#HEADINGA。
        const ::gnss::OutputMsg cls = controller().receiver().classifyFrame(decoder, frame);
        const auto idx = static_cast<size_t>(static_cast<uint8_t>(cls));
        if ((cls != ::gnss::OutputMsg::kNone) && (idx < data::kGnssAppMsgCount)) {
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
            } else {
                const uint32_t period = periodsMs[typeIdx];
                auto &gate = push_gates_[typeIdx];
                if (!gate.due(now, period)) {
                    ctx().data.gnssAppSub.addSkipped(typeIdx);
                } else {
                    const fw::Result wres = ctx().data.gnssAppRing.write(typeIdx, frame);
                    if (fw::isOk(wres)) {
                        gate.onPushed(now, period);
                    } else if (wres == fw::Result::kInvalidArg) {
                        ctx().data.gnssAppSub.addRingOversize(typeIdx);
                        traceGnssSub(GnssSubTrace::kAppDrop, 0U, typeIdx);
                    }
                }
            }
        }
    }
}

/**
 * @brief  分流器拒收字节：记 D 级日志与可选 hex 转储
 * @param  decoder: 拒收的解码器
 * @param  prefix: 被拒收的字节
 * @param  cause: 拒收原因
 * @param  last: 是否为本次重同步的最后一个拒收
 */
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
