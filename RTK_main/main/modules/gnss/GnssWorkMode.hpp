// GNSS 上层工作模式状态机（rover/base/survey-wait/survey/base-tx/fallback；ADR-0014 §2.4）。
// 线程模型：仅 GnssModule 所在的 kIo 任务单线程调用，非 ISR；不分配堆、不直接操作 UART 或 NVS。
#pragma once

#include <cstdint>

#include "def/GnssSubDef.hpp"
#include "gnss/Controller.hpp"
#include "modules/gnss/GnssBaseConfig.hpp"

namespace app {

/**
 * 工作模式状态机（位于 main，不在 components/gnss）：
 * 协调下层 gnss::Controller::requestMode()、GST 水平精度门限、survey RTCM 1005 校验、
 * svy_to_s 总超时回退以及 SAVECONFIG 触发决策。
 */
class GnssWorkMode {
public:
    constexpr GnssWorkMode() noexcept = default;

    /**
     * @brief 复位运行期状态机到 kRover 初始态（例如 gnss freset 后调用）。
     */
    void reset() noexcept;

    /**
     * @brief 收到控制器重探/复位事件时清理进行中的模式请求，并在非 rover 配置下允许重探后重配。
     */
    void onControllerReprobe() noexcept;

    /**
     * @brief 提交一次模式切换请求（非 kRunning 时覆盖保留最后一个待执行请求）。
     * @param mode: 目标工作模式（kRover 或 kBase；kSmartBase 忽略）。
     * @param saveOnSuccess: true 表示切到目标稳态后需持久化接收机配置（手动切换为 true，启动自动应用为 false）。
     */
    void requestMode(WorkModeKind mode, bool saveOnSuccess) noexcept;

    /**
     * @brief 在 GNSS RX 侧收到完整 RTCM3 帧时通知状态机（用于 kSurvey 确认 1005 已开始生成）。
     * @param msgId: 12-bit RTCM3 消息号。
     */
    void onRtcmFrame(uint16_t msgId) noexcept;

    /**
     * @brief 周期驱动工作模式迁移，必要时通过 ctrl.requestMode 下发驱动命令。
     * @param ctrl: 底层 GNSS 控制器。
     * @param cfg: 当前 gnss.base 配置。
     * @param port: ESP32 所接模块串口号。
     * @param navRateHz: 当前导航输出频率（Hz）。
     * @param useConfiguredPort: u-blox 是否跟随 port 选择 UART 键。
     * @param hAccCm: 最新 GST 水平精度估计（cm；kU16Unknown 表示未知）。
     * @param nowMs: 当前单调时间（ms）。
     * @param outNeedSave: 输出 true 表示需要请求接收机 SAVECONFIG/CFG-CFG 持久化。
     * @retval true: 状态快照较上次发布有实质变化（需刷新 DataCenter::gnssWorkMode）。
     * @retval false: 无对外可见变化。
     */
    [[nodiscard]] bool poll(::gnss::Controller &ctrl, const GnssBaseConfig &cfg, uint8_t port,
                            uint8_t navRateHz, bool useConfiguredPort, uint16_t hAccCm,
                            uint32_t nowMs, bool &outNeedSave) noexcept;

    /**
     * @brief 是否允许向 PHONE 转发 GNSS 输出的 RTCM3 基准站差分流。
     * @retval true: 当前处于 kBaseTx 稳态。
     * @retval false: 其他状态（survey 中/rover/回退中）。
     */
    [[nodiscard]] bool shouldForwardRtcm() const noexcept
    {
        return state_ == WorkModeState::kBaseTx;
    }

    /**
     * @brief 当前是否处于基准站相关流程（含配置中/自测中/发数中，用于屏蔽手机 RTCM 注入）。
     * @retval true: 非 kRover 状态（正在或已激活基站模式）。
     * @retval false: kRover 状态。
     */
    [[nodiscard]] bool isBaseActive() const noexcept
    {
        return state_ != WorkModeState::kRover;
    }

    /**
     * @brief 当前是否有正在等待控制器 ACK 的模式切换维护命令。
     * @retval true: 正在执行模式命令。
     */
    [[nodiscard]] bool inFlight() const noexcept
    {
        return waitingCtrlAck_;
    }

    /**
     * @brief 导出当前跨任务可读的工作模式状态快照。
     * @retval status: 包含 mode/baseType/state/lastErr/elapsedS/hAccCm/seq/active。
     */
    [[nodiscard]] GnssWorkModeStatus status() const noexcept;

private:
    /**
     * @brief 把 gnss::MaintenanceResult 映射为 WorkModeErr。
     * @param res: 控制器维护结果。
     * @retval err: 对应的模式错误枚举。
     */
    [[nodiscard]] static WorkModeErr mapMaintErr(::gnss::MaintenanceResult res) noexcept;

    /**
     * @brief 构造发往底层驱动的 ModeRequest。
     * @param action: 模式子动作。
     * @param cfg: 当前 gnss.base 配置。
     * @param port: ESP32 所接模块串口号。
     * @param navRateHz: 当前导航输出频率（Hz）。
     * @param useConfiguredPort: u-blox 是否跟随 port 选择 UART 键。
     * @retval req: 填充完成的驱动请求结构体。
     */
    [[nodiscard]] static ::gnss::ModeRequest makeReq(::gnss::ModeAction action,
                                                     const GnssBaseConfig &cfg, uint8_t port,
                                                     uint8_t navRateHz,
                                                     bool useConfiguredPort) noexcept;

