// OLED 表盘/自检/子菜单/关机进度 UI 模型、状态机、纯整数格式化与渲染接口（OL2）。
// 纯逻辑头，无 IDF/FreeRTOS 依赖；由 OLED 任务或主机测试单线程串行调用，不分配堆，非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "WouoUI_graph.h"
#include "app/RadioMode.hpp"
#include "board/PowerLogic.hpp"
#include "def/GnssSubDef.hpp"
#include "def/OledDef.hpp"
#include "def/OtaDef.hpp"
#include "gnss/Controller.hpp"
#include "gnss/Types.hpp"
#include "modules/oled/OledAnim.hpp"
#include "modules/oled/OledMarquee.hpp"
#include "modules/oled/OledPerf.hpp"

namespace app::oled {

inline constexpr size_t kWidth = 128U;
inline constexpr size_t kHeight = 64U;
inline constexpr size_t kPages = 8U;
inline constexpr size_t kMaxLineChars = 21U;                 // 128 px / 6 px (Font_6_8) = 21 字符
inline constexpr size_t kProductBufBytes = 16U;              // B，产品短名缓冲
inline constexpr size_t kBoardNameBufBytes = 24U;            // B，板型显示名缓冲
inline constexpr size_t kHwVerBufBytes = 16U;                // B，硬件版本缓冲
inline constexpr size_t kFwVerBufBytes = 33U;                // B，固件版本缓冲
inline constexpr size_t kSnBufBytes = 24U;                   // B，序列号缓冲
inline constexpr size_t kDevNameBufBytes = 32U;              // B，蓝牙/Wi-Fi 广播名缓冲
inline constexpr size_t kGnssFwBufBytes = gnss::kVersionLen; // B，GNSS 固件版本短串缓冲
inline constexpr size_t kPeerLabelBufBytes = 16U;            // B，已连蓝牙设备名/地址短串缓冲
inline constexpr size_t kCheckItemCount = 5U;                // 自检项数：PWR/NVS/RF/CHG/GNSS
inline constexpr size_t kDockItemCount = 5U;                 // SAT/POS/COMM/DEV/PWR
inline constexpr int16_t kDockStartX = 2;
inline constexpr int16_t kDockStepX = 25;
inline constexpr int16_t kDockY = 45;
inline constexpr int16_t kDockW = 24;
inline constexpr int16_t kDockH = 19;
inline constexpr size_t kConstellationSlots = 4U; // 卫星子菜单主星座数：GPS/BDS/GLO/GAL
inline constexpr size_t kTopSnrBars = 8U;         // 卫星子菜单 CN0 条数量
inline constexpr uint8_t kSubVisibleRows = 3U;    // 统一子页右侧同时可见行数
inline constexpr int16_t kSubRowStepY = 17;       // px，统一子页行步距（16 px 高 + 1 px 间距）
inline constexpr int16_t kModalOptionStepX = 56;  // px，安全项到确认项的水平距离
inline constexpr uint8_t kSubCommWifiRows = 4U;
inline constexpr uint8_t kSubDeviceRows = 6U;
inline constexpr size_t kPnBufBytes = 17U;
inline constexpr uint32_t kPowerOffConfirmMs = 5000U;     // ms，关机确认页倒计时上限
inline constexpr uint32_t kSubpageIdleTimeoutMs = 60000U; // ms，子页面无操作自动回表盘时长
inline constexpr uint32_t kCheckTickAnimMs = 150U;        // ms，开机自检单项打勾展开动画时长

/** 单星座可见/参与解算卫星数摘要（避免在 ViewModel 中复制 512 B SatTable）。 */
struct SatGroupStat {
    uint8_t visible = 0U;
    uint8_t used = 0U;
};

/** 单颗高载噪比卫星摘要（用于 CN0 柱状图）。 */
struct SatSnrItem {
    char sysLetter = ' '; // G/C/R/E/O
    uint8_t prn = 0U;
    uint8_t snrDbhz = 0U;
    bool used = false;
};

/** 从 DataCenter / AppContext 只读快照拼装的纯值视图模型。 */
struct ViewModel {
    std::array<char, kProductBufBytes> product{};
    std::array<char, kBoardNameBufBytes> boardName{};
    std::array<char, kHwVerBufBytes> hwVer{};
    std::array<char, kFwVerBufBytes> fwVer{};
    std::array<char, kSnBufBytes> sn{};
    std::array<char, kPnBufBytes> pn{};
    std::array<char, kDevNameBufBytes> deviceName{};
    std::array<char, kDevNameBufBytes + 1U> wifiSsid{};
    std::array<uint8_t, 4U> wifiIpv4{};
    std::array<char, kGnssFwBufBytes> gnssFwVer{};
    std::array<char, kPeerLabelBufBytes> btPeerLabel{};
    uint32_t uptimeMs = 0U;
    board::BootMode bootMode = board::BootMode::kNormal;
    RadioMode radioMode = RadioMode::kBluetooth;
    bool nvsReady = false;
    bool btReady = false;
    bool wifiReady = false;
    bool wifiSupported = false;
    uint8_t wifiClients = 0U;
    OledPairingStatus pairing{};
    uint8_t btSessions = 0U;
    bool powerActive = false;
    bool batteryAdcReady = false;
    bool chargerPresent = false;
    bool chargerOk = false;
    bool gnssEnabled = false;
    gnss::State gnssState = gnss::State::kOff;
    gnss::Model gnssModel = gnss::Model::kUnknown;
    bool fixValid = false;
    gnss::FixQuality fixQuality = gnss::FixQuality::kUnknown;
    bool utcValid = false;
    uint8_t utcHour = 0U;
    uint8_t utcMinute = 0U;
    uint8_t utcSecond = 0U;
    int64_t latE9 = 0;
    int64_t lonE9 = 0;
    int32_t altMm = 0;
    uint8_t satsUsed = 0U;
    uint8_t satsVisible = 0U;
    uint16_t hdopX100 = gnss::kU16Unknown;
    uint16_t diffAgeDs = gnss::kU16Unknown;
    uint16_t hAccCm = gnss::kU16Unknown;
    bool workModeActive = false;
    WorkModeKind workMode = WorkModeKind::kRover;
    WorkModeState workModeState = WorkModeState::kRover;
    uint16_t workModeElapsedS = 0U;
    std::array<SatGroupStat, kConstellationSlots> satGroups{}; // 0:GPS 1:BDS 2:GLO 3:GAL
    std::array<SatSnrItem, kTopSnrBars> topSnr{};
    uint8_t topSnrCount = 0U;
    uint8_t batteryPct = board::kBatteryPctUnknown;
    uint32_t batterySimSession = 0U; // power simbat 会话号，变化时电量格立即重新锁定
    uint32_t batteryMv = 0U;
    bool usbPresent = false;
    bool charging = false;
    board::UsbSource usbSource = board::UsbSource::kNone;
    board::ShutdownStage shutdownStage = board::ShutdownStage::kIdle;
    board::ShutdownReason shutdownReason = board::ShutdownReason::kNone;
    uint8_t shutdownDoneMask = 0U;
    uint8_t shutdownDegradedMask = 0U;
    uint8_t shutdownDoneCount = 0U;
    uint8_t shutdownTotalCount = 0U;
    web_ota::OtaProgress otaProgress{};
    KeyHoldSnapshot keyHold{};
    RebootBanner rebootBanner = RebootBanner::kNone;
};

/** 开机自检单项结论。 */
enum class CheckStatus : uint8_t {
    kPending = 0,
    kOk,
    kFail,
    kAbsent, // 可选硬件缺失（如无充电芯片），显示 "-"，不计作失败告警
};

/** 开机自检页面内部阶段。 */
enum class CheckPhase : uint8_t {
    kLogo = 0,
    kChecking,
    kSummary,
    kDone,
};

/**
 * 开机自检状态机（Logo 展开 -> 5 项非阻塞真实自检 -> 滑入表盘）。
 * 绝不阻塞模块初始化，失败项只记告警标志并在进入表盘后于状态栏展示告警图标。
 */
class SystemCheckSm final {
public:
    static constexpr uint32_t kLogoRevealMs = 450U;    // ms，单色 Logo 逐列展开时长
    static constexpr uint32_t kLogoHoldMs = 200U;      // ms，Logo 满显停留时长
    static constexpr uint32_t kMinStepMs = 200U;       // ms，每项自检最短可读展示（<= 300 ms）
    static constexpr uint32_t kResultHoldMs = 100U;    // ms，得出 ✓/✗ 后的最短停留
    static constexpr uint32_t kSummaryHoldMs = 200U;   // ms，全部完成后摘要停留
    static constexpr uint32_t kGnssWaitCapMs = 15000U; // ms，防 GNSS 永久 kOff 的 UI 兜底上限

