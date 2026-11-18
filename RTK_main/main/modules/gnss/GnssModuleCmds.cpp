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
#include "modules/gnss/GnssConsoleValue.hpp"
#include "modules/storage/StorageModule.hpp"
#include "rtos/EspStatus.hpp"

namespace app {

using detail::isValidRateHz;
using detail::traceGnssSub;

/**
 * @brief 分派常规 GNSS 命令与扩展子命令（已在 kRunning 或允许状态下）。
 * @param cmd: 待执行指令。
 * @param nowMs: 当前时间。
 * @param result: 命令执行结果。
 */
void GnssModule::dispatchStandardCmd(const fw::Command &cmd, uint32_t nowMs,
                                     fw::CommandResult &result) noexcept
{
    switch (static_cast<GnssVerb>(cmd.verb)) {
    case GnssVerb::kProbe:
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        cmdProbe(nowMs, result);
        return;
    case GnssVerb::kReset:
        if (cmd.argLen != 0U) {
            result.status = fw::Result::kInvalidArg;
            return;
        }
        cmdReset(nowMs, result);
        return;
    case GnssVerb::kSetRate:
        cmdSetRate(cmd, nowMs, result);
        return;
    case GnssVerb::kSend:
        sendRaw(cmd, result);
        return;
    default:
        break;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kSetOut) {
        cmdSetOut(cmd, nowMs, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kReadback) {
        startReadback(nowMs, result);
        return;
    }
    result.status = fw::Result::kNotSupported;
}

// 三组命令字共用 cmd.verb 一个字节，onCommand 先按维护命令分流；编号重叠会把别的命令当成维护命令。
static_assert(static_cast<uint8_t>(GnssSubVerb::kMode) >
                  static_cast<uint8_t>(GnssMaintenanceVerb::kBaud),
              "GnssSubVerb::kMode must not overlap GnssMaintenanceVerb");
static_assert(static_cast<uint8_t>(GnssSubVerb::kReadback) <
                  static_cast<uint8_t>(GnssMaintenanceVerb::kSave),
              "GnssSubVerb::kReadback must not overlap GnssMaintenanceVerb");

void GnssModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!initialized_) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    const uint32_t now = rtos::nowMs();
    const auto maintenanceVerb = static_cast<GnssMaintenanceVerb>(cmd.verb);
    if (maintenanceVerb == GnssMaintenanceVerb::kSave ||
        maintenanceVerb == GnssMaintenanceVerb::kFactoryReset ||
        maintenanceVerb == GnssMaintenanceVerb::kBaud) {
        cmdMaintenance(cmd, now, result);
        return;
    }
    if (static_cast<GnssSubVerb>(cmd.verb) == GnssSubVerb::kMode) {
        cmdMode(cmd, now, result);
        return;
    }
    if (baud_deferred_ || (controller().state() == ::gnss::State::kMaintenance &&
                           (active_maintenance_ != ::gnss::Maintenance::kSave ||
                            (cmd.verb != static_cast<uint8_t>(GnssVerb::kSetRate) &&
                             cmd.verb != static_cast<uint8_t>(GnssSubVerb::kSetOut))))) {
        result.status = fw::Result::kBusy;
        return;
    }
    dispatchStandardCmd(cmd, now, result);
}

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
    if (controller().state() != ::gnss::State::kRunning &&
        controller().state() != ::gnss::State::kMaintenance) {
        // 与 kSetOut 一致：没有运行中的接收机可确认，直接落盘，下次配置完成时按新频率执行。
        const uint8_t prevRate = ensureRoverCfg().rateHz;
        ensureRoverCfg().rateHz = cmd.args[0];
        const fw::Result saveRes = ctx().storage.saveKeys(
            std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kRateKeyIndex, 1U>());
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
    apply_deferred_ = controller().state() == ::gnss::State::kMaintenance;
    if (!apply_deferred_) {
        beginPendingApply(nowMs);
    }
    result.status = fw::Result::kOk;
}

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
    if (controller().state() != ::gnss::State::kRunning &&
        controller().state() != ::gnss::State::kMaintenance) {
        // 未进入 kRunning 时没有可下发的输出阶段：直接落盘，下次配置完成时按新表重算计划。
        const fw::Result saveRes = ctx().storage.saveKeys(
            std::span{gnss_store::kConfigKeys}.subspan<gnss_store::kOutKeyIndex, 1U>());
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
    apply_deferred_ = controller().state() == ::gnss::State::kMaintenance;
    if (!apply_deferred_) {
        beginPendingApply(nowMs);
    }
    result.status = fw::Result::kOk;
}

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
    // 按字符查看命令字节。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto *command = reinterpret_cast<const char *>(cmd.args.data());
    const std::string_view text{command, cmd.argLen};
    if (text.find_first_of("\r\n") != std::string_view::npos ||
        text.find('\0') != std::string_view::npos ||
        detail::changesConnectedGnssBaud(text, CONFIG_RTK_GNSS_PORT)) {
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
