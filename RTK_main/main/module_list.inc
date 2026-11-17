// 模块声明与注册清单；类型、成员、名称、编号、执行器、周期在此保持同一顺序。
// X-macro（ADR-0013 §2.3）：同一份清单生成 4 种产物，展开点全部在 main/ 内：
//   main/AppContext.hpp    MODULE_DEF(...) +1                         → kModuleCount 计数
//   main/AppContext.hpp    MODULE_DEF(...) type member{};             → AppContext 成员声明
//   main/AppContext.hpp    MODULE_DEF(...) member.bindContext(*this); → bindContexts() 绑定
//   main/app/Executors.cpp MODULE_DEF(...) {name,...}                 → kModules 模块表
// 增删本文件条目后按 `rg -n 'module_list.inc' main/` 复核以上 4 处仍与之匹配。
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
#include "modules/rtcm/RtcmModule.hpp"
#include "modules/storage/StorageModule.hpp"
#include "modules/system/SystemModule.hpp"
#else
// MODULE_DEF(类型, 成员, 名称, ModuleKind, ExecKind, poll_period_ms)
MODULE_DEF(BtLinkModule, bt_link, "bt_link", kBtLink, kLink, 0)
MODULE_DEF(RtcmModule, rtcm, "rtcm", kRtcm, kLink, 0)
// storage.init 必须在 system.prepare（BT 初始化）之前单独调用；执行器再次 init 时为幂等。
MODULE_DEF(StorageModule, storage, "storage", kStorage, kLink, 0)
MODULE_DEF(SystemModule, system, "system", kSystem, kLink, 0)
#if RTK_GNSS_ENABLED
MODULE_DEF(GnssModule, gnss, "gnss", kGnss, kIo, 0)
#endif
// debug 必须在 link 执行器内：追踪环由它的输出任务独占读取，命令也在这里改状态。
// 追踪关闭（CONFIG_RTK_TRACE_ENABLE=n）时不登记（ADR-0009 §2.6）；FW_TRACE_ENABLED 由上方 fw/Trace.hpp 定义。
#if FW_TRACE_ENABLED
MODULE_DEF(DebugModule, debug, "debug", kDebug, kLink, 0)
#endif
#endif
