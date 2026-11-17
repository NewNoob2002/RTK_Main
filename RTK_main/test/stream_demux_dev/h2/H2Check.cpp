// H2 自测（不交付、不参与构建；任务卡允许放 test/stream_demux_dev/h2/）。
// 逐前缀驱动六个解码器（五个内置协议 + 产品帧），核对：
//   1. step 契约：中间前缀一律 kNeedMore、need ≥ 1、len + need ≤ max_frame、need 不越过帧尾；
//   2. 纯函数性：同一前缀调用两次结论与 need 完全相同；
//   3. 合法帧在整段长度上给出 kFrame；破坏末尾字节后不再给出 kFrame；
//   4. 每个早拒绝点；长度超限时在 len + 1 > max_frame 处拒绝；
//   5. Crc.hpp 的核对值、分段累计；产品帧 frame_view 的字段与拒绝条件。

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>

#include "demux/Crc.hpp"
#include "demux/Decoder.hpp"
#include "demux/decoders/Nmea.hpp"
#include "demux/decoders/Rtcm3.hpp"
#include "demux/decoders/Ubx.hpp"
#include "demux/decoders/UnicoreBin.hpp"
#include "demux/decoders/UnicoreHash.hpp"
#include "H2Vectors.h"
#include "proto/DemuxFrame.hpp"

namespace {

int s_pass = 0;
int s_fail = 0;

void check(bool ok, const char *what, std::size_t detail = 0U)
{
    if (ok) {
        ++s_pass;
    } else {
        ++s_fail;
        if (detail != 0U) {
            std::printf("FAIL %s (len=%zu)\n", what, detail);
        } else {
            std::printf("FAIL %s\n", what);
        }
    }
}

demux::Step step_of(const demux::Decoder &decoder, std::initializer_list<uint8_t> bytes,
                    std::size_t &need)
{
    const std::span<const uint8_t> frame(bytes.begin(), bytes.size());
    return decoder.step(frame, need);
}

/** 逐前缀核对一条完整合法帧。 */
void check_valid_frame(const demux::Decoder &decoder, std::span<const uint8_t> frame,
                       const char *name)
{
    check(decoder.first_byte(frame[0]), name);
    for (std::size_t len = 1U; len < frame.size(); ++len) {
        const std::span<const uint8_t> prefix = frame.first(len);
        std::size_t need_a = 0U;
        std::size_t need_b = 0U;
        const demux::Step res_a = decoder.step(prefix, need_a);
        const demux::Step res_b = decoder.step(prefix, need_b);
        check((res_a == res_b) && (need_a == need_b), name, len);
        check(res_a == demux::Step::kNeedMore, name, len);
        check(need_a >= 1U, name, len);
        check((len + need_a) <= decoder.max_frame, name, len);
        // 合法帧的每个前缀都不得要求超出该帧实际字节数。
        check((len + need_a) <= frame.size(), name, len);
    }
    std::size_t need = 0U;
    check(decoder.step(frame, need) == demux::Step::kFrame, name, frame.size());
}

/** 破坏倒数第 3 个字节（校验值/载荷）后，整段不得再被判为 kFrame。 */
void check_corrupted(const demux::Decoder &decoder, std::span<const uint8_t> frame,
                     const char *name)
{
    std::array<uint8_t, 512> copy{};
    check(frame.size() <= copy.size(), name, frame.size());
    for (std::size_t idx = 0U; idx < frame.size(); ++idx) {
        copy[idx] = frame[idx];
    }
    copy[frame.size() - 3U] = static_cast<uint8_t>(copy[frame.size() - 3U] ^ 0x01U);
    std::size_t need = 0U;
    check(decoder.step(std::span<const uint8_t>(copy.data(), frame.size()), need) !=
              demux::Step::kFrame,
          name, frame.size());
}

void check_reject(const demux::Decoder &decoder, std::initializer_list<uint8_t> bytes,
                  const char *name)
{
    std::size_t need = 0U;
    check(step_of(decoder, bytes, need) == demux::Step::kReject, name, bytes.size());
}

/** 无长度字段的 ASCII 协议：正文一直合法但永不结束，必须在 max_frame 之内拒绝且从不超限。 */
/** 超长检查缓冲：须容纳被测解码器 max_frame 加 1 个越界探测字节（hash 解码器上限已由 2048 提到 4096）。 */
constexpr std::size_t kOverflowBufBytes =
    std::max(demux::nmea::kMaxFrame, demux::unicore_hash::kMaxFrame) + 1U;

void check_length_overflow(const demux::Decoder &decoder, uint8_t lead, uint8_t second,
                           const char *name)
{
    static std::array<uint8_t, kOverflowBufBytes> buf{};
    if (decoder.max_frame >= buf.size()) {
        check(false, name, decoder.max_frame); // 新解码器上限超出缓冲：报失败而不是越界
        return;
    }
    buf.fill(0U);
    buf[0] = lead;
    buf[1] = second;
    std::size_t last_need = 0U;
    std::size_t rejected_at = 0U;
    for (std::size_t len = 2U; len <= decoder.max_frame; ++len) {
        buf[len] = 'A';
        std::size_t need = 0U;
        const demux::Step result =
            decoder.step(std::span<const uint8_t>(buf.data(), len + 1U), need);
        if (result == demux::Step::kNeedMore) {
            check(need >= 1U, name, len);
            check((len + 1U + need) <= decoder.max_frame, name, len);
            last_need = need;
        } else {
            check(result == demux::Step::kReject, name, len);
            rejected_at = len + 1U;
            break;
        }
    }
    check((rejected_at == decoder.max_frame) || ((rejected_at == 0U) && (last_need != 0U)), name,
          rejected_at);
}

void check_crc()
{
    const char *digits = "123456789";
    const std::span<const uint8_t> text(reinterpret_cast<const uint8_t *>(digits), 9U);

    check(demux::crc::crc24q(text) == 0xCDE703U, "crc24q(\"123456789\")");
    check(demux::crc::crc32_novatel(text) == 0x2DFD2D88U, "crc32_novatel(\"123456789\")");
    check(demux::crc::crc32_novatel({}) == 0U, "crc32_novatel(\"\")");
    // 分段累计：f(b, f(a, init)) == f(a || b, init)。
    check(demux::crc::crc24q(text.subspan(4U), demux::crc::crc24q(text.first(4U))) == 0xCDE703U,
          "crc24q 分段");
    check(demux::crc::crc32_novatel(text.subspan(4U), demux::crc::crc32_novatel(text.first(4U))) ==
              0x2DFD2D88U,
          "crc32_novatel 分段");
    // 逐字节累计等价于整段。
    uint32_t acc = 0U;
    for (const uint8_t byte : text) {
        const uint8_t one[1] = {byte};
        acc = demux::crc::crc32_novatel(std::span<const uint8_t>(one, 1U), acc);
    }
    check(acc == 0x2DFD2D88U, "crc32_novatel 逐字节累计");
}

void check_frame_view()
{
    std::array<uint8_t, 280> wire{};
    for (std::size_t idx = 0U; idx < sizeof(kProto_Data); ++idx) {
        wire[idx] = kProto_Data[idx];
    }
    const std::span<const uint8_t> data(wire.data(), sizeof(kProto_Data));

    const std::optional<proto::FrameView> view = proto::demux_frame::view(data);
    check(view.has_value(), "view 合法帧");
    if (view.has_value()) {
        check(view->msg_id == 0x0002U, "view msg_id");
        check(view->sender == 0x00U, "view sender");
        check(view->type == 0x04U, "view type");
        check(view->payload.size() == 4U, "view payload 长度");
        check((view->payload.size() == 4U) && (view->payload[0] == 0x01U) &&
                  (view->payload[3] == 0x04U),
              "view payload 内容");
    }

    check(!proto::demux_frame::view(data.first(data.size() - 1U)).has_value(), "view 长度少 1");
    check(!proto::demux_frame::view(data.first(3U)).has_value(), "view 太短");

    wire[2] = 0xB5U; // 第 3 字节改成 Unicore 二进制的 B5
    check(!proto::demux_frame::view(data).has_value(), "view 同步错");
    wire[2] = 0x18U;
    wire[3] = 0x15U; // 头长字节错
    check(!proto::demux_frame::view(data).has_value(), "view 头长错");
    wire[3] = 0x14U;
    wire[12] = 0x01U; // payload_len = 0x0104 > 256
    wire[13] = 0x01U;
    check(!proto::demux_frame::view(data).has_value(), "view 长度超限");
}

} // namespace

