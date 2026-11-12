// 应用上下文拥有全部顶层可变对象，模块成员由注册清单生成。
// 仅 app_main 创建；各模块与执行器的运行期访问遵守各自线程契约，非 ISR。
#pragma once

#include "app_config.hpp"
#include "data/app_data.hpp"
#if RTK_GNSS_ENABLED
#include "modules/gnss/rtcm_pipe.hpp"
#endif
#include "module_list.inc"

namespace app {
/** 固件唯一上下文：拥有 DataCenter、全部模块实例与链路/IO 执行器；仅 app_main 创建，非 ISR。 */
struct AppContext {
    static constexpr size_t kLinkStackBytes = 6144;
#if RTK_GNSS_ENABLED
    static constexpr size_t kIoStackBytes = 4096;
#endif
    static constexpr size_t kModuleCount = 0
// X-macro 生成计数表达式，函数无法展开注册清单；前导 + 不可单独加括号。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)
#define MODULE_DEF(type, member, name, kind, executor, period) +1
#include "module_list.inc"
#undef MODULE_DEF
        ;

    data::DataCenter data{};
// X-macro 生成异构成员声明，constexpr 函数不能替代。
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define MODULE_DEF(type, member, name, kind, executor, period) type member{*this};
#include "module_list.inc"
#undef MODULE_DEF
    rtos::Executor link_executor{};
    rtos::ExecutorStorage<kLinkStackBytes, kModuleCount> link_storage{};
#if RTK_GNSS_ENABLED
    rtos::Executor io_executor{};
    rtos::ExecutorStorage<kIoStackBytes, kModuleCount> io_storage{};
    RtcmPipe rtcm_pipe{};
#endif
};

/** 返回固件全生命周期的上下文；无阻塞、无分配，非 ISR；成员访问遵守各模块契约。 */
[[nodiscard]] AppContext &app_context() noexcept;
} // namespace app
