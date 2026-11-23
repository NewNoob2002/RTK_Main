// 模块声明与注册清单：类型、成员、名称、编号、执行器与周期共用一份顺序。
// 仅启动装配使用；无运行期状态、不分配堆；MODULE_DEF 生成计数、成员、绑定与模块表。
// 增删条目后复核 AppContext.hpp 的三处展开与 app/Executors.cpp 的注册表（ADR-0013 §2.3）。
#ifndef MODULE_DEF
#include "AppConfig.hpp" // RTK_GNSS_ENABLED
#include "modules/bt_link/BtLinkModule.hpp"
#include "fw/Trace.hpp" // FW_TRACE_ENABLED
#if FW_TRACE_ENABLED
#include "modules/debug/DebugModule.hpp"
#endif
#if RTK_GNSS_ENABLED
#include "modules/gnss/GnssModule.hpp"
#endif
#include "modules/power/PowerModule.hpp"
#include "modules/rtcm/RtcmModule.hpp"
#include "modules/storage/StorageModule.hpp"
#include "modules/system/SystemModule.hpp"
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
#include "modules/web/WebModule.hpp"
#endif
#else
// MODULE_DEF(类型, 成员, 名称, ModuleKind, ExecKind, poll_period_ms, ModuleEnable)
MODULE_DEF(BtLinkModule, bt_link, "bt_link", kBtLink, kLink, 0, kBtOnly)
MODULE_DEF(RtcmModule, rtcm, "rtcm", kRtcm, kLink, 0, kBtOnly)
// storage.init 必须先于 system.prepare（读取身份）；执行器再次 init 时为幂等。
MODULE_DEF(StorageModule, storage, "storage", kStorage, kLink, 0, kAlways)
MODULE_DEF(SystemModule, system, "system", kSystem, kLink, 0, kAlways)
MODULE_DEF(PowerModule, power, "power", kPower, kLink, 10, kAlways)
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE) && CONFIG_RTK_WIFI_CONFIG_ENABLE
MODULE_DEF(WebModule, web, "web", kWeb, kLink, 0, kWifiOnly)
#endif
#if RTK_GNSS_ENABLED
MODULE_DEF(GnssModule, gnss, "gnss", kGnss, kIo, 0, kNormalOnly)
#endif
// debug 必须在 link 执行器内：追踪环由它的输出任务独占读取，命令也在这里改状态。
// 追踪关闭（CONFIG_RTK_TRACE_ENABLE=n）时不登记（ADR-0009 §2.6）；FW_TRACE_ENABLED 由上方 fw/Trace.hpp 定义。
#if FW_TRACE_ENABLED
MODULE_DEF(DebugModule, debug, "debug", kDebug, kLink, 0, kAlways)
#endif
#endif
