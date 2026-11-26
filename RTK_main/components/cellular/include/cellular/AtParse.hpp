// AT 响应行解析辅助（ADR-0021 §2.4）：整数/字段切分、CSQ、注册状态、SIM 状态、QIURC 参数。
// 纯函数，无状态，不分配堆，线程/ISR 安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cellular/Types.hpp"

namespace cellular {

/**
 * @brief  不抛异常的子串视图（pos 越界时钳制为空视图）
 * @param  text: 源视图
 * @param  pos:  起始偏移
 * @param  len:  最大长度（缺省取到末尾）
 * @retval 子视图；固件禁用 C++ 异常，std::string_view::substr 越界会拉入 __throw_out_of_range_fmt
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr std::string_view subview(std::string_view text, size_t pos,
                                                 size_t len = std::string_view::npos) noexcept
{
    if (pos >= text.size()) {
        return {};
    }
    text.remove_prefix(pos);
    if (len < text.size()) {
        text.remove_suffix(text.size() - len);
    }
    return text;
}

/**
 * @brief  解析文本开头的十进制整数（允许前导空格与负号，遇非数字停止）
 * @param  text: 输入文本
 * @param[out] out: 成功时写入数值；失败不修改
 * @retval true  解析到至少一位数字且未溢出 int32
 * @retval false 无数字或溢出
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool parseInt(std::string_view text, int32_t &out) noexcept;

/**
 * @brief  取逗号分隔的第 index 个字段（去前后空格，不处理引号内逗号）
 * @param  text:  冒号之后的参数文本
 * @param  index: 字段下标（从 0 起）
 * @retval 字段视图；下标越界返回空视图
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] std::string_view fieldAt(std::string_view text, size_t index) noexcept;

/**
 * @brief  取行内第一个冒号之后的参数文本
 * @param  line: 整行，如 "+CSQ: 20,0"
 * @retval 冒号后文本（含前导空格）；无冒号返回空视图
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] std::string_view argsOf(std::string_view line) noexcept;

/**
 * @brief  解析 "+CSQ: <rssi>,<ber>" 行
 * @param  line: 响应行
 * @param[out] csq: 成功时写入 rssi（0–31，99 表示未知）
 * @retval true  解析成功
 * @retval false 前缀或数值不合法
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool parseCsq(std::string_view line, uint8_t &csq) noexcept;

/**
 * @brief  解析 +CEREG/+CREG/+CGREG 行的注册状态
 * @param  line:    响应行（查询应答 "+CEREG: <n>,<stat>[,...]" 或 URC "+CEREG: <stat>[,...]"）
 * @param[out] reg: 成功时写入注册状态
 * @retval true  解析成功
 * @retval false 前缀不属于注册类或数值非法
 * @note   通过第二字段是否为纯数字区分查询应答与 URC；纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool parseReg(std::string_view line, RegStatus &reg) noexcept;

/**
 * @brief  判断注册状态是否表示已注册（本地或漫游）
 * @param  reg: 注册状态
 * @retval true  kHome 或 kRoaming
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr bool isRegistered(RegStatus reg) noexcept
{
    return (reg == RegStatus::kHome) || (reg == RegStatus::kRoaming);
}

/**
 * @brief  解析 "+CPIN: <code>" 行的 SIM 状态
 * @param  line: 响应行
 * @param[out] sim: 成功时写入 SIM 状态
 * @retval true  解析成功
 * @retval false 前缀不是 +CPIN
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool parseCpin(std::string_view line, SimStatus &sim) noexcept;

/**
 * @brief  把 +CME ERROR 码映射为 SIM 状态（10=未插卡，11/12/…=需 PIN/PUK，其余=异常）
 * @param  code: +CME ERROR 数值码
 * @retval 对应 SIM 状态
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] SimStatus simFromCmeCode(int32_t code) noexcept;

/**
 * @brief  解析 "+QIURC: \"<kind>\",<args>" 行
 * @param  line: URC 整行
 * @param[out] kind: 引号内类型（如 recv/closed/pdpdeact），视图指向 line
 * @param[out] id:   第一个整数参数（recv/closed 为 connectID，pdpdeact 为 contextID）；缺失时为 -1
 * @retval true  是合法的 QIURC 行
 * @retval false 不是 QIURC 或类型缺失
 * @note   纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool parseQiurc(std::string_view line, std::string_view &kind, int32_t &id) noexcept;

} // namespace cellular
