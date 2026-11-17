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
#include <optional>
#include <span>
#include <string_view>

#include "demux/Decoder.hpp"
#include "gnss/Receiver.hpp"

namespace gnss::unicore {

/**
 * Unicore 应答解码器：$ | "command," 或 "CONFIG," | 可打印 ASCII（不含 $ *）| * 两位十六进制 XOR | \r\n。
 * "$command" 由 nmea 的第 2 字节 [A-Z0-9] 规则让出（'c' 小写）；"$CONFIG," 的地址段 6 字符，nmea 按 5 字符
 * 地址规则拒收（ADR-0012 §2.7），故两者可同时挂载。
 */
inline constexpr std::size_t kResponseMaxFrame = 256;
[[nodiscard]] bool response_first_byte(uint8_t byte) noexcept;
[[nodiscard]] demux::Step response_step(std::span<const uint8_t> frame, std::size_t &need) noexcept;
[[nodiscard]] std::size_t explain_response(std::span<const uint8_t> frame, std::span<char> out) noexcept;
inline constexpr demux::Decoder kResponseDecoder{"UnicoreResp", kResponseMaxFrame, &response_first_byte, &response_step,
                                                 &explain_response};

} // namespace gnss::unicore

namespace gnss {

/**
 * Unicore N4 家族驱动（定义在 src/drivers/UnicoreReceiver.cpp），描述基准型号 UM980/UM981（ADR-0012 §2.8）。
 * 有差异的型号从本类派生（Um982Receiver），由 for_model() 在探测后切换；命令生成与应答解析为 protected
 * static，子类直接复用。
 */
class UnicoreReceiver : public Receiver {
public:
    constexpr UnicoreReceiver() noexcept = default;
    // 本类既作型号家族基类（Um982Receiver）又被直接实例化为 constinit 只读单例：基类 Receiver 的
    // protected 非虚析构会挡住命名空间作用域的对象定义，故改为 public virtual——
    // 既满足 -Wnon-virtual-dtor，也能保证经 Receiver* 释放时不漏掉派生部分。
    // 驱动对象仍只在静态存储以 constinit 存活，从不 new/delete。
    virtual ~UnicoreReceiver() = default;

    UnicoreReceiver(const UnicoreReceiver &) = delete;
    UnicoreReceiver &operator=(const UnicoreReceiver &) = delete;
    UnicoreReceiver(UnicoreReceiver &&) = delete;
    UnicoreReceiver &operator=(UnicoreReceiver &&) = delete;

    [[nodiscard]] Family family() const noexcept override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::span<const demux::Decoder *const> decoders() const noexcept override;
    /** UM980/UM981 能力；不含 kHeading（双天线由 Um982Receiver 覆盖）。 */
    [[nodiscard]] uint32_t caps(Model model) const noexcept override;
    /** kUm982 → um982_receiver()，其余 → *this。 */
    [[nodiscard]] const Receiver &for_model(Model model) const noexcept override;
    [[nodiscard]] bool build_probe(CmdBuf &out) const noexcept override;
    [[nodiscard]] bool match_probe(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                   ProbeResult &out) const noexcept override;
    /** 按 rover_script() 的表生成第 index 个启用步骤。 */
    [[nodiscard]] bool rover_step(size_t index, Model model, const RoverConfig &cfg,
                                  ConfigStep &out) const noexcept override;
    [[nodiscard]] Ack classify_reply(const demux::Decoder &dec, std::span<const uint8_t> frame,
                                     std::span<const uint8_t> sent) const noexcept override;
    /** 基类默认（NMEA）+ #SATSINFOA / #HEADINGA（ADR-0012 §2.6）。 */
    [[nodiscard]] OutputMsg classify_frame(const demux::Decoder &dec,
                                           std::span<const uint8_t> frame) const noexcept override;
    /** 基类默认 + "#VERSIONA"/"$command" 专用标签。 */
    size_t frame_tag(const demux::Decoder &dec, std::span<const uint8_t> frame,
                     std::span<char> out) const noexcept override;
    /** 基类默认 + SATSINFOA/GST；caps(model) 含 kHeading 时另加 HEADINGA。 */
    [[nodiscard]] uint32_t output_mask(Model model) const noexcept override;
    /** "<name> COMn <period>" / "UNLOG COMn <name>"（ADR-0012 §2.4）；HEADINGA 需 caps(model) 含 kHeading。 */
    [[nodiscard]] fw::Status build_output(OutputMsg msg, uint32_t period_ms, Model model, const OutputPlan &plan,
                                          CmdBuf &out) const noexcept override;

protected:
    /** 配置脚本的一步：build 生成命令，enabled 为 false 时该步在此配置下跳过（索引顺延）。 */
    struct ScriptStep {
        bool (*build)(const RoverConfig &cfg, ConfigStep &out) noexcept;
        bool (*enabled)(const RoverConfig &cfg) noexcept;
    };

