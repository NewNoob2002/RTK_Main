// OLED 表盘/自检/子菜单/关机进度 UI 模型、状态机、纯整数格式化与渲染接口（OL2）。
// 纯逻辑头，无 IDF/FreeRTOS 依赖；由 OLED 任务或主机测试单线程串行调用，不分配堆，非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "WouoUI_graph.h"
#include "app/RadioMode.hpp"
#include "board/PowerLogic.hpp"
#include "def/GnssSubDef.hpp"
#include "def/OledDef.hpp"
#include "gnss/Controller.hpp"
#include "gnss/Types.hpp"
#include "modules/oled/OledAnim.hpp"

namespace app::oled {

inline constexpr size_t kWidth = 128U;
inline constexpr size_t kHeight = 64U;
inline constexpr size_t kPages = 8U;
inline constexpr size_t kMaxLineChars = 21U;      // 128 px / 6 px (Font_6_8) = 21 字符
inline constexpr size_t kProductBufBytes = 16U;   // B，产品短名缓冲
inline constexpr size_t kBoardNameBufBytes = 24U; // B，板型显示名缓冲
inline constexpr size_t kHwVerBufBytes = 16U;     // B，硬件版本缓冲
inline constexpr size_t kFwVerBufBytes = 20U;     // B，固件版本缓冲
inline constexpr size_t kSnBufBytes = 24U;        // B，序列号缓冲
inline constexpr size_t kDevNameBufBytes = 24U;   // B，蓝牙/Wi-Fi 广播名缓冲
inline constexpr size_t kGnssFwBufBytes = 20U;    // B，GNSS 固件版本短串缓冲
inline constexpr size_t kCheckItemCount = 5U;     // 自检项数：PWR/NVS/RF/CHG/GNSS
inline constexpr size_t kDockItemCount = 5U;      // SAT/POS/COMM/DEV/PWR
inline constexpr int16_t kDockStartX = 2;
inline constexpr int16_t kDockStepX = 25;
inline constexpr int16_t kDockY = 44;
inline constexpr int16_t kDockW = 24;
inline constexpr int16_t kDockH = 19;
inline constexpr size_t kConstellationSlots = 4U; // 卫星子菜单主星座数：GPS/BDS/GLO/GAL
inline constexpr size_t kTopSnrBars = 8U;         // 卫星子菜单 CN0 条数量

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
    std::array<char, kDevNameBufBytes> deviceName{};
    std::array<char, kDevNameBufBytes> wifiSsid{};
    std::array<char, kGnssFwBufBytes> gnssFwVer{};
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
    uint32_t batteryMv = 0U;
    bool usbPresent = false;
    bool charging = false;
    board::UsbSource usbSource = board::UsbSource::kNone;
    board::ShutdownStage shutdownStage = board::ShutdownStage::kIdle;
    board::ShutdownReason shutdownReason = board::ShutdownReason::kNone;
    uint8_t shutdownDoneMask = 0U;
    uint8_t shutdownDegradedMask = 0U;
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
    uint32_t nextWaitMs = kIdlePollPeriodMs;
};

inline constexpr uint32_t kShutdownFinalRetryMs =
    2U; // ms，USB 终帧 I2C 失败或退避后在 30 ms 窗口内补刷间隔

/** UiController 单步调度与控制输出。 */
struct UiDecision {
    OledPage page = OledPage::kSystemCheck;
    bool markShutdownFinalFrame = false; // USB 关机终帧待刷出 I2C，刷出成功后通知 PowerModule
    bool animating = false;              // 当前处于动画帧调度（33 ms）
    bool hasClickDeadline = false;       // 存在待判双击窗口绝对截止
    uint32_t clickDeadlineMs = 0U;       // 待判双击窗口绝对截止时刻（ms）
    uint32_t nextWaitMs = kIdlePollPeriodMs;
    OledActionRequest action{};
};

