// OLED 只读状态页模型、翻页状态机、文本格式化与 WouoUI 渲染（纯逻辑，无 IDF/FreeRTOS 依赖）。
// 所有函数可在主机测试与 OLED 低优先级任务串行调用；不分配堆，非 ISR。
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
#include "def/OledDef.hpp"
#include "gnss/Controller.hpp"
#include "gnss/Types.hpp"

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

/** 从 DataCenter/AppContext 只读快照拼装的纯值视图模型。 */
struct ViewModel {
    std::array<char, kProductBufBytes> product{};
    std::array<char, kBoardNameBufBytes> boardName{};
    std::array<char, kHwVerBufBytes> hwVer{};
    std::array<char, kFwVerBufBytes> fwVer{};
    std::array<char, kSnBufBytes> sn{};
    board::BootMode bootMode = board::BootMode::kNormal;
    RadioMode radioMode = RadioMode::kBluetooth;
    bool btReady = false;
    uint8_t btSessions = 0U;
    bool gnssEnabled = false;
    gnss::State gnssState = gnss::State::kOff;
    bool fixValid = false;
    gnss::FixQuality fixQuality = gnss::FixQuality::kUnknown;
    uint8_t satsUsed = 0U;
    uint16_t hAccCm = gnss::kU16Unknown;
    uint8_t batteryPct = board::kBatteryPctUnknown;
    uint32_t batteryMv = 0U;
    bool usbPresent = false;
    bool charging = false;
    board::ShutdownStage shutdownStage = board::ShutdownStage::kIdle;
    board::ShutdownReason shutdownReason = board::ShutdownReason::kNone;
};

/** 翻页与关机显示状态机的单步决策输出。 */
struct PageDecision {
    OledPage page = OledPage::kSplash;
    bool shouldPowerOff = false; // 进入断电阶段且提示展示够时长，通知驱动关闭面板和 GPIO12
    bool shouldWake = false;     // 30 s 关机超时回退或唤醒失败退避到期后重新点亮面板
    uint32_t nextWaitMs = 0U; // 非零时覆盖默认刷新周期，确保 kCutPower 准时在最短提示窗到期时断电
};

/**
 * OLED 页面状态机：开机画面自动/短按切换、正常模式短按循环翻页、充电待机锁页、关机提示与断电时机。
 * 纯确定性状态机，由 OLED 任务独占，支持 uint32_t 单调时钟自然回绕。
 */
class PageSm final {
public:
    static constexpr uint32_t kSplashDurationMs = 1500U;   // ms，开机画面默认展示时长
    static constexpr uint32_t kShutdownNoticeMinMs = 200U; // ms，断电前关机提示最短可见时长
    static constexpr uint32_t kWakeRetryBackoffMs = 1000U; // ms，面板唤醒失败后的有界重试退避

    constexpr PageSm() noexcept = default;

    /**
     * @brief 按启动运行档初始化页面状态机
     * @param bootMode: 正常启动或充电待机
     * @param nowMs: 当前单调时间（ms）
     * @param initialShortKeys: 初始化时刻的短按计数基线
     * @note 仅启动初始化时调用，不阻塞。
     */
    void reset(board::BootMode bootMode, uint32_t nowMs, uint32_t initialShortKeys = 0U) noexcept;

    /**
     * @brief 推进状态机一步并计算当前应展示的页面与开关屏指令
     * @param nowMs: 当前单调时间（ms）
     * @param shortKeyCount: UI 事件总线累计短按次数
     * @param shutdownStage: 电源模块当前关机阶段
     * @retval 本轮目标页面及是否关屏/恢复
     * @note 由持有者串行调用，不阻塞、不分配堆。
     */
    [[nodiscard]] PageDecision step(uint32_t nowMs, uint32_t shortKeyCount,
                                    board::ShutdownStage shutdownStage) noexcept;

    /**
     * @brief 通知状态机本次面板唤醒失败，保持断电态并按退避窗口自动重试
     * @param nowMs: 唤醒失败时刻的单调时间（ms）
     * @note 仅由 OLED 任务在 wakeFromPowerOff() 失败后调用，不阻塞。
     */
    void onWakeFailed(uint32_t nowMs) noexcept;

