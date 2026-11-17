// 黄金向量比对器（ADR-0012 P3a 第 3 步）：把 golden::* 数据与驱动行为逐项比较，返回第一个不一致的下标。
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
inline const demux::Decoder *decoder_for(golden::DecoderKind kind) noexcept
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

inline bool same_bytes(std::span<const uint8_t> data, std::string_view expect) noexcept
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

/** build_probe：字节与成功标志都要与基线一致。 */
inline bool probe_matches(const gnss::Receiver &receiver, const golden::Probe &expect) noexcept
{
    gnss::CmdBuf cmd;
    const bool ok = receiver.build_probe(cmd);
    if (ok != expect.ok) {
        return false;
    }
    return !ok || same_bytes(cmd.view(), expect.bytes.sv());
}

/** 全部 rover_step 组合；返回第一个不一致的 (case, step) 压缩下标，kAllMatch 表示全部一致。 */
inline size_t steps_mismatch(const gnss::Receiver &receiver, std::span<const golden::RoverCase> cases,
                             std::span<const golden::Step> steps) noexcept
{
    for (size_t c = 0; c < cases.size(); ++c) {
        const golden::RoverCase &item = cases[c];
        gnss::RoverConfig cfg{};
        cfg.rate_hz = item.rate_hz;
        cfg.gsv = item.gsv;
        cfg.port = item.port;
        cfg.port_baud = item.port_baud;
        const auto model = static_cast<gnss::Model>(item.model);
        for (size_t i = 0; i < item.step_count; ++i) {
            gnss::ConfigStep step{};
            if (!receiver.rover_step(i, model, cfg, step)) {
                return c * 1000U + i;
            }
            const golden::Step &want = steps[static_cast<size_t>(item.step_first) + i];
            if ((!same_bytes(step.cmd.view(), want.bytes.sv())) || (step.expect_reply != want.expect_reply) ||
                (step.timeout_ms != want.timeout_ms) || (step.switch_baud != want.switch_baud)) {
                return c * 1000U + i;
            }
        }
        // 脚本必须在这一步结束：多出一步就是行为变化。
        gnss::ConfigStep extra{};
        if (receiver.rover_step(item.step_count, model, cfg, extra)) {
            return c * 1000U + item.step_count;
        }
    }
    return kAllMatch;
}

/** 探测应答样本；resp_decoder 用于 kUnicoreResp，其余按 decoder_for 查表。 */
inline size_t probe_match_mismatch(const gnss::Receiver &receiver, std::span<const golden::ProbeMatchCase> cases,
                                   const demux::Decoder *resp_decoder) noexcept
{
    for (size_t i = 0; i < cases.size(); ++i) {
        const golden::ProbeMatchCase &item = cases[i];
        const demux::Decoder *dec =
            item.decoder == golden::DecoderKind::kUnicoreResp ? resp_decoder : decoder_for(item.decoder);
        if (dec == nullptr) {
            return i;
        }
        gnss::ProbeResult result{};
        const bool hit = receiver.match_probe(*dec, item.frame.view(), result);
        if (hit != item.hit) {
            return i;
        }
        if (hit && ((static_cast<uint8_t>(result.model) != item.model) ||
                    (std::string_view(result.fw_version.data()) != item.fw.sv()))) {
            return i;
        }
    }
    return kAllMatch;
}

/** 应答判定样本；resp_decoder 同 probe_match_mismatch。 */
inline size_t reply_mismatch(const gnss::Receiver &receiver, std::span<const golden::ReplyCase> cases,
                             const demux::Decoder *prompt_decoder) noexcept
{
    for (size_t i = 0; i < cases.size(); ++i) {
        const golden::ReplyCase &item = cases[i];
        // 驱动私有的应答解码器（Unicore 的 $command、ComNav 的提示符）由调用方传入。
        const bool vendor =
            (item.decoder == golden::DecoderKind::kComnavPrompt) || (item.decoder == golden::DecoderKind::kUnicoreResp);
        const demux::Decoder *dec = vendor ? prompt_decoder : decoder_for(item.decoder);
        if (dec == nullptr) {
            return i;
        }
        const gnss::Ack ack = receiver.classify_reply(*dec, item.frame.view(), item.sent.view());
        if (static_cast<uint8_t>(ack) != item.ack) {
            return i;
        }
    }
    return kAllMatch;
}

/** 能力位样本。 */
/**
 * @brief 逐条比对型号能力：期望值来自基线实现，型号差异由 for_model 选出的驱动负责（ADR-0012 §2.8）。
 * @param receiver: 被测的家族驱动。
 * @param cases: 黄金用例（model, caps）。
 * @retval index: 第一个不匹配的下标。
 * @retval kAllMatch: 全部一致。
 */
inline size_t caps_mismatch(const gnss::Receiver &receiver, std::span<const golden::CapsCase> cases) noexcept
{
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto model = static_cast<gnss::Model>(cases[i].model);
        // 先换到该型号的驱动再看能力：家族驱动对 UM982 只报 UM980 能力。
        if (receiver.for_model(model).caps(model) != cases[i].caps) {
            return i;
        }
    }
    return kAllMatch;
}

} // namespace golden_check
