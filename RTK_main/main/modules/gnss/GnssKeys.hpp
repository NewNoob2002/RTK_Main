// GNSS 模块的持久化键表（ADR-0010 §2.7）：NVS 命名空间 "gnss"，分为用户配置组与探测缓存组（storage 目录见
// storeCatalog）。 常量表，可被 storage 在启动早期遍历；非 ISR，无副作用。
#pragma once

#include <algorithm>
#include <array>

#include "data/GnssOutput.hpp"
#include "fw/StoreKey.hpp"
#include "modules/gnss/GnssBaseConfig.hpp"
#include "modules/gnss/GnssModule.hpp"

namespace app::gnss_store {

inline constexpr size_t kRateKeyIndex = 0U;
inline constexpr size_t kOutKeyIndex = 2U;
inline constexpr size_t kGgaGateKeyIndex = 3U;
inline constexpr size_t kGgaMinSatsKeyIndex = 4U;

/**
 * @brief 校验设置命令允许的串口速率。
 * @param baud: 波特率。
 * @retval true 支持；false 其他。
 */
constexpr bool validUserBaud(uint32_t baud) noexcept
{
    constexpr std::array<uint32_t, 8U> kSupported{9600U,   19200U,  38400U,  57600U,
                                                  115200U, 230400U, 460800U, 921600U};
    for (const uint32_t candidate : kSupported) {
        if (baud == candidate) {
            return true;
        }
    }
    return false;
}

// 用户配置组 gnss.config：测量速率、GSV 开关、统一输出配置（gnss/out）、GGA 质量门控与波特率。
inline constexpr std::array<fw::StoreKey, 6U> kConfigKeys = {
    {{.ns = "gnss",
      .key = "rate",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = fw::store_flag::kConsole,
      .min = 1,
      .max = 20,
      .help = "GNSS position output rate (Hz: 1/2/5/10/20)",
      .binding = {.obj = nullptr, .load = &GnssModule::loadRate, .save = &GnssModule::saveRate}},
     {.ns = "gnss",
      .key = "gsv",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = fw::store_flag::kConsole,
      .min = 0,
      .max = 1,
      .help = "GSV satellite view output (0/1)",
      .binding = {.obj = nullptr, .load = &GnssModule::loadGsv, .save = &GnssModule::saveGsv}},
     {.ns = "gnss",
      .key = "out",
      .type = fw::StoreType::kBlob,
      .maxSize = app::data::GnssOutConfig::kBlobBytes,
      .flags = fw::store_flag::kConsole | fw::store_flag::kExport,
      .min = 0,
      .max = 0,
      .help = "GNSS receiver & app output defaults (blob v1)",
      .binding = {.obj = nullptr, .load = &GnssModule::loadOut, .save = &GnssModule::saveOut}},
     {.ns = "gnss",
      .key = "gga_gate",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = fw::store_flag::kConsole | fw::store_flag::kExport,
      .min = 0,
      .max = 1,
      .help = "GGA quality gate enable (0/1)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadGgaGate,
                  .save = &GnssModule::saveGgaGate}},
     {.ns = "gnss",
      .key = "gga_min_sats",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = fw::store_flag::kConsole | fw::store_flag::kExport,
      .min = 0,
      .max = ::gnss::GgaGateConfig::kMaxMinSats,
      .help = "GGA quality gate minimum satellites (0-40)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadGgaMinSats,
                  .save = &GnssModule::saveGgaMinSats}},
     {.ns = "gnss",
      .key = "baud",
      .type = fw::StoreType::kU32,
      .maxSize = 4U,
      .flags = fw::store_flag::kConsole,
      .min = 9600,
      .max = 921600,
      .help = "GNSS receiver UART baud",
      .binding = {
          .obj = nullptr, .load = &GnssModule::loadUserBaud, .save = &GnssModule::saveUserBaud}}}};

// 探测缓存组 gnss.cache：上次识别到的芯片族/型号/波特率，只作下次启动探测的提示，不是用户配置。
inline constexpr std::array<fw::StoreKey, 3U> kCacheKeys = {
    {{.ns = "gnss",
      .key = "family",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = 0U,
      .min = 0,
      .max = 255,
      .help = "last detected GNSS chip family",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadFamily,
                  .save = &GnssModule::saveFamily}},
     {.ns = "gnss",
      .key = "model",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = 0U,
      .min = 0,
      .max = 255,
      .help = "last detected GNSS model",
      .binding = {.obj = nullptr, .load = &GnssModule::loadModel, .save = &GnssModule::saveModel}},
     {.ns = "gnss",
      .key = "cache_baud",
      .type = fw::StoreType::kU32,
      .maxSize = 4U,
      .flags = 0U,
      .min = 0,
      .max = 2000000,
      .help = "last detected GNSS UART baud",
      .binding = {.obj = nullptr, .load = &GnssModule::loadBaud, .save = &GnssModule::saveBaud}}}};

inline constexpr size_t kBaseModeKeyIdx = 0U;
inline constexpr size_t kBaseTypeKeyIdx = 1U;
inline constexpr size_t kBaseSvySKeyIdx = 2U;
inline constexpr size_t kBaseSvyAccCmKeyIdx = 3U;
inline constexpr size_t kBaseSvyToSKeyIdx = 4U;
inline constexpr size_t kBaseFixLatKeyIdx = 5U;
inline constexpr size_t kBaseFixLonKeyIdx = 6U;
inline constexpr size_t kBaseFixHMmKeyIdx = 7U;
inline constexpr size_t kBaseRtcmOutKeyIdx = 8U;
inline constexpr size_t kBaseMsmKeyIdx = 9U;