    constexpr SystemCheckSm() noexcept = default;

    /**
     * @brief 复位开机自检状态机至 Logo 展开阶段
     * @param nowMs: 当前单调时间（ms）
     * @note 仅启动初始化时调用，不阻塞。
     */
    void reset(uint32_t nowMs) noexcept;

    /**
     * @brief 跳过剩余自检展示并结算当前已知告警状态
     * @param vm: 当前只读快照
     * @note 用户双击跳过时调用，不阻塞。
     */
    void skip(const ViewModel &vm) noexcept;

    /**
     * @brief 按当前只读快照推进自检一步（非阻塞）
     * @param nowMs: 当前单调时间（ms）
     * @param vm: 当前只读视图模型
     * @retval true 本轮刚进入 kDone（调用方可启动滑入表盘过渡）
     * @retval false 仍在进行或此前已完成
     * @note 纯确定性推进，不访问硬件，支持 uint32_t 回绕。
     */
    [[nodiscard]] bool step(uint32_t nowMs, const ViewModel &vm) noexcept;

    /**
     * @brief 查询自检当前所处大阶段
     * @retval 当前阶段枚举
     * @note 纯查询。
     */
    [[nodiscard]] CheckPhase phase() const noexcept
    {
        return phase_;
    }

    /**
     * @brief 查询当前正在推进的自检项下标（0..4）
     * @retval 当前项下标
     * @note 纯查询。
     */
    [[nodiscard]] size_t currentIndex() const noexcept
    {
        return activeIdx_;
    }

    /**
     * @brief 查询指定自检项状态
     * @param idx: 项下标（0..4）
     * @retval 该项当前状态
     * @note 纯查询。
     */
    [[nodiscard]] CheckStatus itemStatus(size_t idx) const noexcept
    {
        return (idx < kCheckItemCount) ? items_[idx] : CheckStatus::kFail;
    }

