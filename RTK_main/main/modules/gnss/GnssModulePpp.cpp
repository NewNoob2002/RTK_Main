// kIo 上执行手机 PPP（0x33 Survey）与截止角/星座（0x06 Survey）命令并在重新配置后重放 NVS 值。
// 仅 GNSS 任务调用，非 ISR，无堆分配。UART 与 NVS 写入可等待后端。
#include "modules/gnss/GnssModule.hpp"

#include "driver/uart.h"

#include "AppContext.hpp"
#include "fw/Log.hpp"
#include "modules/storage/StoreKeys.hpp"

namespace app {
namespace {

/** @brief 将协议层的 SatTrackConfig 转换为驱动脚本参数。 @param cfg: 协议配置。 @retval 脚本配置。 */
::gnss::SatMaskConfig toSatMaskConfig(const core::SatTrackConfig &cfg) noexcept
{
    return {
        .cutoffDeg = cfg.cutoffDeg,
        .gps = cfg.gps != 0U,
        .glo = cfg.glo != 0U,
        .bds = cfg.bds != 0U,
        .gal = cfg.gal != 0U,
        .sbas = cfg.sbas != 0U,
    };
}

} // namespace

bool GnssModule::canStartPhonePpp() noexcept
{
    return controller().state() == ::gnss::State::kRunning && !boot_baud_pending_ &&
           !boot_baud_in_flight_ && !boot_baud_save_in_flight_ && !baud_deferred_ &&
           pending_apply_ == PendingApply::kNone &&
           !(work_mode_.has_value() && work_mode_->inFlight());
}

void GnssModule::cmdPhonePpp(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const ::gnss::Model model = controller().detected().probe.model;
    if (cmd.argLen != 1U || cmd.args[0] > 1U || cmd.seq == 0U) {
        result.status = fw::Result::kInvalidArg;
    } else if (!canStartPhonePpp()) {
        result.status = fw::Result::kBusy;
    } else if (!::gnss::supportsPpp(model)) {
        result.status = fw::Result::kNotSupported;
    } else if (!ctx().bootConfig.userNvsReady || ctx().bootConfig.safeDefaults) {
        result.status = fw::Result::kInvalidState;
    } else {
        phone_ppp_enabled_ = cmd.args[0] != 0U;
        phone_ppp_seq_ = cmd.seq;
        phone_ppp_.start(phone_ppp_enabled_);
    }
}

void GnssModule::cmdPhoneSatCfg(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const auto decoded = core::decodeSatTrackConfig(std::span{cmd.args}.first(cmd.argLen));
    if (!decoded.has_value() || cmd.seq == 0U) {
        result.status = fw::Result::kInvalidArg;
    } else if (!canStartPhonePpp()) {
        result.status = fw::Result::kBusy;
    } else if (!::gnss::supportsSatMask(controller().detected().probe.model)) {
        result.status = fw::Result::kNotSupported;
    } else if (!ctx().bootConfig.userNvsReady || ctx().bootConfig.safeDefaults) {
        result.status = fw::Result::kInvalidState;
    } else {
        // decodeSatTrackConfig 已校验 cutoffDeg <= 90，start 必成功。
        const bool started = phone_sat_.start(toSatMaskConfig(*decoded));
        if (started) {
            phone_sat_cfg_ = *decoded;
            phone_sat_seq_ = cmd.seq;
        } else {
            result.status = fw::Result::kInvalidState;
        }
    }
}

bool GnssModule::writePhoneTx(std::span<const uint8_t> tx) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const int written = uart_write_bytes(port, tx.data(), tx.size());
    if (written > 0) {
        // 已排队的字节数为正，故转换无损。
        uart_tx_bytes_ += static_cast<uint32_t>(written);
    }
    return (written >= 0) && (static_cast<size_t>(written) == tx.size());
}

