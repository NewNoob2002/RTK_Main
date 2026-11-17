// GNSS 模块命令侧：应用层指令解析（探测/复位/频率/输出/透传）与异步执行（ADR-0010 §2.7）。
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

using detail::isValidRateHz;
using detail::kApplyTimeoutMs;
using detail::traceGnssSub;

/**
 * @brief 处理一条应用层指令（kProbe/kReset/kSetRate/kSend/kSetOut/kReadback）
 * @param cmd: 指令（verb 为 GnssVerb 或 GnssSubVerb）
 * @param result: 输出，执行结果；不支持的子命令返回 kNotSupported
 * @note 在 IO 执行器任务里调用，不阻塞；读回子命令只发起请求，结果由 AppData 发布
 */
void GnssModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!initialized_) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    const uint32_t now = rtos::nowMs();
    switch (static_cast<GnssVerb>(cmd.verb)) {
    case GnssVerb::kProbe:
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        cmdProbe(now, result);
        return;
    case GnssVerb::kReset:
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        cmdReset(now, result);
        return;
    case GnssVerb::kSetRate:
        cmdSetRate(cmd, now, result);
        return;
    case GnssVerb::kSend:
        sendRaw(cmd, result);
        return;
    default:
        break;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kSetOut) {
        cmdSetOut(cmd, now, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kReadback) {
        startReadback(now, result);
        return;
    }
    result.status = fw::Result::kNotSupported;
}

/**
 * @brief  gnss probe：标记人工重探，清空最近定位并让控制器重新探测
 * @param  nowMs: 当前时间
 * @param  result: 写入命令结果（kOk）
 * @note   仅 kIo 调用，非阻塞。
 */
void GnssModule::cmdProbe(uint32_t nowMs, fw::CommandResult &result) noexcept
{
    // 人工重探：清空最近定位，避免旧定位在重探期间被当成当前值发布。
    manual_reprobe_ = true;
    parser().reset();
    ctx().data.gnssFix.write(::gnss::Fix{});
    controller().requestReprobe(nowMs);
    pollController(nowMs);
    publishStatus(nowMs);
    result.status = fw::Result::kOk;
}

/**
 * @brief  gnss reset：清空最近定位后停再启动控制器
 * @param  nowMs: 当前时间
 * @param  result: 写入命令结果（kOk）
 * @note   仅 kIo 调用，非阻塞。
 */
void GnssModule::cmdReset(uint32_t nowMs, fw::CommandResult &result) noexcept
{
    // 人工复位：先清空定位再停/启控制器，重配流程由控制器自己推进。
    manual_reprobe_ = true;
    parser().reset();
    ctx().data.gnssFix.write(::gnss::Fix{});
    controller().stop(nowMs);
    pollController(nowMs);
    controller().start(nowMs);
    pollController(nowMs);
    publishStatus(nowMs);
    result.status = fw::Result::kOk;
}

/**
 * @brief  gnss rate：两阶段提交更新输出频率（ADR-0012 §2.4）
 * @param  cmd: 指令，args[0] 必须是支持的频率
 * @param  nowMs: 当前时间
 * @param  result: 写入命令结果（kInvalidArg / kBusy / 落盘错误 / kOk）
 * @note   仅 kIo 调用，非阻塞；NVS 写只在非 kRunning 的直落盘路径发生。
 */
void GnssModule::cmdSetRate(const fw::Command &cmd, uint32_t nowMs,
                            fw::CommandResult &result) noexcept
{
    if ((cmd.argLen != 1U) || !isValidRateHz(cmd.args[0])) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    // 上一次变更尚未判定时拒绝：否则 pending_prev_* 会被暂存值覆盖，回滚到的不是 NVS 中的旧值。
    if (pending_apply_ != PendingApply::kNone) {
        result.status = fw::Result::kBusy;
        return;
    }
    if (controller().state() != ::gnss::State::kRunning) {
        // 与 kSetOut 一致：没有运行中的接收机可确认，直接落盘，下次配置完成时按新频率执行。
        const uint8_t prevRate = ensureRoverCfg().rateHz;
        ensureRoverCfg().rateHz = cmd.args[0];
        const fw::Result saveRes = ctx().storage.saveKeys(gnss_store::kConfigKeys);
        if (!fw::isOk(saveRes)) {
            ensureRoverCfg().rateHz = prevRate;
            result.status = saveRes;
            return;
        }
        controller().setOutputPlan(makePlan());
        last_apply_ = GnssApply::kOk;
        ++apply_seq_;
        publishStatus(nowMs);
        result.status = fw::Result::kOk;
        return;
    }
    // 两阶段提交（ADR-0012 §2.4）：先暂存并重配，成功后才写 NVS；失败由 tickPendingApply 回滚。
    pending_prev_rate_ = ensureRoverCfg().rateHz;
    ensureRoverCfg().rateHz = cmd.args[0];
    pending_apply_ = PendingApply::kRate;
    pending_seq_ = controller().stats().outputSeq + 1U;
    pending_deadline_ms_ = nowMs + kApplyTimeoutMs;
    controller().setOutputPlan(makePlan());
    controller().requestReconfigure(nowMs);
    pollController(nowMs);
    publishStatus(nowMs);
    result.status = fw::Result::kOk;
}

