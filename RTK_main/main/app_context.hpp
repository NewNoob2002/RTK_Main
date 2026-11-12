// 应用上下文拥有全部顶层可变对象，模块成员由注册清单生成。
// 仅 app_main 创建；各模块与执行器的运行期访问遵守各自线程契约，非 ISR。
#pragma once

#include "app_config.hpp"
#include "data/app_data.hpp"
#include "module_list.inc"

namespace rtk::app {
/** 固件唯一上下文：拥有 DataCenter、全部模块实例与链路执行器；仅 app_main 创建，非 ISR。 */
struct AppContext {
    static constexpr size_t kLinkStackBytes = 6144;
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
    os::Executor link_executor{};
    os::ExecutorStorage<kLinkStackBytes, kModuleCount> link_storage{};
};

/** 返回固件全生命周期的上下文；无阻塞、无分配，非 ISR；成员访问遵守各模块契约。 */
[[nodiscard]] AppContext &app_context() noexcept;
} // namespace rtk::app
