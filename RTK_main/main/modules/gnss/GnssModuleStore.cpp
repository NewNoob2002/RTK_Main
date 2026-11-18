// GNSS 模块 NVS 绑定：型号/波特率/频率/GSV/输出配置的编解码（ADR-0011 存储分层）。
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
using detail::kBaudMax;
using detail::kBaudMin;
using detail::readU32;
using detail::traceGnssSub;
using detail::writeU32;

fw::Result GnssModule::loadFamily(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty()) {
        return fw::Result::kInvalidArg;
    }
    // 家族号越界（旧格式或损坏）由 familyFromU8() 报告，避免手工维护上限。
    const std::optional<::gnss::Family> family = ::gnss::familyFromU8(in[0]);
    if (!family.has_value()) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.hint_.family = *family;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveFamily(const void * /*obj*/, std::span<uint8_t> out,
                                  size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    out[0] = static_cast<uint8_t>(appContext().gnss.hint_.family);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadModel(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty()) {
        return fw::Result::kInvalidArg;
    }
    const std::optional<::gnss::Model> model = ::gnss::modelFromU8(in[0]);
    if (!model.has_value()) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.hint_.model = *model;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveModel(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    out[0] = static_cast<uint8_t>(appContext().gnss.hint_.model);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadBaud(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() < 4U) {
        return fw::Result::kInvalidArg;
    }
    const uint32_t baud = readU32(in);
    if ((baud != 0U) && ((baud < kBaudMin) || (baud > kBaudMax))) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.hint_.baud = baud;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveBaud(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.size() < 4U) {
        return fw::Result::kInvalidArg;
    }
    writeU32(appContext().gnss.hint_.baud, out);
    len = 4U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadUserBaud(void *, std::span<const uint8_t> in) noexcept
{
    if (in.size() != sizeof(uint32_t)) {
        return fw::Result::kInvalidArg;
    }
    const uint32_t baud = readU32(in);
    if (!gnss_store::validUserBaud(baud) && baud != CONFIG_RTK_GNSS_DEFAULT_BAUD) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.preferred_baud_ = baud;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveUserBaud(const void *, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.size() < sizeof(uint32_t)) {
        return fw::Result::kNoSpace;
    }
    const uint32_t baud = appContext().gnss.preferred_baud_;
    writeU32(baud == 0U ? CONFIG_RTK_GNSS_DEFAULT_BAUD : baud, out);
    len = sizeof(uint32_t);
    return fw::Result::kOk;
}

fw::Result GnssModule::loadRate(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || !isValidRateHz(in[0])) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.ensureRoverCfg().rateHz = in[0];
    appContext().gnss.rate_loaded_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveRate(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    const auto &gnss = appContext().gnss;
    out[0] = (gnss.rate_loaded_ && gnss.rover_cfg_.has_value())
                 ? gnss.rover_cfg_->rateHz
                 : static_cast<uint8_t>(CONFIG_RTK_GNSS_RATE_HZ);
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadGsv(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || (in[0] > 1U)) {
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.ensureRoverCfg().gsv = (in[0] != 0U);
    appContext().gnss.gsv_loaded_ = true;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveGsv(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    if (out.empty()) {
        return fw::Result::kInvalidArg;
    }
    const auto &gnss = appContext().gnss;
    out[0] =
        (gnss.gsv_loaded_ && gnss.rover_cfg_.has_value()) ? (gnss.rover_cfg_->gsv ? 1U : 0U) : 1U;
    len = 1U;
    return fw::Result::kOk;
}

fw::Result GnssModule::loadOut(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    data::GnssOutConfig parsed{};
    if (!fw::isOk(data::GnssOutConfig::decode(in, parsed))) {
        auto &gnss = appContext().gnss;
        if (!gnss.out_cfg_.isValid()) {
            gnss.out_cfg_ = data::GnssOutConfig::defaults();
        }
        traceGnssSub(GnssSubTrace::kOutCfg, gnss.out_cfg_.mask, 1U);
        return fw::Result::kInvalidArg;
    }
    appContext().gnss.out_cfg_ = parsed;
    return fw::Result::kOk;
}

fw::Result GnssModule::saveOut(const void * /*obj*/, std::span<uint8_t> out, size_t &len) noexcept
{
    const auto &cfg = appContext().gnss.out_cfg_;
    const auto effective = cfg.isValid() ? cfg : data::GnssOutConfig::defaults();
    return effective.encode(out, len);
}

} // namespace app