int main()
{
    check_crc();

    check_valid_frame(demux::rtcm3::kDecoder,
                      std::span<const uint8_t>(kRtcm3_1005, sizeof(kRtcm3_1005)), "rtcm3 1005");
    check_valid_frame(demux::rtcm3::kDecoder,
                      std::span<const uint8_t>(kRtcm3_Empty, sizeof(kRtcm3_Empty)), "rtcm3 空载荷");
    check_valid_frame(demux::nmea::kDecoder, std::span<const uint8_t>(kNmea_Gga, sizeof(kNmea_Gga)),
                      "nmea GGA");
    check_valid_frame(demux::nmea::kDecoder,
                      std::span<const uint8_t>(kNmea_LowerHex, sizeof(kNmea_LowerHex)),
                      "nmea 小写十六进制");
    check_valid_frame(demux::ubx::kDecoder,
                      std::span<const uint8_t>(kUbxMonVer, sizeof(kUbxMonVer)), "ubx MON-VER");
    check_valid_frame(demux::ubx::kDecoder,
                      std::span<const uint8_t>(kUbxNavPvt, sizeof(kUbxNavPvt)), "ubx NAV-PVT");
    check_valid_frame(demux::unicore_bin::kDecoder,
                      std::span<const uint8_t>(kUcbin_Min, sizeof(kUcbin_Min)),
                      "unicore_bin 最小帧");
    check_valid_frame(demux::unicore_bin::kDecoder,
                      std::span<const uint8_t>(kUcbin_Payload, sizeof(kUcbin_Payload)),
                      "unicore_bin 带载荷");
    check_valid_frame(demux::unicore_hash::kDecoder,
                      std::span<const uint8_t>(kHash_Crc, sizeof(kHash_Crc)), "hash CRC 形式");
    check_valid_frame(demux::unicore_hash::kDecoder,
                      std::span<const uint8_t>(kHash_Xor, sizeof(kHash_Xor)), "hash XOR 形式");
    check_valid_frame(proto::demux_frame::kDecoder,
                      std::span<const uint8_t>(kProto_Data, sizeof(kProto_Data)), "产品帧 数据");
    check_valid_frame(proto::demux_frame::kDecoder,
                      std::span<const uint8_t>(kProto_Empty, sizeof(kProto_Empty)),
                      "产品帧 空载荷");
    check_valid_frame(proto::demux_frame::kDecoder,
                      std::span<const uint8_t>(kProto_Max, sizeof(kProto_Max)), "产品帧 最大载荷");

    check_corrupted(demux::rtcm3::kDecoder,
                    std::span<const uint8_t>(kRtcm3_1005, sizeof(kRtcm3_1005)), "rtcm3 坏帧");
    check_corrupted(demux::nmea::kDecoder, std::span<const uint8_t>(kNmea_Gga, sizeof(kNmea_Gga)),
                    "nmea 坏帧");
    check_corrupted(demux::ubx::kDecoder, std::span<const uint8_t>(kUbxNavPvt, sizeof(kUbxNavPvt)),
                    "ubx 坏帧");
    check_corrupted(demux::unicore_bin::kDecoder,
                    std::span<const uint8_t>(kUcbin_Payload, sizeof(kUcbin_Payload)),
                    "unicore_bin 坏帧");
    check_corrupted(demux::unicore_hash::kDecoder,
                    std::span<const uint8_t>(kHash_Crc, sizeof(kHash_Crc)), "hash 坏帧");
    check_corrupted(proto::demux_frame::kDecoder,
                    std::span<const uint8_t>(kProto_Data, sizeof(kProto_Data)), "产品帧 坏帧");

    // 早拒绝点
    std::size_t need = 0U;
    check(step_of(demux::rtcm3::kDecoder, {0xD3U}, need) == demux::Step::kNeedMore,
          "rtcm3 第 1 字节");
    check(need == 1U, "rtcm3 第 1 字节 need");
    check_reject(demux::rtcm3::kDecoder, {0xD3U, 0x04U}, "rtcm3 保留位非 0");
    check(step_of(demux::nmea::kDecoder, {0x24U}, need) == demux::Step::kNeedMore,
          "nmea 第 1 字节");
    check_reject(demux::nmea::kDecoder, {0x24U, 0x61U}, "nmea 第 2 字节小写");
    check_reject(demux::nmea::kDecoder, {0x24U, 0x47U, 0x01U}, "nmea 正文不可打印");
    check_reject(demux::nmea::kDecoder, {0x24U, 0x47U, 0x24U}, "nmea 正文含 '$'");
    check_reject(demux::ubx::kDecoder, {0xB5U, 0x61U}, "ubx 第 2 字节");
    check_reject(demux::ubx::kDecoder, {0xB5U, 0x62U, 0x01U, 0x02U, 0xFFU, 0xFFU}, "ubx 长度超限");
    check_reject(demux::unicore_bin::kDecoder, {0xAAU, 0x18U}, "unicore_bin 第 2 字节");
    check_reject(demux::unicore_bin::kDecoder, {0xAAU, 0x44U, 0x18U},
                 "unicore_bin 第 3 字节（产品帧）");
    check_reject(demux::unicore_bin::kDecoder,
                 {0xAAU, 0x44U, 0xB5U, 0x00U, 0x01U, 0x00U, 0xFFU, 0xFFU}, "unicore_bin 长度超限");
    check_reject(demux::unicore_hash::kDecoder, {0x23U, 0x61U}, "hash 第 2 字节小写");
    check_reject(demux::unicore_hash::kDecoder, {0x23U, 0x41U, 0x24U}, "hash 正文含 '$'");
    check_reject(demux::unicore_hash::kDecoder, {0x23U, 0x41U, 0x23U}, "hash 正文含 '#'");
    check_reject(proto::demux_frame::kDecoder, {0xAAU, 0x44U, 0xB5U},
                 "产品帧 第 3 字节（Unicore）");
    check_reject(proto::demux_frame::kDecoder, {0xAAU, 0x44U, 0x18U, 0x15U}, "产品帧 头长字节");
    check_reject(proto::demux_frame::kDecoder,
                 {0xAAU, 0x44U, 0x18U, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                  0x00U, 0x01U, 0x01U},
                 "产品帧 payload_len 超限");

    // 帧长未知的 ASCII 协议：永不结束时必须在本协议 max_frame 处拒绝
    check_length_overflow(demux::nmea::kDecoder, 0x24U, 0x47U, "nmea 超长");
    check_length_overflow(demux::unicore_hash::kDecoder, 0x23U, 0x41U, "hash 超长");

    check_frame_view();

    std::printf("H2 self-check: %d passed, %d failed\n", s_pass, s_fail);
    return (s_fail == 0) ? 0 : 1;
}