    /** 基础流动站脚本（UNLOG → MODE ROVER → [CONFIG 波特率]）；型号需要额外步骤时覆盖。 */
    [[nodiscard]] virtual std::span<const ScriptStep> rover_script() const noexcept;

    // ---- 基础脚本的步骤（rover_script() 内的函数局部表引用它们，子类脚本可复用） ----
    /** 第 0 步 "UNLOG COMn"：先关掉全部输出，再由输出阶段按 OutputPlan 重新开启。 */
    [[nodiscard]] static bool step_unlog_all(const RoverConfig &cfg, ConfigStep &out) noexcept;
    /** 第 1 步 "MODE ROVER"。 */
    [[nodiscard]] static bool step_mode_rover(const RoverConfig &cfg, ConfigStep &out) noexcept;
    /** 第 2 步 "CONFIG COMn <baud>"：发送后不等应答，主机随即切换波特率。 */
    [[nodiscard]] static bool step_port_baud(const RoverConfig &cfg, ConfigStep &out) noexcept;
    /** 步骤启用条件：总是。 */
    [[nodiscard]] static bool step_always(const RoverConfig &cfg) noexcept;
    /** 步骤启用条件：cfg.port_baud != 0。 */
    [[nodiscard]] static bool step_if_port_baud(const RoverConfig &cfg) noexcept;

    // ---- 命令库：只往 out 写一条完整命令（含 \r\n，先清空），失败返回 false ----
    [[nodiscard]] static bool cmd_unlog(CmdBuf &out, uint8_t port) noexcept; // "UNLOG COMn"
    [[nodiscard]] static bool cmd_mode_rover(CmdBuf &out) noexcept;          // "MODE ROVER"
    [[nodiscard]] static bool cmd_config_baud(CmdBuf &out, uint8_t port,
                                              uint32_t baud) noexcept; // "CONFIG COMn <baud>"
    [[nodiscard]] static bool cmd_log_msg(CmdBuf &out, std::string_view name, uint8_t port,
                                          uint32_t period_ms) noexcept; // "<name> COMn <period>"
    [[nodiscard]] static bool cmd_unlog_msg(CmdBuf &out, std::string_view name,
                                            uint8_t port) noexcept;    // "UNLOG COMn <name>"
    [[nodiscard]] static bool cmd_version_query(CmdBuf &out) noexcept; // "VERSIONA"

    // ---- 应答解析：每个函数只判一件事 ----
    [[nodiscard]] static bool resp_has_valid_tail(std::span<const uint8_t> frame) noexcept;
    [[nodiscard]] static std::optional<size_t> resp_find_tag(std::span<const uint8_t> frame) noexcept;
    [[nodiscard]] static bool resp_echo_matches(std::span<const uint8_t> frame, size_t tag_pos,
                                                std::span<const uint8_t> sent) noexcept;
    [[nodiscard]] static std::span<const uint8_t> resp_text(std::span<const uint8_t> frame, size_t tag_pos) noexcept;
};

/** UM982 双天线型号：在家族基础上增加 kHeading（HEADINGA 输出）。 */
class Um982Receiver final : public UnicoreReceiver {
public:
    constexpr Um982Receiver() noexcept = default;

    [[nodiscard]] std::string_view name() const noexcept override; // "unicore-um982"
    /** 家族能力 + kHeading。 */
    [[nodiscard]] uint32_t caps(Model model) const noexcept override;
    /** 型号驱动不再细分：返回 *this。 */
    [[nodiscard]] const Receiver &for_model(Model model) const noexcept override;
};

/** 驱动单例（.cpp 内 constinit 静态对象）。线程/ISR 安全：是（只读）。 */
[[nodiscard]] const Receiver &unicore_receiver() noexcept;
/** UM982 型号驱动单例；一般经 unicore_receiver().for_model(Model::kUm982) 取得。 */
[[nodiscard]] const Receiver &um982_receiver() noexcept;

} // namespace gnss
