// 司南（ComNav）K8 系列驱动（K803 等）：NovAtel 风格 ASCII 命令，实验性。
// 当前依据为公开资料的 NovAtel 兼容语法（LOG/UNLOGALL/COM，应答 "<OK" 行），
// 未取得 K803 官方命令手册，未经实物验证；拿到手册后必须先修订 ADR-0012/ADR-0010 的 ComNav 小节再改实现。
// 构建开关：IDF 下 CONFIG_RTK_GNSS_DRV_COMNAV（默认关），主机 CMake 下 RTK_GNSS_DRV_COMNAV。
// 线程安全：是（驱动无可变状态；解码器函数为纯函数）。ISR 安全：是。
// 冻结接口（ADR-0012 §3）。实现方不得修改；变更须先修订 ADR-0012。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "gnss/Receiver.hpp"

namespace gnss::comnav {

/** "<OK\r\n" / "<ERROR:...\r\n" 等以 '<' 开头的应答行解码器（可打印 ASCII 直到 \r\n，无校验）。 */
inline constexpr std::size_t kPromptMaxFrame = 128;
[[nodiscard]] bool prompt_first_byte(uint8_t byte) noexcept;
[[nodiscard]] demux::Step prompt_step(std::span<const uint8_t> frame, std::size_t &need) noexcept;
[[nodiscard]] std::size_t explain_prompt(std::span<const uint8_t> frame, std::span<char> out) noexcept;
inline constexpr demux::Decoder kPromptDecoder{"ComnavPrompt", kPromptMaxFrame, &prompt_first_byte, &prompt_step,
                                               &explain_prompt};

} // namespace gnss::comnav

namespace gnss {

/** K803 驱动（定义在 src/drivers/ComnavReceiver.cpp）。 */
class ComnavReceiver : public Receiver {
public:
    constexpr ComnavReceiver() noexcept = default;
    // 去掉 final 后必须公开虚析构：基类 Receiver 的 protected 非虚析构会挡住命名空间作用域的对象定义，
    // 且 -Wnon-virtual-dtor 视为错误（与 UnicoreReceiver 同一处理）。
    // 驱动对象仍只在静态存储以 constinit 存活，从不 new/delete，故删除拷贝/移动。
    virtual ~ComnavReceiver() = default;

    ComnavReceiver(const ComnavReceiver &) = delete;
    ComnavReceiver &operator=(const ComnavReceiver &) = delete;
    ComnavReceiver(ComnavReceiver &&) = delete;
    ComnavReceiver &operator=(ComnavReceiver &&) = delete;

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
    /** K803 只能输出 GGA/RMC/GSA/GSV；SATSINFOA/HEADINGA/GST 不支持。 */
    [[nodiscard]] uint32_t output_mask(Model model) const noexcept override;
    /** "LOG COMn <name> ONTIME <period>" / "UNLOG COMn <name>"（ADR-0012 §2.4）。 */
    [[nodiscard]] fw::Status build_output(OutputMsg msg, uint32_t period_ms, Model model, const OutputPlan &plan,
                                          CmdBuf &out) const noexcept override;

protected:
    // ---- 命令库：只往 out 写一条完整命令（含 \r\n，先清空），失败返回 false ----
    [[nodiscard]] static bool cmd_unlog_all(CmdBuf &out, uint8_t port) noexcept;               // "UNLOGALL COMn"
    [[nodiscard]] static bool cmd_mode_rover(CmdBuf &out) noexcept;                            // "MODE ROVER"
    [[nodiscard]] static bool cmd_com_baud(CmdBuf &out, uint8_t port, uint32_t baud) noexcept; // "COM COMn <baud>"
    [[nodiscard]] static bool cmd_log_ontime(CmdBuf &out, std::string_view name, uint8_t port,
                                             uint32_t period_ms) noexcept; // "LOG COMn <name> ONTIME <period>"
    [[nodiscard]] static bool cmd_unlog_msg(CmdBuf &out, std::string_view name,
                                            uint8_t port) noexcept;    // "UNLOG COMn <name>"
    [[nodiscard]] static bool cmd_version_query(CmdBuf &out) noexcept; // "LOG VERSIONA ONCE"

    // ---- 应答判定：只判成功/失败，具体文本不进主流程 ----
    [[nodiscard]] static bool resp_is_ok(std::span<const uint8_t> frame) noexcept;    // 以 "<OK" 开头
    [[nodiscard]] static bool resp_is_error(std::span<const uint8_t> frame) noexcept; // 以 "<ERROR" 开头
};

/** 驱动单例（.cpp 内 constinit 静态对象）。线程/ISR 安全：是（只读）。 */
[[nodiscard]] const Receiver &comnav_receiver() noexcept;

} // namespace gnss
