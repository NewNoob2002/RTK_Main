// Unicore 流动站配置脚本：命令生成（cmd_*）与脚本步骤（step_*，ADR-0010 §2.4）。
// 结构（P3s）：两者都是 UnicoreReceiver 的 protected static，型号子类（Um982Receiver）直接复用；
// 本文件只负责“写出一条完整命令/一步脚本”，不关心何时发送、如何等应答。
// 线程/调用约束：纯函数（无全局可变状态），可从任意任务调用，不可阻塞。
// 堆：不使用。浮点：不使用（频率参数按手册写死为文本）。
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "gnss/drivers/UnicoreReceiver.hpp"

#include "../AsciiCmd.hpp"
#include "UnicoreKeys.hpp"

namespace gnss {
namespace {

using unicore::kConfigTimeoutMs;

} // namespace

// ---- 命令库：每个函数只把一条完整命令（含 "\r\n"）写进 out，不关心什么时候发 ----

/**
 * @brief 关闭该口全部报文输出："UNLOG COMn"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号（1..4）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdUnlog(CmdBuf &out, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::appendUint(out, port) && out.append("\r\n");
}

/**
 * @brief 切到流动站模式："MODE ROVER"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdModeRover(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("MODE ROVER\r\n");
}

/**
 * @brief 改端口波特率："CONFIG COMn <baud>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param port: 接收机端口号。
 * @param baud: 目标波特率（如 921600）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdConfigBaud(CmdBuf &out, uint8_t port, uint32_t baud) noexcept
{
    out.clear();
    return out.append("CONFIG COM") && ascii::appendUint(out, port) && out.append(" ") &&
           ascii::appendUint(out, baud) && out.append("\r\n");
}

/**
 * @brief 以给定周期开启一类报文："<name> COMn <period>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGGA"）。
 * @param port: 接收机端口号。
 * @param periodMs: 周期（毫秒），必须是 10 ms 整数倍。
 * @retval true: 已写入完整命令。
 * @retval false: 周期粒度不合法或缓冲空间不足。
 */
bool UnicoreReceiver::cmdLogMsg(CmdBuf &out, std::string_view name, uint8_t port,
                                uint32_t periodMs) noexcept
{
    out.clear();
    return out.append(name) && out.append(" COM") && ascii::appendUint(out, port) &&
           out.append(" ") && ascii::appendPeriod(out, periodMs) && out.append("\r\n");
}

/**
 * @brief 关闭一类报文："UNLOG COMn <name>"。
 * @param out: 目标命令缓冲（先清空）。
 * @param name: 报文体名字（如 "GPGSV"）。
 * @param port: 接收机端口号。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdUnlogMsg(CmdBuf &out, std::string_view name, uint8_t port) noexcept
{
    out.clear();
    return out.append("UNLOG COM") && ascii::appendUint(out, port) && out.append(" ") &&
           out.append(name) && out.append("\r\n");
}

/**
 * @brief 探测用的版本查询："VERSIONA"。
 * @param out: 目标命令缓冲（先清空）。
 * @retval true: 已写入完整命令。
 * @retval false: 缓冲空间不足。
 */
bool UnicoreReceiver::cmdVersionQuery(CmdBuf &out) noexcept
{
    out.clear();
    return out.append("VERSIONA\r\n");
}

// ---- 基础脚本的步骤与启用条件 ----

/**
 * @brief 步骤启用条件：总是。
 * @param cfg: 流动站配置（本判断不使用）。
 * @retval true: 恒为 true。
 */
bool UnicoreReceiver::stepAlways(const RoverConfig &cfg) noexcept
{
    static_cast<void>(cfg);
    return true;
}

/**
 * @brief 步骤启用条件：cfg.portBaud != 0（为 0 时脚本提前结束）。
 * @param cfg: 流动站配置。
 * @retval true: 需要改波特率。
 * @retval false: 不改波特率。
 */
bool UnicoreReceiver::stepIfPortBaud(const RoverConfig &cfg) noexcept
{
    return cfg.portBaud != 0U;
}

/**
 * @brief 第 0 步：先关掉全部输出，再由输出阶段按 OutputPlan 重新开启。
 * @param cfg: 流动站配置（用端口号）。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepUnlogAll(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    if (!cmdUnlog(out.cmd, cfg.port)) {
        return false;
    }
    out.expectReply = true;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = 0;
    return true;
}

/**
 * @brief 第 1 步：切到流动站模式。
 * @param cfg: 流动站配置（本步不使用）。
 * @param out: 输出步骤。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepModeRover(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    static_cast<void>(cfg);
    if (!cmdModeRover(out.cmd)) {
        return false;
    }
    out.expectReply = true;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = 0;
    return true;
}

/**
 * @brief 第 2 步：改端口波特率；手册要求切换后不再按旧速率等待应答。
 * @param cfg: 流动站配置（用端口号与目标波特率）。
 * @param out: 输出步骤（switchBaud = 目标波特率）。
 * @retval true: 步骤可用。
 * @retval false: 命令生成失败。
 */
bool UnicoreReceiver::stepPortBaud(const RoverConfig &cfg, ConfigStep &out) noexcept
{
    if (!cmdConfigBaud(out.cmd, cfg.port, cfg.portBaud)) {
        return false;
    }
    out.expectReply = false;
    out.timeoutMs = kConfigTimeoutMs;
    out.switchBaud = cfg.portBaud;
    return true;
}

} // namespace gnss
