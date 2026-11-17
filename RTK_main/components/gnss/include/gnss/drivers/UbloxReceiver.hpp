// u-blox F9 代驱动（ZED-F9P）：UBX-MON-VER 探测 + UBX-CFG-VALSET 二进制配置（ADR-0010 §2.2/§2.4）；构建开关 IDF CONFIG_RTK_GNSS_DRV_UBLOX / host RTK_GNSS_DRV_UBLOX。
// 线程安全：是（驱动无可变状态，解码器为纯函数）。ISR 安全：是（无锁、不阻塞）。
// 堆与阻塞：不使用堆（命令写入调用方的 CmdBuf）、不阻塞。冻结接口（ADR-0012 §3）实现方不得修改；变更须先修订 ADR-0012。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "gnss/Receiver.hpp"

namespace gnss {

/**
 * u-blox F9 代家族基类（定义在 src/drivers/UbloxReceiver.cpp）；应答解码器使用 demux::ubx::kDecoder。
 * 家族基类，只能经 final 叶子类实例化（ADR-0012 §2.8）：构造与析构都是 protected 非虚，拷贝/移动删除；
 * 实例为 ZedF9pReceiver 的 constinit 静态对象。
 *
 * @note   探测帧 B5 62 0A 04 00 00 0E 34，扩展字段 "MOD=ZED-F9P" 定型号；配置层 RAM|BBR，ACK 按 class/id 06 8A 匹配。
 * @note   键：CFG-RATE-MEAS、CFG-MSGOUT-NMEA_ID_{GGA,RMC,GSA,GSV}_UART1、CFG-UART1INPROT-RTCM3X、CFG-UART1-BAUDRATE。
 */
class UbloxReceiver : public Receiver {
public:
    UbloxReceiver(const UbloxReceiver &) = delete;
    UbloxReceiver &operator=(const UbloxReceiver &) = delete;
    UbloxReceiver(UbloxReceiver &&) = delete;
    UbloxReceiver &operator=(UbloxReceiver &&) = delete;

    [[nodiscard]] Family family() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::span<const demux::Decoder *const> decoders() const noexcept override;
    [[nodiscard]] uint32_t caps(Model model) const noexcept override;
    [[nodiscard]] bool buildProbe(CmdBuf &out) const noexcept override;
    [[nodiscard]] bool matchProbe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                  ProbeResult &out) const noexcept override;
    [[nodiscard]] bool roverStep(size_t index, Model model, const RoverConfig &cfg,
                                 ConfigStep &out) const noexcept override;
    [[nodiscard]] Ack classifyReply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                    std::span<const uint8_t> sent) const noexcept override;
    /** "UBX cc-ii"（与 main 原 describe_frame() 文本一致）。 */
    size_t frameTag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                    std::span<char> out) const noexcept override;
    /** CFG-MSGOUT-NMEA_ID_*_UART1 支持的 5 类（GGA/RMC/GSA/GST/GSV）；SATSINFOA/HEADINGA 不支持。 */
    [[nodiscard]] uint32_t outputMask(Model model) const noexcept override;
    /** CFG-MSGOUT VALSET（单键），值 = 每历元次数 round(periodMs*rateHz/1000)；0 关闭。 */
    [[nodiscard]] fw::Status buildOutput(OutputMsg msg, uint32_t periodMs, Model model,
                                         const OutputPlan &plan,
                                         CmdBuf &out) const noexcept override;

protected:
    // 家族基类不可直接实例化（CPP-011）：构造/析构 protected、析构非虚，保证可平凡析构且不生成删除析构。
    constexpr UbloxReceiver() noexcept = default;
    ~UbloxReceiver() = default;

    // ---- 命令库：每个函数只组一条完整 UBX 帧（sync + class/id + len + payload + Fletcher） ----
    [[nodiscard]] static bool cmdBuildFrame(CmdBuf &out, uint8_t cls, uint8_t id,
                                            std::span<const uint8_t> payload) noexcept;
    [[nodiscard]] static bool cmdPollMonVer(CmdBuf &out) noexcept; // UBX-MON-VER 轮询（空载荷）
    [[nodiscard]] static bool cmdConfigureReceiver(CmdBuf &out, uint16_t periodMs) noexcept;
    [[nodiscard]] static bool cmdSetBaudrate(CmdBuf &out, uint32_t baud) noexcept;
    [[nodiscard]] static bool cmdValsetMsgout(CmdBuf &out, uint32_t key, uint8_t epochs) noexcept;

    // ---- 应答判定：只判帧结构与 ACK 类别，不看具体配置内容 ----
    [[nodiscard]] static size_t respPayloadLen(std::span<const uint8_t> frame) noexcept;
    [[nodiscard]] static bool respHasValidAck(std::span<const uint8_t> frame) noexcept;
    [[nodiscard]] static bool respAckClassId(std::span<const uint8_t> frame, uint8_t cls,
                                             uint8_t id) noexcept;
};

/** ZED-F9P 基准型号叶子类：无覆盖，家族默认行为即 F9P 行为（name() 仍为 "ublox"）。 */
class ZedF9pReceiver final : public UbloxReceiver {
public:
    constexpr ZedF9pReceiver() noexcept = default;
};

} // namespace gnss