    /**
     * @brief 查询是否至少有一项自检失败（kAbsent 不算失败）
     * @retval true 存在失败项，表盘状态栏应显示告警图标
     * @retval false 全部正常或仅可选外设缺失
     * @note 纯查询。
     */
    [[nodiscard]] bool hasWarning() const noexcept
    {
        return hasWarning_;
    }

    /**
     * @brief 计算 Logo 阶段单色展开进度（0..1024）
     * @param nowMs: 当前单调时间（ms）
     * @retval Q10 展开进度
     * @note 纯查询，无浮点。
     */
    [[nodiscard]] uint16_t logoRevealQ10(uint32_t nowMs) const noexcept;

    /**
     * @brief 查询当前步骤已等待时长（ms，用于 Pending 旋转帧与超 1 s 计时显示）
     * @param nowMs: 当前单调时间（ms）
     * @retval 当前步骤等待毫秒数
     * @note 纯查询。
     */
    [[nodiscard]] uint32_t stepElapsedMs(uint32_t nowMs) const noexcept
    {
        return nowMs - stepStartMs_;
    }

    /**
     * @brief 查询指定自检项得出结论的单调时刻（用于逐项打勾展开动画）
     * @param idx: 自检项下标（0..4）
     * @retval 该项得出结论的单调时间（ms）
     * @note 纯查询。
     */
    [[nodiscard]] uint32_t itemResolvedMs(size_t idx) const noexcept
    {
        return (idx < kCheckItemCount) ? itemResolvedMs_[idx] : 0U;
    }

    /**
     * @brief 查询自检页当前是否需要 30 fps 动画刷新
     * @retval true Logo 展开或 Pending 旋转动画期间
     * @retval false 已完成
     * @note 纯查询。
     */
    [[nodiscard]] bool animating() const noexcept
    {
        return phase_ != CheckPhase::kDone;
    }

private:
    /**
     * @brief 按只读快照评估单个自检项的真实状态
     * @param idx: 自检项下标（0:PWR 1:NVS 2:RF 3:CHG 4:GNSS）
     * @param vm: 只读视图模型
     * @param elapsedMs: 该项已等待毫秒数
     * @retval 该项实时判定结论
     * @note 纯函数，不阻塞、不访问驱动。
     */
    [[nodiscard]] static CheckStatus evaluateItem(size_t idx, const ViewModel &vm,
                                                  uint32_t elapsedMs) noexcept;

    CheckPhase phase_ = CheckPhase::kLogo;
    std::array<CheckStatus, kCheckItemCount> items_{};
    std::array<uint32_t, kCheckItemCount> itemResolvedMs_{};
    size_t activeIdx_ = 0U;
    uint32_t phaseStartMs_ = 0U;
    uint32_t stepStartMs_ = 0U;
    uint32_t stepResolvedMs_ = 0U;
    bool stepResolved_ = false;
    bool hasWarning_ = false;
};

/** 刷屏后底层 I2C 传输结论（纯逻辑镜像，避免 OledUi 依赖硬件驱动头）。 */
enum class FlushOutcome : uint8_t {
    kDisabled = 0, // 面板离线或已关闭
    kWritten,      // 脏页全部写出成功
    kSkipped,      // 0 脏页（当前帧已完整落屏）
    kBackoff,      // 处于连续错误退避期，未确认落屏
    kInterrupted,  // 帧截止已到，还有未发送的脏页
    kError,        // 写出失败
};

/** 刷屏后调度结论：是否允许置位 USB 关机终帧完成标志及下一轮精确等待时长。 */
struct PostFlushDecision {
    bool markFinalFlushed = false;
    uint32_t nextWaitMs = kWaitForEvent;
};

inline constexpr uint32_t kShutdownFinalRetryMs =
    2U; // ms，USB 终帧 I2C 失败或退避后在 30 ms 窗口内补刷间隔

/** UiController 单步调度与控制输出。 */
struct UiDecision {
    OledPage page = OledPage::kSystemCheck;
    bool markShutdownFinalFrame = false; // USB 关机终帧待刷出 I2C，刷出成功后通知 PowerModule
    bool markRebootFinalFrame = false;   // 软件重启过渡帧待刷出并静默 I2C
    bool animating = false;              // 当前处于动画帧调度（33 ms）
    bool hasClickDeadline = false;       // 存在待判双击窗口绝对截止
    uint32_t clickDeadlineMs = 0U;       // 待判双击窗口绝对截止时刻（ms）
    uint32_t nextWaitMs = kWaitForEvent;
    OledActionRequest action{};
};

enum class UiModal : uint8_t { kNone, kWifiConfirm, kPair, kPowerOffConfirm };

/** 单帧缓冲内已绘制的弹窗内容，供局部刷新复用。 */
struct ModalFrame {
    UiModal modal = UiModal::kNone;
    OledPairingStatus pairing{};
};
/** 供渲染器只读读取的完整 UI 瞬时渲染状态。 */
struct UiRenderState {
    OledPage page = OledPage::kSystemCheck;
    uint8_t dockSelected = 0U;
    int16_t dockFocusX = 2;
    uint8_t satSubTab = 0U;
    bool hasCheckWarning = false;
    uint8_t shutdownPct = 0U;
    uint8_t subSelected = 0U;
    int16_t subFocusY = 0;
    int16_t subScrollY = 0;
    int16_t squishInset = 0;
    bool holdOverlayVisible = false;
    bool holdOverlayCollapsing = false;
    uint16_t holdProgressQ10 = 0U;
    uint16_t holdCollapseQ10 = 0U;
    uint32_t powerOffRemainMs = 0U;
    uint16_t standbyRingQ10 = 0U;
    const char *heroPrevBadge = nullptr;
    uint16_t heroRollQ10 = kEaseScale;
    UiModal modal = UiModal::kNone;
    int16_t modalFocusX = 0;
    RebootBanner rebootBanner = RebootBanner::kNone;
    uint32_t nowMs = 0U;
    const SystemCheckSm *checkSm = nullptr;
    const PageTransition *transition = nullptr;
    const NumberRoll *satsRoll = nullptr;
    Marquee *marquee = nullptr;       // 可选，持有本次画面各字段的滚动状态。
    board::BatteryBarsView batBars{}; // 电量格结论（0 格 = 未知，OL7）
};

/**
 * OLED 主界面控制器（按键消歧、自检、表盘/子菜单导航、关机进度、动画与帧率调度）。
 * 全零默认构造落 .dram0.bss，占用约 120 B 静态内存，零堆分配、零浮点。
 */
class UiController final {
public:
    constexpr UiController() noexcept = default;

