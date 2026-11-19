// 控制台公共辅助（ADR-0007 §4.3）：命令投递到执行器与结果回报、MAC/角色格式化。
// 线程模型：仅在 console REPL 任务调用，非 ISR。
#pragma once

#include <cstddef>
#include <span>

#include "data/AppData.hpp"

namespace app {

inline constexpr size_t kMacStrBuf = 18U;
inline constexpr const char *kUnavailableInWifi = "unavailable in wifi mode";

/**
 * @brief 检查控制台操作在当前射频模式下是否可用
 * @retval true 当前为蓝牙模式
 * @retval false 当前为 Wi-Fi 配置模式，已输出 ERR
 * @note 仅 console REPL 任务，非 ISR，输出可阻塞。
 */
[[nodiscard]] bool requireBluetooth() noexcept;

/**
 * @brief 角色的可读名（NONE/PHONE/ALARM/PENDING）。
 * @param role: 会话角色
 * @retval 角色静态短名；未知角色返回 ?
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] const char *roleName(core::Role role) noexcept;
/**
 * @brief 打印 6 字节地址（大写 hex，冒号分隔）。
 * @param addr: 对端蓝牙地址
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
void printMac(const core::BtAddr &addr) noexcept;
/**
 * @brief 格式化 6 字节地址到固定缓冲（"AA:BB:CC:DD:EE:FF"）。纯函数，线程/ISR 安全。
 * @param addr: 对端蓝牙地址
 * @param[out] out: 恰好 kMacStrBuf 字节的输出，含终止符
 * @note 纯计算，线程/ISR 安全，不阻塞。
 */
void formatMac(const core::BtAddr &addr, std::span<char, kMacStrBuf> out) noexcept;
/**
 * @brief 把命令结果转为 cli::fail 报错（kOk 静默返回 true，由 trampoline 打印 OK）。
 * @param what: 保留的调用描述；实现未使用
 * @param[out] result: 命令执行结果
 * @retval true 状态为 kOk，不输出
 * @retval false 已打印失败原因
 * @note 仅 console REPL 任务，输出可阻塞，非 ISR。
 */
[[nodiscard]] bool report(const char *what, const fw::CommandResult &result) noexcept;
/**
 * @brief 把命令投递给指定执行器并等待原始结果（调用方自行处理 out.status）；仅 console REPL 任务，非 ISR。
 * @param executor: 目标执行器
 * @param module: 必须就绪的模块
 * @param downMsg: 执行器不可用的提示，可为空
 * @param command: 待投递指令
 * @param[out] out: 原始命令结果；仅 true 时可用
 * @retval true 已收到执行器结果，命令本身状态见 out
 * @retval false 执行器/模块不可用或 call 失败，out 不保证有效
 * @note 仅 console REPL 任务，最多等待 kCommandTimeoutMs；超时不撤销命令，非 ISR。
 */
[[nodiscard]] bool submitRawTo(rtos::Executor &executor, ModuleKind module, const char *downMsg,
                               const fw::Command &command, fw::CommandResult &out) noexcept;
/**
 * @brief 把命令投递给链路执行器并等待原始结果（调用方自行处理 out.status）；仅 console REPL 任务，非 ISR。
 * @param command: 待投递指令
 * @param[out] out: 原始命令结果，仅 true 时可用
 * @retval true 已收到链路执行器结果，状态见 out
 * @retval false 目标或执行器不可用，或调用失败，out 不保证有效
 * @note 仅 console REPL 任务，可等待 kCommandTimeoutMs，非 ISR。
 */
[[nodiscard]] bool submitRaw(const fw::Command &command, fw::CommandResult &out) noexcept;
/**
 * @brief 把命令投递给链路执行器并等待结果；仅 console REPL 任务调用，非 ISR。
 * @param command: 待投递指令
 * @param what: 保留的调用描述，report 当前忽略此值
 * @retval true 提交成功且命令状态为 kOk
 * @retval false 提交或执行失败，已打印原因
 * @note 仅 console REPL 任务，可等待 kCommandTimeoutMs，非 ISR。
 */
[[nodiscard]] bool submit(const fw::Command &command, const char *what) noexcept;

} // namespace app
