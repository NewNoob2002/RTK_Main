// GNSS 模块的持久化键表（ADR-0010 §2.7）：NVS 命名空间 "gnss"，分为用户配置组与探测缓存组（storage 目录见 store_catalog）。
// 常量表，可被 storage 在启动早期遍历；非 ISR，无副作用。
#pragma once

#include <array>

#include "fw/store_key.hpp"
#include "modules/gnss/gnss_module.hpp"

namespace app::gnss_store {

// 用户配置组 gnss.config：测量速率与 GSV 开关，控制台/手机修改后整组保存（NVS 对未变的值不重复写）。
inline constexpr std::array<fw::StoreKey, 2U> kConfigKeys = {{
    {.ns = "gnss",
     .key = "rate",
     .type = fw::StoreType::kU8,
     .max_size = 1U,
     .flags = fw::store_flag::kConsole,
     .min = 1,
     .max = 20,
     .help = "GNSS position output rate (Hz: 1/2/5/10/20)",
     .binding = {.obj = nullptr, .load = &GnssModule::load_rate, .save = &GnssModule::save_rate}},
    {.ns = "gnss",
     .key = "gsv",
     .type = fw::StoreType::kU8,
     .max_size = 1U,
     .flags = fw::store_flag::kConsole,
     .min = 0,
     .max = 1,
     .help = "GSV satellite view output (0/1)",
     .binding = {.obj = nullptr, .load = &GnssModule::load_gsv, .save = &GnssModule::save_gsv}}
}};

// 探测缓存组 gnss.cache：上次识别到的芯片族/型号/波特率，只作下次启动探测的提示，不是用户配置。
inline constexpr std::array<fw::StoreKey, 3U> kCacheKeys = {{
    {.ns = "gnss",
     .key = "family",
     .type = fw::StoreType::kU8,
     .max_size = 1U,
     .flags = 0U,
     .min = 0,
     .max = 255,
     .help = "last detected GNSS chip family",
     .binding = {.obj = nullptr, .load = &GnssModule::load_family, .save = &GnssModule::save_family}},
    {.ns = "gnss",
     .key = "model",
     .type = fw::StoreType::kU8,
     .max_size = 1U,
     .flags = 0U,
     .min = 0,
     .max = 255,
     .help = "last detected GNSS model",
     .binding = {.obj = nullptr, .load = &GnssModule::load_model, .save = &GnssModule::save_model}},
    {.ns = "gnss",
     .key = "baud",
     .type = fw::StoreType::kU32,
     .max_size = 4U,
     .flags = 0U,
     .min = 0,
     .max = 2000000,
     .help = "last detected GNSS UART baud",
     .binding = {.obj = nullptr, .load = &GnssModule::load_baud, .save = &GnssModule::save_baud}}
}};

static_assert(fw::valid(kConfigKeys[0]) && fw::valid(kConfigKeys[1]), "gnss.config 键必须合法");
static_assert(fw::valid(kCacheKeys[0]) && fw::valid(kCacheKeys[1]) && fw::valid(kCacheKeys[2]), "gnss.cache 键必须合法");
static_assert(fw::unique_keys(kConfigKeys) && fw::unique_keys(kCacheKeys), "gnss 键表不得有重复的 ns/key");

} // namespace app::gnss_store
