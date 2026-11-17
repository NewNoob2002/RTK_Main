// 启动降级原因常量表（CODING-STYLE §5.x 规则 2）：所有 setDegradedReason/abort/步骤失败原因只引用这里的常量。
// 字符串内容即 `sys mem`/`sys boot` 显示的诊断文本，修改即改变对外诊断输出；均为静态存储，可跨任务长期引用。
#pragma once

namespace app::boot_reason {

// 启动缓冲分配与绑定（BootBuffers）
inline constexpr const char *kAllocOpen = "boot_alloc:open";
inline constexpr const char *kAllocSeal = "boot_alloc:seal";
inline constexpr const char *kAllocAbort = "boot_alloc:abort";
inline constexpr const char *kBindDebug = "bind:debug";
inline constexpr const char *kBindSpp = "bind:spp";
inline constexpr const char *kBindGnss = "bind:gnss";
inline constexpr const char *kRollbackSppInUse = "rollback:spp_in_use";

// 模块初始化步骤（BootFlow）
inline constexpr const char *kDebugInit = "debug_init";
inline constexpr const char *kStorageInit = "storage_init";
inline constexpr const char *kTaskStartTimeout = "task_start_timeout";

// 执行器任务启动（Executors::startLink/startIo）
inline constexpr const char *kLinkInit = "link_init";
inline constexpr const char *kLinkTaskStart = "link_task_start";
inline constexpr const char *kLinkModuleStart = "link_module_start";
inline constexpr const char *kIoInit = "io_init";
inline constexpr const char *kIoTaskStart = "io_task_start";
inline constexpr const char *kIoModuleStart = "io_module_start";

// 失败停机（Executors::stopBootTasks）
inline constexpr const char *kStopTaskTimeout = "stop:task_timeout";
inline constexpr const char *kStopGnss = "stop:gnss";
inline constexpr const char *kStopSpp = "stop:spp";

// 故障注入（RTK_BOOT_FAULT_STEP）：实际原因为 kFaultPrefix + 步骤名，写缓冲失败时用回退值。
inline constexpr const char *kFaultPrefix = "fault:";
inline constexpr const char *kFaultFallback = "fault:step";

} // namespace app::boot_reason