    /**
     * @brief 查询当前所处页面
     * @retval 当前页面枚举
     * @note 纯查询，不阻塞。
     */
    [[nodiscard]] OledPage page() const noexcept
    {
        return page_;
    }

private:
    /**
     * @brief 处理非空闲关机阶段的页面切换、精确断电等待与关屏触发
     * @param nowMs: 当前单调时间（ms）
     * @param shutdownStage: 非空闲关机阶段
     * @retval 关机页面及断电/精确等待决策
     * @note 仅 PageSm::step 内部调用，不阻塞。
     */
    [[nodiscard]] PageDecision stepShutdown(uint32_t nowMs,
                                            board::ShutdownStage shutdownStage) noexcept;

    board::BootMode bootMode_ = board::BootMode::kNormal;
    OledPage page_ = OledPage::kSplash;
    uint32_t splashStartMs_ = 0U;
    uint32_t lastShortKeys_ = 0U;
    uint32_t shutdownNoticeSinceMs_ = 0U;
    uint32_t wakeFailedAtMs_ = 0U;
    bool inShutdown_ = false;
    bool poweredOff_ = false;
    bool wakeRetryActive_ = false;
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
     * @note 若传输失败不调用本函数，下次刷新会自动重试该页。
     */
    void commitPage(uint8_t page, uint64_t hash) noexcept;

private:
    std::array<uint64_t, kPages> committedHashes_{};
    uint8_t validMask_ = 0U; // bit p = 1 表示 page p 已有有效提交摘要
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
 * @brief 格式化射频模式短标签（用于状态页顶栏左侧）
 * @param mode: 当前射频模式
 * @param btReady: 蓝牙是否就绪
 * @param btSessions: 已连接蓝牙会话数
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯函数，不分配堆。
 */
void formatRadioShort(RadioMode mode, bool btReady, uint8_t btSessions,
                      std::span<char> out) noexcept;

/**
 * @brief 格式化电量与充电短标签（用于状态页顶栏右侧，如 `"85%+"`、`"100%U"`、`"--%"`）
 * @param pct: 电量百分比，未知时为 kBatteryPctUnknown
 * @param usbPresent: USB 是否在位
 * @param charging: 是否正在充电
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯函数，不分配堆。
 */
void formatBatteryShort(uint8_t pct, bool usbPresent, bool charging, std::span<char> out) noexcept;

/**
 * @brief 格式化电池电压文本（如 `"4.05V"`，未知或 0 时 `"--.-V"`）
 * @param mv: 电池电压（mV）
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯整数定点运算，不使用浮点。
 */
void formatBatteryVoltage(uint32_t mv, std::span<char> out) noexcept;

/**
 * @brief 格式化 GNSS 状态行（如 `"GNSS: RUN"`、`"GNSS: OFF"`）
 * @param enabled: GNSS 模块是否启用
 * @param state: GNSS 控制器状态
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯函数，不分配堆。
 */
void formatGnssStateLine(bool enabled, gnss::State state, std::span<char> out) noexcept;

/**
 * @brief 格式化定位解质量行（如 `"FIX : FIXED"`、`"FIX : --"`）
 * @param enabled: GNSS 是否启用
 * @param state: GNSS 状态
 * @param fixValid: GGA 解是否有效
 * @param quality: GGA 质量枚举
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯函数，不分配堆。
 */
void formatFixLine(bool enabled, gnss::State state, bool fixValid, gnss::FixQuality quality,
                   std::span<char> out) noexcept;

/**
 * @brief 格式化卫星数与水平精度行（如 `"SAT : 18  0.02m"`、`"SAT : --"`）
 * @param enabled: GNSS 是否启用
 * @param state: GNSS 状态
 * @param fixValid: GGA 解是否有效
 * @param satsUsed: 参与解算卫星数
 * @param hAccCm: GST 水平精度（cm），未知时为 gnss::kU16Unknown
 * @param[out] out: 输出 NUL 结尾缓冲
 * @note 纯整数定点运算，不使用浮点。
 */
void formatSatsAccLine(bool enabled, gnss::State state, bool fixValid, uint8_t satsUsed,
                       uint16_t hAccCm, std::span<char> out) noexcept;

/**
 * @brief 使用 WouoUI 图形引擎将指定只读页面渲染到 1 KB 帧缓冲
 * @param page: 目标页面
 * @param vm: 视图数据快照
 * @param[out] fb: 8×128 单色帧缓冲（由调用方静态持有）
 * @note 单线程调用（OLED 任务或主机测试）；不访问硬件、不分配堆，非 ISR。
 */
void renderPage(OledPage page, const ViewModel &vm, ScreenBuff &fb) noexcept;

} // namespace app::oled
