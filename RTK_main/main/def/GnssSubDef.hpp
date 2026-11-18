// GNSS App 数据通路（ADR-0011）扩展命令字与只追加追踪事件码；不修改冻结的 def/GnssDef.hpp。
// 线程安全：纯常量与数据结构定义。ISR 安全：是。
#pragma once

#include <cstdint>

namespace app {

/**
 * 投递给 GnssModule 的 扩展命令字（接续 def/GnssDef.hpp 中 GnssVerb 0..3，从 4 起编）。
 * args[0..7] 按 GnssOutConfig::kBlobBytes 布局编码。
 */
enum class GnssSubVerb : uint8_t {
    kSetOut = 4,
    kReadback = 5, // 控制台 gnss config：读回模块配置（ADR-0012 §2.7），无参数
};

/**
 * TraceCat::kGnss 的 追加追踪事件码（接续 def/GnssDef.hpp 中 GnssTrace 0..9，从 10 起编，不重排旧码）。
 */
enum class GnssSubTrace : uint8_t {
    kAppMask = 10, // val0 = 订阅掩码，val1 = 变化来源（0=App订阅/查询，1=会话断开归零）
    kAppDrop = 11, // val0 = 丢弃源（0=App环超长，1=查询超256B，2=最新帧缓存超限），val1 = typeIdx
    kOutCfg = 12,  // val0 = 启用掩码，val1 = 状态（0=加载/生效，1=回退默认）
};

} // namespace app
