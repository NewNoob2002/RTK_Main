// Unicore N4 平台驱动（UM980/UM981/UM982）：ASCII 命令、应答判定与 "#VERSIONA" 型号识别（ADR-0010 §2.2/§2.4）；构建开关 IDF CONFIG_RTK_GNSS_DRV_UNICORE / host RTK_GNSS_DRV_UNICORE。
// 线程安全：是（驱动无可变状态，解码器为纯函数）。ISR 安全：是（无锁、不阻塞）。
// 堆与阻塞：不使用堆（命令写入调用方的 CmdBuf）、不阻塞。冻结接口（ADR-0012 §3）实现方不得修改；变更须先修订 ADR-0012。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "gnss/Receiver.hpp"

namespace gnss::unicore {

/**
 * Unicore 应答解码器：$ | "command," 或 "CONFIG," | 可打印 ASCII（不含 $ *）| * 两位十六进制 XOR（含 '$'）| \r\n。
 * "$command" 由 nmea 的第 2 字节 [A-Z0-9] 规则让出（'c' 小写）；"$CONFIG," 能过 nmea 的字符规则，靠的是 Unicore
 * 校验和含 '$'——nmea 校验失败后 demux 重扫，本解码器接手（ADR-0012 §2.7 现场证据）。若某固件对 $CONFIG 用标准
 * NMEA 校验和，这些行会被 nmea 认领、读回 0 行（HIL 实测，见 ADR-0012 §2.7）。
 */
inline constexpr std::size_t kResponseMaxFrame = 256;
[[nodiscard]] bool responseFirstByte(uint8_t byte) noexcept;
[[nodiscard]] demux::Step responseStep(std::span<const uint8_t> frame, std::size_t &need) noexcept;
[[nodiscard]] std::size_t explainResponse(std::span<const uint8_t> frame,
                                          std::span<char> out) noexcept;
inline constexpr demux::Decoder kResponseDecoder{
    "UnicoreResp", kResponseMaxFrame, &responseFirstByte, &responseStep, &explainResponse};

} // namespace gnss::unicore

namespace gnss {

/**
 * Unicore N4 家族基类（定义在 src/drivers/UnicoreReceiver.cpp），描述基准型号 UM980/UM981（ADR-0012 §2.8）。
 * 家族基类，只能经 final 叶子类实例化（ADR-0012 §2.8）：构造与析构都是 protected 非虚，拷贝/移动删除；
 * 实例为 Um980Receiver（UM980/UM981）与 Um982Receiver 的 constinit 静态对象。
 * 命令生成与应答解析为 protected static，子类直接复用。
 *
 * @note   应答 "$command,<原命令>,response: OK*hh"（失败为 PARSING 等文本，XOR 同 NMEA）；版本 "#VERSIONA,..." 由 demux::unicore_hash 成帧（手册 V2 R1.6 §7）。
 */
class UnicoreReceiver : public Receiver {
public:
    UnicoreReceiver(const UnicoreReceiver &) = delete;
    UnicoreReceiver &operator=(const UnicoreReceiver &) = delete;
    UnicoreReceiver(UnicoreReceiver &&) = delete;
    UnicoreReceiver &operator=(UnicoreReceiver &&) = delete;

    [[nodiscard]] Family family() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::span<const demux::Decoder *const> decoders() const noexcept override;
    /** UM980/UM981 能力；不含 kHeading（双天线由 Um982Receiver 覆盖）。 */
    [[nodiscard]] uint32_t caps(Model model) const noexcept override;
    /** kUm982 → detail::kUm982Receiver，其余 → *this。 */
    [[nodiscard]] const Receiver &forModel(Model model) const noexcept override;
    [[nodiscard]] bool buildProbe(CmdBuf &out) const noexcept override;
    [[nodiscard]] bool matchProbe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                  ProbeResult &out) const noexcept override;
    /** 读回命令 "CONFIG"（ADR-0012 §2.7）：UM980 回一组 $CONFIG,<port>,<config>*hh 行。 */
    [[nodiscard]] bool buildReadback(CmdBuf &out) const noexcept override;
    /** 只认 UnicoreResp 交付的 $CONFIG 行，取 "$CONFIG," 与 "*" 之间的正文（超长截断）。 */
    size_t matchReadback(const demux::Decoder &dec, std::span<const uint8_t> frame,
                         std::span<char> out) const noexcept override;
    /** 按 roverScript() 的表生成第 index 个启用步骤。 */
    [[nodiscard]] bool roverStep(size_t index, Model model, const RoverConfig &cfg,
                                 ConfigStep &out) const noexcept override;
    [[nodiscard]] Ack classifyReply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                    std::span<const uint8_t> sent) const noexcept override;
    /** 基类默认（NMEA）+ #SATSINFOA / #HEADINGA（ADR-0012 §2.6）。 */
    [[nodiscard]] OutputMsg classifyFrame(const demux::Decoder &dec,
                                          std::span<const uint8_t> frame) const noexcept override;
    /** 基类默认 + "#VERSIONA"/"$command" 专用标签。 */
    size_t frameTag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                    std::span<char> out) const noexcept override;
    /** 基类默认 + SATSINFOA/GST；caps(model) 含 kHeading 时另加 HEADINGA。 */
    [[nodiscard]] uint32_t outputMask(Model model) const noexcept override;
    /** "<name> COMn <period>" / "UNLOG COMn <name>"（ADR-0012 §2.4）；HEADINGA 需 caps(model) 含 kHeading。 */
    [[nodiscard]] fw::Status buildOutput(OutputMsg msg, uint32_t periodMs, Model model,
                                         const OutputPlan &plan,
                                         CmdBuf &out) const noexcept override;

    [[nodiscard]] bool saveStep(ConfigStep &out) const noexcept override;
    [[nodiscard]] bool factoryResetStep(uint8_t port, const ProbeResult &detected,
                                        ConfigStep &out) const noexcept override;
    [[nodiscard]] bool baudStep(uint8_t port, uint32_t baud,
                                ConfigStep &out) const noexcept override;

