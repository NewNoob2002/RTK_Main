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

bool GnssModule::canStartSeq() noexcept
{
    return controller().state() == ::gnss::State::kRunning && !boot_baud_pending_ &&
           !boot_baud_in_flight_ && !boot_baud_save_in_flight_ && !baud_deferred_ &&
           pending_apply_ == PendingApply::kNone &&
           !(work_mode_.has_value() && work_mode_->inFlight());
}

void GnssModule::cmdApplyPpp(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const ::gnss::Model model = controller().detected().probe.model;
    if (cmd.argLen != 1U || cmd.args[0] > 1U || cmd.seq == 0U) {
        result.status = fw::Result::kInvalidArg;
    } else if (!canStartSeq()) {
        result.status = fw::Result::kBusy;
    } else if (!::gnss::supportsPpp(model)) {
        result.status = fw::Result::kNotSupported;
    } else if (!ctx().bootConfig.userNvsReady || ctx().bootConfig.safeDefaults) {
        result.status = fw::Result::kInvalidState;
    } else {
        ppp_target_enabled_ = cmd.args[0] != 0U;
        ppp_ticket_seq_ = cmd.seq;
        ppp_seq_.start(ppp_target_enabled_);
        result.status = fw::Result::kOk;
        result.detail = fw::kDetailDeferred;
    }
}

void GnssModule::cmdApplySatTrack(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const auto decoded = core::decodeSatTrackConfig(std::span{cmd.args}.first(cmd.argLen));
    if (!decoded.has_value() || cmd.seq == 0U) {
        result.status = fw::Result::kInvalidArg;
    } else if (!canStartSeq()) {
        result.status = fw::Result::kBusy;
    } else if (!::gnss::supportsSatMask(controller().detected().probe.model)) {
        result.status = fw::Result::kNotSupported;
    } else if (!ctx().bootConfig.userNvsReady || ctx().bootConfig.safeDefaults) {
        result.status = fw::Result::kInvalidState;
    } else {
        // decodeSatTrackConfig 已校验 cutoffDeg <= 90，start 必成功。
        const bool started = sat_seq_.start(toSatMaskConfig(*decoded));
        if (started) {
            sat_target_cfg_ = *decoded;
            sat_ticket_seq_ = cmd.seq;
            result.status = fw::Result::kOk;
            result.detail = fw::kDetailDeferred;
        } else {
            result.status = fw::Result::kInvalidState;
        }
    }
}

bool GnssModule::writeSeqTx(std::span<const uint8_t> tx) noexcept
{
    const auto port = static_cast<uart_port_t>(CONFIG_RTK_GNSS_UART_NUM);
    const int written = uart_write_bytes(port, tx.data(), tx.size());
    if (written > 0) {
        // 已排队的字节数为正，故转换无损。
        uart_tx_bytes_ += static_cast<uint32_t>(written);
    }
    return (written >= 0) && (static_cast<size_t>(written) == tx.size());
}

void GnssModule::startPendingReplay() noexcept
{
    if (!canStartSeq()) {
        return;
    }
    if (sat_replay_) {
        sat_replay_ = false;
        const auto saved = ctx().data.gnssSatTrack.read();
        if (saved.has_value() && ::gnss::supportsSatMask(controller().detected().probe.model)) {
            static_cast<void>(sat_seq_.start(toSatMaskConfig(*saved)));
            return;
        }
    }
    if (ppp_replay_) {
        ppp_replay_ = false;
        const auto saved = ctx().data.gnssPppEnabled.read();
        // 缺少键或型号不支持 PPP（如 CM680）时不重放，保留接收机原配置。
        if (saved.has_value() && ::gnss::supportsPpp(controller().detected().probe.model)) {
            ppp_seq_.start(*saved);
        }
    }
}

bool GnssModule::pollPppAndSat(uint32_t nowMs) noexcept
{
    if (!ppp_seq_.active() && !sat_seq_.active()) {
        startPendingReplay();
    }
    if (ppp_seq_.active()) {
        std::span<const uint8_t> tx{};
        const auto result = ppp_seq_.poll(nowMs, tx);
        if (result.has_value()) {
            finishPpp(*result);
        } else if (!tx.empty() && !writeSeqTx(tx)) {
            ppp_seq_ = {};
            finishPpp(fw::Result::kIoError);
        }
        return true;
    }
    if (sat_seq_.active()) {
        std::span<const uint8_t> tx{};
        const auto result = sat_seq_.poll(nowMs, tx);
        if (result.has_value()) {
            finishSatTrack(*result);
        } else if (!tx.empty() && !writeSeqTx(tx)) {
            sat_seq_ = {};
            finishSatTrack(fw::Result::kIoError);
        }
        return true;
    }
    return false;
}

void GnssModule::finishPpp(fw::Result status) noexcept
{
    if (fw::isOk(status) && ppp_ticket_seq_ != 0U) {
        const uint32_t enabled = ppp_target_enabled_ ? 1U : 0U;
        if (store::writeValue(store::kPhonePppKey, enabled) == ESP_OK) {
            ctx().data.gnssPppEnabled.write(ppp_target_enabled_);
            ctx().data.configRev.fetch_add(1U, std::memory_order_release);
            ppp_replay_ = false;
        } else {
            status = fw::Result::kIoError;
        }
    }
    if (!fw::isOk(status)) {
        FW_LOGW(fw::TraceCat::kGnss, "<ppp> configuration failed (%s)", fw::toString(status));
    }
    completeSetting(ppp_ticket_seq_, status);
}

void GnssModule::finishSatTrack(fw::Result status) noexcept
{
    if (fw::isOk(status) && sat_ticket_seq_ != 0U) {
        const auto encoded = core::encodeSatTrackConfig(sat_target_cfg_);
        if (store::writeBlob(store::kPhoneSatCfgKey, encoded) == ESP_OK) {
            ctx().data.gnssSatTrack.write(sat_target_cfg_);
            ctx().data.configRev.fetch_add(1U, std::memory_order_release);
            sat_replay_ = false;
        } else {
            status = fw::Result::kIoError;
        }
    }
    if (!fw::isOk(status)) {
        FW_LOGW(fw::TraceCat::kGnss, "<sat> configuration failed (%s)", fw::toString(status));
    }
    completeSetting(sat_ticket_seq_, status);
}

} // namespace app