enum class UiModal : uint8_t { kNone, kWifiConfirm, kPair };
/** 供渲染器只读读取的完整 UI 瞬时渲染状态。 */
struct UiRenderState {
    OledPage page = OledPage::kSystemCheck;
    uint8_t dockSelected = 0U;
    int16_t dockFocusX = 2;
    uint8_t satSubTab = 0U;
    bool hasCheckWarning = false;
    uint8_t shutdownPct = 0U;
    uint8_t commSelected = 0U;
    UiModal modal = UiModal::kNone;
    uint32_t nowMs = 0U;
    const SystemCheckSm *checkSm = nullptr;
    const PageTransition *transition = nullptr;
    const NumberRoll *satsRoll = nullptr;
    const NumberRoll *batRoll = nullptr;
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
     * @retval 本轮渲染页、USB 终帧通知、双击绝对截止与下一帧精确等待时长
     * @note 仅 OLED 任务或主机测试串行调用，不阻塞、不分配堆。
     */
    [[nodiscard]] UiDecision step(uint32_t nowMs, std::span<const uint32_t> shortKeyTimes,
                                  const ViewModel &vm) noexcept;
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
        return page_;
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
     * @brief 在正常交互页（自检/表盘/子菜单）消费已确认的单击或双击事件
     * @param click: 消歧后的按键事件
     * @param nowMs: 当前单调时间（ms）
     * @param vm: 只读视图模型
     * @note 仅 step() 内部调用。
     */
    void handleClick(ClickEvent click, uint32_t nowMs, const ViewModel &vm) noexcept;

    /** @brief 返回表盘并启动与进入对应的收回过渡。 @param nowMs: 当前时间。 */
    void leaveSubmenu(uint32_t nowMs) noexcept;

    /**
     * @brief 确认弹窗与通信菜单消费一个已判定的点击。
     * @param click: 点击类型。
     * @param nowMs: 当前单调时间。
     * @param vm: 只读视图数据。
     * @retval 要交给 kLink 的动作或空值。
     */
    [[nodiscard]] OledActionRequest handleCommClick(ClickEvent click, uint32_t nowMs,
                                                    const ViewModel &vm) noexcept;

    /**
     * @brief 对当前弹窗和页面消费按键，先处理已入队的生产时间戳。
     * @param nowMs: 当前单调时间。
     * @param times: 待处理的短按时间戳。
     * @param vm: 只读视图数据。
     * @retval 本轮提交的动作。
     */
    [[nodiscard]] OledActionRequest handleInput(uint32_t nowMs, std::span<const uint32_t> times,
                                                const ViewModel &vm) noexcept;

    /**
     * @brief 计算表盘底部指定入口图标的屏幕矩形
     * @param idx: 入口下标（0..3）
     * @retval 对应图标槽位矩形
     * @note 纯函数。
     */
    [[nodiscard]] static Rect dockSlotRect(uint8_t idx) noexcept;

    /**
     * @brief 将表盘入口下标映射为目标子菜单枚举
     * @param idx: 入口下标（0..3）
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
    bool inShutdown_ = false;
    bool hasWarning_ = false;
    uint8_t commSelected_ = 0U;
    bool wifiConfirm_ = false;
    bool pairDecisionSent_ = false;
    uint32_t pairDialogId_ = 0U;
    ClickDetector click_{};
    SystemCheckSm checkSm_{};
    PageTransition transition_{};
    FixedSpring dockSpring_{};
    NumberRoll satsRoll_{};
    NumberRoll batRoll_{};
};

/**
 * 8 页 64-bit FNV-1a 摘要跟踪器：仅占用 72 B 状态（省去第二份 1 KB 帧缓冲），
 * 精确识别“内容未变不刷新”及按页增量写出。
 */
class FrameTracker final {
public:
    constexpr FrameTracker() noexcept = default;

    /**
     * @brief 清除已缓存摘要，使下一帧强制写出全部 8 页
     * @note 初始化或面板恢复上电时调用，不阻塞。
     */
    void invalidate() noexcept;

    /**
     * @brief 使传输失败页的摘要失效，面板可能已接收部分数据
     * @param page: 页编号（0..7）
     * @note OLED 任务串行调用，不阻塞。
     */
    void invalidatePage(uint8_t page) noexcept;

