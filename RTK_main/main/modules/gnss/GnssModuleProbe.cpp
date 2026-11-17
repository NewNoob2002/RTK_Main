// GNSS 模块控制器侧：状态机轮询、状态迁移日志、配置读回与待落地变更（ADR-0010 §2.7、ADR-0012 §2.7）。
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

using detail::kMaxActionsPerPoll;
using detail::kReadbackLineBytes;
using detail::toResult;

/**
 * @brief 跑一轮控制器状态机并把动作落到 UART，收尾时检测读回窗口结束
 * @param nowMs: 单调时间（毫秒）
 * @note 在模块任务里调用，最多 kMaxActionsPerPoll 次动作，不阻塞
 */
void GnssModule::pollController(uint32_t nowMs) noexcept
{
    for (size_t iter = 0U; iter < kMaxActionsPerPoll; ++iter) {
        const ::gnss::Action act = controller().poll(nowMs);
        if (act.kind == ::gnss::Action::Kind::kNone) {
            break;
        }
        applyAction(act);
    }
    updateControllerStatus();
    finishReadbackIfDone();
}

/**
 * @brief 把一个控制器动作落到 UART（上电/改波特率/下发报文）
 * @param act: 控制器给出的动作
 * @note 在模块任务里调用，阻塞在 UART 写队列上，非 ISR
 */
void GnssModule::applyAction(const ::gnss::Action &act) noexcept
{
    switch (act.kind) {
    case ::gnss::Action::Kind::kPower: {
        const fw::Result err = applyPower(act.on);
        if (!fw::isOk(err)) {
            traceGnss(GnssTrace::kState, static_cast<uint32_t>(controller().state()),
                      static_cast<uint32_t>(err));
        }
        break;
    }
    case ::gnss::Action::Kind::kSetBaud: {
        const uint32_t oldBaud = host_baud_;
        const fw::Result err = applyBaud(act.baud);
        if (fw::isOk(err)) {
            traceGnss(GnssTrace::kBaud, act.baud, oldBaud);
            FW_LOGI(fw::TraceCat::kGnss, "<uart> baud %lu (was %lu)",
                    static_cast<unsigned long>(act.baud), static_cast<unsigned long>(oldBaud));
        }
        break;
    }
    case ::gnss::Action::Kind::kWrite:
        applyWriteAction(act);
        break;
    case ::gnss::Action::Kind::kNone:
    default:
        break;
    }
}

/**
 * @brief 读回收尾（ADR-0012 §2.7）：控制器回 kRunning 并推进 readbackSeq 后发布一次结论
 * @note 在模块任务里调用，非阻塞
 */
void GnssModule::finishReadbackIfDone() noexcept
{
    if (controller().stats().readbackSeq == readback_base_seq_) {
        return;
    }
    readback_base_seq_ = controller().stats().readbackSeq;
    FW_LOGI(fw::TraceCat::kGnss, "<cfg> Readback done, %u lines%s",
            static_cast<unsigned>(readback_lines_), readback_truncated_ ? " (truncated)" : "");
    publishReadback(readback_lines_, readback_len_, true, readback_truncated_, false);
}

/**
 * @brief  把控制器状态变化与探测结果记为 I 级日志并更新快照
 */
void GnssModule::updateControllerStatus() noexcept
{
    const auto curStats = controller().stats();
    if (curStats.stepNacks > prev_ctrl_stats_.stepNacks) {
        traceGnss(GnssTrace::kStepNack, step_idx_, 0U);
    }
    if (curStats.stepTimeouts > prev_ctrl_stats_.stepTimeouts) {
        traceGnss(GnssTrace::kStepTimeout, step_idx_, step_retry_ + 1U);
    }
    const auto curState = controller().state();
    if (curState != last_state_) {
        traceGnss(GnssTrace::kState, static_cast<uint32_t>(curState),
                  static_cast<uint32_t>(last_state_));
        FW_LOGI(fw::TraceCat::kGnss, "<sm> state %s -> %s", ::gnss::stateName(last_state_),
                ::gnss::stateName(curState));
        onControllerStateChanged(last_state_, curState);
        last_state_ = curState;
    }
    prev_ctrl_stats_ = curStats;
}