    /**
     * @brief 发送一步控制器模式命令并处理同步返回。
     * @param ctrl: 底层 GNSS 控制器。
     * @param action: 待下发的模式动作。
     * @param cfg: 当前 gnss.base 配置。
     * @param port: ESP32 所接模块串口号。
     * @param navRateHz: 当前导航输出频率（Hz）。
     * @param useConfiguredPort: u-blox 是否跟随 port 选择 UART 键。
     */
    void startCtrlStep(::gnss::Controller &ctrl, ::gnss::ModeAction action,
                       const GnssBaseConfig &cfg, uint8_t port, uint8_t navRateHz,
                       bool useConfiguredPort) noexcept;

    /**
     * @brief 启动用户或启动阶段指定的模式切换请求。
     * @param ctrl: 底层 GNSS 控制器。
     * @param target: 目标工作模式。
     * @param saveOnSuccess: true 表示切到目标稳态后请求 SAVECONFIG。
     * @param cfg: 当前 gnss.base 配置。
     * @param port: ESP32 所接模块串口号。
     * @param navRateHz: 当前导航输出频率（Hz）。
     * @param useConfiguredPort: u-blox 是否跟随 port 选择 UART 键。
     * @param nowMs: 当前单调时间（ms）。
     */
    void beginRequest(::gnss::Controller &ctrl, WorkModeKind target, bool saveOnSuccess,
                      const GnssBaseConfig &cfg, uint8_t port, uint8_t navRateHz,
                      bool useConfiguredPort, uint32_t nowMs) noexcept;

    /**
     * @brief 处理底层 ModeAction 成功后进入的下一状态与保存请求。
     * @param nowMs: 当前单调时间（ms）。
     * @param cfg: 当前 gnss.base 配置。
     * @param outNeedSave: 若需持久保存接收机置为 true。
     */
    void handleCtrlStepOk(uint32_t nowMs, const GnssBaseConfig &cfg, bool &outNeedSave) noexcept;

    /**
     * @brief 处理底层 ModeAction 完成结果并驱动状态前进或回退。
     * @param ctrl: 底层 GNSS 控制器。
     * @param res: 控制器维护返回结果。
     * @param cfg: 当前 gnss.base 配置。
     * @param port: ESP32 所接模块串口号。
     * @param navRateHz: 当前导航输出频率（Hz）。
     * @param useConfiguredPort: u-blox 是否跟随 port 选择 UART 键。
     * @param nowMs: 当前单调时间（ms）。
     * @param outNeedSave: 若需持久保存接收机置为 true。
     */
    void onCtrlStepDone(::gnss::Controller &ctrl, ::gnss::MaintenanceResult res,
                        const GnssBaseConfig &cfg, uint8_t port, uint8_t navRateHz,
                        bool useConfiguredPort, uint32_t nowMs, bool &outNeedSave) noexcept;

    /**
     * @brief 推进 kSurveyWait 与 kSurvey 的定时与超时回退逻辑。
     * @param ctrl: 底层 GNSS 控制器。
     * @param cfg: 当前 gnss.base 配置。
     * @param port: ESP32 所接模块串口号。
     * @param navRateHz: 当前导航输出频率（Hz）。
     * @param useConfiguredPort: u-blox 是否跟随 port 选择 UART 键。
     * @param hAccCm: 最新 GST 水平精度估计（cm）。
     * @param nowMs: 当前单调时间（ms）。
     * @param outNeedSave: 若需持久保存接收机置为 true。
     */
    void stepSurvey(::gnss::Controller &ctrl, const GnssBaseConfig &cfg, uint8_t port,
                    uint8_t navRateHz, bool useConfiguredPort, uint16_t hAccCm, uint32_t nowMs,
                    bool &outNeedSave) noexcept;

    /**
     * @brief 触发安全回退：先切回 rover（如支持），记录 W 日志后回到 kRover。
     * @param ctrl: 底层 GNSS 控制器。
     * @param err: 导致回退的错误原因。
     * @param cfg: 当前 gnss.base 配置。
     * @param port: ESP32 所接模块串口号。
     * @param navRateHz: 当前导航输出频率（Hz）。
     * @param useConfiguredPort: u-blox 是否跟随 port 选择 UART 键。
     */
    void enterFallback(::gnss::Controller &ctrl, WorkModeErr err, const GnssBaseConfig &cfg,
                       uint8_t port, uint8_t navRateHz, bool useConfiguredPort) noexcept;

    /**
     * @brief 根据状态字与关键读数是否变化判断是否推进发布序号。
     * @param cfg: 当前 gnss.base 配置。
     * @param hAccCm: 最新 GST 水平精度估计（cm）。
     * @retval true: 快照发生变化，需刷新 DataCenter。
     * @retval false: 无变化。
     */
    [[nodiscard]] bool finalizeSnapshot(const GnssBaseConfig &cfg, uint16_t hAccCm) noexcept;

    WorkModeKind mode_ = WorkModeKind::kRover;
    BaseType baseType_ = BaseType::kSurvey;
    WorkModeState state_ = WorkModeState::kRover;
    WorkModeErr lastErr_ = WorkModeErr::kNone;

    bool pending_ = false;
    WorkModeKind pendingMode_ = WorkModeKind::kRover;
    bool pendingSave_ = false;
    bool saveOnSuccess_ = false;
    bool bootApplied_ = false;

    bool waitingCtrlAck_ = false;
    uint32_t expectedSeq_ = 0U;
    ::gnss::ModeAction inflightAction_ = ::gnss::ModeAction::kRover;
    bool fallbackPending_ = false;

    uint32_t reqStartMs_ = 0U;
    uint32_t surveyStartMs_ = 0U;
    bool rtcm1005Armed_ = false;
    bool seenRtcm1005_ = false;

    uint16_t elapsedS_ = 0U;
    uint16_t hAccCm_ = ::gnss::kU16Unknown;
    uint32_t seq_ = 0U;

    bool hasPublished_ = false;
    GnssWorkModeStatus lastPublished_{};
};

} // namespace app