void GnssModule::startPendingPhoneConfig() noexcept
{
    const auto pppCmd = ctx().data.phonePppCommand.read();
    if (pppCmd.seq != ctx().data.phonePppResult.read().seq) {
        fw::CommandResult result{.seq = pppCmd.seq};
        onCommand(pppCmd, result);
        if (!fw::isOk(result.status)) {
            ctx().data.phonePppResult.write(result);
        }
        return;
    }
    const auto satCmd = ctx().data.phoneSatCommand.read();
    if (satCmd.seq != ctx().data.phoneSatResult.read().seq) {
        fw::CommandResult result{.seq = satCmd.seq};
        onCommand(satCmd, result);
        if (!fw::isOk(result.status)) {
            ctx().data.phoneSatResult.write(result);
        }
        return;
    }
    if (!canStartPhonePpp()) {
        return;
    }
    if (phone_sat_replay_) {
        phone_sat_replay_ = false;
        const auto saved = ctx().data.phoneSatConfig.read();
        if (saved.has_value() && ::gnss::supportsSatMask(controller().detected().probe.model)) {
            static_cast<void>(phone_sat_.start(toSatMaskConfig(*saved)));
            return;
        }
    }
    if (phone_ppp_replay_) {
        phone_ppp_replay_ = false;
        const auto saved = ctx().data.phonePppEnabled.read();
        // 缺少键或型号不支持 PPP（如 CM680）时不重放，保留接收机原配置。
        if (saved.has_value() && ::gnss::supportsPpp(controller().detected().probe.model)) {
            phone_ppp_.start(*saved);
        }
    }
}

bool GnssModule::pollPhonePpp(uint32_t nowMs) noexcept
{
    if (!phone_ppp_.active() && !phone_sat_.active()) {
        startPendingPhoneConfig();
    }
    if (phone_ppp_.active()) {
        std::span<const uint8_t> tx{};
        const auto result = phone_ppp_.poll(nowMs, tx);
        if (result.has_value()) {
            finishPhonePpp(*result);
        } else if (!tx.empty() && !writePhoneTx(tx)) {
            phone_ppp_ = {};
            finishPhonePpp(fw::Result::kIoError);
        }
        return true;
    }
    if (phone_sat_.active()) {
        std::span<const uint8_t> tx{};
        const auto result = phone_sat_.poll(nowMs, tx);
        if (result.has_value()) {
            finishPhoneSatCfg(*result);
        } else if (!tx.empty() && !writePhoneTx(tx)) {
            phone_sat_ = {};
            finishPhoneSatCfg(fw::Result::kIoError);
        }
        return true;
    }
    return false;
}

void GnssModule::finishPhonePpp(fw::Result status) noexcept
{
    if (fw::isOk(status) && phone_ppp_seq_ != 0U) {
        const uint32_t enabled = phone_ppp_enabled_ ? 1U : 0U;
        if (store::writeValue(store::kPhonePppKey, enabled) == ESP_OK) {
            ctx().data.phonePppEnabled.write(phone_ppp_enabled_);
            phone_ppp_replay_ = false;
        } else {
            status = fw::Result::kIoError;
        }
    }
    if (!fw::isOk(status)) {
        FW_LOGW(fw::TraceCat::kGnss, "<ppp> configuration failed (%s)", fw::toString(status));
    }
    if (phone_ppp_seq_ != 0U) {
        ctx().data.phonePppResult.write({.seq = phone_ppp_seq_, .status = status});
        phone_ppp_seq_ = 0U;
    }
}

void GnssModule::finishPhoneSatCfg(fw::Result status) noexcept
{
    if (fw::isOk(status) && phone_sat_seq_ != 0U) {
        const auto encoded = core::encodeSatTrackConfig(phone_sat_cfg_);
        if (store::writeBlob(store::kPhoneSatCfgKey, encoded) == ESP_OK) {
            ctx().data.phoneSatConfig.write(phone_sat_cfg_);
            phone_sat_replay_ = false;
        } else {
            status = fw::Result::kIoError;
        }
    }
    if (!fw::isOk(status)) {
        FW_LOGW(fw::TraceCat::kGnss, "<sat> configuration failed (%s)", fw::toString(status));
    }
    if (phone_sat_seq_ != 0U) {
        ctx().data.phoneSatResult.write({.seq = phone_sat_seq_, .status = status});
        phone_sat_seq_ = 0U;
    }
}

} // namespace app
