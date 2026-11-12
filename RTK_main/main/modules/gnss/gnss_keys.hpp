// GNSS 模块的持久化键表（ADR-0010 §2.7）：NVS 命名空间 "gnss"，保存探测提示与测量速率/GSV 开关。
// 常量表，可被 storage 在启动早期遍历；非 ISR，无副作用。
#pragma once

#include <array>

#include "fw/store_key.hpp"
#include "modules/gnss/gnss_module.hpp"

namespace app::gnss_store {

inline constexpr std::array<fw::StoreKey, 5U> kKeys = {{
    {.ns = "gnss",
     .key = "family",
     .type = fw::StoreType::kU8,
     .max_size = 1U,
     .flags = 0U,
     .min = 0,
     .max = 255,
     .help = "上次探测到的 GNSS 芯片族",
     .binding = {.obj = nullptr, .load = &GnssModule::load_family, .save = &GnssModule::save_family}},
    {.ns = "gnss",
     .key = "model",
     .type = fw::StoreType::kU8,
     .max_size = 1U,
     .flags = 0U,
     .min = 0,
     .max = 255,
     .help = "上次探测到的 GNSS 型号",
     .binding = {.obj = nullptr, .load = &GnssModule::load_model, .save = &GnssModule::save_model}},
    {.ns = "gnss",
     .key = "baud",
     .type = fw::StoreType::kU32,
     .max_size = 4U,
     .flags = 0U,
     .min = 0,
     .max = 2000000,
     .help = "上次探测到的 GNSS 串口波特率",
     .binding = {.obj = nullptr, .load = &GnssModule::load_baud, .save = &GnssModule::save_baud}},
    {.ns = "gnss",
     .key = "rate",
     .type = fw::StoreType::kU8,
     .max_size = 1U,
     .flags = fw::store_flag::kConsole,
     .min = 1,
     .max = 20,
     .help = "GNSS 定位输出频率 (Hz: 1/2/5/10/20)",
     .binding = {.obj = nullptr, .load = &GnssModule::load_rate, .save = &GnssModule::save_rate}},
    {.ns = "gnss",
     .key = "gsv",
     .type = fw::StoreType::kU8,
     .max_size = 1U,
     .flags = fw::store_flag::kConsole,
     .min = 0,
     .max = 1,
     .help = "GSV 卫星视图输出开关 (0/1)",
     .binding = {.obj = nullptr, .load = &GnssModule::load_gsv, .save = &GnssModule::save_gsv}},
}};

inline constexpr size_t kFamilyKeyIdx = 0U;
inline constexpr size_t kModelKeyIdx = 1U;
inline constexpr size_t kBaudKeyIdx = 2U;
inline constexpr size_t kRateKeyIdx = 3U;
inline constexpr size_t kGsvKeyIdx = 4U;

static_assert(fw::valid(kKeys[kFamilyKeyIdx]), "gnss/family 必须是合法 u8 键");
static_assert(fw::valid(kKeys[kModelKeyIdx]), "gnss/model 必须是合法 u8 键");
static_assert(fw::valid(kKeys[kBaudKeyIdx]), "gnss/baud 必须是合法 u32 键");
static_assert(fw::valid(kKeys[kRateKeyIdx]), "gnss/rate 必须是合法 u8 键");
static_assert(fw::valid(kKeys[kGsvKeyIdx]), "gnss/gsv 必须是合法 u8 键");
static_assert(fw::unique_keys(kKeys), "gnss 键表不得有重复的 ns/key");

} // namespace app::gnss_store
