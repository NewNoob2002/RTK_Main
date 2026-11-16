// u-blox F9 代驱动（ZED-F9P）：UBX 二进制配置。
// 探测：UBX-MON-VER 轮询（B5 62 0A 04 00 00 0E 34）→ MON-VER 应答，扩展字段 "MOD=ZED-F9P" 定型号。
// 配置：UBX-CFG-VALSET（layer RAM|BBR）逐组键值 → UBX-ACK-ACK / ACK-NAK（class/id 匹配 06 8A）。
//   键：CFG-RATE-MEAS、CFG-MSGOUT-NMEA_ID_{GGA,RMC,GSA,GSV}_UART1、CFG-UART1INPROT-RTCM3X=1、CFG-UART1-BAUDRATE。
// 依据：u-blox F9 HPG 1.32 Interface Description（未在实物上验证，HIL 只有 UM980）。
// 构建开关：IDF 下 CONFIG_RTK_GNSS_DRV_UBLOX，主机 CMake 下 RTK_GNSS_DRV_UBLOX。
// 线程安全：是（驱动无可变状态）。ISR 安全：是。
// 冻结接口（ADR-0012 §3）。实现方不得修改；变更须先修订 ADR-0012。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "gnss/Receiver.hpp"

namespace gnss {

/** ZED-F9P 驱动（定义在 src/drivers/UbloxReceiver.cpp）；应答解码器使用 demux::ubx::kDecoder。 */
class UbloxReceiver final : public Receiver {
public:
    constexpr UbloxReceiver() noexcept = default;

    [[nodiscard]] Family family() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::span<const demux::Decoder *const> decoders() const noexcept override;
    [[nodiscard]] uint32_t caps(Model model) const noexcept override;
    [[nodiscard]] bool build_probe(CmdBuf &out) const noexcept override;
    [[nodiscard]] bool match_probe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                   ProbeResult &out) const noexcept override;
    [[nodiscard]] bool rover_step(size_t index, Model model, const RoverConfig &cfg,
                                  ConfigStep &out) const noexcept override;
    [[nodiscard]] Ack classify_reply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                     std::span<const uint8_t> sent) const noexcept override;
    /** "UBX cc-ii"（与 main 原 describe_frame() 文本一致）。 */
    size_t frame_tag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                     std::span<char> out) const noexcept override;
};

/** 驱动单例（.cpp 内 constinit 静态对象）。线程/ISR 安全：是（只读）。 */
[[nodiscard]] const Receiver &ublox_receiver() noexcept;

} // namespace gnss