// 工作模式与基站配置组 gnss.base（ADR-0014 §2.5/§2.6）。
inline constexpr std::array<fw::StoreKey, 10U> kBaseKeys = {
    {{.ns = "gnss",
      .key = "mode",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = fw::store_flag::kConsole,
      .min = 0,
      .max = 2,
      .help = "GNSS work mode (0=rover, 1=base, 2=smartbase)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseModeKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseModeKeyIdx>}},
     {.ns = "gnss",
      .key = "base_type",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = fw::store_flag::kConsole,
      .min = 0,
      .max = 1,
      .help = "Base position source (0=survey, 1=fixed)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseTypeKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseTypeKeyIdx>}},
     {.ns = "gnss",
      .key = "svy_s",
      .type = fw::StoreType::kU16,
      .maxSize = 2U,
      .flags = fw::store_flag::kConsole,
      .min = GnssBaseConfig::kMinSvyS,
      .max = GnssBaseConfig::kMaxSvyS,
      .help = "Survey-in minimum duration (s)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseSvySKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseSvySKeyIdx>}},
     {.ns = "gnss",
      .key = "svy_acc_cm",
      .type = fw::StoreType::kU16,
      .maxSize = 2U,
      .flags = fw::store_flag::kConsole,
      .min = GnssBaseConfig::kMinSvyAccCm,
      .max = GnssBaseConfig::kMaxSvyAccCm,
      .help = "Survey-in accuracy threshold (cm)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseSvyAccCmKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseSvyAccCmKeyIdx>}},
     {.ns = "gnss",
      .key = "svy_to_s",
      .type = fw::StoreType::kU16,
      .maxSize = 2U,
      .flags = fw::store_flag::kConsole,
      .min = GnssBaseConfig::kMinSvyToS,
      .max = GnssBaseConfig::kMaxSvyToS,
      .help = "Survey-in total timeout (s)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseSvyToSKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseSvyToSKeyIdx>}},
     {.ns = "gnss",
      .key = "fix_lat",
      .type = fw::StoreType::kBlob,
      .maxSize = GnssBaseConfig::kFixCoordBlobBytes,
      .flags = fw::store_flag::kConsole | fw::store_flag::kExport,
      .min = 0,
      .max = 0,
      .help = "Fixed base latitude (int64 LE 1e-9 deg)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseFixLatKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseFixLatKeyIdx>}},
     {.ns = "gnss",
      .key = "fix_lon",
      .type = fw::StoreType::kBlob,
      .maxSize = GnssBaseConfig::kFixCoordBlobBytes,
      .flags = fw::store_flag::kConsole | fw::store_flag::kExport,
      .min = 0,
      .max = 0,
      .help = "Fixed base longitude (int64 LE 1e-9 deg)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseFixLonKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseFixLonKeyIdx>}},
     {.ns = "gnss",
      .key = "fix_h_mm",
      .type = fw::StoreType::kBlob,
      .maxSize = GnssBaseConfig::kFixHeightBlobBytes,
      .flags = fw::store_flag::kConsole | fw::store_flag::kExport,
      .min = 0,
      .max = 0,
      .help = "Fixed base ellipsoidal height (int32 LE mm)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseFixHMmKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseFixHMmKeyIdx>}},
     {.ns = "gnss",
      .key = "rtcm_out",
      .type = fw::StoreType::kBlob,
      .maxSize = GnssBaseConfig::kRtcmOutBlobBytes,
      .flags = fw::store_flag::kConsole | fw::store_flag::kExport,
      .min = 0,
      .max = 0,
      .help = "Base RTCM3 output rates (14B blob v1)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseRtcmOutKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseRtcmOutKeyIdx>}},
     {.ns = "gnss",
      .key = "msm",
      .type = fw::StoreType::kU8,
      .maxSize = 1U,
      .flags = fw::store_flag::kConsole,
      .min = GnssBaseConfig::kMsm4,
      .max = GnssBaseConfig::kMsm7,
      .help = "Base MSM version (4 or 7)",
      .binding = {.obj = nullptr,
                  .load = &GnssModule::loadBaseSlot<kBaseMsmKeyIdx>,
                  .save = &GnssModule::saveBaseSlot<kBaseMsmKeyIdx>}}}};

static_assert(std::ranges::all_of(kConfigKeys, fw::valid), "gnss.config 键必须合法");
static_assert(std::ranges::all_of(kBaseKeys, fw::valid), "gnss.base 键必须合法");
static_assert(kConfigKeys.back().key == "baud", "早期波特率键必须是 gnss.config 末项");
static_assert(kConfigKeys[kRateKeyIndex].key == "rate" && kConfigKeys[kOutKeyIndex].key == "out" &&
              kConfigKeys[kGgaGateKeyIndex].key == "gga_gate" &&
              kConfigKeys[kGgaMinSatsKeyIndex].key == "gga_min_sats");
static_assert(fw::valid(kCacheKeys[0]) && fw::valid(kCacheKeys[1]) && fw::valid(kCacheKeys[2]),
              "gnss.cache 键必须合法");
static_assert(fw::uniqueKeys(kConfigKeys) && fw::uniqueKeys(kCacheKeys) &&
                  fw::uniqueKeys(kBaseKeys),
              "gnss 键表不得有重复的 ns/key");

} // namespace app::gnss_store
