// 设置键编译期注册表与值校验/命令编码接口（ADR-0018 §2.3）。
// 纯 C++20，无 IDF/FreeRTOS 头；所有函数无可变状态、不阻塞、不分配堆，线程/ISR 安全。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "fw/Command.hpp"
#include "settings/Types.hpp"

namespace settings {

/** 键命令编码函数类型：把规范值编码进 cmd（填写 verb/argLen/args；target 由后端按 owner 填充）。 */
using EncodeFn = bool (*)(const CanonicalValue &val, fw::Command &cmd) noexcept;

/** 键取值域查询函数类型：结合当前 GNSS 能力快照与开关产出 Domain。 */
using DomainFn = void (*)(const GnssCaps &caps, bool gnssEnabled, Domain &out) noexcept;

/** 单个可配置键的编译期元数据描述（ADR-0018 §2.3）。 */
struct KeySpec final {
    KeyId id = KeyId::kGnssRate;
    std::string_view name{};
    KeyKind type = KeyKind::kEnum;
    Owner owner = Owner::kNone;
    uint8_t verb = 0U;
    EncodeFn encode = nullptr;
    DomainFn domain = nullptr;
    PersistKind persist = PersistKind::kNone;
    uint8_t access = 0U;
    bool readable = true; // false 表示只写键（bt.wl.add / bt.wl.del）
};

/**
 * @brief  返回编译期常量键注册表（全部 16 个已登记设置键）
 * @retval 指向静态 constexpr 数组的只读视图
 * @note   纯函数，线程/ISR 安全，不分配堆。
 */
[[nodiscard]] std::span<const KeySpec> keyTable() noexcept;

/**
 * @brief  按键名查找设置键定义（区分大小写）
 * @param  name: 点分键名（如 "gnss.rate"）
 * @retval 找到时返回表项指针；未知键返回 nullptr
 * @note   纯函数，线程/ISR 安全，不分配堆。
 */
[[nodiscard]] const KeySpec *findKey(std::string_view name) noexcept;

/**
 * @brief  按 KeyId 取设置键定义
 * @param  id: 键枚举编号
 * @retval 对应表项指针；越界返回 nullptr
 * @note   纯函数，线程/ISR 安全，不分配堆。
 */
[[nodiscard]] const KeySpec *keyAt(KeyId id) noexcept;

/**
 * @brief  按指定键类型与当前取值域校验文本值并产出规范值
 * @param  spec: 设置键定义
 * @param  dom:  该键当前的取值域（须已由 domain 查询得到）
 * @param  text: 文本输入（如 "10"、"5000"、"AA:BB:CC:DD:EE:FF"）
 * @param[out] out: 成功时写入规范值；任何失败均保持 out 原内容不变
 * @retval Error::kOk          校验通过
 * @retval Error::kUnsupported dom.supported 为 false
 * @retval Error::kBadValue    格式错误或取值不在 dom 允许范围内
 * @note   纯函数，线程/ISR 安全，不阻塞、不分配堆。
 */
[[nodiscard]] Error validateValue(const KeySpec &spec, const Domain &dom, std::string_view text,
                                  CanonicalValue &out) noexcept;

/**
 * @brief  把周期毫秒（0/50/100/200/500/1000/2000/5000/...）转换为协议有符号 int8_t 间隔
 * @param  periodMs: 周期毫秒（0=关；1000=-1；500/200/100/50=2/5/10/20；整秒 >1000 为 -sec）
 * @param[out] interval: 成功时写入间隔编码；失败不修改
 * @retval true  可无损编码
 * @retval false 无法表示为有效 int8_t 间隔
 * @note   纯函数，线程/ISR 安全。显式 1000 ms 使用 -1 避免触发 followsFixRate。
 */
[[nodiscard]] bool periodMsToInterval(int32_t periodMs, int8_t &interval) noexcept;

} // namespace settings