/**
 * @brief 读回期间把一帧转成一行文本追加到 gnssReadbackText（ADR-0012 §2.7）。
 *
 * 交接缓冲是唯一的跳任务载体：kIo 在读回进行中逐行追加，控制台看到 gnssReadback.seq 变化后按 len 读取，
 * 读写不重叠（见 AppData.hpp 该成员的注释），因此不需要锁。
 * @param decoder: 该帧的解码器。
 * @param frame: 整帧字节。
 * @note 仅 kIo 调用，非阻塞，不分配。
 */
void GnssModule::appendReadbackLine(const demux::Decoder &decoder,
                                    std::span<const uint8_t> frame) noexcept
{
    std::array<char, kReadbackLineBytes> line{};
    // 留一格给 '\0'：下面按 %s 打 D 级日志，正文截断到 kReadbackLineBytes-1 才不会越界读。
    const size_t len = controller().receiver().matchReadback(
        decoder, frame, std::span<char>(line.data(), line.size() - 1U));
    if (len == 0U) {
        return;
    }
    ++readback_lines_; // 命中即计数：即使整行因容量不足被丢弃也算一行（见 GnssReadback::lines）
    FW_LOGD(fw::TraceCat::kGnss, "<cfg> RX %s", line.data());
    auto &text = ctx().data.gnssReadbackText;
    const size_t used = readback_len_;
    if ((used + len + 1U) > text.size()) {
        readback_truncated_ = true;
        return;
    }
    for (size_t i = 0U; i < len; ++i) {
        text[used + i] = line[i];
    }
    text[used + len] = '\n';
    // 上面的容量检查已保证 used + len + 1 <= text.size()（1536），窄化到 uint16_t 安全。
    readback_len_ = static_cast<uint16_t>(used + len + 1U);
}

/**
 * @brief 发起一次配置读回，并把被拒原因就地发布（ADR-0012 §2.7）。
 * @param nowMs: 当前时间。
 * @param result: 写入命令结果（kOk / kBusy / kNotSupported）。
 * @note 仅 kIo 调用；被拒时立即发布结论，控制台不必等到超时。
 */
void GnssModule::startReadback(uint32_t nowMs, fw::CommandResult &result) noexcept
{
    ::gnss::CmdBuf probe{};
    // 先单独问一句驱动能力：requestReadback() 把“不支持”与“状态不对”合为一个 false。
    const bool supported = controller().receiver().buildReadback(probe);
    if (controller().requestReadback(nowMs)) {
        readback_len_ = 0U;
        readback_lines_ = 0U;
        readback_truncated_ = false;
        readback_base_seq_ = controller().stats().readbackSeq;
        result.status = fw::Result::kOk;
        return;
    }
    publishReadback(0U, 0U, supported, false, supported);
    result.status = supported ? fw::Result::kBusy : fw::Result::kNotSupported;
}

/**
 * @brief 发布一次读回结论，seq 严格递增（ADR-0012 §2.7）。
 *
 * 失败路径按卡要求“预支” stats+1，成功路径用 stats 原值，两者可能撞车；取 max(上一发布+1) 保证控制台
 * “等 seq 变化”的判定不会漏掉任何一次结论。
 * @param lines: 命中行数。
 * @param len: 文本有效字节。
 * @param supported: 驱动是否支持读回。
 * @param truncated: 是否有行因容量不足被丢弃。
 * @param busy: 是否因状态不对被拒。
 * @note 仅 kIo 调用。
 */
void GnssModule::publishReadback(uint16_t lines, uint16_t len, bool supported, bool truncated,
                                 bool busy) noexcept
{
    const uint32_t statsSeq = controller().stats().readbackSeq;
    const uint32_t seq = statsSeq > readback_pub_seq_ ? statsSeq : (readback_pub_seq_ + 1U);
    readback_pub_seq_ = seq;
    GnssReadback out{};
    out.seq = seq;
    out.lines = lines;
    out.len = len;
    out.supported = supported;
    out.truncated = truncated;
    out.busy = busy;
    ctx().data.gnssReadback.write(out);
}