    /**
     * @brief 计算与已提交画面相比发生变化的页位掩码（bit0..bit7 对应 page 0..7）
     * @param fb: 当前 8×128 帧缓冲
     * @param[out] hashesOut: 输出本帧 8 页的 64-bit FNV-1a 摘要
     * @param forceFull: true 时强制返回 0xFF
     * @retval 脏页位掩码；0 表示整帧内容完全未变，无需写 I2C
     * @note 纯计算，不阻塞。
     */
    [[nodiscard]] uint8_t computeDirtyMask(const ScreenBuff &fb,
                                           std::array<uint64_t, kPages> &hashesOut,
                                           bool forceFull = false) const noexcept;

    /**
     * @brief 记录单个页成功写入面板后的摘要（仅在该页 I2C 传输成功后调用）
     * @param page: 页编号（0..7）
     * @param hash: 该页对应的 64-bit 摘要
     * @note 传输失败时调用 invalidatePage()。
     */
    void commitPage(uint8_t page, uint64_t hash) noexcept;

private:
    std::array<uint64_t, kPages> committedHashes_{};
    uint8_t validMask_ = 0U;
};

/**
 * @brief 计算单页 128 字节数据的 64-bit FNV-1a 摘要
 * @param bytes: 单页字节视图
 * @retval 64-bit 摘要值
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] uint64_t fnv1a64(std::span<const uint8_t> bytes) noexcept;

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
 * @param frameUs: 本帧渲染+I2C 总耗时（us）
 * @param postFlushNowMs: 刷屏结束后进入休眠前的单调时刻（ms，用于按双击绝对截止重算剩余时间）
 * @retval 终帧置位判定与下轮等待时长（ms；0 表示双击截止已到应立即处理）
 * @note 纯函数，不阻塞；仅在 outcome 为 kWritten/kSkipped 时允许标记终帧完成，kError/kBackoff 则缩短至 2 ms 重试。
 */
[[nodiscard]] PostFlushDecision settleFrameWait(const UiDecision &dec, FlushOutcome outcome,
                                                uint32_t frameUs, uint32_t postFlushNowMs) noexcept;

/**
 * @brief 按关机阶段与完成掩码计算目标关机进度百分比（0..100）
 * @param stage: 关机阶段
 * @param doneMask: ShutdownDoneBit 组合掩码
 * @retval 目标进度百分比（0..100）
 * @note 纯函数。
 */
[[nodiscard]] uint8_t shutdownTargetPercent(board::ShutdownStage stage, uint8_t doneMask) noexcept;

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
 * @brief 从 gnss::SatTable 提取紧凑的星座分布与 Top-8 CN0 摘要写入 ViewModel
 * @param table: 卫星表快照
 * @param[out] vm: 目标视图模型（更新 satsVisible / satGroups / topSnr / topSnrCount）
 * @note 零堆分配，在独立栈帧调用避免撑大渲染栈。
 */
void summarizeSatTable(const gnss::SatTable &table, ViewModel &vm) noexcept;

/**
 * @brief 使用 WouoUI 图形引擎将当前 UI 状态（含页面过渡与动效）渲染到 1 KB 帧缓冲
 * @param state: UiController 输出的只读渲染状态
 * @param vm: 视图数据快照
 * @param[out] fb: 8×128 单色帧缓冲（由调用方静态持有）
 * @note 单线程调用（OLED 任务或主机测试）；不访问硬件、不分配堆，非 ISR。
 */
void renderUi(const UiRenderState &state, const ViewModel &vm, ScreenBuff &fb) noexcept;

/**
 * @brief 兼容接口：以静止状态直接渲染指定单页到 1 KB 帧缓冲（供驱动测试与快速渲染使用）
 * @param page: 目标页面
 * @param vm: 视图数据快照
 * @param[out] fb: 8×128 单色帧缓冲
 * @note 单线程调用，不分配堆，非 ISR。
 */
void renderPage(OledPage page, const ViewModel &vm, ScreenBuff &fb) noexcept;

} // namespace app::oled
