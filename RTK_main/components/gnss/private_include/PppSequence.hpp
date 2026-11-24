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

    /** @brief 查询下一轮 poll 是否准备向串口写入命令（供 RTCM 帧边界门控）。 @retval true 待发命令。 */
    [[nodiscard]] bool needsTx() const noexcept
    {
        return active() &&
               (sent_.view().empty() || ((ack_ == Ack::kAck) && (commands_.size() > 1U)));
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

/** 串行执行 Unicore（ASCII）或 ZED-F9P（UBX-CFG-VALSET）截止角与星座开关脚本。 */
class SatMaskSequence final {
public:
    static constexpr uint32_t kReplyMs = 1000U;
    static constexpr uint32_t kF9pSignalSettleMs = 600U;
    static constexpr uint32_t kF9pResyncMs = 300U;
    static constexpr size_t kStepCount = 6U;
    static constexpr size_t kF9pStepCount = 1U;

    /**
     * @brief 开始 0x06 截止角/星座脚本。
     * @param cfg: 合法的截止角与星座配置。
     * @param model: 当前接收机型号（默认 kUm980 保持既有调用兼容）。
     * @param includeSbas: F9P 是否在 VALSET 中携带 CFG-SIGNAL-SBAS_ENA（HPG >= 1.13）。
     * @retval true: 已启动脚本。
     * @retval false: 截止角越界、F9P 关闭 GPS 或型号不支持。
     */
    [[nodiscard]] bool start(const SatMaskConfig &cfg, Model model = Model::kUm980,
                             bool includeSbas = true) noexcept;

    /** @brief 查询脚本是否正在执行。 @retval true 仍在执行。 */
    [[nodiscard]] bool active() const noexcept
    {
        return phase_ != Phase::kIdle;
    }

    /** @brief 查询下一轮 poll 是否需要向串口发送命令帧（供 RTCM 帧边界门控）。 @retval true 待发命令。 */
    [[nodiscard]] bool needsTx() const noexcept
    {
        return (phase_ == Phase::kSend) || ((phase_ == Phase::kAwaitAck) && (ack_ == Ack::kAck) &&
                                            ((step_ + 1U) < totalSteps()));
    }

    /** @brief 查询是否处于 F9P CFG-SIGNAL 变更后的非阻塞稳定期。 @retval true 正在 600 ms 稳定等待。 */
    [[nodiscard]] bool settling() const noexcept
    {
        return phase_ == Phase::kSettle;
    }

    /** @brief 查询是否处于超时后的 300 ms 重同步期。 @retval true 正在丢弃迟到应答。 */
    [[nodiscard]] bool resyncing() const noexcept
    {
        return phase_ == Phase::kResync;
    }

    /**
     * @brief 返回最近一次已发送命令的只读视图（用于 NAK 诊断日志）。
     * @retval 已发送帧字节视图；空闲时为空。
     */
    [[nodiscard]] std::span<const uint8_t> lastSent() const noexcept
    {
        return sent_.view();
    }

    /**
     * @brief 返回当前型号对应的总步骤数。
     * @retval 1（ZED-F9P 单帧原子 VALSET）或 6（Unicore ASCII）。
     */
    [[nodiscard]] size_t totalSteps() const noexcept
    {
        return (model_ == Model::kZedF9p) ? kF9pStepCount : kStepCount;
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
    enum class Phase : uint8_t {
        kIdle = 0U,
        kSend,
        kAwaitAck,
        kSettle,
        kResync,
    };

    /** @brief 按当前步骤下标构造命令到 sent_。 @retval true 已生成命令。 */
    [[nodiscard]] bool buildCurrentStep() noexcept;

    /**
     * @brief 推进 kAwaitAck 阶段：等待应答、失败转重同步或推进到下一步/稳定期。
     * @param nowMs: 当前单调时钟。
     * @param[out] advanceToSend: 需立即构造并发送下一步时置 true。
     * @retval nullopt 仍在等待或转入发送/稳定/重同步；结果则序列已结束。
     */
    [[nodiscard]] std::optional<fw::Result> pollAwaitAck(uint32_t nowMs,
                                                         bool &advanceToSend) noexcept;

    SatMaskConfig cfg_{};
    Model model_ = Model::kUnknown;
    bool include_sbas_ = false;
    size_t step_ = 0U;
    Phase phase_ = Phase::kIdle;
    CmdBuf sent_{};
    uint32_t sent_ms_ = 0U;
    uint32_t phase_ms_ = 0U;
    fw::Result pending_result_ = fw::Result::kOk;
    Ack ack_ = Ack::kNone;
};

} // namespace gnss