/**
 * @brief 推进待应用配置的超时计时
 * @param nowMs: 单调毫秒时间戳
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
void GnssModule::tickPendingApply(uint32_t nowMs) noexcept
{
    if (pending_apply_ == PendingApply::kNone) {
        return;
    }
    const auto stats = controller().stats();
    const bool landed = stats.outputSeq >= pending_seq_;
    // 超时按有符号差判断，毫秒计数回绕时仍成立。
    const bool timeout = static_cast<int32_t>(nowMs - pending_deadline_ms_) >= 0;
    if (!landed && !timeout) {
        return;
    }
    if (landed && (stats.failedMask == 0U)) {
        const fw::Result saveRes = ctx().storage.saveKeys(gnss_store::kConfigKeys);
        last_apply_ = fw::isOk(saveRes) ? GnssApply::kOk : GnssApply::kFailed;
    } else {
        // 有语句失败或超时：NVS 保持旧值，界面与计划一起回滚到变更前。
        if (pending_apply_ == PendingApply::kOut) {
            out_cfg_ = pending_prev_out_;
            ctx().data.gnssOutCfg.write(out_cfg_);
        } else {
            ensureRoverCfg().rateHz = pending_prev_rate_;
        }
        last_apply_ = GnssApply::kRestored;
        controller().setOutputPlan(makePlan());
        static_cast<void>(controller().requestOutput(controller().outputPlan(), nowMs));
    }
    pending_apply_ = PendingApply::kNone;
    ++apply_seq_;
    publishStatus(nowMs);
}

/**
 * @brief 把探测到的驱动信息落库
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
void GnssModule::persistDetectedHint() noexcept
{
    const auto &det = controller().detected();
    if (det.receiver == nullptr) {
        return;
    }
    const ::gnss::Family detFamily = det.receiver->family();
    const ::gnss::Model detModel = det.probe.model;
    const uint32_t detBaud = (det.baud != 0U) ? det.baud : host_baud_;
    if ((detFamily == hint_.family) && (detModel == hint_.model) && (detBaud == hint_.baud)) {
        return;
    }
    hint_.family = detFamily;
    hint_.model = detModel;
    hint_.baud = detBaud;
    const fw::Result saveRes = ctx().storage.saveKeys(gnss_store::kCacheKeys);
    if (!fw::isOk(saveRes)) {
        // StorageModule::saveKeys 内部已发 kStore trace；此处保留 hint_ 内存值避免每轮重复写盘。
    }
}

/**
 * @brief 按动作给 GNSS 上电或断电
 * @param on: true 表示上电
 * @retval kOk 表示成功；其他为失败原因
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
fw::Result GnssModule::applyPower(bool on) noexcept
{
#if CONFIG_RTK_GNSS_POWER_GPIO >= 0
    const auto powerPin = static_cast<gpio_num_t>(CONFIG_RTK_GNSS_POWER_GPIO);
    const bool activeHigh = (CONFIG_RTK_GNSS_POWER_ACTIVE_HIGH != 0);
    const uint32_t level = (on == activeHigh) ? 1U : 0U;
    return toResult(rtos::fromEspErr(gpio_set_level(powerPin, level)));
#else
    static_cast<void>(on);
    return fw::Result::kOk;
#endif
}

/**
 * @brief 切换 GNSS UART 波特率
 * @param baud: 波特率
 * @retval kOk 表示成功；其他为失败原因
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
fw::Result GnssModule::applyBaud(uint32_t baud) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const esp_err_t baudErr = uart_set_baudrate(port, baud);
    if (baudErr != ESP_OK) {
        return toResult(rtos::fromEspErr(baudErr));
    }
    const esp_err_t flushErr = uart_flush_input(port);
    if (flushErr != ESP_OK) {
        return toResult(rtos::fromEspErr(flushErr));
    }
    host_baud_ = baud;
    return fw::Result::kOk;
}

/**
 * @brief 按输出配置生成报文输出计划
 * @retval 生成的输出计划
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
::gnss::OutputPlan GnssModule::makePlan() noexcept
{
    uint8_t rateHz = 0U;
    bool gsv = false;
    if (rover_cfg_.has_value()) {
        rateHz = rover_cfg_->rateHz;
        gsv = rover_cfg_->gsv;
    }
    const uint32_t caps = controller().detected().caps;
    return data::makeOutputPlan(out_cfg_, rateHz, gsv, static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT),
                                caps);
}

/**
 * @brief  把控制器下发的一条 UART 写动作落到串口，并记录探测/配置进度
 * @param  act: 控制器动作（kind 必为 kWrite）
 * @note   仅 kIo 调用；写失败只记日志，不改状态机。
 */