    /**
     * @brief 按启动运行档复位 UI 控制器
     * @param bootMode: 正常启动或充电待机
     * @param nowMs: 当前单调时间（ms）
     * @note 仅启动初始化时调用，不阻塞。
     */
    void reset(board::BootMode bootMode, uint32_t nowMs) noexcept;

    /**
     * @brief 推进一轮 UI 状态机、按生产时间戳消歧双击、动画物理与帧周期调度
     * @param nowMs: 当前单调时间（ms）
     * @param shortKeyTimes: 本轮自 UiEventBus 弹出的消抖短按生产时间戳序列（按生产顺序）
     * @param vm: 当前只读视图模型
     * @param longKeyArrived: 本轮是否收到长按 2.5 s 到达事件（触发关机确认页）
     * @retval 本轮渲染页、USB 终帧通知、双击绝对截止与下一帧精确等待时长
     * @note 仅 OLED 任务或主机测试串行调用，不阻塞、不分配堆。
     */
    [[nodiscard]] UiDecision step(uint32_t nowMs, std::span<const uint32_t> shortKeyTimes,
                                  const ViewModel &vm, bool longKeyArrived = false) noexcept;
    /**
     * @brief 构造供渲染器消费的只读渲染状态视图
     * @param nowMs: 当前单调时间（ms）
     * @retval 只读渲染状态
     * @note 纯查询，不阻塞。
     */
    [[nodiscard]] UiRenderState renderState(uint32_t nowMs) const noexcept;

    /**
     * @brief 查询当前逻辑页面
     * @retval 当前页面枚举
     * @note 纯查询。
     */
    [[nodiscard]] OledPage page() const noexcept
    {
        return otaVisible_ ? OledPage::kOta : page_;
    }

    /**
     * @brief 查询当前表盘底部选中图标索引（0..3）
     * @retval 选中图标下标
     * @note 纯查询。
     */
    [[nodiscard]] uint8_t dockSelected() const noexcept
    {
        return dockSelected_;
    }

    /**
     * @brief 查询当前子页选中列表行下标
     * @retval 列表行下标（0 起）
     * @note 纯查询。
     */
    [[nodiscard]] uint8_t subSelected() const noexcept
    {
        return subSelected_;
    }

    /**
     * @brief 查询开机自检是否留下了失败告警标志
     * @retval true 状态栏应显示告警图标
     * @retval false 无告警
     * @note 纯查询。
     */
    [[nodiscard]] bool hasWarning() const noexcept
    {
        return hasWarning_;
    }

private:
    /**
     * @brief 显示当前上传页面、关闭被覆盖的弹窗。
     * @param nowMs: 当前单调毫秒。
     * @param vm: 本帧快照。
     * @retval OTA 覆盖当前页的决定；当前没有 OTA 提示时返回空值。
     * @note 仅 OLED 任务。
     */
    [[nodiscard]] std::optional<UiDecision> stepOta(uint32_t nowMs, const ViewModel &vm) noexcept;

    /** @brief 关闭被全屏提示覆盖的弹窗与交互。 @note 仅 OLED 任务。 */
    void dismissOverlays() noexcept;

    /**
     * @brief 处理非空闲关机阶段的进度推进与 USB 终帧判定（无 USB 不再主动关屏）
     * @param nowMs: 当前单调时间（ms）
     * @param vm: 只读视图模型
     * @retval 本轮关机页决策
     * @note 仅 step() 内部调用。
     */
    [[nodiscard]] UiDecision stepShutdown(uint32_t nowMs, const ViewModel &vm) noexcept;

    /**
     * @brief 从关机态退出（30 s 超时回退）时恢复页面
     * @retval 本轮恢复决策
     * @note 仅 step() 内部调用。
     */
    [[nodiscard]] UiDecision recoverFromShutdown() noexcept;

