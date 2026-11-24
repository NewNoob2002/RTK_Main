// 输出计划黄金等价比对：基础脚本 + buildOutput 逐句下发，合起来必须与方言基线脚本的命令集合一致。
// 基线（ADR-0012 §2.4 之前的 Dialect）把语句输出写死在 rover 脚本里；当前拆成"基础脚本 3 步 + 每句一条命令"，
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

/** 控制台只允许这几档频率（main 的 isValidRateHz）；其余频率不在本轮比对范围。 */
inline bool rateSupported(uint8_t rateHz) noexcept
{
    return (rateHz == kRate1Hz) || (rateHz == kRate2Hz) || (rateHz == kRate5Hz) ||
           (rateHz == kRate10Hz) || (rateHz == kRate20Hz);
}

/** 由黄金用例还原输出计划：周期取 1000/rate，GSV 跟 gsv 开关，SATSINFO/HEADING 取 1 s。 */
inline gnss::OutputPlan planFrom(const golden::RoverCase &item) noexcept
{
    gnss::OutputPlan plan{};
    const uint32_t period = kOneSecondMs / static_cast<uint32_t>(item.rateHz);
    const auto idx = [](gnss::OutputMsg msg) {
        return static_cast<size_t>(static_cast<uint8_t>(msg));
    };
    plan.periodMs[idx(gnss::OutputMsg::kGga)] = period;
    plan.periodMs[idx(gnss::OutputMsg::kSatsInfo)] = kOneSecondMs;
    plan.periodMs[idx(gnss::OutputMsg::kHeading)] = kOneSecondMs;
    plan.periodMs[idx(gnss::OutputMsg::kGst)] = period;
    plan.periodMs[idx(gnss::OutputMsg::kRmc)] = period;
    plan.periodMs[idx(gnss::OutputMsg::kGsa)] = period;
    plan.periodMs[idx(gnss::OutputMsg::kGsv)] = item.gsv ? kOneSecondMs : 0U;
    plan.port = item.port;
    plan.rateHz = item.rateHz;
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
/**
 * @brief 收集一个用例的完整命令集合：基础脚本 + 每个支持的语句一条下发命令。
 * @param receiver: 被测的家族驱动（内部按用例型号经 forModel 换到具体驱动）。
 * @param item: 黄金用例（型号/频率/端口/波特率）。
 * @param out: 输出，命令集合。
 * @param skipDisabled: true = 跳过 period 0 的语句（NMEA 驱动的开机首轮行为）。
 * @retval true: 收集完成。
 * @retval false: 命令生成失败或缓冲已满。
 */
inline bool collectPlan(const gnss::Receiver &receiver, const golden::RoverCase &item,
                        CommandSet &out, bool skipDisabled = true) noexcept
{
    gnss::RoverConfig cfg{};
    cfg.rateHz = item.rateHz;
    cfg.gsv = item.gsv;
    cfg.port = item.port;
    cfg.portBaud = item.portBaud;
    const auto model = static_cast<gnss::Model>(item.model);
    // UM982 的 HEADINGA 只能由型号驱动下发：先换到该型号的驱动（家族驱动自身保持不变）。
    const gnss::Receiver &driver = receiver.forModel(model);

    size_t index = 0U;
    for (;; ++index) {
        gnss::ConfigStep step{};
        if (!driver.roverStep(index, model, cfg, step)) {
            break;
        }
        if (!out.add(step.cmd)) {
            return false;
        }
    }

    const gnss::OutputPlan plan = planFrom(item);
    for (size_t i = 0U; i < plan.periodMs.size(); ++i) {
        if (skipDisabled && (plan.periodMs[i] == 0U)) {
            // NMEA 驱动：开机首轮 Controller 跳过关闭项（基线脚本里也没有这条命令）。
            // u-blox 相反：基线用一条 VALSET 把 GA 置 0，所以关闭项也要比。
            continue;
        }
        const auto msg = static_cast<gnss::OutputMsg>(static_cast<uint8_t>(i));
        gnss::CmdBuf cmd;
        const fw::Status st = driver.buildOutput(msg, plan.periodMs[i], model, plan, cmd);
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
inline bool sameSpan(std::span<const uint8_t> lhs, std::span<const uint8_t> rhs) noexcept
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
inline bool sameMultiset(const CommandSet &lhs, std::span<const golden::Step> steps,
                         const golden::RoverCase &item) noexcept
{
    const size_t count = static_cast<size_t>(item.stepCount);
    if (lhs.count != count) {
        return false;
    }
    std::array<bool, CommandSet::kMaxCommands> used{};
    for (size_t i = 0U; i < count; ++i) {
        const std::span<const uint8_t> want =
            steps[static_cast<size_t>(item.stepFirst) + i].bytes.view();
        bool matched = false;
        for (size_t j = 0U; (j < lhs.count) && !matched; ++j) {
            if (used[j]) {
                continue;
            }
            if (sameSpan(lhs.views[j], want)) {
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
inline size_t planMismatch(const gnss::Receiver &receiver, std::span<const golden::RoverCase> cases,
                           std::span<const golden::Step> steps) noexcept
{
    for (size_t c = 0U; c < cases.size(); ++c) {
        const golden::RoverCase &item = cases[c];
        if (!rateSupported(item.rateHz)) {
            continue; // 面板不支持的频率（黄金向量里的 3 Hz）不参与等价比对
        }
        CommandSet commands{};
        if (!collectPlan(receiver, item, commands)) {
            return c;
        }
        if (!sameMultiset(commands, steps, item)) {
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
inline constexpr uint8_t kUbxPayloadOffset = 6U;      // 同步 2 + 类 + ID + 长度 2
inline constexpr size_t kUbxIdOffset = 3U;            // 帧内 ID 字节位置
inline constexpr uint8_t kLegacyValsetHeadBytes = 3U; // 历史黄金向量的 3 字节头
inline constexpr uint8_t kValsetHeadBytes = 4U;       // HPG 1.32 4 字节头
inline constexpr size_t kCrcBytes = 2U;
inline constexpr size_t kKeyBytes = 4U;
inline constexpr uint8_t kSizeShift = 28U;
inline constexpr uint8_t kSizeMask = 0x07U;
inline constexpr uint32_t kLegacyKeyGsaUart1 = 0x209100BFU;
inline constexpr uint32_t kLegacyKeyGsvUart1 = 0x209100C3U;
inline constexpr uint32_t kKeyGsaUart1 = 0x209100C0U;
inline constexpr uint32_t kKeyGsvUart1 = 0x209100C5U;
inline constexpr std::array<uint32_t, 7U> kSilencedExtraNmeaUart1Keys{
    0x209100CAU, // GLL
    0x209100B1U, // VTG
    0x209100D9U, // ZDA
    0x209100B6U, // GNS
    0x209100A7U, // DTM
    0x209100CFU, // GRS
    0x209100DEU, // GBS
};
inline constexpr uint8_t kUbxWidthOneBit = 1U;
inline constexpr uint8_t kUbxWidthOneByte = 2U;
inline constexpr uint8_t kUbxWidthTwoBytes = 3U;
inline constexpr uint8_t kUbxWidthFourBytes = 4U;

/** u-blox key 的 bit[31:28] 是值宽度编码：1→1 bit（按 1 字节）、2→1 字节、3→2 字节、4→4 字节。 */
inline size_t valueWidth(uint8_t code) noexcept
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

/**
 * @brief 从一帧 CFG-VALSET 取出全部 (key, value) 对。
 * @param frame: 完整 UBX 帧。
 * @param[out] out: 输出键值对数组。
 * @param headBytes: VALSET 载荷头长度（新驱动为 4，历史黄金向量为 3）。
 * @retval 提取到的键值对数；0 表示非法 VALSET 或越界。
 */
inline size_t valsetPairs(std::span<const uint8_t> frame,
                          std::array<std::pair<uint32_t, uint32_t>, 16> &out,
                          size_t headBytes = kValsetHeadBytes) noexcept
{
    if ((frame.size() < (static_cast<size_t>(kUbxPayloadOffset) + headBytes + kCrcBytes)) ||
        (frame[0] != kUbxSync1) || (frame[1] != kUbxSync2) || (frame[2] != kUbxClsCfg) ||
        (frame[kUbxIdOffset] != kUbxIdValset)) {
        return 0U;
    }
    if ((headBytes == kValsetHeadBytes) &&
        ((frame[kUbxPayloadOffset] != 0U) || (frame[kUbxPayloadOffset + 2U] != 0U) ||
         (frame[kUbxPayloadOffset + 3U] != 0U))) {
        return 0U;
    }
    const size_t body = frame.size() - static_cast<size_t>(kUbxPayloadOffset) - kCrcBytes;
    const size_t end = static_cast<size_t>(kUbxPayloadOffset) + body;
    size_t pos = static_cast<size_t>(kUbxPayloadOffset) + headBytes;
    size_t count = 0U;
    while ((pos + kKeyBytes) <= end) {
        uint32_t key = 0U;
        for (size_t i = 0U; i < kKeyBytes; ++i) {
            key |= static_cast<uint32_t>(frame[pos + i]) << (8U * i);
        }
        const size_t width = valueWidth(static_cast<uint8_t>((key >> kSizeShift) & kSizeMask));
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
inline size_t dropKey(std::span<std::pair<uint32_t, uint32_t>> pairs, uint32_t key) noexcept
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

inline bool pairSetEqual(std::span<const std::pair<uint32_t, uint32_t>> lhs,
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

/**
 * @brief 检查集合中是否存在 (key, expectedVal) 项。
 * @param pairs: 键值对切片。
 * @param key: 键 ID。
 * @param expectedVal: 期望值。
 * @retval true 命中；false 不存在或值不同。
 */
inline bool hasPair(std::span<const std::pair<uint32_t, uint32_t>> pairs, uint32_t key,
                    uint32_t expectedVal) noexcept
{
    for (const auto &item : pairs) {
        if ((item.first == key) && (item.second == expectedVal)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 将集合中指定旧键重映射到新键（用于兼容历史黄金向量中的错误键 ID）。
 * @param pairs: 键值对切片。
 * @param oldKey: 旧键 ID。
 * @param newKey: 新键 ID。
 */
inline void remapKey(std::span<std::pair<uint32_t, uint32_t>> pairs, uint32_t oldKey,
                     uint32_t newKey) noexcept
{
    for (auto &item : pairs) {
        if (item.first == oldKey) {
            item.first = newKey;
        }
    }
}

/**
 * @brief 把若干 VALSET 帧展开成 (key, value) 集合（同一 key 后写的生效）。
 * @param frames: VALSET 帧列表。
 * @param[out] pairs: 合并后的键值对数组。
 * @param[out] count: 合并后的有效条数。
 * @param headBytes: VALSET 载荷头字节数（4 或 3）。
 * @retval true 全部帧合法并已合并；false 存在非法帧或溢出。
 */
inline bool mergeValsets(std::span<const std::span<const uint8_t>> frames,
                         std::array<std::pair<uint32_t, uint32_t>, 32> &pairs, size_t &count,
                         size_t headBytes = kValsetHeadBytes) noexcept
{
    count = 0U;
    for (const auto frame : frames) {
        std::array<std::pair<uint32_t, uint32_t>, 16> local{};
        const size_t n = valsetPairs(frame, local, headBytes);
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

/**
 * @brief u-blox：把新驱动（4 字节头+静默 12 句+修正 GSA/GSV）与历史黄金向量按语义集合比对。
 * @param receiver: 被测 u-blox 驱动。
 * @param cases: 黄金用例表。
 * @param steps: 黄金步骤字节表。
 * @retval kAllMatch 全部匹配；否则返回首个不匹配的用例下标。
 */
inline size_t ubxPlanMismatch(const gnss::Receiver &receiver,
                              std::span<const golden::RoverCase> cases,
                              std::span<const golden::Step> steps) noexcept
{
    for (size_t c = 0U; c < cases.size(); ++c) {
        const golden::RoverCase &item = cases[c];
        CommandSet commands{};
        if (!collectPlan(receiver, item, commands, false) ||
            (commands.count > commands.kMaxCommands)) {
            return c;
        }
        std::array<std::pair<uint32_t, uint32_t>, 32> got{};
        size_t gotCount = 0U;
        if (!mergeValsets(
                std::span<const std::span<const uint8_t>>{commands.views.data(), commands.count},
                got, gotCount, kValsetHeadBytes)) {
            return c;
        }
        std::array<std::pair<uint32_t, uint32_t>, 32> want{};
        size_t wantCount = 0U;
        std::array<std::span<const uint8_t>, CommandSet::kMaxCommands> wantViews{};
        const size_t count = static_cast<size_t>(item.stepCount);
        for (size_t i = 0U; i < count; ++i) {
            wantViews[i] = steps[static_cast<size_t>(item.stepFirst) + i].bytes.view();
        }
        if (!mergeValsets(std::span<const std::span<const uint8_t>>{wantViews.data(), count}, want,
                          wantCount, kLegacyValsetHeadBytes)) {
            return c;
        }
        // U1 显式静默 7 条计划外标准 NMEA 语句（历史黄金向量未发这 7 键）：断言新驱动全写 0 后剔除。
        for (const uint32_t extraKey : kSilencedExtraNmeaUart1Keys) {
            if (!hasPair(std::span<const std::pair<uint32_t, uint32_t>>{got.data(), gotCount},
                         extraKey, 0U)) {
                return c;
            }
            gotCount =
                dropKey(std::span<std::pair<uint32_t, uint32_t>>{got.data(), gotCount}, extraKey);
        }
        // 历史黄金向量把 GSA_UART1 误写成 0x209100BF（GSA_I2C），U1 已修正为 0x209100C0。
        remapKey(std::span<std::pair<uint32_t, uint32_t>>{want.data(), wantCount},
                 kLegacyKeyGsaUart1, kKeyGsaUart1);
        // GSV 的历元数由输出计划周期换算（周期 1 s × 频率），基线固定写 1；这是 ADR-0012 §2.4 期望的行为差异，
        // 历史基线误写 GSV 为 0x209100C3（GSA_SPI），U1 已修正为 0x209100C5。
        gotCount =
            dropKey(std::span<std::pair<uint32_t, uint32_t>>{got.data(), gotCount}, kKeyGsvUart1);
        wantCount = dropKey(std::span<std::pair<uint32_t, uint32_t>>{want.data(), wantCount},
                            kLegacyKeyGsvUart1);
        if (!pairSetEqual(std::span<const std::pair<uint32_t, uint32_t>>{got.data(), gotCount},
                          std::span<const std::pair<uint32_t, uint32_t>>{want.data(), wantCount})) {
            return c;
        }
    }
    return golden_check::kAllMatch;
}

} // namespace plan_check
