// 组合根内部接口（ADR-0009）：控制台命令门控、启动阶段推进、启动记录查询与消费者启停。
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
 * 放在头文件里是因为模块表/执行器配置需要在常量初始化阶段取成员地址；同名访问器为 appContext()。
 */
inline constinit AppContext s_app{};

/** 控制台命令描述符：所有命令走同一门控 handler，按启动阶段放行；描述符与表在 REPL 启动前固定。 */
struct GatedCommand {
    cli::Handler handler = nullptr;
    void *ctx = nullptr;
    bool safe = false; // console-only 阶段是否放行
};

/**
 * @brief 按启动阶段放行或拒绝控制台命令
 * @param ctx: 指向启动期固定的 GatedCommand
 * @param argc: 命令参数个数（含动词）
 * @param argv: 命令参数数组
 * @retval 0 handler 成功
 * @retval 非零 启动门控拒绝或 handler 失败
 * @note 仅 console REPL 任务；可能随 handler 阻塞，非 ISR。
 * @note 门控 handler：kStarting/kStopping 拒绝全部命令，kConsoleOnly 只放行 safe 组，kRunning 全部放行。
 */
int gatedCommand(void *ctx, int argc, char **argv) noexcept;

/**
 * @brief 把启动阶段切到仅诊断控制台模式
 * @note 仅启动任务调用；原子发布，不阻塞，非 ISR。
 */
void setConsoleOnly() noexcept;

/**
 * @brief 取得已执行的启动步骤记录
 * @retval 启动前为空，启动后为只读记录视图
 * @note 启动完成后由 console 任务读取，非 ISR；记录存储活过启动任务。
 */
[[nodiscard]] std::span<const BootRecord> bootRecords() noexcept;

#if defined(ESP_PLATFORM)
/**
 * @brief 把静态执行器实例绑定到组合根
 * @note 仅启动前单线程调用，先于任何执行器访问；非 ISR，不阻塞。
 */
void bindExecutors() noexcept;

/**
 * @brief  安装组件日志直写 sink（ADR-0009 §2.12），使启动第一步起的 FW_LOG* 带时间戳与类别输出到控制台
 * @retval false 已安装或参数无效（只可能是重复调用）
 * @note   启动任务第一步调用；文本环由 DebugModule::attachLog 在 REPL 放行前接管。
 */
[[nodiscard]] bool installDirectLog() noexcept;

/**
 * @brief 建立控制台输出通道并登记当前档位的命令，REPL 任务创建但不放行。
 * @retval false 失败（由 console 步骤降级）
 * @note   启动任务调用，须早于任何业务任务（会重装 UART0 驱动）。
 */
[[nodiscard]] bool initConsole() noexcept;

/**
 * @brief 放行 REPL，供 DEV 和 PRODUCTION 在启动成功或降级时调用。
 * @note   initConsole 失败时只记录错误。
 */
void runConsole() noexcept;

/**
 * @brief 启动链路执行器并等待首批模块就绪；成功返回 nullptr，失败返回降级原因字符串。
 * @param linkStack: 外供链路任务栈，须活到重启
 * @param launched: 已创建任务位掩码，任务创建成功即追加链路位
 * @retval nullptr 启动成功
 * @retval 非空 链路初始化、任务创建或模块启动失败原因
 * @note 仅启动任务；等待屏障可阻塞至 5 s，非 ISR。
 */
[[nodiscard]] const char *startLink(std::span<StackType_t> linkStack, uint32_t &launched) noexcept;

#if RTK_GNSS_ENABLED
/**
 * @brief 启动 IO 执行器并等待首批模块就绪；成功返回 nullptr，失败返回降级原因字符串。
 * @param ioStack: 外供 IO 任务栈，须活到重启
 * @param launched: 已创建任务位掩码，任务创建成功即追加 IO 位
 * @retval nullptr 启动成功
 * @retval 非空 IO 初始化、任务创建或模块启动失败原因
 * @note 仅启动任务；等待屏障可阻塞至 5 s，非 ISR。
 */
[[nodiscard]] const char *startIo(std::span<StackType_t> ioStack, uint32_t &launched) noexcept;
#endif

/**
 * @brief 启动 4G 联网执行器（kNet）；仅 Feature::kCellular 板型的正常启动调用，成功返回 nullptr。
 * @param netStack: 外供 kNet 任务栈，须活到重启
 * @param netBlockMem: 外供 NetBlock 内存（执行器+控制存储+运行态），须活到重启
 * @param launched: 已创建任务位掩码，任务创建成功即追加 net 位
 * @retval nullptr 启动成功
 * @retval 非空 初始化、关机参与者登记、任务创建或模块启动失败原因
 * @note 仅启动任务；等待屏障可阻塞至 5 s，非 ISR。
 */
[[nodiscard]] const char *startNet(std::span<StackType_t> netStack, std::span<uint8_t> netBlockMem,
                                   uint32_t &launched) noexcept;

/**
 * @brief 等待指定任务到达启动屏障
 * @param bits: 要等待的任务位掩码
 * @retval true 指定任务全部到达
 * @retval false 等待达到 5 s 上限
 * @note 仅启动任务，可阻塞，非 ISR。
 */
[[nodiscard]] bool waitStarted(uint32_t bits) noexcept;

/**
 * @brief 撤销启动：等待已启动任务停稳后停止 GNSS UART 与 SPP，并置 console-only；SEALED 块始终保留。
 * @param launched: 必须确认停稳的已创建任务位掩码
 * @note 仅启动任务；停稳等待至多 5 s，超时保留驱动和存储；非 ISR。
 */
void stopBootTasks(uint32_t launched) noexcept;
#endif // ESP_PLATFORM

} // namespace app
