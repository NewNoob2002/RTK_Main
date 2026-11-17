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
[[nodiscard]] bool promptFirstByte(uint8_t byte) noexcept;
[[nodiscard]] demux::Step promptStep(std::span<const uint8_t> frame, std::size_t &need) noexcept;
[[nodiscard]] std::size_t explainPrompt(std::span<const uint8_t> frame,
                                        std::span<char> out) noexcept;
inline constexpr demux::Decoder kPromptDecoder{"ComnavPrompt", kPromptMaxFrame, &promptFirstByte,
                                               &promptStep, &explainPrompt};

} // namespace gnss::comnav

namespace gnss {

/**
 * K8 系列（K803 等）家族基类（定义在 src/drivers/ComnavReceiver.cpp）。
 * 家族基类，只能经 final 叶子类实例化（ADR-0012 §2.8）：构造与析构都是 protected 非虚，拷贝/移动删除；
 * 实例为 K803Receiver 的 constinit 静态对象。
 */
class ComnavReceiver : public Receiver {
public:
    ComnavReceiver(const ComnavReceiver &) = delete;
    ComnavReceiver &operator=(const ComnavReceiver &) = delete;
    ComnavReceiver(ComnavReceiver &&) = delete;
    ComnavReceiver &operator=(ComnavReceiver &&) = delete;

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
    /** K803 只能输出 GGA/RMC/GSA/GSV；SATSINFOA/HEADINGA/GST 不支持。 */
    [[nodiscard]] uint32_t outputMask(Model model) const noexcept override;
    /** "LOG COMn <name> ONTIME <period>" / "UNLOG COMn <name>"（ADR-0012 §2.4）。 */
    [[nodiscard]] fw::Status buildOutput(OutputMsg msg, uint32_t periodMs, Model model,
                                         const OutputPlan &plan,
                                         CmdBuf &out) const noexcept override;

protected:
    // 家族基类不可直接实例化（CPP-011）：构造/析构 protected、析构非虚，保证可平凡析构且不生成删除析构。
    constexpr ComnavReceiver() noexcept = default;
    ~ComnavReceiver() = default;

    // ---- 命令库：只往 out 写一条完整命令（含 \r\n，先清空），失败返回 false ----
    [[nodiscard]] static bool cmdUnlogAll(CmdBuf &out, uint8_t port) noexcept; // "UNLOGALL COMn"
    [[nodiscard]] static bool cmdModeRover(CmdBuf &out) noexcept;              // "MODE ROVER"
    [[nodiscard]] static bool cmdComBaud(CmdBuf &out, uint8_t port,
                                         uint32_t baud) noexcept; // "COM COMn <baud>"
    [[nodiscard]] static bool
    cmdLogOntime(CmdBuf &out, std::string_view name, uint8_t port,
                 uint32_t periodMs) noexcept; // "LOG COMn <name> ONTIME <period>"
    [[nodiscard]] static bool cmdUnlogMsg(CmdBuf &out, std::string_view name,
                                          uint8_t port) noexcept;    // "UNLOG COMn <name>"
    [[nodiscard]] static bool cmdVersionQuery(CmdBuf &out) noexcept; // "LOG VERSIONA ONCE"

    // ---- 应答判定：只判成功/失败，具体文本不进主流程 ----
    [[nodiscard]] static bool respIsOk(std::span<const uint8_t> frame) noexcept; // 以 "<OK" 开头
    [[nodiscard]] static bool
    respIsError(std::span<const uint8_t> frame) noexcept; // 以 "<ERROR" 开头
};

/** K803 基准型号叶子类：无覆盖，家族默认行为即 K803 行为（name() 仍为 "comnav"）。 */
class K803Receiver final : public ComnavReceiver {
public:
    constexpr K803Receiver() noexcept = default;
};

} // namespace gnss
