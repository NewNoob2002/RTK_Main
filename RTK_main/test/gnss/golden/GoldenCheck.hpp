// 黄金向量比对器（ADR-0012 §2.4）：把 golden::* 数据与驱动行为逐项比较，返回第一个不一致的下标。
// 只用第一层断言无关的比较函数（返回 size_t 下标，SIZE_MAX 表示全部一致），调用方再用 Unity 断言，
// 这样同一份比对逻辑能被三个驱动的测试文件共用而不依赖 Unity 宏展开位置。
// 线程/ISR 安全：纯函数（只读输入）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Rtcm3.hpp"
#include "demux/decoders/Ubx.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "gnss/Receiver.hpp"
#include "GoldenData.hpp"

namespace golden_check {

inline constexpr size_t kAllMatch = static_cast<size_t>(-1);

/** 驱动头导出的私有解码器：只有 Unicore/ComNav 有，故用一张查表集中映射。 */
inline const demux::Decoder *decoderFor(golden::DecoderKind kind) noexcept
{
    switch (kind) {
    case golden::DecoderKind::kNmea:
        return &demux::nmea::kDecoder;
    case golden::DecoderKind::kRtcm3:
        return &demux::rtcm3::kDecoder;
    case golden::DecoderKind::kUnicoreHash:
        return &demux::unicore_hash::kDecoder;
    case golden::DecoderKind::kUbx:
        return &demux::ubx::kDecoder;
    case golden::DecoderKind::kUnicoreResp:
    case golden::DecoderKind::kComnavPrompt:
    default:
        return nullptr; // 由调用方按需替换（见 resp_decoder_for）
    }
}

inline bool sameBytes(std::span<const uint8_t> data, std::string_view expect) noexcept
{
    if (data.size() != expect.size()) {
        return false;
    }
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] != static_cast<uint8_t>(expect[i])) {
            return false;
        }
    }
    return true;
}

/** buildProbe：字节与成功标志都要与基线一致。 */
inline bool probeMatches(const gnss::Receiver &receiver, const golden::Probe &expect) noexcept
{
    gnss::CmdBuf cmd;
    const bool ok = receiver.buildProbe(cmd);
    if (ok != expect.ok) {
        return false;
    }
    return !ok || sameBytes(cmd.view(), expect.bytes.sv());
}

/** 全部 roverStep 组合；返回第一个不一致的 (case, step) 压缩下标，kAllMatch 表示全部一致。 */
inline size_t stepsMismatch(const gnss::Receiver &receiver,
                            std::span<const golden::RoverCase> cases,
                            std::span<const golden::Step> steps) noexcept
{
    for (size_t c = 0; c < cases.size(); ++c) {
        const golden::RoverCase &item = cases[c];
        gnss::RoverConfig cfg{};
        cfg.rateHz = item.rateHz;
        cfg.gsv = item.gsv;
        cfg.port = item.port;
        cfg.portBaud = item.portBaud;
        const auto model = static_cast<gnss::Model>(item.model);
        for (size_t i = 0; i < item.stepCount; ++i) {
            gnss::ConfigStep step{};
            if (!receiver.roverStep(i, model, cfg, step)) {
                return c * 1000U + i;
            }
            const golden::Step &want = steps[static_cast<size_t>(item.stepFirst) + i];
            if ((!sameBytes(step.cmd.view(), want.bytes.sv())) ||
                (step.expectReply != want.expectReply) || (step.timeoutMs != want.timeoutMs) ||
                (step.switchBaud != want.switchBaud)) {
                return c * 1000U + i;
            }
        }
        // 脚本必须在这一步结束：多出一步就是行为变化。
        gnss::ConfigStep extra{};
        if (receiver.roverStep(item.stepCount, model, cfg, extra)) {
            return c * 1000U + item.stepCount;
        }
    }
    return kAllMatch;
}

/** 探测应答样本；respDecoder 用于 kUnicoreResp，其余按 decoderFor 查表。 */
inline size_t probeMatchMismatch(const gnss::Receiver &receiver,
                                 std::span<const golden::ProbeMatchCase> cases,
                                 const demux::Decoder *respDecoder) noexcept
{
    for (size_t i = 0; i < cases.size(); ++i) {
        const golden::ProbeMatchCase &item = cases[i];
        const demux::Decoder *dec = item.decoder == golden::DecoderKind::kUnicoreResp
                                        ? respDecoder
                                        : decoderFor(item.decoder);
        if (dec == nullptr) {
            return i;
        }
        gnss::ProbeResult result{};
        const bool hit = receiver.matchProbe(*dec, item.frame.view(), result);
        if (hit != item.hit) {
            return i;
        }
        if (hit && ((static_cast<uint8_t>(result.model) != item.model) ||
                    (std::string_view(result.fwVersion.data()) != item.fw.sv()))) {
            return i;
        }
    }
    return kAllMatch;
}

/** 应答判定样本；respDecoder 同 probeMatchMismatch。 */
inline size_t replyMismatch(const gnss::Receiver &receiver,
                            std::span<const golden::ReplyCase> cases,
                            const demux::Decoder *promptDecoder) noexcept
{
    for (size_t i = 0; i < cases.size(); ++i) {
        const golden::ReplyCase &item = cases[i];
        // 驱动私有的应答解码器（Unicore 的 $command、ComNav 的提示符）由调用方传入。
        const bool vendor = (item.decoder == golden::DecoderKind::kComnavPrompt) ||
                            (item.decoder == golden::DecoderKind::kUnicoreResp);
        const demux::Decoder *dec = vendor ? promptDecoder : decoderFor(item.decoder);
        if (dec == nullptr) {
            return i;
        }
        const gnss::Ack ack = receiver.classifyReply(*dec, item.frame.view(), item.sent.view());
        if (static_cast<uint8_t>(ack) != item.ack) {
            return i;
        }
    }
    return kAllMatch;
}

/** 能力位样本。 */
/**
 * @brief 逐条比对型号能力：期望值来自基线实现，型号差异由 forModel 选出的驱动负责（ADR-0012 §2.8）。
 * @param receiver: 被测的家族驱动。
 * @param cases: 黄金用例（model, caps）。
 * @retval index: 第一个不匹配的下标。
 * @retval kAllMatch: 全部一致。
 */
inline size_t capsMismatch(const gnss::Receiver &receiver,
                           std::span<const golden::CapsCase> cases) noexcept
{
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto model = static_cast<gnss::Model>(cases[i].model);
        // 先换到该型号的驱动再看能力：家族驱动对 UM982 只报 UM980 能力。
        if (receiver.forModel(model).caps(model) != cases[i].caps) {
            return i;
        }
    }
    return kAllMatch;
}

} // namespace golden_check