/**
 * @brief  gnss setout：两阶段提交更新输出表
 * @param  cmd: 指令，args 为 GnssOutConfig 编码块
 * @param  nowMs: 当前时间
 * @param  result: 写入命令结果（kInvalidArg / kBusy / 落盘错误 / kOk）
 * @note   仅 kIo 调用，非阻塞。
 */
void GnssModule::cmdSetOut(const fw::Command &cmd, uint32_t nowMs,
                           fw::CommandResult &result) noexcept
{
    data::GnssOutConfig next{};
    if ((cmd.argLen != data::GnssOutConfig::kBlobBytes) ||
        !fw::isOk(data::GnssOutConfig::decode(std::span<const uint8_t>{cmd.args.data(), cmd.argLen},
                                              next))) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    if (pending_apply_ != PendingApply::kNone) {
        result.status = fw::Result::kBusy;
        return;
    }
    const data::GnssOutConfig prev = out_cfg_;
    out_cfg_ = next;
    ctx().data.gnssOutCfg.write(out_cfg_);
    traceGnssSub(GnssSubTrace::kOutCfg, out_cfg_.mask, 0U);
    if (controller().state() != ::gnss::State::kRunning) {
        // 未进入 kRunning 时没有可下发的输出阶段：直接落盘，下次配置完成时按新表重算计划。
        const fw::Result saveRes = ctx().storage.saveKeys(gnss_store::kConfigKeys);
        if (!fw::isOk(saveRes)) {
            out_cfg_ = prev;
            ctx().data.gnssOutCfg.write(out_cfg_);
            result.status = saveRes;
            return;
        }
        controller().setOutputPlan(makePlan());
        last_apply_ = GnssApply::kOk;
        ++apply_seq_;
        publishStatus(nowMs);
        result.status = fw::Result::kOk;
        return;
    }
    pending_prev_out_ = prev;
    pending_apply_ = PendingApply::kOut;
    pending_seq_ = controller().stats().outputSeq + 1U;
    pending_deadline_ms_ = nowMs + kApplyTimeoutMs;
    controller().setOutputPlan(makePlan());
    static_cast<void>(controller().requestOutput(controller().outputPlan(), nowMs));
    pollController(nowMs);
    publishStatus(nowMs);
    result.status = fw::Result::kOk;
}

/**
 * @brief 把原始字节直接写入 GNSS UART
 * @param cmd: cmd 见调用点
 * @param result: result 见调用点
 * @note 仅 console REPL/模块任务路径，非 ISR；不分配堆。
 */
void GnssModule::sendRaw(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const auto st = controller().state();
    if ((st != ::gnss::State::kRunning) && (st != ::gnss::State::kFailed)) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    if ((cmd.argLen == 0U) || (cmd.argLen > cmd.args.size())) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    std::array<char, fw::kCommandArgBytes + 2U> line{};
    // argLen 已检查不超过固定 args；size_t 足以表示它。
    const auto len = static_cast<size_t>(cmd.argLen);
    std::memcpy(line.data(), cmd.args.data(), len);
    line[len] = '\r';
    line[len + 1U] = '\n';
    // Kconfig UART 号限制为 0..2。
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const int written = uart_write_bytes(port, line.data(), len + 2U);
    if (written < 0) {
        result.status = fw::Result::kIoError;
        return;
    }
    // 驱动返回非负写入字节数，最大为 line.size()，uint32_t 足以表示。
    uart_tx_bytes_ += static_cast<uint32_t>(written);
    result.status = fw::Result::kOk;
}

} // namespace app
