// 组合根内部接口（R8a）：控制台命令门控、启动阶段推进、启动记录查询与消费者启停。
// 线程：仅启动任务、console REPL 任务或执行器任务调用各自的声明；非 ISR。
#pragma once

#include <cstdint>
#include <span>

#include "cli/Console.hpp"

#include "app/BootSteps.hpp"
#include "AppConfig.hpp"
#include "AppContext.hpp"

namespace app {

/**
 * 组合根单例：inline 变量让各 TU 看到同一对象，constinit 保证静态初始化不执行代码。
 * 放在头文件里是因为模块表/执行器配置需要在常量初始化阶段取成员地址；同名访问器为 app_context()。
 */
inline constinit AppContext s_app{};

/** 控制台命令描述符：所有命令走同一门控 handler，按启动阶段放行；描述符与表在 REPL 启动前固定。 */
struct GatedCommand {
    cli::Handler handler = nullptr;
    void *ctx = nullptr;
    bool safe = false; // console-only 阶段是否放行
};

/** 门控 handler：kStarting/kStopping 拒绝全部命令，kConsoleOnly 只放行 safe 组，kRunning 全部放行。 */
int gated_command(void *ctx, int argc, char **argv) noexcept;

/** 进入 kConsoleOnly 阶段（启动失败后只留诊断命令）。 */
void set_console_only() noexcept;

/** 已执行的启动步骤记录；boot 返回后只读，boot 之前为空。 */
[[nodiscard]] std::span<const BootRecord> boot_records() noexcept;

#if defined(ESP_PLATFORM)
/** 把静态执行器实例绑定到组合根（contexts 步骤调用，先于任何执行器使用）。 */
void bind_executors() noexcept;

/**
 * @brief  安装组件日志直写 sink（ADR-0009 §2.12），使启动第一步起的 FW_LOG* 带时间戳与类别输出到控制台
 * @retval false 已安装或参数无效（只可能是重复调用）
 * @note   启动任务第一步调用；文本环由 DebugModule::attach_log 在 REPL 放行前接管。
 */
[[nodiscard]] bool install_direct_log() noexcept;

/**
 * @brief  建立控制台输出通道并登记全部命令表（来自各模块清单），REPL 任务创建但不放行。
 * @retval false 失败（由 console 步骤降级）
 * @note   启动任务调用，须早于任何业务任务（会重装 UART0 驱动）。
 */
[[nodiscard]] bool init_console() noexcept;

/**
 * @brief  放行 REPL：打印提示符、开始接收命令。启动流程最后调用，成功与降级路径都会走到。
 * @note   init_console 失败时只记录错误。
 */
void run_console() noexcept;

/** 启动链路执行器并等待首批模块就绪；成功返回 nullptr，失败返回降级原因字符串。 */
[[nodiscard]] const char *start_link(std::span<StackType_t> link_stack, uint32_t &launched) noexcept;

#if RTK_GNSS_ENABLED
/** 启动 IO 执行器并等待首批模块就绪；成功返回 nullptr，失败返回降级原因字符串。 */
[[nodiscard]] const char *start_io(std::span<StackType_t> io_stack, uint32_t &launched) noexcept;
#endif

/** 等待已启动任务到达启动屏障；超时记录 task_start_timeout 并返回 false。 */
[[nodiscard]] bool wait_started(uint32_t bits) noexcept;

/** 撤销启动：等待已启动任务停稳后停止 GNSS UART 与 SPP，并置 console-only；SEALED 块始终保留。 */
void stop_boot_tasks(uint32_t launched) noexcept;
#endif // ESP_PLATFORM

} // namespace app
