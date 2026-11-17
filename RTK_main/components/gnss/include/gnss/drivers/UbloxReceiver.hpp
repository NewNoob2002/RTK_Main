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
class UbloxReceiver : public Receiver {
public:
    constexpr UbloxReceiver() noexcept = default;
    // 去掉 final 后必须公开虚析构：基类 Receiver 的 protected 非虚析构会挡住命名空间作用域的对象定义，
    // 且 -Wnon-virtual-dtor 视为错误（与 UnicoreReceiver 同一处理）。
    // 驱动对象仍只在静态存储以 constinit 存活，从不 new/delete，故删除拷贝/移动。
    virtual ~UbloxReceiver() = default;

    UbloxReceiver(const UbloxReceiver &) = delete;
    UbloxReceiver &operator=(const UbloxReceiver &) = delete;
    UbloxReceiver(UbloxReceiver &&) = delete;
    UbloxReceiver &operator=(UbloxReceiver &&) = delete;

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
    /** CFG-MSGOUT-NMEA_ID_*_UART1 支持的 5 类（GGA/RMC/GSA/GST/GSV）；SATSINFOA/HEADINGA 不支持。 */
    [[nodiscard]] uint32_t output_mask(Model model) const noexcept override;
    /** CFG-MSGOUT VALSET（单键），值 = 每历元次数 round(period_ms*rate_hz/1000)；0 关闭。 */
    [[nodiscard]] fw::Status build_output(OutputMsg msg, uint32_t period_ms, Model model, const OutputPlan &plan,
                                          CmdBuf &out) const noexcept override;

protected:
    // ---- 命令库：每个函数只组一条完整 UBX 帧（sync + class/id + len + payload + Fletcher） ----
    [[nodiscard]] static bool cmd_build_frame(CmdBuf &out, uint8_t cls, uint8_t id,
                                              std::span<const uint8_t> payload) noexcept;
    [[nodiscard]] static bool cmd_poll_mon_ver(CmdBuf &out) noexcept; // UBX-MON-VER 轮询（空载荷）
    [[nodiscard]] static bool cmd_configure_receiver(CmdBuf &out, uint16_t period_ms) noexcept;
    [[nodiscard]] static bool cmd_set_baudrate(CmdBuf &out, uint32_t baud) noexcept;
    [[nodiscard]] static bool cmd_valset_msgout(CmdBuf &out, uint32_t key, uint8_t epochs) noexcept;

    // ---- 应答判定：只判帧结构与 ACK 类别，不看具体配置内容 ----
    [[nodiscard]] static size_t resp_payload_len(std::span<const uint8_t> frame) noexcept;
    [[nodiscard]] static bool resp_has_valid_ack(std::span<const uint8_t> frame) noexcept;
    [[nodiscard]] static bool resp_ack_class_id(std::span<const uint8_t> frame, uint8_t cls, uint8_t id) noexcept;
};

/** 驱动单例（.cpp 内 constinit 静态对象）。线程/ISR 安全：是（只读）。 */
[[nodiscard]] const Receiver &ublox_receiver() noexcept;

} // namespace gnss
