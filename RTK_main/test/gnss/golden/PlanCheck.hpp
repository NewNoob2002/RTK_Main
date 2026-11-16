// P3b 黄金等价比对：基础脚本 + build_output 逐句下发，合起来必须与 P3a 基线脚本的命令集合一致。
// 基线（ADR-0012 P3a 之前的 Dialect）把语句输出写死在 rover 脚本里；P3b 拆成"基础脚本 3 步 + 每句一条命令"，
// 因此这里按多重集比较：只要基线出现过的那条命令，新实现也必须恰好出现一次，且不多不少。
// 只用普通函数（不依赖 Unity 宏），三个驱动的测试文件共用；返回第一个不一致的下标，kAllMatch 表示全部一致。
// 线程/ISR 安全：纯函数（只读输入，栈上构造命令）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "gnss/Receiver.hpp"
#include "GoldenData.hpp"
#include "GoldenCheck.hpp"

namespace plan_check {

inline constexpr uint32_t kOneSecondMs = 1000U;
inline constexpr uint8_t kRate1Hz = 1U;
inline constexpr uint8_t kRate2Hz = 2U;
inline constexpr uint8_t kRate5Hz = 5U;
inline constexpr uint8_t kRate10Hz = 10U;
inline constexpr uint8_t kRate20Hz = 20U;

/** 控制台只允许这几档频率（main 的 is_valid_rate_hz）；其余频率不在本轮比对范围。 */
inline bool rate_supported(uint8_t rate_hz) noexcept
{
    return (rate_hz == kRate1Hz) || (rate_hz == kRate2Hz) || (rate_hz == kRate5Hz) || (rate_hz == kRate10Hz) ||
           (rate_hz == kRate20Hz);
}

/** 由黄金用例还原输出计划：周期取 1000/rate，GSV 跟 gsv 开关，SATSINFO/HEADING 取 1 s。 */
inline gnss::OutputPlan plan_from(const golden::RoverCase &item) noexcept
{
    gnss::OutputPlan plan{};
    const uint32_t period = kOneSecondMs / static_cast<uint32_t>(item.rate_hz);
    const auto idx = [](gnss::OutputMsg msg) { return static_cast<size_t>(static_cast<uint8_t>(msg)); };
    plan.period_ms[idx(gnss::OutputMsg::kGga)] = period;
    plan.period_ms[idx(gnss::OutputMsg::kSatsInfo)] = kOneSecondMs;
    plan.period_ms[idx(gnss::OutputMsg::kHeading)] = kOneSecondMs;
    plan.period_ms[idx(gnss::OutputMsg::kGst)] = period;
    plan.period_ms[idx(gnss::OutputMsg::kRmc)] = period;
    plan.period_ms[idx(gnss::OutputMsg::kGsa)] = period;
    plan.period_ms[idx(gnss::OutputMsg::kGsv)] = item.gsv ? kOneSecondMs : 0U;
    plan.port = item.port;
    plan.rate_hz = item.rate_hz;
    return plan;
}

/** 收集到的命令：缓冲固定，视图指向缓冲自身（固定数组，不会重新分配）。 */
struct CommandSet {
    static constexpr size_t kMaxCommands = 24U;
    std::array<gnss::CmdBuf, kMaxCommands> bufs{};
    std::array<std::span<const uint8_t>, kMaxCommands> views{};
    size_t count = 0U;

    bool add(const gnss::CmdBuf &cmd) noexcept
    {
        if (count >= kMaxCommands) {
            return false;
        }
        bufs[count] = cmd;
        views[count] = bufs[count].view();
        ++count;
        return true;
    }
};

/** 收集一个用例的完整命令集合：基础脚本 + 每个支持的语句一条下发命令。 */
inline bool collect_plan(const gnss::Receiver &receiver, const golden::RoverCase &item, CommandSet &out,
                         bool skip_disabled = true) noexcept
{
    gnss::RoverConfig cfg{};
    cfg.rate_hz = item.rate_hz;
    cfg.gsv = item.gsv;
    cfg.port = item.port;
    cfg.port_baud = item.port_baud;
    const auto model = static_cast<gnss::Model>(item.model);

    size_t index = 0U;
    for (;; ++index) {
        gnss::ConfigStep step{};
        if (!receiver.rover_step(index, model, cfg, step)) {
            break;
        }
        if (!out.add(step.cmd)) {
            return false;
        }
    }

    const gnss::OutputPlan plan = plan_from(item);
    for (size_t i = 0U; i < plan.period_ms.size(); ++i) {
        if (skip_disabled && (plan.period_ms[i] == 0U)) {
            // NMEA 驱动：开机首轮 Controller 跳过关闭项（基线脚本里也没有这条命令）。
            // u-blox 相反：基线用一条 VALSET 把 GA 置 0，所以关闭项也要比。
            continue;
        }
        const auto msg = static_cast<gnss::OutputMsg>(static_cast<uint8_t>(i));
        gnss::CmdBuf cmd;
        const fw::Status st = receiver.build_output(msg, plan.period_ms[i], model, plan, cmd);
        if (st == fw::Status::kNotSupported) {
            continue;
        }
        if (st != fw::Status::kOk) {
            return false;
        }
        if (!out.add(cmd)) {
            return false;
        }
    }
    return true;
}

/** 字节序列相等（不经过字符串视图，避免在测试里做类型双关）。 */
inline bool same_span(std::span<const uint8_t> lhs, std::span<const uint8_t> rhs) noexcept
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0U; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    return true;
}