protected:
    // 家族基类不可直接实例化（CPP-011）：构造/析构 protected、析构非虚，保证可平凡析构且不生成删除析构。
    constexpr UnicoreReceiver() noexcept = default;
    ~UnicoreReceiver() = default;

    /** 配置脚本的一步：build 生成命令，enabled 为 false 时该步在此配置下跳过（索引顺延）。 */
    struct ScriptStep {
        bool (*build)(const RoverConfig &cfg, ConfigStep &out) noexcept;
        bool (*enabled)(const RoverConfig &cfg) noexcept;
    };

    /** 基础流动站脚本（UNLOG → MODE ROVER → [CONFIG 波特率]）；型号需要额外步骤时覆盖。 */
    [[nodiscard]] virtual std::span<const ScriptStep> roverScript() const noexcept;

    // ---- 基础脚本的步骤（roverScript() 内的函数局部表引用它们，子类脚本可复用） ----

    /** 第 0 步 "UNLOG COMn"：先关掉全部输出，再由输出阶段按 OutputPlan 重新开启。 */
    [[nodiscard]] static bool stepUnlogAll(const RoverConfig &cfg, ConfigStep &out) noexcept;
    /** 第 1 步 "MODE ROVER"。 */
    [[nodiscard]] static bool stepModeRover(const RoverConfig &cfg, ConfigStep &out) noexcept;
    /** 第 2 步 "CONFIG COMn <baud>"：发送后不等应答，主机随即切换波特率。 */
    [[nodiscard]] static bool stepPortBaud(const RoverConfig &cfg, ConfigStep &out) noexcept;
    /** 步骤启用条件：总是。 */
    [[nodiscard]] static bool stepAlways(const RoverConfig &cfg) noexcept;
    /** 步骤启用条件：cfg.portBaud != 0。 */
    [[nodiscard]] static bool stepIfPortBaud(const RoverConfig &cfg) noexcept;

    // ---- 命令库：只往 out 写一条完整命令（含 \r\n，先清空），失败返回 false ----
    [[nodiscard]] static bool cmdUnlog(CmdBuf &out, uint8_t port) noexcept; // "UNLOG COMn"
    [[nodiscard]] static bool cmdModeRover(CmdBuf &out) noexcept;           // "MODE ROVER"
    [[nodiscard]] static bool cmdConfigBaud(CmdBuf &out, uint8_t port,
                                            uint32_t baud) noexcept; // "CONFIG COMn <baud>"
    [[nodiscard]] static bool cmdLogMsg(CmdBuf &out, std::string_view name, uint8_t port,
                                        uint32_t periodMs) noexcept; // "<name> COMn <period>"
    [[nodiscard]] static bool cmdUnlogMsg(CmdBuf &out, std::string_view name,
                                          uint8_t port) noexcept;    // "UNLOG COMn <name>"
    [[nodiscard]] static bool cmdVersionQuery(CmdBuf &out) noexcept; // "VERSIONA"

    // ---- 应答解析：每个函数只判一件事 ----
    [[nodiscard]] static bool respHasValidTail(std::span<const uint8_t> frame) noexcept;
    [[nodiscard]] static std::optional<size_t> respFindTag(std::span<const uint8_t> frame) noexcept;
    [[nodiscard]] static bool respEchoMatches(std::span<const uint8_t> frame, size_t tagPos,
                                              std::span<const uint8_t> sent) noexcept;
    [[nodiscard]] static std::span<const uint8_t> respText(std::span<const uint8_t> frame,
                                                           size_t tagPos) noexcept;
};

/** UM980/UM981 基准型号叶子类：无覆盖，家族默认行为即基准型号行为（name() 仍为 "unicore"）。 */
class Um980Receiver final : public UnicoreReceiver {
public:
    constexpr Um980Receiver() noexcept = default;
};

/** UM982 双天线型号叶子类：在家族基础上增加 kHeading（HEADINGA 输出）。 */
class Um982Receiver final : public UnicoreReceiver {
public:
    constexpr Um982Receiver() noexcept = default;

    [[nodiscard]] std::string_view name() const noexcept override; // "unicore-um982"
    /** 家族能力 + kHeading。 */
    [[nodiscard]] uint32_t caps(Model model) const noexcept override;
    /** 型号驱动不再细分：返回 *this。 */
    [[nodiscard]] const Receiver &forModel(Model model) const noexcept override;
};

} // namespace gnss
