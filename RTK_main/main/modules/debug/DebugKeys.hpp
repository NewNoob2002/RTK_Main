// debug 模块的持久化键表（ADR-0009 §2.4/§2.11/§2.12）：NVS 命名空间 "dbg"，两个 u32 掩码（事件、十六进制）、
// u16 报告周期与 u32 各类别日志级别。
// 常量表，可被 storage 在启动早期遍历；非 ISR，无副作用。
#pragma once

#include <array>
#include <cstddef>

#include "fw/Log.hpp"
#include "fw/StoreKey.hpp"
#include "modules/debug/DebugModule.hpp"

namespace app::debug {

// 键表下标：static_assert 按名引用，避免字面量下标被判为魔数。
constexpr size_t kIdxMask = 0U;
constexpr size_t kIdxReport = 1U;
constexpr size_t kIdxHex = 2U;
constexpr size_t kIdxLogLevel = 3U;

// 键表必须满足 fw::valid（类型宽度与取值范围一致），否则 NVS 通路会拒绝它。
inline constexpr std::array<fw::StoreKey, 4U> kKeys = {{
    {.ns = "dbg",
     .key = "mask",
     .type = fw::StoreType::kU32,
     .maxSize = 4U,
     .flags = fw::store_flag::kConsole,
     .min = 0,
     .max = static_cast<int32_t>(fw::kTraceAllMask),
     .help = "enabled trace category mask",
     .binding = {.obj = nullptr, .load = &DebugModule::loadMask, .save = &DebugModule::saveMask}},
    {.ns = "dbg",
     .key = "report",
     .type = fw::StoreType::kU16,
     .maxSize = 2U,
     .flags = fw::store_flag::kConsole,
     .min = 0,
     .max = 3600,
     .help = "periodic report interval (s, 0 = off)",
     .binding = {.obj = nullptr,
                 .load = &DebugModule::loadReport,
                 .save = &DebugModule::saveReport}},
    {.ns = "dbg",
     .key = "hex",
     .type = fw::StoreType::kU32,
     .maxSize = 4U,
     .flags = fw::store_flag::kConsole,
     .min = 0,
     .max = static_cast<int32_t>(fw::kTraceAllMask),
     .help = "hex dump category mask",
     .binding = {.obj = nullptr, .load = &DebugModule::loadHex, .save = &DebugModule::saveHex}},
    {.ns = "dbg",
     .key = "loglv",
     .type = fw::StoreType::kU32,
     .maxSize = 4U,
     .flags = fw::store_flag::kConsole,
     .min = 0,
     .max = static_cast<int32_t>(fw::logLevelsAll(fw::LogLevel::kDebug)),
     .help = "per-category log level, 2 bits each (0=E 1=W 2=I 3=D)",
     .binding = {.obj = nullptr, .load = &DebugModule::loadLoglv, .save = &DebugModule::saveLoglv}},
}};

static_assert(fw::valid(kKeys[kIdxMask]), "dbg/mask 必须是合法 u32 键");
static_assert(fw::valid(kKeys[kIdxReport]), "dbg/report 必须是合法 u16 键");
static_assert(fw::valid(kKeys[kIdxHex]), "dbg/hex 必须是合法 u32 键");
static_assert(fw::valid(kKeys[kIdxLogLevel]), "dbg/loglv 必须是合法 u32 键");
static_assert(fw::uniqueKeys(kKeys), "debug 键表不得有重复的 ns/key");

} // namespace app::debug
