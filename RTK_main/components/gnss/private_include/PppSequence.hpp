// 串行执行 Unicore PPP 与 0x06 截止角/星座配置并逐条核对回显。
// 由接收机所属任务独占，不阻塞，不分配，非 ISR。
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "fw/Result.hpp"
#include "gnss/Receiver.hpp"

namespace gnss {

/**
 * @brief 判断指定接收机型号是否支持 Unicore PPP（CONFIG PPP ENABLE E6-HAS）。
 * @param model: 探测得到的接收机型号。
 * @retval true UM980/UM981/UM982 支持 PPP。
 * @retval false CM680 及其他型号不支持 PPP（不下发 PPP 命令）。
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool supportsPpp(Model model) noexcept;

/**
 * @brief 判断指定接收机型号是否支持 0x06 截止角/星座开关脚本（MASK/UNMASK/CONFIG SBAS）。
 * @param model: 探测得到的接收机型号。
 * @retval true UM980/UM981/UM982 与兼容 Unicore 指令集的 CM680 支持。
 * @retval false 其他或未知型号不支持。
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] bool supportsSatMask(Model model) noexcept;

class PppSequence final {
public:
    static constexpr uint32_t kReplyMs = 1000U;

    /** @brief 开始脚本。 @param enabled: PPP 开关。 @note 调用方须独占接收机命令通道。 */
    void start(bool enabled) noexcept;

    /** @brief 查询是否有脚本在执行。 @retval true 仍在执行。 */
    [[nodiscard]] bool active() const noexcept
    {
        return !commands_.empty();
    }

    /**
     * @brief 推进脚本，最多发出一条命令。
     * @param nowMs: 当前单调时钟。
     * @param[out] tx: 非空时须转交 UART，视图有效至下一轮 poll。
     * @retval nullopt 等待 ACK 或正在发送。
     * @retval 结果 脚本已结束。
     */
    [[nodiscard]] std::optional<fw::Result> poll(uint32_t nowMs,
                                                 std::span<const uint8_t> &tx) noexcept;

    /**
     * @brief 记录当前命令的匹配应答。
     * @param receiver: Unicore 驱动。
     * @param decoder: 帧来源。
     * @param frame: 已校验的接收机帧。
     * @param nowMs: 接收时钟。
     */
    void onFrame(const Receiver &receiver, const demux::Decoder &decoder,
                 std::span<const uint8_t> frame, uint32_t nowMs) noexcept;

private:
    std::span<const std::string_view> commands_{};
    CmdBuf sent_{};
    uint32_t sent_ms_ = 0U;
    Ack ack_ = Ack::kNone;
};

/** Survey 0x06 截止角与星座开关下发参数（默认全零以保持全局宿主对象位于 .bss）。 */
struct SatMaskConfig {
    static constexpr uint8_t kMaxCutoffDeg = 90U;
    static constexpr uint8_t kDefaultCutoffDeg = 15U;

    uint8_t cutoffDeg = 0U;
    bool gps = false;
    bool glo = false;
    bool bds = false;
    bool gal = false;
    bool sbas = false;
};

/** 串行执行 Unicore 截止角与星座开关脚本并逐条核对回显。 */
class SatMaskSequence final {
public:
    static constexpr uint32_t kReplyMs = 1000U;
    static constexpr size_t kStepCount = 6U;

    /** @brief 开始 0x06 截止角/星座脚本。 @param cfg: 合法的截止角与星座配置。 @retval true 已启动。 @retval false 截止角越界。 */
    [[nodiscard]] bool start(const SatMaskConfig &cfg) noexcept;

    /** @brief 查询脚本是否正在执行。 @retval true 仍在执行。 */
    [[nodiscard]] bool active() const noexcept
    {
        return active_;
    }

    /**
     * @brief 推进脚本，最多发出一条命令。
     * @param nowMs: 当前单调时钟。
     * @param[out] tx: 非空时须转交 UART，视图有效至下一轮 poll。
     * @retval nullopt 等待 ACK 或正在发送。
     * @retval 结果 脚本已结束（kOk / kIoError / kTimeout / kNoSpace / kInvalidState）。
     */
    [[nodiscard]] std::optional<fw::Result> poll(uint32_t nowMs,
                                                 std::span<const uint8_t> &tx) noexcept;

    /**
     * @brief 记录当前命令的匹配应答。
     * @param receiver: Unicore 驱动。
     * @param decoder: 帧来源。
     * @param frame: 已校验的接收机帧。
     * @param nowMs: 接收时钟。
     */
    void onFrame(const Receiver &receiver, const demux::Decoder &decoder,
                 std::span<const uint8_t> frame, uint32_t nowMs) noexcept;

private:
    /** @brief 按当前步骤下标构造命令到 sent_。 @retval true 已生成命令。 */
    [[nodiscard]] bool buildCurrentStep() noexcept;

    SatMaskConfig cfg_{};
    size_t step_ = 0U;
    bool active_ = false;
    CmdBuf sent_{};
    uint32_t sent_ms_ = 0U;
    Ack ack_ = Ack::kNone;
};

} // namespace gnss
