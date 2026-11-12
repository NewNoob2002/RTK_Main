// debug 模块的持久化键表（ADR-0009 §2.4）：NVS 命名空间 "dbg"，逐键 u32 掩码与 u16 报告周期。
// 常量表，可被 storage 在启动早期遍历；非 ISR，无副作用。
#pragma once

#include <array>

#include "fw/store_key.hpp"
#include "modules/debug/debug_module.hpp"

namespace app::debug {

// 键表必须满足 fw::valid（类型宽度与取值范围一致），否则 NVS 通路会拒绝它。
inline constexpr std::array<fw::StoreKey, 2U> kKeys = {{
    {.ns = "dbg",
     .key = "mask",
     .type = fw::StoreType::kU32,
     .max_size = 4U,
     .flags = fw::store_flag::kConsole,
     .min = 0,
     .max = static_cast<int32_t>(fw::kTraceAllMask),
     .help = "enabled trace category mask",
     .binding = {.obj = nullptr, .load = &DebugModule::load_mask, .save = &DebugModule::save_mask}},
    {.ns = "dbg",
     .key = "report",
     .type = fw::StoreType::kU16,
     .max_size = 2U,
     .flags = fw::store_flag::kConsole,
     .min = 0,
     .max = 3600,
     .help = "periodic report interval (s, 0 = off)",
     .binding = {.obj = nullptr, .load = &DebugModule::load_report, .save = &DebugModule::save_report}},
}};

static_assert(fw::valid(kKeys[0]), "dbg/mask 必须是合法 u32 键");
static_assert(fw::valid(kKeys[1]), "dbg/report 必须是合法 u16 键");
static_assert(fw::unique_keys(kKeys), "debug 键表不得有重复的 ns/key");

} // namespace app::debug