void GnssModule::applyWriteAction(const ::gnss::Action &act) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const int written = uart_write_bytes(port, act.bytes.data(), act.bytes.size());
    if (written > 0) {
        uart_tx_bytes_ += static_cast<uint32_t>(written);
    }
    // 控制器下发的命令原文（去掉行尾 CR/LF）；探测阶段标 <probe>，其余标 <cfg>，
    // 方向固定写 TX（MCU → 接收机）：旧格式 "model <- cmd" 容易被读成接收（ADR-0009 §2.12）。
    std::size_t cmdLen = act.bytes.size();
    while ((cmdLen > 0U) &&
           ((act.bytes[cmdLen - 1U] == '\r') || (act.bytes[cmdLen - 1U] == '\n'))) {
        --cmdLen;
    }
    // IDF action 的字节数有固定上限；逐字节转成 char 后保留命令原文供诊断。
    std::array<char, ::gnss::kMaxCommand> cmdText{};
    const size_t textLen = std::min(cmdLen, cmdText.size());
    for (size_t i = 0U; i < textLen; ++i) {
        cmdText[i] = std::bit_cast<char>(act.bytes[i]);
    }
    const auto st = controller().state();
    // textLen 不超过固定命令数组，远小于 INT_MAX。
    FW_LOGI(fw::TraceCat::kGnss, "%s TX \"%.*s\"",
            (st == ::gnss::State::kProbing) ? "<probe>" : "<cfg>", static_cast<int>(textLen),
            cmdText.data());
    const auto curStats = controller().stats();
    if ((st == ::gnss::State::kProbing) && (curStats.probesSent > prev_ctrl_stats_.probesSent)) {
        traceGnss(GnssTrace::kProbeSend,
                  static_cast<uint32_t>(controller().probeReceiver().family()), host_baud_);
        prev_ctrl_stats_.probesSent = curStats.probesSent;
    } else if ((st == ::gnss::State::kConfiguring) &&
               (curStats.stepsSent > prev_ctrl_stats_.stepsSent)) {
        const bool retried = (curStats.stepNacks > prev_ctrl_stats_.stepNacks) ||
                             (curStats.stepTimeouts > prev_ctrl_stats_.stepTimeouts);
        if (retried) {
            ++step_retry_;
        } else if (step_sent_once_) {
            traceGnss(GnssTrace::kStepAck, step_idx_, 0U);
            ++step_idx_;
            step_retry_ = 0U;
        }
        step_sent_once_ = true;
        traceGnss(GnssTrace::kStepSend, step_idx_, step_retry_);
        prev_ctrl_stats_.stepsSent = curStats.stepsSent;
    }
}

/**
 * @brief  状态迁移的副作用：探测结果、步进计数与输出计划重算
 * @param  prev: 迁移前状态
 * @param  cur: 迁移后状态
 * @note   仅 kIo 调用；只在 prev != cur 时调用一次。
 */
void GnssModule::onControllerStateChanged(::gnss::State prev, ::gnss::State cur) noexcept
{
    if ((prev == ::gnss::State::kProbing) && (cur == ::gnss::State::kConfiguring)) {
        const auto det = controller().detected();
        const uint32_t baud = (det.baud != 0U) ? det.baud : host_baud_;
        traceGnss(GnssTrace::kProbeHit, static_cast<uint32_t>(det.probe.model), baud);
        FW_LOGI(fw::TraceCat::kGnss, "<probe> detected %s at %lu",
                ::gnss::modelName(det.probe.model), static_cast<unsigned long>(baud));
        step_idx_ = 0U;
        step_retry_ = 0U;
        step_sent_once_ = false;
    } else if ((prev == ::gnss::State::kConfiguring) && (cur == ::gnss::State::kRunning)) {
        if (step_sent_once_) {
            traceGnss(GnssTrace::kStepAck, step_idx_, 0U);
        }
        persistDetectedHint();
        // 型号与 caps 已知，输出计划按支持的语句位重算一次（ADR-0012 §2.4）。
        controller().setOutputPlan(makePlan());
    } else if ((prev == ::gnss::State::kRunning) && (cur == ::gnss::State::kProbing)) {
        const auto curStats = controller().stats();
        if (!manual_reprobe_ && (curStats.reprobes > prev_ctrl_stats_.reprobes)) {
            const uint32_t silenceMs = timing_.has_value() ? timing_->silenceMs : 0U;
            traceGnss(GnssTrace::kSilence, silenceMs, 0U);
        }
        manual_reprobe_ = false;
    } else {
        // 其余迁移（断电/失败/读回）没有额外副作用。
    }
}

} // namespace app