    /**
     * @brief 推进自检与动画物理并计算下一帧等待时长
     * @param nowMs: 当前单调时间（ms）
     * @param dtMs: 距上一轮步进间隔（ms）
     * @param vm: 只读视图模型
     * @retval 本轮渲染页与帧周期决策
     * @note 仅 step() 内部调用。
     */
    [[nodiscard]] UiDecision finishActiveStep(uint32_t nowMs, uint32_t dtMs,
                                              const ViewModel &vm) noexcept;
    /**
     * @brief 推进充电待机页的进度环、电池滚动与 <= 2 fps 充电动画调度
     * @param nowMs: 当前单调时间（ms）
     * @param dtMs: 距上一轮步进经过的毫秒数
     * @param vm: 只读视图模型
     * @retval 本轮充电待机页决策
     * @note 仅 step() 内部调用。
     */
    [[nodiscard]] UiDecision stepChargeStandby(uint32_t nowMs, uint32_t dtMs,
                                               const ViewModel &vm) noexcept;

    /**
     * @brief 更新正常模式长按进度浮层（600..2500 ms）与提前松开 150 ms 收起状态
     * @param nowMs: 当前单调时间（ms）
     * @param vm: 只读视图模型
     * @param longKeyArrived: 本轮是否收到长按到达事件
     * @note 仅 step() 内部调用。
     */
    void updateHoldOverlay(uint32_t nowMs, const ViewModel &vm, bool longKeyArrived) noexcept;

    /**
     * @brief 进入关机确认弹窗（POWER OFF?，5 s 自动取消，优先级高于配对弹窗）
     * @param nowMs: 当前单调时间（ms）
     * @note 仅 step() 内部调用。
     */
    void enterPowerOffConfirm(uint32_t nowMs) noexcept;

    /**
     * @brief 返回表盘并启动与进入对应的收回过渡
     * @param nowMs: 当前时间（ms）
     */
    void leaveSubmenu(uint32_t nowMs) noexcept;

    /**
     * @brief 消费一个已判定的单击或双击事件（按优先级：关机确认 -> 配对 -> Wi-Fi 确认 -> 页面）
     * @param click: 点击类型
     * @param nowMs: 当前单调时间（ms）
     * @param vm: 只读视图数据
     * @retval 要交给 kLink 的动作或空值
     */
    [[nodiscard]] OledActionRequest dispatchClick(ClickEvent click, uint32_t nowMs,
                                                  const ViewModel &vm) noexcept;

    /**
     * @brief 同步配对请求与绝对截止，关机确认期间保持排队。
     * @param nowMs: 当前单调时间。
     * @param vm: 本轮视图数据。
     * @retval true 发生弹窗上下文变更。
     */
    [[nodiscard]] bool updatePairPrompt(uint32_t nowMs, const ViewModel &vm) noexcept;

    /** @brief 查询当前可见弹窗。 @retval 按关机、配对、Wi-Fi 优先排列的可见类型。 */
    [[nodiscard]] UiModal visibleModal() const noexcept;

    /** @brief 弹窗焦点复位到安全项，清除上一弹窗的挤压反馈。 */
    void resetModalFocus() noexcept;

    /**
     * @brief 单击移动焦点，双击执行该弹窗当前选中的选项。
     * @param modal: 当前可见确认弹窗。
     * @param click: 已判定的点击类型。
     * @param nowMs: 当前单调时间（ms）。
     * @retval 交给 kLink 的动作，或直接关闭时的空动作。
     */
    [[nodiscard]] OledActionRequest handleModalClick(UiModal modal, ClickEvent click,
                                                     uint32_t nowMs) noexcept;

    /**
     * @brief 消费表盘页的单击切入口与双击展开子页
     * @param click: 已判定的点击类型
     * @param nowMs: 当前单调时间（ms）
     */
    void handleDialClick(ClickEvent click, uint32_t nowMs) noexcept;

    /**
     * @brief 更新表盘中部解状态徽章变化触发的 180 ms 竖向切换状态
     * @param nowMs: 当前单调时间（ms）
     * @param vm: 视图数据
     */
    void updateHeroBadgeRoll(uint32_t nowMs, const ViewModel &vm) noexcept;

    /**
     * @brief 依据活跃动画、长按 600 ms 起点与双击截止计算本轮精确休眠时长
     * @param nowMs: 当前单调时间（ms）
     * @param vm: 视图数据
     * @param animating: 当前是否处于 30 fps 动画态
     * @retval 完整的本轮 UiDecision
     */
    [[nodiscard]] UiDecision buildActiveDecision(uint32_t nowMs, const ViewModel &vm,
                                                 bool animating) const noexcept;

    /**
     * @brief 电量格状态机需要的下一次唤醒等待时长（充电填充/低电闪烁/迟滞计时）
     * @param nowMs: 当前单调时间（ms）
     * @retval 等待毫秒，无需唤醒时为 kWaitForEvent
     */
    [[nodiscard]] uint32_t barsWaitMs(uint32_t nowMs) const noexcept;

    /**
     * @brief 处理统一可滚动列表子页（位置/通信/设备/电源）的单击滚动与双击确认
     * @param click: 点击类型
     * @param nowMs: 当前单调时间（ms）
     * @param vm: 只读视图数据
     * @retval 要交给 kLink 的动作或空值
     */
    [[nodiscard]] OledActionRequest handleSubListClick(ClickEvent click, uint32_t nowMs,
                                                       const ViewModel &vm) noexcept;

