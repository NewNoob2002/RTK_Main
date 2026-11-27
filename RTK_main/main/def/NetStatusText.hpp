// 蜂窝状态短文本与控制台、OLED 共用的 ICCID 掩码。
// 纯函数，可跨任务及 ISR 调用，不分配堆。
#pragma once

#include <string_view>

#include "cellular/Types.hpp"

namespace app::net_status {

/**
 * @brief 阶段名
 * @param phase: 链路阶段
 * @retval 静态字符串
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr const char *phaseName(cellular::Phase phase) noexcept
{
    switch (phase) {
    case cellular::Phase::kOff:
        return "off";
    case cellular::Phase::kPowerCycle:
        return "power_cycle";
    case cellular::Phase::kPowerOn:
        return "power_on";
    case cellular::Phase::kSync:
        return "sync";
    case cellular::Phase::kSimCheck:
        return "sim_check";
    case cellular::Phase::kRegister:
        return "register";
    case cellular::Phase::kPdpUp:
        return "pdp_up";
    case cellular::Phase::kReady:
        return "ready";
    case cellular::Phase::kPowerFault:
        return "power-fault"; // 裁决 r5：与报告统一拼写
    default:
        return "?";
    }
}

/**
 * @brief SIM 状态名
 * @param sim: SIM 状态
 * @retval 静态字符串
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr const char *simName(cellular::SimStatus sim) noexcept
{
    switch (sim) {
    case cellular::SimStatus::kUnknown:
        return "unknown";
    case cellular::SimStatus::kReady:
        return "ready";
    case cellular::SimStatus::kAbsent:
        return "absent";
    case cellular::SimStatus::kPinRequired:
        return "pin_required";
    case cellular::SimStatus::kError:
        return "error";
    default:
        return "?";
    }
}

/**
 * @brief 注册状态名
 * @param reg: 注册状态
 * @retval 静态字符串
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr const char *regName(cellular::RegStatus reg) noexcept
{
    switch (reg) {
    case cellular::RegStatus::kUnknown:
        return "unknown";
    case cellular::RegStatus::kNotRegistered:
        return "not_registered";
    case cellular::RegStatus::kSearching:
        return "searching";
    case cellular::RegStatus::kDenied:
        return "denied";
    case cellular::RegStatus::kHome:
        return "home";
    case cellular::RegStatus::kRoaming:
        return "roaming";
    default:
        return "?";
    }
}

/**
 * @brief 把 ICCID 中段打码（保留头 6 位与尾 4 位），避免日志泄露完整卡号
 * @param iccid: 以 0 结尾的 ICCID 文本
 * @param[out] out: 打码结果缓冲（以 0 结尾）
 * @note 纯函数，线程/ISR 安全。
 */
constexpr void maskIccid(const std::array<char, cellular::kIccidBytes> &iccid,
                         std::array<char, cellular::kIccidBytes> &out) noexcept
{
    constexpr size_t kIccidShowHead = 6U;
    constexpr size_t kIccidShowTail = 4U;
    // 最多保留 size - 1 个输入字符，始终为输出留终止符。
    const std::string_view text{iccid.data(), iccid.size() - 1U};
    const size_t len = text.substr(0U, text.find('\0')).size();
    out.fill('\0');
    for (size_t i = 0U; i < len; ++i) {
        const bool keep = (i < kIccidShowHead) || (i + kIccidShowTail >= len);
        out[i] = keep ? iccid[i] : '*';
    }
}

} // namespace app::net_status