/** 多重集相等：每条命令按字节出现次数一致。 */
inline bool same_multiset(const CommandSet &lhs, std::span<const golden::Step> steps,
                          const golden::RoverCase &item) noexcept
{
    const size_t count = static_cast<size_t>(item.step_count);
    if (lhs.count != count) {
        return false;
    }
    std::array<bool, CommandSet::kMaxCommands> used{};
    for (size_t i = 0U; i < count; ++i) {
        const std::span<const uint8_t> want = steps[static_cast<size_t>(item.step_first) + i].bytes.view();
        bool matched = false;
        for (size_t j = 0U; (j < lhs.count) && !matched; ++j) {
            if (used[j]) {
                continue;
            }
            if (same_span(lhs.views[j], want)) {
                used[j] = true;
                matched = true;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

/** NMEA 驱动（Unicore/ComNav）：命令字节直接按文本比较。 */
inline size_t plan_mismatch(const gnss::Receiver &receiver, std::span<const golden::RoverCase> cases,
                            std::span<const golden::Step> steps) noexcept
{
    for (size_t c = 0U; c < cases.size(); ++c) {
        const golden::RoverCase &item = cases[c];
        if (!rate_supported(item.rate_hz)) {
            continue; // 面板不支持的频率（黄金向量里的 3 Hz）不参与等价比对
        }
        CommandSet commands{};
        if (!collect_plan(receiver, item, commands)) {
            return c;
        }
        if (!same_multiset(commands, steps, item)) {
            return c;
        }
    }
    return golden_check::kAllMatch;
}

// ---- u-blox：VALSET 帧里是 (key, value) 列表，按对比较而不是按字节 ----

inline constexpr uint8_t kUbxSync1 = 0xB5U;
inline constexpr uint8_t kUbxSync2 = 0x62U;
inline constexpr uint8_t kUbxClsCfg = 0x06U;
inline constexpr uint8_t kUbxIdValset = 0x8AU;
inline constexpr uint8_t kUbxPayloadOffset = 6U; // 同步 2 + 类 + ID + 长度 2
inline constexpr size_t kUbxIdOffset = 3U;       // 帧内 ID 字节位置
inline constexpr uint8_t kValsetHeadBytes = 3U;  // version + layers + reserved
inline constexpr size_t kCrcBytes = 2U;
inline constexpr size_t kKeyBytes = 4U;
inline constexpr uint8_t kSizeShift = 28U;
inline constexpr uint8_t kSizeMask = 0x07U;
inline constexpr uint32_t kKeyGsvUart1 = 0x209100C3U;
inline constexpr uint8_t kUbxWidthOneBit = 1U;
inline constexpr uint8_t kUbxWidthOneByte = 2U;
inline constexpr uint8_t kUbxWidthTwoBytes = 3U;
inline constexpr uint8_t kUbxWidthFourBytes = 4U;

/** u-blox key 的 bit[31:28] 是值宽度编码：1→1 bit（按 1 字节）、2→1 字节、3→2 字节、4→4 字节。 */
inline size_t value_width(uint8_t code) noexcept
{
    switch (code) {
    case kUbxWidthOneBit:
    case kUbxWidthOneByte:
        return 1U;
    case kUbxWidthTwoBytes:
        return 2U;
    case kUbxWidthFourBytes:
        return 4U;
    default:
        return 1U;
    }
}

/** 从一帧 CFG-VALSET 里取出所有 (key, value) 对；返回对数，0 表示不是合法 VALSET。 */
inline size_t valset_pairs(std::span<const uint8_t> frame, std::array<std::pair<uint32_t, uint32_t>, 8> &out) noexcept
{
    if ((frame.size() < (static_cast<size_t>(kUbxPayloadOffset) + kCrcBytes)) || (frame[0] != kUbxSync1) ||
        (frame[1] != kUbxSync2) || (frame[2] != kUbxClsCfg) || (frame[kUbxIdOffset] != kUbxIdValset)) {
        return 0U;
    }
    const size_t body = frame.size() - static_cast<size_t>(kUbxPayloadOffset) - kCrcBytes;
    const size_t end = static_cast<size_t>(kUbxPayloadOffset) + body;
    size_t pos = static_cast<size_t>(kUbxPayloadOffset) + static_cast<size_t>(kValsetHeadBytes);
    size_t count = 0U;
    while ((pos + kKeyBytes) <= end) {
        uint32_t key = 0U;
        for (size_t i = 0U; i < kKeyBytes; ++i) {
            key |= static_cast<uint32_t>(frame[pos + i]) << (8U * i);
        }
        const size_t width = value_width(static_cast<uint8_t>((key >> kSizeShift) & kSizeMask));
        if (((pos + kKeyBytes + width) > end) || (count >= out.size())) {
            return 0U;
        }
        uint32_t value = 0U;
        for (size_t i = 0U; i < width; ++i) {
            value |= static_cast<uint32_t>(frame[pos + kKeyBytes + i]) << (8U * i);
        }
        out[count] = {key, value};
        ++count;
        pos += kKeyBytes + width;
    }
    return count;
}

/** 从集合里去掉一个键（返回剩余个数），用于声明"这一项行为有意不同"。 */
inline size_t drop_key(std::span<std::pair<uint32_t, uint32_t>> pairs, uint32_t key) noexcept
{
    size_t out = 0U;
    for (size_t i = 0U; i < pairs.size(); ++i) {
        if (pairs[i].first != key) {
            pairs[out] = pairs[i];
            ++out;
        }
    }
    return out;
}

inline bool pair_set_equal(std::span<const std::pair<uint32_t, uint32_t>> lhs,
                           std::span<const std::pair<uint32_t, uint32_t>> rhs) noexcept
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    size_t matched = 0U;
    for (const auto &item : lhs) {
        for (const auto &other : rhs) {
            if ((item.first == other.first) && (item.second == other.second)) {
                ++matched;
                break;
            }
        }
    }
    return matched == lhs.size();
}

/** 把若干 VALSET 帧展开成 (key, value) 集合（同一 key 后写的生效，未出现的 key 等价于 0）。 */
inline bool merge_valsets(std::span<const std::span<const uint8_t>> frames,
                          std::array<std::pair<uint32_t, uint32_t>, 16> &pairs, size_t &count) noexcept
{
    count = 0U;
    for (const auto frame : frames) {
        std::array<std::pair<uint32_t, uint32_t>, 8> local{};
        const size_t n = valset_pairs(frame, local);
        if (n == 0U) {
            return false;
        }
        for (size_t k = 0U; k < n; ++k) {
            bool merged = false;
            for (size_t j = 0U; j < count; ++j) {
                if (pairs[j].first == local[k].first) {
                    pairs[j].second = local[k].second;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                if (count >= pairs.size()) {
                    return false;
                }
                pairs[count] = local[k];
                ++count;
            }
        }
    }
    return true;
}

/** u-blox：把两侧的 VALSET 全部展开成对集合后比较。 */
inline size_t ubx_plan_mismatch(const gnss::Receiver &receiver, std::span<const golden::RoverCase> cases,
                                std::span<const golden::Step> steps) noexcept
{
    for (size_t c = 0U; c < cases.size(); ++c) {
        const golden::RoverCase &item = cases[c];
        CommandSet commands{};
        if (!collect_plan(receiver, item, commands, false) || (commands.count > commands.kMaxCommands)) {
            return c;
        }
        std::array<std::pair<uint32_t, uint32_t>, 16> got{};
        size_t got_count = 0U;
        if (!merge_valsets(std::span<const std::span<const uint8_t>>{commands.views.data(), commands.count}, got,
                           got_count)) {
            return c;
        }
        std::array<std::pair<uint32_t, uint32_t>, 16> want{};
        size_t want_count = 0U;
        std::array<std::span<const uint8_t>, CommandSet::kMaxCommands> want_views{};
        const size_t count = static_cast<size_t>(item.step_count);
        for (size_t i = 0U; i < count; ++i) {
            want_views[i] = steps[static_cast<size_t>(item.step_first) + i].bytes.view();
        }
        if (!merge_valsets(std::span<const std::span<const uint8_t>>{want_views.data(), count}, want, want_count)) {
            return c;
        }
        // GSV 的历元数在 P3b 由周期换算（周期 1 s × 频率），基线固定写 1；这是 ADR-0012 §2.4 期望的行为差异，
        // 其余键仍逐对比较（未出现等价于 0）。
        got_count = drop_key(std::span<std::pair<uint32_t, uint32_t>>{got.data(), got_count}, kKeyGsvUart1);
        want_count = drop_key(std::span<std::pair<uint32_t, uint32_t>>{want.data(), want_count}, kKeyGsvUart1);
        if (!pair_set_equal(std::span<const std::pair<uint32_t, uint32_t>>{got.data(), got_count},
                            std::span<const std::pair<uint32_t, uint32_t>>{want.data(), want_count})) {
            return c;
        }
    }
    return golden_check::kAllMatch;
}

} // namespace plan_check