    /**
     * @brief 对当前弹窗和页面消费按键，先处理已入队的生产时间戳
     * @param nowMs: 当前单调时间（ms）
     * @param times: 待处理的短按时间戳
     * @param vm: 只读视图数据
     * @retval 本轮提交的动作
     */
    [[nodiscard]] OledActionRequest handleInput(uint32_t nowMs, std::span<const uint32_t> times,
                                                const ViewModel &vm) noexcept;

    /**
     * @brief 取得指定统一列表子页的总行数（含末尾 Back 项）
     * @param page: 当前页面枚举
     * @param vm: 视图模型
     * @retval 列表总行数，非列表页返回 0
     * @note 纯函数。
     */
    [[nodiscard]] static uint8_t subpageRowCount(OledPage page, const ViewModel &vm) noexcept;

    /**
     * @brief 计算表盘底部指定入口图标的屏幕矩形
     * @param idx: 入口下标（0..4）
     * @retval 对应图标槽位矩形
     * @note 纯函数。
     */
    [[nodiscard]] static Rect dockSlotRect(uint8_t idx) noexcept;

    /**
     * @brief 将表盘入口下标映射为目标子菜单枚举
     * @param idx: 入口下标（0..4）
     * @retval 对应子菜单页面
     * @note 纯函数。
     */
    [[nodiscard]] static OledPage dockTargetPage(uint8_t idx) noexcept;

    board::BootMode bootMode_ = board::BootMode::kNormal;
    OledPage page_ = OledPage::kSystemCheck;
    uint8_t dockSelected_ = 0U;
    uint8_t satSubTab_ = 0U;
    uint8_t shutdownPct_ = 0U;
    uint32_t lastStepMs_ = 0U;
    uint32_t lastActivityMs_ = 0U;
    bool inShutdown_ = false;
    bool otaVisible_ = false;
    bool hasWarning_ = false;
    uint8_t subSelected_ = 0U;
    uint8_t subScrollTop_ = 0U;
    bool wifiConfirm_ = false;
    bool powerOffConfirm_ = false;
    uint32_t powerOffConfirmStartMs_ = 0U;
    bool pairDecisionSent_ = false;
    uint32_t pairDialogId_ = 0U;
    uint32_t suppressedPairId_ = 0U;
    bool holdOverlayActive_ = false;
    bool holdOverlayCollapsing_ = false;
    bool holdOverlayDismissed_ = false;
    uint16_t holdProgressQ10_ = 0U;
    uint32_t holdCollapseStartMs_ = 0U;
    uint16_t standbyRingQ10_ = 0U;
    const char *heroPrevBadge_ = nullptr;
    const char *heroCurrBadge_ = nullptr;
    uint32_t heroChangeStartMs_ = 0U;
    bool heroRolling_ = false;
    RebootBanner rebootBanner_ = RebootBanner::kNone;
    ClickDetector click_{};
    SystemCheckSm checkSm_{};
    PageTransition transition_{};
    FixedSpring dockSpring_{};
    FixedSpring subFocusSpring_{};
    FixedSpring subScrollSpring_{};
    FixedSpring modalFocusSpring_{};
    SquishAnim squish_{};
    NumberRoll satsRoll_{};
    board::BatteryBars bars_{}; // 电量格迟滞/动画状态机（OL7，约 40 B）
};

/** 每页用 16 列一组的摘要定位脏列，在唯一帧缓冲上找出覆盖所有变化的区间。 */
class FrameTracker final {
public:
    static constexpr size_t kBlockWidth = 16U;
    static_assert(kWidth % kBlockWidth == 0U);
    using PageHashes = std::array<uint32_t, kWidth / kBlockWidth>;

    /** 页内半开区间 [first, end)，空区间表示整页已在屏上。 */
    struct Columns {
        size_t first = 0U;
        size_t end = 0U;
    };

    /** @brief 清除全部已提交摘要，下一次必须完整重写。 */
    void invalidate() noexcept;

    /** @brief 使失败页失效。 @param page: 页号（0..7）。 */
    void invalidatePage(uint8_t page) noexcept;

    /**
     * @brief 比较一页的列组摘要，返回覆盖首末变化列组的范围。
     * @param page: 页号（0..7）。
     * @param data: 本页 128 列数据。
     * @param[out] hashes: 本页当前摘要。
     * @param forceFull: 是否强制整页。
     * @retval 需要送出的列范围。
     */
    [[nodiscard]] Columns dirtyColumns(uint8_t page, std::span<const uint8_t, kWidth> data,
                                       PageHashes &hashes, bool forceFull = false) const noexcept;

