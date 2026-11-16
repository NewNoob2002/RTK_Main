// Unicore N4 平台驱动（UM980/UM981/UM982）：ASCII 命令以 \r\n 结尾；
// 命令应答 "$command,<原命令>,response: OK*hh\r\n"（失败为 PARSING/其他文本），XOR 校验同 NMEA；
// 版本查询 "VERSIONA\r\n" → "#VERSIONA,...;\"UM980\",\"<fw>\",...*crc32\r\n"（demux::unicore_hash 成帧）。
// 依据：Unicore N4 高精度产品命令手册 V2 R1.6 §7（GPGGA COMx rate、UNLOG、CONFIG COMx baud、MODE ROVER、VERSIONA）。
// 构建开关：IDF 下 CONFIG_RTK_GNSS_DRV_UNICORE，主机 CMake 下 RTK_GNSS_DRV_UNICORE。
// 线程安全：是（驱动无可变状态；解码器函数为纯函数）。ISR 安全：是。
// 冻结接口（ADR-0012 §3）。实现方不得修改；变更须先修订 ADR-0012。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "gnss/Receiver.hpp"

namespace gnss::unicore {

/**
 * "$command,...*hh\r\n" 应答解码器：$ | "command," | 可打印 ASCII（不含 $ *）| * 两位十六进制 XOR | \r\n。
 * 与 demux::nmea 首字节相同（$），由 nmea 的第 2 字节 [A-Z0-9] 规则让出（'c' 小写），故两者可同时挂载。
 */
inline constexpr std::size_t kResponseMaxFrame = 256;
[[nodiscard]] bool response_first_byte(uint8_t byte) noexcept;
[[nodiscard]] demux::Step response_step(std::span<const uint8_t> frame, std::size_t &need) noexcept;
[[nodiscard]] std::size_t explain_response(std::span<const uint8_t> frame, std::span<char> out) noexcept;
inline constexpr demux::Decoder kResponseDecoder{"UnicoreResp", kResponseMaxFrame, &response_first_byte, &response_step,
                                                 &explain_response};

} // namespace gnss::unicore

namespace gnss {

/** UM980/981/982 家族驱动（定义在 src/drivers/UnicoreReceiver.cpp）。 */
class UnicoreReceiver final : public Receiver {
public:
    constexpr UnicoreReceiver() noexcept = default;

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
    /** 基类默认（NMEA）+ #SATSINFOA / #HEADINGA（ADR-0012 §2.6）。 */
    [[nodiscard]] OutputMsg classify_frame(const demux::Decoder &dec,
                                           std::span<const uint8_t> frame) const noexcept override;
    /** 基类默认 + "#VERSIONA"/"$command" 专用标签，与 main 原 describe_frame() 文本一致。 */
    size_t frame_tag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                     std::span<char> out) const noexcept override;
    /** 基类默认 + SATSINFOA/GST，双天线型号（UM982）另加 HEADINGA。 */
    [[nodiscard]] uint32_t output_mask(Model model) const noexcept override;
    /** "<name> COMn <period>" / "UNLOG COMn <name>"（ADR-0012 §2.4）；10 ms 以上且可表示为秒时成功。 */
    [[nodiscard]] fw::Status build_output(OutputMsg msg, uint32_t period_ms, Model model, const OutputPlan &plan,
                                          CmdBuf &out) const noexcept override;
};

/** 驱动单例（.cpp 内 constinit 静态对象）。线程/ISR 安全：是（只读）。 */
[[nodiscard]] const Receiver &unicore_receiver() noexcept;

} // namespace gnss