    /**
     * @brief 仅在整段发送成功后提交该页摘要。
     * @param page: 页号（0..7）。
     * @param hashes: 本页已发送或未变化的列组摘要。
     */
    void commitPage(uint8_t page, const PageHashes &hashes) noexcept;

private:
    std::array<PageHashes, kPages> committedHashes_{};
    uint8_t validMask_ = 0U;
};

/**
 * @brief 将文本安全拷贝并截断到定长字符缓冲，非可打印 ASCII 字符替换为 '?'
 * @param src: 源字符串视图
 * @param[out] dst: 目标 NUL 结尾缓冲
 * @note 纯函数，防止 WouoUI 字体查表越界，不分配堆。
 */
void sanitizeCopy(std::string_view src, std::span<char> dst) noexcept;

/**
 * @brief 格式化标签+值行（如 `"SN  : P2001"`），超长按缓冲容量安全截断
 * @param label: 固定前缀标签
 * @param value: 动态值文本，空串回落为 `"--"`
 * @param[out] out: 输出 NUL 结尾行缓冲
 * @note 纯函数，不分配堆。
 */
void formatLabelValue(std::string_view label, std::string_view value, std::span<char> out) noexcept;

/**
 * @brief 格式化电池电压文本（如 `"4.05V"`，未知或 0 时 `"--.-V"`）
 * @param mv: 电池电压（mV）
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯整数定点运算，不使用浮点。
 */
void formatBatteryVoltage(uint32_t mv, std::span<char> out) noexcept;

/**
 * @brief 判断当前是否应按“充电中”态渲染（有充电芯片看 IC 状态，无充电芯片按 USB 在位且未满电）
 * @param vm: 视图模型
 * @retval true 应显示充电闪电与充电流动动画
 * @retval false 非充电态（满电或仅电池）
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr bool isEffectiveCharging(const ViewModel &vm) noexcept
{
    constexpr uint8_t kFullBatteryPct = 100U;
    const bool full =
        (vm.batteryPct != board::kBatteryPctUnknown) && (vm.batteryPct >= kFullBatteryPct);
    return vm.charging || (!vm.chargerPresent && vm.usbPresent && !full);
}

/**
 * @brief 格式化水平精度（仅 fixValid 且已知时输出；>=99.99 m 显示 ">99m"，无定位返回 false）
 * @param fixValid: 当前定位解是否有效
 * @param hAccCm: 水平精度估计（cm，未知为 gnss::kU16Unknown）
 * @param[out] out: 输出 NUL 结尾缓冲
 * @retval true 已写入有效精度文本
 * @retval false 无定位或精度未知（out 置空串）
 * @note 纯整数运算，不分配堆。
 */
[[nodiscard]] bool formatHorizontalAcc(bool fixValid, uint16_t hAccCm,
                                       std::span<char> out) noexcept;

/**
 * @brief 格式化表盘右侧双行摘要（供渲染器与主机测试共用，确保无定位不显示饱和精度）
 * @param vm: 视图数据
 * @param[out] topOut: 顶行输出缓冲
 * @param[out] botOut: 底行输出缓冲
 * @note 纯函数，不分配堆。
 */
void formatDialRightSummary(const ViewModel &vm, std::span<char> topOut,
                            std::span<char> botOut) noexcept;

/**
 * @brief 格式化 1e-9 度定点经纬度（如 `"31.230415N"` 或 `"121.473701E"`，无浮点）
 * @param degE9: 1e-9 度有符号坐标
 * @param posHemisphere: 正半球字母（'N' 或 'E'）
 * @param negHemisphere: 负半球字母（'S' 或 'W'）
 * @param valid: 定位解是否有效
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯 64 位整数定点运算，不使用浮点。
 */
void formatCoordE9(int64_t degE9, char posHemisphere, char negHemisphere, bool valid,
                   std::span<char> out) noexcept;

/**
 * @brief 格式化毫米级海拔（如 `"12.34m"` 或 `"-5.20m"`，无效时 `"--"`）
 * @param altMm: 海拔高度（mm）
 * @param valid: 定位解是否有效
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯整数定点运算，不使用浮点。
 */
void formatAltMm(int32_t altMm, bool valid, std::span<char> out) noexcept;

/**
 * @brief 格式化 HDOP×100 与差分龄期 0.1 s 组合行（如 `"HDOP 0.85 AGE 1.2s"`）
 * @param hdopX100: HDOP×100，未知为 gnss::kU16Unknown
 * @param diffAgeDs: 差分龄期 0.1 s，未知为 gnss::kU16Unknown
 * @param valid: 定位解是否有效
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯整数定点运算，不使用浮点。
 */
void formatHdopAge(uint16_t hdopX100, uint16_t diffAgeDs, bool valid, std::span<char> out) noexcept;

/**
 * @brief 格式化运行时长（如 `"01:23:45"` 或 `"2d 03:15"`）
 * @param uptimeMs: 单调运行毫秒数
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯整数运算，不使用浮点。
 */
void formatUptime(uint32_t uptimeMs, std::span<char> out) noexcept;

/**
 * @brief 取得表盘中部大号解状态/基站模式短词（NONE/SPS/DGPS/FLOAT/FIX/BASE/PROBE/OFF）
 * @param vm: 视图模型
 * @retval 静态短词指针
 * @note 纯函数。
 */
[[nodiscard]] const char *dialHeroBadge(const ViewModel &vm) noexcept;

/** 表盘中部右侧双行状态文本（非运行定位态按 GNSS 状态精准展示）。 */
struct DialGnssDetail {
    const char *top = "GNSS";
    const char *bot = "DISABLED";
};

/**
 * @brief 取得非定位运行态下表盘右侧按 GNSS 枚举区分的双行状态文案
 * @param vm: 视图模型
 * @retval 双行静态英文短词（每行 <= 9 字符）
 * @note 纯函数。
 */
[[nodiscard]] DialGnssDetail dialGnssDetail(const ViewModel &vm) noexcept;

/**
 * @brief 判断 GNSS 状态是否处于可展示实时定位/卫星数据的运行态（kRunning 或 kReadback）
 * @param state: GNSS 控制器状态
 * @retval true 应展示实时定位与卫星数
 * @retval false 处于离线/探测/配置/失败等非定位态
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr bool isGnssRunningLike(gnss::State state) noexcept
{
    return (state == gnss::State::kRunning) || (state == gnss::State::kReadback);
}

/**
 * @brief 根据本轮 UI 决策、实际刷屏结果与刷屏后当前时间计算终帧通知与精确休眠时长
 * @param dec: 本轮 UiController::step() 决策
 * @param outcome: 底层 SH1106 刷屏结论
 * @param frameStartMs: 本轮开始时刻（ms），用于计算非动画 UI 截止。
 * @param postFlushNowMs: 刷屏结束后进入休眠前的单调时刻（ms，用于按双击绝对截止重算剩余时间）
 * @retval 终帧置位判定与下轮等待时长（ms；0 表示双击截止已到应立即处理）
 * @note 纯函数，不阻塞；仅在 outcome 为 kWritten/kSkipped 时允许标记终帧完成，kError/kBackoff 则缩短至 2 ms 重试。
 */
[[nodiscard]] PostFlushDecision settleFrameWait(const UiDecision &dec, FlushOutcome outcome,
                                                uint32_t frameStartMs,
                                                uint32_t postFlushNowMs) noexcept;

/**
 * @brief 按关机参与者表完成数（优先）或阶段完成掩码计算目标关机进度百分比（0..100）
 * @param stage: 关机阶段
 * @param doneMask: ShutdownDoneBit 组合掩码（终态判定与无参与者计数时的回退）
 * @param doneCount: 参与者表已完成项数（totalCount > 0 时生效）
 * @param totalCount: 参与者表总项数（0 表示回退到 doneMask 分段进度）
 * @retval 目标进度百分比（0..100）
 * @note 纯函数。
 */
[[nodiscard]] uint8_t shutdownTargetPercent(board::ShutdownStage stage, uint8_t doneMask,
                                            uint8_t doneCount = 0U,
                                            uint8_t totalCount = 0U) noexcept;

/**
 * @brief 按关机阶段、真实完成掩码与降级掩码取得当前阶段简短英文提示
 * @param stage: 关机阶段
 * @param doneMask: ShutdownDoneBit 组合掩码
 * @param degradedMask: ShutdownDegradedBit 降级结果组合掩码
 * @param radioMode: 当前蓝牙或 SoftAP 模式
 * @retval 静态阶段或降级提示文本（如 SAVE TIMEOUT / RF TIMEOUT / SAFE IO ERR）
 * @note 纯函数。
 */
[[nodiscard]] const char *shutdownStageText(board::ShutdownStage stage, uint8_t doneMask,
                                            uint8_t degradedMask, RadioMode radioMode) noexcept;

/**
 * @brief 取得关机原因首行英文提示（LOW BATTERY / CONSOLE / KEY / PHONE / WEB）
 * @param reason: 关机原因枚举
 * @retval 静态只读原因字符串
 * @note 纯函数。
 */
[[nodiscard]] const char *shutdownReasonText(board::ShutdownReason reason) noexcept;

/**
 * @brief 取得表盘底部入口的短名称标签（SAT / POS / COMM / DEV / PWR）
 * @param idx: 入口索引（0..4）
 * @retval 静态短名称字符串
 * @note 纯函数。
 */
[[nodiscard]] const char *dockItemName(uint8_t idx) noexcept;

/**
 * @brief 从 gnss::SatTable 提取紧凑的星座分布与 Top-8 CN0 摘要写入 ViewModel
 * @param table: 卫星表快照
 * @param[out] vm: 目标视图模型（更新 satsVisible / satGroups / topSnr / topSnrCount）。
 * @param detail: 卫星子页面需要柱图和信噪比排序时为 true。
 * @note 零堆分配，在独立栈帧调用避免撑大渲染栈。
 */
void summarizeSatTable(const gnss::SatTable &table, ViewModel &vm, bool detail = true) noexcept;

/**
 * @brief 使用 WouoUI 图形引擎将当前 UI 状态（含页面过渡与动效）渲染到 1 KB 帧缓冲
 * @param state: UiController 输出的只读渲染状态
 * @param vm: 视图数据快照
 * @param[out] fb: 8×128 单色帧缓冲（由调用方静态持有）
 * @note 单线程调用（OLED 任务或主机测试）；不访问硬件、不分配堆，非 ISR。
 * @param previousStatusBar: 同一缓冲中含上一帧完整状态栏时可在内容转场中复用。
 * @param modalFrame: 同一缓冲上一次渲染的弹窗，可为空，切换缓冲时应清除。
 * @retval 当前帧含可复用状态栏。
 */
bool renderUi(const UiRenderState &state, const ViewModel &vm, ScreenBuff &fb,
              bool previousStatusBar = false, ModalFrame *modalFrame = nullptr) noexcept;

/**
 * @brief 兼容接口：以静止状态直接渲染指定单页到 1 KB 帧缓冲（供驱动测试与快速渲染使用）
 * @param page: 目标页面
 * @param vm: 视图数据快照
 * @param[out] fb: 8×128 单色帧缓冲
 * @note 单线程调用，不分配堆，非 ISR。
 */
void renderPage(OledPage page, const ViewModel &vm, ScreenBuff &fb) noexcept;

} // namespace app::oled
