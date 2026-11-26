// OLED WouoUI 图形渲染器（OL4）：自检打勾动画、表盘 UTC/状态滚动/入口名、统一左主卡右列表子页、
// 充电待机重做、长按进度浮层、关机确认弹窗、关机原因与图标里程碑、展开错峰过渡。
// 仅 OLED 任务或主机测试单线程串行调用；无 IDF/硬件依赖，无浮点、不分配堆，非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/oled/OledUi.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "modules/oled/OledIcons.hpp"

namespace app::oled {
namespace {

constexpr int16_t kCanvasW = static_cast<int16_t>(kWidth);
constexpr int16_t kCanvasH = static_cast<int16_t>(kHeight);
constexpr char kMinPrintableAscii = ' ';
constexpr char kMaxPrintableAscii = '~';
constexpr uint8_t kMaxPercent = 100U;
constexpr uint32_t kMsPerSec = 1000U;
constexpr uint16_t kHdopScale = 100U;
constexpr uint16_t kDiffAgeScale = 10U;

// 通用 10 px 顶栏与基础布局像素常量。
constexpr int16_t kHeaderTextY = 1;
constexpr int16_t kHeaderLineY = 10;
constexpr int16_t kDockIconOffsetX = 4; // (24 - 16) / 2
constexpr int16_t kDockIconY = 47;
constexpr int16_t kDockIconSize = 16;
constexpr int16_t kDockLabelY = 36;
constexpr uint8_t kDockCornerR = 3U;
constexpr int16_t kParallaxShiftPx = 42;
constexpr size_t kRollBufBytes = 12U;
constexpr size_t kShortBufBytes = 16U;
constexpr size_t kGnssTagBufBytes = 13U;
constexpr size_t kWaitBufBytes = 12U;
constexpr size_t kSubSatHdrBufBytes = 20U;

// 10 px 状态栏像素常量（表盘与统一子页共用）。
constexpr int16_t kBatBoxW = 23;
constexpr int16_t kBatBoxH = 8;
constexpr int16_t kBatTipX1 = 23;
constexpr int16_t kBatTipX2 = 24;
constexpr int16_t kBatTipY0 = 2;
constexpr int16_t kBatTipY1 = 5;
constexpr int16_t kBatSegX0 = 2;    // 第一格左缘（相对图标 x）
constexpr int16_t kBatSegStepX = 4; // 格距：格宽 3 + 间隙 1
constexpr int16_t kBatSegW = 3;
constexpr int16_t kBatSegY = 2;
constexpr int16_t kBatFillH = 4;
constexpr int16_t kStatusSatTextX = 10;
constexpr int16_t kStatusWarnX = 23;
constexpr int16_t kStatusFixMinX = 32;
constexpr int16_t kStatusFixAreaW = 34;
constexpr int16_t kStatusRadioX = 69;
constexpr int16_t kStatusBoltX = 79;
constexpr int16_t kBoltW = 5;
constexpr int16_t kIcon7W = 7;
constexpr int16_t kIcon8H = 8;
constexpr int16_t kStatusBatIconX = 103; // 图标宽 25（含正极凸起），右缘贴 128 列边界

// 开机自检页像素常量。
constexpr uint8_t kLogoBoxR = 3U;
constexpr int16_t kLogoTitleY = 10;
constexpr int16_t kLogoLineX0 = 18;
constexpr int16_t kLogoLineInset = 19;
constexpr int16_t kLogoLineY = 38;
constexpr int16_t kLogoFwY = 44;
constexpr int16_t kCheckFracX = 106;
constexpr int16_t kCheckRowStartY = 13;
constexpr int16_t kCheckRowStepY = 10;
constexpr size_t kCheckVisibleRows = 4U;
constexpr int16_t kCheckLabelX = 4;
constexpr int16_t kCheckGnssInfoX = 44;
constexpr int16_t kCheckFailX = 82;
constexpr int16_t kCheckWaitX = 94;
constexpr int16_t kCheckIconX = 116;
constexpr int16_t kCheckSpinX = 117;
constexpr int16_t kCheckScrollX = 126;
constexpr int16_t kCheckScrollY0 = 13;
constexpr int16_t kCheckScrollH = 38;
constexpr int16_t kCheckScrollThumbH = 30;
constexpr int16_t kCheckScrollShiftPx = 8;
constexpr int16_t kCheckBarY = 55;
constexpr int16_t kCheckBarW = 124;
constexpr int16_t kCheckBarH = 7;
constexpr int16_t kCheckBarFillY = 57;
constexpr int16_t kCheckBarFillH = 3;
constexpr uint32_t kCheckBarFillMaxW = 120U;
constexpr uint32_t kSpinStepMs = 100U;
constexpr size_t kCheckIdxGnss = 4U;

// 表盘中部像素常量。
constexpr int16_t kDialHeroX = 2;
constexpr int16_t kDialHeroY = 12;
constexpr int16_t kDialHeroW = 60;
constexpr int16_t kDialHeroH = 24;
constexpr int16_t kDialInfoMinX = 62;
constexpr int16_t kDialRightEdgeX = 127;
constexpr size_t kDialRightMaxChars = 11U;
constexpr int16_t kDialInfoTopY = 13;
constexpr int16_t kDialInfoBotY = 24;
constexpr int16_t kDockPillY = 35;
constexpr int16_t kDockPillH = 9;
// 统一子页框架（左 40×52 主卡 + 右 3 行列表 + 3 px 滚动条）像素常量。
constexpr int16_t kSubBodyY = 12;
constexpr int16_t kSubCardX = 0;
constexpr int16_t kSubCardW = 40;
constexpr int16_t kSubCardH = 52;
constexpr uint8_t kSubCardR = 3U;
constexpr int16_t kSubCardIconX = 12;
constexpr int16_t kSubCardIconY = 4;
constexpr int16_t kSubCardVal1Y = 24;
constexpr int16_t kSubCardVal2Y = 37;
constexpr int16_t kSubListX = 42;
constexpr int16_t kSubListW = 82;
constexpr int16_t kSubListH = 52;
constexpr int16_t kSubRowH = 16;
constexpr int16_t kSubRowIconX = 3;
constexpr int16_t kSubRowIconYOff = 4;
constexpr int16_t kSubRowTextX = 11;
constexpr int16_t kSubRowValRightX = 80;
constexpr int16_t kSubScrollX = 125;
constexpr int16_t kSubScrollTrackY0 = 13;
constexpr int16_t kSubScrollTrackH = 50;
constexpr int16_t kSubScrollThumbW = 3;
constexpr int16_t kSubScrollMinThumbH = 10;
constexpr int16_t kCardBatCapX = 17;
constexpr int16_t kCardBatCapY = 4;
constexpr int16_t kCardBatCapW = 6;
constexpr int16_t kCardBatCapH = 2;
constexpr int16_t kCardBatBodyX = 14;
constexpr int16_t kCardBatBodyY = 6;
constexpr int16_t kCardBatBodyW = 12;
constexpr int16_t kCardBatBodyH = 14;
constexpr int16_t kCardBatFillX = 16;
constexpr int16_t kCardBatFillBaseY = 17; // 最低一格的行 y（相对卡片），自下而上每格步距 2 px
constexpr int16_t kCardBatSegStepY = 2;
constexpr int16_t kCardBatFillW = 8;
constexpr int16_t kCardUsbIconX = 30;
constexpr int16_t kCardUsbIconY = 9;
constexpr size_t kSubRowLabelBytes = 12U;
constexpr size_t kSubRowValBytes = 16U;
constexpr size_t kSubPosRowCount = 5U;
constexpr size_t kSubCommBtRowCount = 4U;
constexpr size_t kSubDeviceRowCount = 5U;
constexpr size_t kSubPowerRowCount = 5U;

// 卫星子页像素常量。
constexpr int16_t kSubSatTitleX = 10;
constexpr int16_t kSubSatColStartX = 6;
constexpr int16_t kSubSatColStepX = 31;
constexpr int16_t kSubSatCountY = 13;
constexpr int16_t kSubSatVisBarW = 14;
constexpr int16_t kSubSatUsedBarOffX = 6;
constexpr int16_t kSubSatUsedBarW = 10;
constexpr int16_t kSubSatLabelY = 55;
constexpr int16_t kSubSatNoDataY = 30;
constexpr int16_t kSubSnrStartX = 4;
constexpr int16_t kSubSnrStepX = 15;
constexpr int16_t kSubSnrBarW = 10;
constexpr int16_t kSubSnrLetterOffX = 3;

// 充电待机页像素常量（左侧大号 5 格电池，右侧保持进度环，底部 FW/HW）。
constexpr int16_t kStandbyStatusY = 2;
constexpr int16_t kStandbyVoltX = 30;
constexpr int16_t kStandbyStatusX = 2;
constexpr int16_t kStandbyBatX = 2;
constexpr int16_t kStandbyBatY = 14;
constexpr int16_t kStandbyBatW = 58;
constexpr int16_t kStandbyBatH = 34;
constexpr int16_t kStandbyBatTipX = 60;
constexpr int16_t kStandbyBatTipY = 25;
constexpr int16_t kStandbyBatTipW = 3;
constexpr int16_t kStandbyBatTipH = 12;
constexpr int16_t kStandbySegX0 = 5;
constexpr int16_t kStandbySegStepX = 11; // 格宽 9 + 间隙 2
constexpr int16_t kStandbySegW = 9;
constexpr int16_t kStandbySegY = 18;
constexpr int16_t kStandbySegH = 26;
constexpr int16_t kStandbyRingCx = 98;
constexpr int16_t kStandbyRingCy = 20;
constexpr int16_t kStandbyHintX = 68;
constexpr int16_t kStandbyHintY = 40;
constexpr int16_t kStandbyStartingX = 74;
constexpr int16_t kStandbyFooterLineY = 52;
constexpr int16_t kStandbyFooterTextY = 54;

// 关机页与弹窗/浮层像素常量。
constexpr int16_t kRingCx = 26;
constexpr int16_t kRingCy = 38;
constexpr int16_t kRingDotSize = 3;
constexpr int16_t kShutPctY = 34;
constexpr int16_t kShutRightX = 50;
constexpr int16_t kShutStageY = 17;
constexpr int16_t kShutBarY = 30;
constexpr int16_t kShutBarW = 74;
constexpr int16_t kShutFillX = 52;
constexpr int16_t kShutFillY = 32;
constexpr uint32_t kShutFillMaxW = 70U;
constexpr int16_t kShutStepsY = 44;
constexpr uint8_t kModalCornerR = 3U;
constexpr int16_t kWifiModalTitleY = 9;
constexpr int16_t kWifiModalWarningY = 26;
constexpr int16_t kPairModalTitleY = 7;
constexpr int16_t kPairModalPeerY = 18;
constexpr int16_t kPairModalNumberY = 29;
constexpr int16_t kPwrModalTitleY = 10;
constexpr int16_t kRebootIconY = 8;
constexpr int16_t kRebootTitleY = 30;
constexpr int16_t kRebootSubY = 46;
constexpr int16_t kPwrModalBarX = 12;
constexpr int16_t kPwrModalBarY = 32;
constexpr int16_t kPwrModalBarW = 84;
constexpr int16_t kPwrModalBarH = 8;
constexpr int16_t kPwrModalSecX = 101;
constexpr int16_t kPwrModalTextOffX = 11;
constexpr size_t kModalInsideX = 4U;
constexpr int16_t kModalButtonX = 11;
constexpr int16_t kModalButtonY = 48;
constexpr int16_t kModalButtonW = 50;
constexpr int16_t kModalButtonH = 12;
constexpr int16_t kModalSingleButtonW = 70; // 单选项按钮宽度，容纳 "POWER OFF"（9 字 × 6 px）
constexpr int16_t kModalSingleButtonX = (kCanvasW - kModalSingleButtonW) / 2;
constexpr int16_t kModalButtonLabelY = kModalButtonY + 2;
constexpr size_t kModalButtonPage = 6U;
constexpr size_t kModalButtonTailPage = 7U;
constexpr size_t kModalTimerPage = 4U;
constexpr uint8_t kModalButtonTailKeep = 0xF0U;
constexpr uint8_t kModalClearPage = 0U;
constexpr int16_t kShutPendingDotOffY = 3;
constexpr int16_t kHoldBoxY = 10;
constexpr int16_t kHoldBoxW = 116;
constexpr int16_t kHoldBoxH = 44;
constexpr int16_t kHoldCollapseMinW = 24;
constexpr int16_t kHoldContentMinH = 28;
constexpr int16_t kHoldTitleY = 6;
constexpr int16_t kHoldIconY = 20;

/** 统一子页列表项描述（零堆定长结构）。 */
struct SubListRow {
    const uint8_t *icon7x8 = nullptr;
    std::array<char, kSubRowLabelBytes> label{};
    std::array<char, kSubRowValBytes> value{};
    bool isBack = false;
};

/**
 * @brief 在 NUL 结尾字符数组中取得有效文本视图
 * @param buf: 字符数组
 * @retval 不含末尾 NUL 的字符串视图
 * @note 纯函数。
 */
[[nodiscard]] std::string_view viewOf(std::span<const char> buf) noexcept
{
    size_t len = 0U;
    while ((len < buf.size()) && (buf[len] != '\0')) {
        ++len;
    }
    return {buf.data(), len};
}

/**
 * @brief 将可打印 ASCII 字符串转为无符号字节数组供 WouoUI 绘制
 * @param src: 源文本
 * @param[out] out: NUL 结尾的无符号字节缓冲
 * @note 纯函数，越界字符替换为 '?'。
 */
void toWouoBytes(std::string_view src, std::span<uint8_t> out) noexcept
{
    if (out.empty()) {
        return;
    }
    const size_t maxChars = out.size() - 1U;
    const size_t count = std::min(src.size(), maxChars);
    for (size_t i = 0U; i < count; ++i) {
        const char ch = src[i];
        const char safe = ((ch >= kMinPrintableAscii) && (ch <= kMaxPrintableAscii)) ? ch : '?';
        // safe 位于 0x20..0x7E，转为 uint8_t 安全。
        out[i] = static_cast<uint8_t>(safe);
    }
    out[count] = 0U;
}

/**
 * @brief 按坐标绘制单行文本
 * @param canvas: 目标画布
 * @param x: 起始 x 坐标
 * @param y: 起始 y 坐标
 * @param font: WouoUI 字体
 * @param text: 文本视图
 * @note 不分配堆。
 */
void drawText(Canvas &canvas, int16_t x, int16_t y, const sFONT &font,
              std::string_view text) noexcept
{
    std::array<uint8_t, kMaxLineChars + 1U> bytes{};
    toWouoBytes(text, bytes);
    WouoUI_CanvasDrawStr(&canvas, x, y, font, bytes.data());
}

/**
 * @brief 在画布水平居中绘制单行文本
 * @param canvas: 目标画布
 * @param y: 起始 y 坐标
 * @param font: WouoUI 字体
 * @param text: 文本视图
 * @note 不分配堆。
 */
void drawCentered(Canvas &canvas, int16_t y, const sFONT &font, std::string_view text) noexcept
{
    std::array<char, kMaxLineChars + 1U> clean{};
    sanitizeCopy(text, clean);
    const uint16_t width = WouoUI_GetStrWidth(clean.data(), font);
    // width < canvas.w 时差值一半位于 0..63，转 int16_t 安全。
    const int16_t x = (static_cast<int32_t>(width) < canvas.w)
                          ? static_cast<int16_t>(
                                (static_cast<int32_t>(canvas.w) - static_cast<int32_t>(width)) / 2)
                          : int16_t{0};
    drawText(canvas, x, y, font, viewOf(clean));
}

/**
 * @brief 使用滚动剪裁画布绘制带竖向切入动画的整数文本（静止时直接输出）
 * @param parent: 父画布
 * @param x: 区域左上角 x
 * @param y: 区域左上角 y
 * @param font: 字体
 * @param roll: 对应 NumberRoll 状态（可为 nullptr 表示静态绘制）
 * @param value: 当前显示数值
 * @param suffix: 可选后缀（如 `"%"` 或空串）
 * @param nowMs: 当前单调时间（ms）
 * @note 纯定点，不分配堆。
 */
void drawRollingValue(const Canvas &parent, int16_t x, int16_t y, const sFONT &font,
                      const NumberRoll *roll, int32_t value, std::string_view suffix,
                      uint32_t nowMs) noexcept
{
    std::array<char, kRollBufBytes> curBuf{};
    static_cast<void>(std::snprintf(curBuf.data(), curBuf.size(), "%d%.*s", static_cast<int>(value),
                                    static_cast<int>(suffix.size()), suffix.data()));
    const uint16_t textW = WouoUI_GetStrWidth(curBuf.data(), font);
    const int16_t clipH = static_cast<int16_t>(font.Height);
    const int16_t clipW = static_cast<int16_t>(std::min<uint16_t>(textW + font.Width, kWidth));
    Canvas clip{
        .start_x = static_cast<int16_t>(parent.start_x + x),
        .start_y = static_cast<int16_t>(parent.start_y + y),
        .w = clipW,
        .h = clipH,
    };

    if ((roll == nullptr) || !roll->active(nowMs)) {
        drawText(clip, 0, 0, font, viewOf(curBuf));
        return;
    }

    std::array<char, kRollBufBytes> prevBuf{};
    static_cast<void>(std::snprintf(prevBuf.data(), prevBuf.size(), "%d%.*s",
                                    static_cast<int>(roll->previous()),
                                    static_cast<int>(suffix.size()), suffix.data()));
    const uint16_t prog = roll->progressQ10(nowMs);
    const int16_t offset = lerpFixed(0, clipH, prog);
    const int16_t prevY = roll->increasing() ? static_cast<int16_t>(-offset) : offset;
    const int16_t curY = roll->increasing() ? static_cast<int16_t>(clipH - offset)
                                            : static_cast<int16_t>(offset - clipH);
    drawText(clip, 0, prevY, font, viewOf(prevBuf));
    drawText(clip, 0, curY, font, viewOf(curBuf));
}

/**
 * @brief 取得实际应点亮的格数（低电闪烁熄灭相位少画最后一格）
 * @param bars: 电量格结论
 * @retval 0..5 点亮格数
 */
[[nodiscard]] uint8_t visibleBars(const board::BatteryBarsView &bars) noexcept
{
    return (bars.blinkOff && bars.bars > 0U) ? static_cast<uint8_t>(bars.bars - 1U) : bars.bars;
}

/**
 * @brief 绘制 25x8 单色电池图标（含右端正极凸起），内部按 5 格显示电量
 * @param canvas: 目标画布
 * @param x: 左上角 x
 * @param y: 左上角 y
 * @param bars: 电量格结论（bars==0 表示未知，仅画外框）
 * @note 不分配堆。低电闪烁熄灭相位隐藏最后一格。
 */
void drawBatteryIcon(Canvas &canvas, int16_t x, int16_t y,
                     const board::BatteryBarsView &bars) noexcept
{
    WouoUI_CanvasDrawRBoxEmpty(&canvas, x, y, kBatBoxW, kBatBoxH, 1U);
    WouoUI_CanvasDrawLine_V(&canvas, static_cast<int16_t>(x + kBatTipX1),
                            static_cast<int16_t>(y + kBatTipY0),
                            static_cast<int16_t>(y + kBatTipY1));
    WouoUI_CanvasDrawLine_V(&canvas, static_cast<int16_t>(x + kBatTipX2),
                            static_cast<int16_t>(y + kBatTipY0),
                            static_cast<int16_t>(y + kBatTipY1));
    const uint8_t lit = visibleBars(bars);
    for (uint8_t i = 0U; i < lit; ++i) {
        WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(x + kBatSegX0 + (i * kBatSegStepX)),
                              static_cast<int16_t>(y + kBatSegY), kBatSegW, kBatFillH, 0U);
    }
}

/**
 * @brief 绘制 32 点圆形进度环（半径 15 px，外圈未亮段用点状底轨表示）
 * @param canvas: 目标画布
 * @param cx: 圆心 x
 * @param cy: 圆心 y
 * @param progressQ10: 进度（0..1024）
 * @note 供关机页和充电待机唤醒环共用。
 */
void drawProgressRing(Canvas &canvas, int16_t cx, int16_t cy, uint16_t progressQ10) noexcept
{
    const uint16_t clamped = std::min(progressQ10, kEaseScale);
    const size_t litCount =
        (static_cast<size_t>(clamped) * icons::kRingPts32.size()) / static_cast<size_t>(kEaseScale);
    for (size_t i = 0U; i < icons::kRingPts32.size(); ++i) {
        const int16_t px = static_cast<int16_t>(cx + icons::kRingPts32[i].dx);
        const int16_t py = static_cast<int16_t>(cy + icons::kRingPts32[i].dy);
        if (i < litCount) {
            WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(px - 1),
                                  static_cast<int16_t>(py - 1), kRingDotSize, kRingDotSize, 0U);
        } else if ((i & 1U) == 0U) {
            WouoUI_CanvasDrawPoint(&canvas, px, py);
        }
    }
}

/**
 * @brief 绘制表盘与子页共用的顶部 10 px 状态栏（卫星数、告警、解状态、BT/Wi-Fi、充电闪电与电量）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void drawStatusBar(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    WouoUI_CanvasDrawBMP(&canvas, 0, kHeaderTextY, kIcon8H, kIcon8H, icons::kSat8x8.data(), 1U);
    if (vm.gnssEnabled && isGnssRunningLike(vm.gnssState)) {
        const int32_t sats = vm.fixValid ? static_cast<int32_t>(vm.satsUsed) : 0;
        drawRollingValue(canvas, kStatusSatTextX, kHeaderTextY, Font_6_8, state.satsRoll, sats, "",
                         state.nowMs);
    } else {
        drawText(canvas, kStatusSatTextX, kHeaderTextY, Font_6_8, "--");
    }

    if (state.hasCheckWarning) {
        WouoUI_CanvasDrawBMP(&canvas, kStatusWarnX, kHeaderTextY, kIcon7W, kIcon8H,
                             icons::kWarn7x8.data(), 1U);
    }

    // 中部解状态短词（与表盘和所有子页顶栏统一）。
    const char *const badge = dialHeroBadge(vm);
    const uint16_t bw = WouoUI_GetStrWidth(badge, Font_6_8);
    const int16_t badgeX =
        (bw < static_cast<uint16_t>(kStatusFixAreaW))
            ? static_cast<int16_t>(kStatusFixMinX +
                                   ((kStatusFixAreaW - static_cast<int16_t>(bw)) / 2))
            : kStatusFixMinX;
    drawText(canvas, badgeX, kHeaderTextY, Font_6_8, badge);

    // 射频状态图标：Wi-Fi 模式画 Wi-Fi 扇形，蓝牙模式区分已连接/待机。
    if (vm.radioMode == RadioMode::kWifiConfig) {
        WouoUI_CanvasDrawBMP(&canvas, kStatusRadioX, kHeaderTextY, kIcon8H, kIcon8H,
                             icons::kWifi8x8.data(), 1U);
    } else if (vm.btReady) {
        const uint8_t *btBmp =
            (vm.btSessions > 0U) ? icons::kBtConn8x8.data() : icons::kBtWait8x8.data();
        WouoUI_CanvasDrawBMP(&canvas, kStatusRadioX, kHeaderTextY, kIcon8H, kIcon8H, btBmp, 1U);
    }

    if (isEffectiveCharging(vm)) {
        WouoUI_CanvasDrawBMP(&canvas, kStatusBoltX, kHeaderTextY, kBoltW, kIcon8H,
                             icons::kBolt5x8.data(), 1U);
    } else if (vm.usbPresent) {
        WouoUI_CanvasDrawBMP(&canvas, kStatusBoltX, kHeaderTextY, kBoltW, kIcon8H,
                             icons::kPlug5x8.data(), 1U);
    }
    drawBatteryIcon(canvas, kStatusBatIconX, kHeaderTextY, state.batBars);
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);
}

/**
 * @brief 渲染开机自检 Logo 逐列展开帧
 * @param canvas: 目标画布
 * @param sm: 自检状态机（可为 nullptr）
 * @param nowMs: 当前单调时间（ms）
 * @param vm: 视图数据
 * @note 仅 renderSystemCheck 调用。
 */
void renderSystemCheckLogo(const Canvas &canvas, const SystemCheckSm *sm, uint32_t nowMs,
                           const ViewModel &vm) noexcept
{
    const uint16_t reveal = (sm != nullptr) ? sm->logoRevealQ10(nowMs) : kEaseScale;
    const int16_t revealW = lerpFixed(0, kCanvasW, reveal);
    Canvas logoClip{
        .start_x = canvas.start_x,
        .start_y = canvas.start_y,
        .w = revealW,
        .h = canvas.h,
    };
    WouoUI_CanvasDrawRBoxEmpty(&logoClip, 2, 2, static_cast<int16_t>(kCanvasW - 4),
                               static_cast<int16_t>(kCanvasH - 4), kLogoBoxR);
    const std::string_view prod = viewOf(vm.product);
    const std::string_view fw = viewOf(vm.fwVer);
    drawCentered(logoClip, kLogoTitleY, Font_12_24, prod.empty() ? "RTK" : prod);
    WouoUI_CanvasDrawLine_H(&logoClip, kLogoLineX0, static_cast<int16_t>(kCanvasW - kLogoLineInset),
                            kLogoLineY);
    drawCentered(logoClip, kLogoFwY, Font_6_8, fw.empty() ? "BOOTING" : fw);
}

/**
 * @brief 渲染自检列表单行（名称、GNSS 型号/固件、逐项打勾展开动画或 Pending 旋转动画）
 * @param canvas: 目标画布
 * @param idx: 自检项下标（0..4）
 * @param rowY: 本行顶边像素 Y 坐标
 * @param label: 左侧名称
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 仅 renderSystemCheck 调用。
 */
void renderSystemCheckRow(Canvas &canvas, size_t idx, int16_t rowY, const char *label,
                          const UiRenderState &state, const ViewModel &vm) noexcept
{
    constexpr std::array<char, 4U> kSpinner{'|', '/', '-', '\\'};
    const SystemCheckSm *sm = state.checkSm;
    drawText(canvas, kCheckLabelX, rowY, Font_6_8, label);

    const CheckStatus st = sm->itemStatus(idx);
    if (st == CheckStatus::kOk) {
        if ((idx == kCheckIdxGnss) && (vm.gnssModel != gnss::Model::kUnknown)) {
            std::array<char, kGnssTagBufBytes> gnssTag{};
            const std::string_view fw = viewOf(vm.gnssFwVer);
            if (!fw.empty()) {
                formatLabelValue(gnss::modelName(vm.gnssModel), fw, gnssTag);
            } else {
                sanitizeCopy(gnss::modelName(vm.gnssModel), gnssTag);
            }
            drawText(canvas, kCheckGnssInfoX, rowY, Font_6_8, viewOf(gnssTag));
        }
        // 逐项打勾左向右展开动画（150 ms）。
        const uint32_t sinceOk = state.nowMs - sm->itemResolvedMs(idx);
        const uint16_t tickProg = easeOutCubic(sinceOk, kCheckTickAnimMs);
        const int16_t clipW = std::max<int16_t>(1, lerpFixed(1, kIcon7W, tickProg));
        Canvas tickClip{
            .start_x = static_cast<int16_t>(canvas.start_x + kCheckIconX),
            .start_y = static_cast<int16_t>(canvas.start_y + rowY),
            .w = clipW,
            .h = kIcon8H,
        };
        WouoUI_CanvasDrawBMP(&tickClip, 0, 0, kIcon7W, kIcon8H, icons::kCheck7x8.data(), 1U);
        return;
    }
    if (st == CheckStatus::kFail) {
        drawText(canvas, kCheckFailX, rowY, Font_6_8, "FAIL");
        WouoUI_CanvasDrawBMP(&canvas, kCheckIconX, rowY, kIcon7W, kIcon8H, icons::kCross7x8.data(),
                             1U);
        return;
    }
    if (st == CheckStatus::kAbsent) {
        WouoUI_CanvasDrawBMP(&canvas, kCheckIconX, rowY, kIcon7W, kIcon8H, icons::kDash7x8.data(),
                             1U);
        return;
    }
    if ((sm->phase() == CheckPhase::kChecking) && (idx == sm->currentIndex())) {
        const uint32_t waitS = sm->stepElapsedMs(state.nowMs) / kMsPerSec;
        if (waitS >= 1U) {
            std::array<char, kWaitBufBytes> waitBuf{};
            static_cast<void>(
                std::snprintf(waitBuf.data(), waitBuf.size(), "%us", static_cast<unsigned>(waitS)));
            drawText(canvas, kCheckWaitX, rowY, Font_6_8, viewOf(waitBuf));
        }
        const size_t spinIdx = (state.nowMs / kSpinStepMs) % kSpinner.size();
        std::array<char, 2U> sp{kSpinner[spinIdx], '\0'};
        drawText(canvas, kCheckSpinX, rowY, Font_6_8, viewOf(sp));
    }
}

/**
 * @brief 渲染开机自检页（Logo 逐列展开 -> 5 项逐项打勾列表与平滑进度条）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderSystemCheck(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    const SystemCheckSm *sm = state.checkSm;
    const CheckPhase phase = (sm != nullptr) ? sm->phase() : CheckPhase::kLogo;
    if (phase == CheckPhase::kLogo) {
        renderSystemCheckLogo(canvas, sm, state.nowMs, vm);
        return;
    }

    drawText(canvas, 0, kHeaderTextY, Font_6_8, "SYSTEM CHECK");
    const size_t doneCount =
        (phase == CheckPhase::kChecking) ? sm->currentIndex() : kCheckItemCount;
    std::array<char, 8U> stepFrac{};
    static_cast<void>(std::snprintf(stepFrac.data(), stepFrac.size(), "%u/%u",
                                    static_cast<unsigned>(doneCount),
                                    static_cast<unsigned>(kCheckItemCount)));
    drawText(canvas, kCheckFracX, kHeaderTextY, Font_6_8, viewOf(stepFrac));
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    const std::array<const char *, kCheckItemCount> kNames{
        "PWR/BAT", "NVS",  (vm.radioMode == RadioMode::kWifiConfig) ? "WIFI AP" : "BT SPP",
        "CHARGER", "GNSS",
    };
    // 每屏最多显示 4 行（行距 10 px 避免 6×8 字体上下粘连），检查到第 5 项或完成时滚动显示后 4 项。
    const size_t activeIdx =
        (phase == CheckPhase::kChecking) ? sm->currentIndex() : (kCheckItemCount - 1U);
    const size_t firstIdx =
        (activeIdx >= kCheckVisibleRows) ? (kCheckItemCount - kCheckVisibleRows) : 0U;
    for (size_t slot = 0U; slot < kCheckVisibleRows; ++slot) {
        const size_t idx = firstIdx + slot;
        const auto rowY =
            static_cast<int16_t>(kCheckRowStartY + (static_cast<int16_t>(slot) * kCheckRowStepY));
        renderSystemCheckRow(canvas, idx, rowY, kNames[idx], state, vm);
    }
    for (int16_t y = kCheckScrollY0; y < (kCheckScrollY0 + kCheckScrollH);
         y = static_cast<int16_t>(y + 2)) {
        WouoUI_CanvasDrawPoint(&canvas, kCheckScrollX, y);
    }
    const auto thumbY =
        static_cast<int16_t>(kCheckScrollY0 + ((firstIdx > 0U) ? kCheckScrollShiftPx : 0));
    WouoUI_CanvasDrawRBox(&canvas, kCheckScrollX, thumbY, 2, kCheckScrollThumbH, 0U);
    WouoUI_CanvasDrawRBoxEmpty(&canvas, 2, kCheckBarY, kCheckBarW, kCheckBarH, 1U);
    uint32_t smoothUnits = static_cast<uint32_t>(doneCount) * kEaseScale;
    if ((phase == CheckPhase::kChecking) && (sm->itemStatus(doneCount) != CheckStatus::kPending)) {
        smoothUnits += easeOutCubic(state.nowMs - sm->itemResolvedMs(doneCount), kCheckTickAnimMs);
    }
    const auto fillW = static_cast<int16_t>((smoothUnits * kCheckBarFillMaxW) /
                                            (kCheckItemCount * static_cast<uint32_t>(kEaseScale)));
    if (fillW > 0) {
        WouoUI_CanvasDrawRBox(&canvas, 4, kCheckBarFillY, fillW, kCheckBarFillH, 0U);
    }
}

/**
 * @brief 格式化表盘右侧卫星数摘要文本（`SAT used/vis` 或 `SAT used`）
 * @param vm: 视图数据
 * @param[out] out: 输出缓冲区
 * @note 仅 formatDialGnssRight 调用，不分配堆。
 */
void formatDialSatLine(const ViewModel &vm, std::span<char> out) noexcept
{
    const unsigned used = vm.fixValid ? static_cast<unsigned>(vm.satsUsed) : 0U;
    if (vm.satsVisible > 0U) {
        static_cast<void>(std::snprintf(out.data(), out.size(), "SAT %u/%u", used,
                                        static_cast<unsigned>(vm.satsVisible)));
    } else {
        static_cast<void>(std::snprintf(out.data(), out.size(), "SAT %u", used));
    }
}

/**
 * @brief 格式化表盘右侧双行摘要（GNSS 运行中分支：优先显示 UTC + 精度/卫星，否则精度 + 卫星）
 * @param vm: 视图数据
 * @param[out] topOut: 顶行输出缓冲区
 * @param[out] botOut: 底行输出缓冲区
 * @note 仅 formatDialRightInfo 调用，不分配堆。
 */
void formatDialGnssRight(const ViewModel &vm, std::span<char> topOut,
                         std::span<char> botOut) noexcept
{
    std::array<char, kShortBufBytes> accBuf{};
    const bool hasAcc = formatHorizontalAcc(vm.fixValid, vm.hAccCm, accBuf);
    if (vm.utcValid) {
        static_cast<void>(std::snprintf(
            topOut.data(), topOut.size(), "%02u:%02u:%02uZ", static_cast<unsigned>(vm.utcHour),
            static_cast<unsigned>(vm.utcMinute), static_cast<unsigned>(vm.utcSecond)));
        if (hasAcc) {
            const unsigned used = static_cast<unsigned>(vm.satsUsed);
            if (vm.satsVisible > 0U) {
                const int written =
                    std::snprintf(botOut.data(), botOut.size(), "%s %u/%u", accBuf.data(), used,
                                  static_cast<unsigned>(vm.satsVisible));
                if ((written > 0) && (static_cast<size_t>(written) <= kDialRightMaxChars)) {
                    return;
                }
            }
            static_cast<void>(std::snprintf(botOut.data(), botOut.size(), "ACC %s", accBuf.data()));
        } else if (vm.fixValid) {
            if (vm.satsVisible > 0U) {
                formatDialSatLine(vm, botOut);
            } else {
                sanitizeCopy("ACC --", botOut);
            }
        } else {
            sanitizeCopy("NO FIX", botOut);
        }
        return;
    }
    if (hasAcc) {
        sanitizeCopy(viewOf(accBuf), topOut);
    } else {
        sanitizeCopy(vm.fixValid ? "ACC --" : "NO FIX", topOut);
    }
    formatDialSatLine(vm, botOut);
}

/**
 * @brief 格式化表盘右侧双行摘要文本
 * @param vm: 视图数据
 * @param[out] topOut: 顶行输出缓冲区
 * @param[out] botOut: 底行输出缓冲区
 * @note 仅 renderDialplate 调用，不分配堆。
 */
void formatDialRightInfo(const ViewModel &vm, std::span<char> topOut,
                         std::span<char> botOut) noexcept
{
    if (vm.workModeActive && (vm.workMode != WorkModeKind::kRover)) {
        static_cast<void>(std::snprintf(topOut.data(), topOut.size(), "%us",
                                        static_cast<unsigned>(vm.workModeElapsedS)));
        sanitizeCopy(vm.workModeState == WorkModeState::kBaseTx ? "TX RTCM" : "BASE", botOut);
        return;
    }
    if (vm.gnssEnabled && isGnssRunningLike(vm.gnssState)) {
        formatDialGnssRight(vm, topOut, botOut);
        return;
    }
    const DialGnssDetail detail = dialGnssDetail(vm);
    sanitizeCopy(detail.top, topOut);
    sanitizeCopy(detail.bot, botOut);
}

/**
 * @brief 渲染主表盘（10 px 状态栏 + 大号滚动解状态 + UTC/精度 + 选中入口名 + 5 图标弹簧底座）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderDialplate(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    // 左侧大号解状态，支持状态切换 180 ms 竖向滚动切入。
    const char *const badge = dialHeroBadge(vm);
    Canvas heroClip{
        .start_x = static_cast<int16_t>(canvas.start_x + kDialHeroX),
        .start_y = static_cast<int16_t>(canvas.start_y + kDialHeroY),
        .w = kDialHeroW,
        .h = kDialHeroH,
    };
    if ((state.heroRollQ10 < kEaseScale) && (state.heroPrevBadge != nullptr)) {
        const int16_t offset = lerpFixed(0, kDialHeroH, state.heroRollQ10);
        drawText(heroClip, 0, static_cast<int16_t>(-offset), Font_12_24, state.heroPrevBadge);
        drawText(heroClip, 0, static_cast<int16_t>(kDialHeroH - offset), Font_12_24, badge);
    } else {
        drawText(heroClip, 0, 0, Font_12_24, badge);
    }

    std::array<char, kShortBufBytes> rightTop{};
    std::array<char, kShortBufBytes> rightBot{};
    formatDialRightInfo(vm, rightTop, rightBot);
    const int16_t topW = static_cast<int16_t>(WouoUI_GetStrWidth(rightTop.data(), Font_6_8));
    const int16_t botW = static_cast<int16_t>(WouoUI_GetStrWidth(rightBot.data(), Font_6_8));
    const int16_t topX =
        std::max<int16_t>(kDialInfoMinX, static_cast<int16_t>(kDialRightEdgeX - topW));
    const int16_t botX =
        std::max<int16_t>(kDialInfoMinX, static_cast<int16_t>(kDialRightEdgeX - botW));
    drawText(canvas, topX, kDialInfoTopY, Font_6_8, viewOf(rightTop));
    drawText(canvas, botX, kDialInfoBotY, Font_6_8, viewOf(rightBot));
    // 底部 dock 选中项上方反白胶囊标签，明确标识当前入口并跟随选中框中心。
    const char *const dockName = dockItemName(state.dockSelected);
    const int16_t nameW = static_cast<int16_t>(WouoUI_GetStrWidth(dockName, Font_6_8));
    const int16_t pillW = static_cast<int16_t>(nameW + 4);
    const int16_t centerX = static_cast<int16_t>(state.dockFocusX + (kDockW / 2));
    const int16_t pillX = std::clamp<int16_t>(static_cast<int16_t>(centerX - (pillW / 2)), 0,
                                              static_cast<int16_t>(kCanvasW - pillW));
    WouoUI_CanvasDrawRBox(&canvas, pillX, kDockPillY, pillW, kDockPillH, 1U);
    WouoUI_GraphSetPenColor(0U);
    drawText(canvas, static_cast<int16_t>(pillX + 2), kDockLabelY, Font_6_8, dockName);
    WouoUI_GraphSetPenColor(1U);
    const std::array<const uint8_t *, kDockItemCount> kDockIcons{
        icons::kDockSat16x16.data(), icons::kDockPos16x16.data(), icons::kDockComm16x16.data(),
        icons::kDockDev16x16.data(), icons::kDockPwr16x16.data(),
    };
    for (uint8_t i = 0U; i < kDockItemCount; ++i) {
        const auto iconX = static_cast<int16_t>(
            kDockStartX + (static_cast<int16_t>(i) * kDockStepX) + kDockIconOffsetX);
        WouoUI_CanvasDrawBMP(&canvas, iconX, kDockIconY, kDockIconSize, kDockIconSize,
                             kDockIcons[i], 1U);
    }
    const int16_t inset = (state.page == OledPage::kDialplate) ? state.squishInset : 0;
    WouoUI_CanvasDrawRBoxEmpty(&canvas, static_cast<int16_t>(state.dockFocusX + inset),
                               static_cast<int16_t>(kDockY + inset),
                               static_cast<int16_t>(kDockW - (2 * inset)),
                               static_cast<int16_t>(kDockH - (2 * inset)), kDockCornerR);
}

/**
 * @brief 绘制统一子页左侧 40×52 主卡（圆角框 + 16×16 图标 + 关键值 + 辅助短词）
 * @param canvas: 目标画布
 * @param state: 渲染状态（提供错峰偏移与电池滚动）
 * @param icon16x16: 16×16 图标（nullptr 表示改用动态电池图形）
 * @param keyVal: 主关键值（居中，最多 6 字符）
 * @param subVal: 次短词（居中，最多 6 字符）
 * @param drawDynamicBat: true 时在图标区绘制 5 格电量的 16×16 电池图形
 * @param vm: 视图数据（供动态电池图形读取 USB 在位状态）
 * @note 仅统一子页渲染函数调用。
 */
void drawSubMainCard(const Canvas &canvas, const UiRenderState &state, const uint8_t *icon16x16,
                     std::string_view keyVal, std::string_view subVal, bool drawDynamicBat,
                     const ViewModel &vm) noexcept
{
    const int16_t cardLift =
        (state.transition != nullptr) ? state.transition->rowOffsetY(state.nowMs, 0U) : 0;
    const Canvas cardClip{
        .start_x = static_cast<int16_t>(canvas.start_x + kSubCardX),
        .start_y = static_cast<int16_t>(canvas.start_y + kSubBodyY),
        .w = kSubCardW,
        .h = kSubCardH,
    };
    Canvas cardInner{
        .start_x = cardClip.start_x,
        .start_y = static_cast<int16_t>(cardClip.start_y + cardLift),
        .w = kSubCardW,
        .h = kSubCardH,
    };
    WouoUI_CanvasDrawRBoxEmpty(&cardInner, 0, 0, kSubCardW, kSubCardH, kSubCardR);
    if (drawDynamicBat) {
        // 16×16 动态垂直电池图形（顶部电极 + 内部电量填充）。
        WouoUI_CanvasDrawRBox(&cardInner, kCardBatCapX, kCardBatCapY, kCardBatCapW, kCardBatCapH,
                              0U);
        WouoUI_CanvasDrawRBoxEmpty(&cardInner, kCardBatBodyX, kCardBatBodyY, kCardBatBodyW,
                                   kCardBatBodyH, 1U);
        const uint8_t lit = visibleBars(state.batBars);
        for (uint8_t i = 0U; i < lit; ++i) {
            WouoUI_CanvasDrawRBox(&cardInner, kCardBatFillX,
                                  static_cast<int16_t>(kCardBatFillBaseY - (i * kCardBatSegStepY)),
                                  kCardBatFillW, 1, 0U);
        }
        if (vm.usbPresent) {
            WouoUI_CanvasDrawBMP(&cardInner, kCardUsbIconX, kCardUsbIconY, kBoltW, kIcon8H,
                                 icons::kPlug5x8.data(), 1U);
        }
    } else if (icon16x16 != nullptr) {
        WouoUI_CanvasDrawBMP(&cardInner, kSubCardIconX, kSubCardIconY, kDockIconSize, kDockIconSize,
                             icon16x16, 1U);
    }

    drawCentered(cardInner, kSubCardVal1Y, Font_6_8, keyVal);
    if (!subVal.empty()) {
        drawCentered(cardInner, kSubCardVal2Y, Font_6_8, subVal);
    }
}

/**
 * @brief 在子页列表裁剪区中绘制单行条目（图标、名称、右对齐截断值或 Back）
 * @param listClip: 列表裁剪画布
 * @param row: 列表项数据
 * @param rowY: 本行顶边相对 listClip 的 Y 偏移（px）
 * @note 仅 drawSubList 调用，不分配堆。
 */
void drawSubRow(Canvas &listClip, const SubListRow &row, int16_t rowY) noexcept
{
    const int16_t contentY = static_cast<int16_t>(rowY + kSubRowIconYOff);
    if (row.isBack) {
        WouoUI_CanvasDrawBMP(&listClip, kSubRowIconX, contentY, kIcon7W, kIcon8H,
                             icons::kRowBack7x8.data(), 1U);
        drawText(listClip, kSubRowTextX, contentY, Font_6_8, "Back");
        return;
    }
    if (row.icon7x8 != nullptr) {
        WouoUI_CanvasDrawBMP(&listClip, kSubRowIconX, contentY, kIcon7W, kIcon8H, row.icon7x8, 1U);
    }
    const std::string_view lbl = viewOf(row.label);
    const std::string_view val = viewOf(row.value);
    constexpr int16_t kUnlabelledValueX = 2; // 13 个 Font_6_8 字符可完整放入右侧列表。
    int16_t minValX = (lbl.empty() && row.icon7x8 == nullptr) ? kUnlabelledValueX : kSubRowTextX;
    if (!lbl.empty()) {
        drawText(listClip, kSubRowTextX, contentY, Font_6_8, lbl);
        const int16_t lblW = static_cast<int16_t>(WouoUI_GetStrWidth(row.label.data(), Font_6_8));
        minValX = static_cast<int16_t>(kSubRowTextX + lblW + 2);
    }
    if (!val.empty()) {
        const int16_t availW =
            std::max<int16_t>(0, static_cast<int16_t>(kSubRowValRightX - minValX));
        const size_t maxChars = static_cast<size_t>(availW / static_cast<int16_t>(Font_6_8.Width));
        std::array<char, kSubRowValBytes> clipped{};
        const size_t copyLen = std::min(val.size(), std::min(maxChars, clipped.size() - 1U));
        sanitizeCopy(val.substr(0U, copyLen), clipped);
        const int16_t valW = static_cast<int16_t>(WouoUI_GetStrWidth(clipped.data(), Font_6_8));
        const int16_t valX =
            std::max<int16_t>(minValX, static_cast<int16_t>(kSubRowValRightX - valW));
        drawText(listClip, valX, contentY, Font_6_8, viewOf(clipped));
    }
}

/**
 * @brief 绘制统一子页右侧 3 px 滚动指示条
 * @param canvas: 目标画布
 * @param scrollY: 当前滚动像素偏移
 * @param rowCount: 列表总行数（须大于 kSubVisibleRows）
 * @note 仅 drawSubList 调用。
 */
void drawSubScrollbar(Canvas &canvas, int16_t scrollY, size_t rowCount) noexcept
{
    for (int16_t y = kSubScrollTrackY0; y < (kSubScrollTrackY0 + kSubScrollTrackH);
         y = static_cast<int16_t>(y + 2)) {
        WouoUI_CanvasDrawPoint(&canvas, static_cast<int16_t>(kSubScrollX + 1), y);
    }
    const int16_t thumbH = std::max<int16_t>(
        kSubScrollMinThumbH, static_cast<int16_t>((kSubScrollTrackH * kSubVisibleRows) /
                                                  static_cast<int16_t>(rowCount)));
    const int16_t maxScrollPx =
        static_cast<int16_t>((static_cast<int16_t>(rowCount) - kSubVisibleRows) * kSubRowStepY);
    const int16_t clampedScroll = std::clamp<int16_t>(scrollY, 0, maxScrollPx);
    const int16_t travel = static_cast<int16_t>(kSubScrollTrackH - thumbH);
    const int16_t thumbY = static_cast<int16_t>(
        kSubScrollTrackY0 +
        ((maxScrollPx > 0) ? ((static_cast<int32_t>(travel) * clampedScroll) / maxScrollPx) : 0));
    WouoUI_CanvasDrawRBox(&canvas, kSubScrollX, thumbY, kSubScrollThumbW, thumbH, 1U);
}

/**
 * @brief 绘制统一子页右侧可滚动列表（最多 3 行可见 + 弹簧选择框 + 挤压反馈 + 滚动指示条）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param rows: 列表项视图
 * @note 仅统一子页渲染函数调用，不分配堆。
 */
void drawSubList(Canvas &canvas, const UiRenderState &state,
                 std::span<const SubListRow> rows) noexcept
{
    Canvas listClip{
        .start_x = static_cast<int16_t>(canvas.start_x + kSubListX),
        .start_y = static_cast<int16_t>(canvas.start_y + kSubBodyY),
        .w = kSubListW,
        .h = kSubListH,
    };

    for (size_t i = 0U; i < rows.size(); ++i) {
        const int16_t stagger =
            (state.transition != nullptr)
                ? state.transition->rowOffsetY(state.nowMs, static_cast<uint8_t>(i))
                : 0;
        const int16_t rowY = static_cast<int16_t>((static_cast<int16_t>(i) * kSubRowStepY) -
                                                  state.subScrollY + stagger);
        if ((rowY >= kSubListH) || ((rowY + kSubRowH) <= 0)) {
            continue;
        }
        drawSubRow(listClip, rows[i], rowY);
    }

    // 弹簧跟随选中框 + 双击确认挤压反馈（收缩 2 px 后回弹）。
    if (!rows.empty()) {
        const int16_t selY = static_cast<int16_t>(state.subFocusY - state.subScrollY);
        const int16_t inset = state.squishInset;
        WouoUI_CanvasDrawRBoxEmpty(&listClip, inset, static_cast<int16_t>(selY + inset),
                                   static_cast<int16_t>(kSubListW - (2 * inset)),
                                   static_cast<int16_t>(kSubRowH - (2 * inset)), 2U);
    }

    if (rows.size() > kSubVisibleRows) {
        drawSubScrollbar(canvas, state.subScrollY, rows.size());
    }
}

/**
 * @brief 构造单条普通子页列表行
 * @param icon: 7×8 行图标
 * @param label: 左侧名称（可为空）
 * @param value: 右对齐值文本
 * @retval 填充好的 SubListRow
 * @note 纯辅助函数。
 */
[[nodiscard]] SubListRow makeRow(const uint8_t *icon, std::string_view label,
                                 std::string_view value) noexcept
{
    SubListRow row{};
    row.icon7x8 = icon;
    sanitizeCopy(label, row.label);
    sanitizeCopy(value, row.value);
    return row;
}

/**
 * @brief 构造统一的左箭头 Back 列表行
 * @retval isBack=true 的 SubListRow
 * @note 纯辅助函数。
 */
[[nodiscard]] SubListRow makeBackRow() noexcept
{
    SubListRow row{};
    row.isBack = true;
    return row;
}

/**
 * @brief 渲染卫星子菜单子页 0（GPS/BDS/GLO/GAL 可见与参与解算两段柱状图）
 * @param canvas: 目标画布
 * @param vm: 视图数据
 * @note 仅 renderSubSat 调用。
 */
void renderSubSatSys(Canvas &canvas, const ViewModel &vm) noexcept
{
    constexpr std::array<const char *, kConstellationSlots> kLabels{"GPS", "BDS", "GLO", "GAL"};
    constexpr int16_t kBarBottomY = 52;
    constexpr int16_t kBarMaxH = 29;
    constexpr uint8_t kMaxScaleSats = 20U;
    for (size_t i = 0U; i < kConstellationSlots; ++i) {
        const auto colX =
            static_cast<int16_t>(kSubSatColStartX + (static_cast<int16_t>(i) * kSubSatColStepX));
        const SatGroupStat &g = vm.satGroups[i];
        std::array<char, 8U> cnt{};
        static_cast<void>(std::snprintf(cnt.data(), cnt.size(), "%u/%u",
                                        static_cast<unsigned>(g.used),
                                        static_cast<unsigned>(g.visible)));
        drawText(canvas, colX, kSubSatCountY, Font_6_8, viewOf(cnt));
        const uint8_t visClamped = std::min(g.visible, kMaxScaleSats);
        const uint8_t usedClamped = std::min(g.used, visClamped);
        const auto visH =
            static_cast<int16_t>((static_cast<uint32_t>(visClamped) * kBarMaxH) / kMaxScaleSats);
        const auto usedH =
            static_cast<int16_t>((static_cast<uint32_t>(usedClamped) * kBarMaxH) / kMaxScaleSats);
        if (visH > 0) {
            WouoUI_CanvasDrawRBoxEmpty(&canvas, static_cast<int16_t>(colX + 4),
                                       static_cast<int16_t>(kBarBottomY - visH), kSubSatVisBarW,
                                       visH, 0U);
        }
        if (usedH > 0) {
            WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(colX + kSubSatUsedBarOffX),
                                  static_cast<int16_t>(kBarBottomY - usedH), kSubSatUsedBarW, usedH,
                                  0U);
        }
        drawText(canvas, static_cast<int16_t>(colX + 2), kSubSatLabelY, Font_6_8, kLabels[i]);
    }
}

/**
 * @brief 渲染卫星子菜单子页 1（Top-8 载噪比 CN0 柱状图）
 * @param canvas: 目标画布
 * @param vm: 视图数据
 * @note 仅 renderSubSat 调用。
 */
void renderSubSatCn0(Canvas &canvas, const ViewModel &vm) noexcept
{
    if (vm.topSnrCount == 0U) {
        drawCentered(canvas, kSubSatNoDataY, Font_6_8, "NO SIGNAL DATA");
        return;
    }
    constexpr int16_t kSnrBottomY = 53;
    constexpr int16_t kSnrMaxH = 30;
    constexpr uint8_t kSnrMaxDb = 55U;
    for (size_t i = 0U; i < vm.topSnrCount; ++i) {
        const auto barX =
            static_cast<int16_t>(kSubSnrStartX + (static_cast<int16_t>(i) * kSubSnrStepX));
        const SatSnrItem &item = vm.topSnr[i];
        const uint8_t snrClamped = std::min(item.snrDbhz, kSnrMaxDb);
        const auto barH =
            static_cast<int16_t>((static_cast<uint32_t>(snrClamped) * kSnrMaxH) / kSnrMaxDb);
        std::array<char, 4U> valBuf{};
        static_cast<void>(
            std::snprintf(valBuf.data(), valBuf.size(), "%u", static_cast<unsigned>(item.snrDbhz)));
        drawText(canvas, barX, kSubSatCountY, Font_6_8, viewOf(valBuf));
        if (barH > 0) {
            if (item.used) {
                WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(barX + 1),
                                      static_cast<int16_t>(kSnrBottomY - barH), kSubSnrBarW, barH,
                                      0U);
            } else {
                WouoUI_CanvasDrawRBoxEmpty(&canvas, static_cast<int16_t>(barX + 1),
                                           static_cast<int16_t>(kSnrBottomY - barH), kSubSnrBarW,
                                           barH, 0U);
            }
        }
        std::array<char, 2U> letter{item.sysLetter, '\0'};
        drawText(canvas, static_cast<int16_t>(barX + kSubSnrLetterOffX), kSubSatLabelY, Font_6_8,
                 viewOf(letter));
    }
}

/**
 * @brief 绘制卫星子菜单的状态栏，显示 used/visible 与当前图表。
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void drawSatHeader(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    WouoUI_CanvasDrawBMP(&canvas, 0, kHeaderTextY, kIcon8H, kIcon8H, icons::kSat8x8.data(), 1U);
    drawText(canvas, kSubSatTitleX, kHeaderTextY, Font_6_8,
             (state.satSubTab == 0U) ? "SATS SYS" : "SATS CN0");
    std::array<char, kSubSatHdrBufBytes> hdrRight{};
    static_cast<void>(std::snprintf(hdrRight.data(), hdrRight.size(), "%u/%u %u/2",
                                    vm.fixValid ? static_cast<unsigned>(vm.satsUsed) : 0U,
                                    static_cast<unsigned>(vm.satsVisible),
                                    static_cast<unsigned>(state.satSubTab + 1U)));
    const uint16_t rw = WouoUI_GetStrWidth(hdrRight.data(), Font_6_8);
    const int16_t rx = (rw < kWidth) ? static_cast<int16_t>(kWidth - rw) : int16_t{0};
    drawText(canvas, rx, kHeaderTextY, Font_6_8, viewOf(hdrRight));
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);
}

/**
 * @brief 绘制卫星子菜单内容区。
 * @param canvas: 目标画布。
 * @param state: 渲染状态。
 * @param vm: 视图数据。
 */
void renderSubSat(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    if (state.satSubTab == 0U) {
        renderSubSatSys(canvas, vm);
    } else {
        renderSubSatCn0(canvas, vm);
    }
}

/**
 * @brief 渲染位置子页（左主卡解状态+精度，右列表：纬度、经度、高程、HDOP/AGE、Back）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderSubPos(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    std::array<char, kShortBufBytes> accBuf{};
    if (!formatHorizontalAcc(vm.fixValid, vm.hAccCm, accBuf)) {
        sanitizeCopy("--", accBuf);
    }
    drawSubMainCard(canvas, state, icons::kDockPos16x16.data(), dialHeroBadge(vm), viewOf(accBuf),
                    false, vm);

    std::array<char, kShortBufBytes> latBuf{};
    std::array<char, kShortBufBytes> lonBuf{};
    std::array<char, kShortBufBytes> altBuf{};
    std::array<char, kShortBufBytes> dopBuf{};
    formatCoordE9(vm.latE9, 'N', 'S', vm.fixValid, latBuf);
    formatCoordE9(vm.lonE9, 'E', 'W', vm.fixValid, lonBuf);
    formatAltMm(vm.altMm, vm.fixValid, altBuf);

    std::array<char, 8U> hdopStr{};
    std::array<char, 8U> ageStr{};
    if (!vm.fixValid || (vm.hdopX100 == gnss::kU16Unknown)) {
        sanitizeCopy("--", hdopStr);
    } else {
        static_cast<void>(std::snprintf(hdopStr.data(), hdopStr.size(), "%u.%02u",
                                        static_cast<unsigned>(vm.hdopX100 / kHdopScale),
                                        static_cast<unsigned>(vm.hdopX100 % kHdopScale)));
    }
    if (!vm.fixValid || (vm.diffAgeDs == gnss::kU16Unknown)) {
        sanitizeCopy("--", ageStr);
    } else {
        static_cast<void>(std::snprintf(ageStr.data(), ageStr.size(), "%u.%us",
                                        static_cast<unsigned>(vm.diffAgeDs / kDiffAgeScale),
                                        static_cast<unsigned>(vm.diffAgeDs % kDiffAgeScale)));
    }
    static_cast<void>(
        std::snprintf(dopBuf.data(), dopBuf.size(), "%s %s", hdopStr.data(), ageStr.data()));

    std::array<SubListRow, kSubPosRowCount> rows{{
        makeRow(icons::kRowLat7x8.data(), "", viewOf(latBuf)),
        makeRow(icons::kRowLon7x8.data(), "", viewOf(lonBuf)),
        makeRow(icons::kRowAlt7x8.data(), "ALT", viewOf(altBuf)),
        makeRow(icons::kRowDop7x8.data(), "HA", viewOf(dopBuf)),
        makeBackRow(),
    }};
    drawSubList(canvas, state, rows);
}

/**
 * @brief 绘制 Wi-Fi 模式 COMM 左卡与右列表，地址行使用完整列表宽度。
 * @param canvas: 目标画布。
 * @param state: 渲染状态。
 * @param vm: 已发布的 SoftAP 数据与客户端数。
 */
void renderCommWifi(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    std::array<char, kShortBufBytes> clients{};
    static_cast<void>(std::snprintf(clients.data(), clients.size(), "%u STA",
                                    static_cast<unsigned>(vm.wifiClients)));
    drawSubMainCard(canvas, state, icons::kCardWifi16x16.data(), "WIFI", viewOf(clients), false,
                    vm);
    const auto ssid = vm.wifiReady ? viewOf(vm.wifiSsid) : std::string_view{"OFF"};
    std::array<char, kSubRowValBytes> ip{};
    if (vm.wifiReady) {
        const auto [first, second, third, fourth] = vm.wifiIpv4;
        static_cast<void>(std::snprintf(ip.data(), ip.size(), "%u.%u.%u.%u",
                                        static_cast<unsigned>(first), static_cast<unsigned>(second),
                                        static_cast<unsigned>(third),
                                        static_cast<unsigned>(fourth)));
    }
    const auto ipView = viewOf(ip);
    const std::array<SubListRow, kSubCommWifiRows> rows{{
        makeRow(icons::kRowRf7x8.data(), "Mode", "WF>BT"),
        makeRow(icons::kRowPeer7x8.data(), "", ssid.empty() ? "--" : ssid),
        makeRow(nullptr, "", ipView.empty() ? "--" : ipView),
        makeBackRow(),
    }};
    drawSubList(canvas, state, rows);
}

/**
 * @brief 渲染通信子页，按当前射频模式选择列表。
 * @param canvas: 目标画布。
 * @param state: 渲染状态。
 * @param vm: 视图数据。
 */
void renderComm(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    if (vm.radioMode == RadioMode::kWifiConfig) {
        renderCommWifi(canvas, state, vm);
        return;
    }
    std::array<char, kShortBufBytes> subBuf{};
    static_cast<void>(std::snprintf(subBuf.data(), subBuf.size(), "%u CON",
                                    static_cast<unsigned>(vm.btSessions)));
    drawSubMainCard(canvas, state, icons::kCardBt16x16.data(), "BT", viewOf(subBuf), false, vm);

    std::array<char, kShortBufBytes> pairVal{};
    const int32_t remaining = static_cast<int32_t>(vm.pairing.windowDeadlineMs - state.nowMs);
    if (vm.pairing.windowOpen && (remaining > 0)) {
        const unsigned seconds = (static_cast<uint32_t>(remaining) - 1U) / kMsPerSec + 1U;
        static_cast<void>(std::snprintf(pairVal.data(), pairVal.size(), "%us", seconds));
    } else {
        sanitizeCopy("OPEN", pairVal);
    }
    const std::string_view peer = viewOf(vm.btPeerLabel);
    const std::string_view peerText = (vm.btSessions > 0U)
                                          ? (peer.empty() ? std::string_view{"LINK"} : peer)
                                          : std::string_view{"NONE"};

    std::array<SubListRow, kSubCommBtRowCount> rows{{
        makeRow(icons::kRowRf7x8.data(), "Mode", vm.wifiSupported ? "BT>WF" : "BT"),
        makeRow(icons::kRowPair7x8.data(), "Pair", viewOf(pairVal)),
        makeRow(icons::kRowPeer7x8.data(), "Dev", peerText),
        makeBackRow(),
    }};
    drawSubList(canvas, state, rows);
}

/**
 * @brief 渲染设备子页（左主卡设备图标+型号，右列表：FW、SN、BT 名、运行时间、Back）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderSubDevice(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    const std::string_view prod = viewOf(vm.product);
    const std::string_view hw = viewOf(vm.hwVer);
    drawSubMainCard(canvas, state, icons::kDockDev16x16.data(), prod.empty() ? "RTK" : prod,
                    hw.empty() ? "--" : hw, false, vm);

    std::array<char, kShortBufBytes> upBuf{};
    formatUptime(vm.uptimeMs, upBuf);
    const std::string_view fw = viewOf(vm.fwVer);
    const std::string_view sn = viewOf(vm.sn);
    const std::string_view dev = viewOf(vm.deviceName);

    std::array<SubListRow, kSubDeviceRowCount> rows{{
        makeRow(icons::kRowInfo7x8.data(), "FW", fw.empty() ? "--" : fw),
        makeRow(icons::kRowInfo7x8.data(), "SN", sn.empty() ? "--" : sn),
        makeRow(icons::kRowRf7x8.data(), "", dev.empty() ? "--" : dev),
        makeRow(icons::kRowClock7x8.data(), "UP", viewOf(upBuf)),
        makeBackRow(),
    }};
    drawSubList(canvas, state, rows);
}

/**
 * @brief 渲染电源子页（左主卡 5 格电池图标，右列表：电压、充电状态、USB、Power off、Back）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderSubPower(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    const bool full =
        (vm.batteryPct != board::kBatteryPctUnknown) && (vm.batteryPct >= kMaxPercent);
    const bool effChg = isEffectiveCharging(vm);
    const char *cardSub = effChg ? "CHG" : (full ? "FULL" : (vm.usbPresent ? "USB" : "BAT"));
    std::array<char, kShortBufBytes> voltBuf{};
    formatBatteryVoltage(vm.batteryMv, voltBuf);
    // 主卡：16×16 电池 5 格 + 状态词 + 电压（不显示百分比数字，OL7）。
    drawSubMainCard(canvas, state, nullptr, cardSub, viewOf(voltBuf), true, vm);
    const char *chgState = "NO IC";
    if (vm.charging) {
        chgState = "CHARGING";
    } else if (!vm.chargerPresent) {
        chgState = vm.usbPresent ? (full ? "USB FULL" : "USB PWR") : "NO IC";
    } else {
        chgState = full ? "FULL" : "IDLE";
    }
    std::array<SubListRow, kSubPowerRowCount> rows{{
        makeRow(icons::kRowVolt7x8.data(), "VOLT", viewOf(voltBuf)),
        makeRow(icons::kRowBolt7x8.data(), "CHG", chgState),
        makeRow(icons::kRowUsb7x8.data(), "USB", vm.usbPresent ? "ON" : "OFF"),
        makeRow(icons::kRowPwr7x8.data(), "Power off", ">"),
        makeBackRow(),
    }};
    drawSubList(canvas, state, rows);
}

/**
 * @brief 绘制充电待机页左侧大号 5 格电池（充电时格数循环填充，充满常亮，低电闪烁）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @note 仅 renderChargeStandby 调用，不分配堆。
 */
void drawStandbyBattery(Canvas &canvas, const UiRenderState &state) noexcept
{
    WouoUI_CanvasDrawRBoxEmpty(&canvas, kStandbyBatX, kStandbyBatY, kStandbyBatW, kStandbyBatH, 2U);
    WouoUI_CanvasDrawRBox(&canvas, kStandbyBatTipX, kStandbyBatTipY, kStandbyBatTipW,
                          kStandbyBatTipH, 0U);
    const uint8_t lit = visibleBars(state.batBars);
    for (uint8_t i = 0U; i < lit; ++i) {
        WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(kStandbySegX0 + (i * kStandbySegStepX)),
                              kStandbySegY, kStandbySegW, kStandbySegH, 1U);
    }
}

/**
 * @brief 渲染重做后的充电待机页（左侧大号 5 格电池图标+状态/电压，右侧保持环+HOLD 2s ON，底行 FW/HW）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 充电中动画 <= 2 fps，充满停止保持 0 写入。
 */
void renderChargeStandby(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    const bool full =
        (vm.batteryPct != board::kBatteryPctUnknown) && (vm.batteryPct >= kMaxPercent);
    const char *statusWord =
        isEffectiveCharging(vm) ? "CHG" : (full ? "FULL" : (vm.usbPresent ? "USB" : "BAT"));
    drawText(canvas, kStandbyStatusX, kStandbyStatusY, Font_6_8, statusWord);
    if (vm.batteryMv > 0U) {
        std::array<char, kShortBufBytes> voltBuf{};
        formatBatteryVoltage(vm.batteryMv, voltBuf);
        drawText(canvas, kStandbyVoltX, kStandbyStatusY, Font_6_8, viewOf(voltBuf));
    }

    drawStandbyBattery(canvas, state);

    // 右侧电源图标 + 保持开机实时进度环 + HOLD 2s ON。
    drawProgressRing(canvas, kStandbyRingCx, kStandbyRingCy, state.standbyRingQ10);
    WouoUI_CanvasDrawBMP(&canvas, static_cast<int16_t>(kStandbyRingCx - 8),
                         static_cast<int16_t>(kStandbyRingCy - 8), kDockIconSize, kDockIconSize,
                         icons::kDockPwr16x16.data(), 1U);
    if (vm.keyHold.standbyConfirm || (state.rebootBanner == RebootBanner::kStarting)) {
        drawText(canvas, kStandbyStartingX, kStandbyHintY, Font_6_8, "STARTING");
    } else {
        drawText(canvas, kStandbyHintX, kStandbyHintY, Font_6_8, "HOLD 2s ON");
    }

    // 底部一行 FW / HW。
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kStandbyFooterLineY);
    std::array<char, kMaxLineChars + 1U> footer{};
    const std::string_view fw = viewOf(vm.fwVer);
    const std::string_view hw = viewOf(vm.hwVer);
    static_cast<void>(std::snprintf(
        footer.data(), footer.size(), "FW %.*s  HW %.*s",
        static_cast<int>(fw.empty() ? 2U : fw.size()), fw.empty() ? "--" : fw.data(),
        static_cast<int>(hw.empty() ? 2U : hw.size()), hw.empty() ? "--" : hw.data()));
    drawCentered(canvas, kStandbyFooterTextY, Font_6_8, viewOf(footer));
}

/**
 * @brief 绘制关机收尾三个小图标勾选里程碑（保存 / 射频 / 外设断电）
 * @param canvas: 目标画布
 * @param vm: 视图数据
 * @note 仅 renderShutdown 调用。
 */
void drawShutdownMilestones(Canvas &canvas, const ViewModel &vm) noexcept
{
    const bool saveOk =
        (vm.shutdownDoneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed)) != 0U;
    const bool saveDeg =
        (vm.shutdownDegradedMask &
         board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout)) != 0U;
    const bool rfOk = (vm.shutdownDoneMask &
                       board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced)) != 0U;
    const bool rfDeg =
        (vm.shutdownDegradedMask &
         board::shutdownDegradedBit(board::ShutdownDegradedBit::kRadioTimeout)) != 0U;
    const bool safeOk = (vm.shutdownDoneMask &
                         board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe)) != 0U;
    const bool safeDeg =
        (vm.shutdownDegradedMask &
         board::shutdownDegradedBit(board::ShutdownDegradedBit::kPeripheralsFailed)) != 0U;
    const bool cutOk =
        (vm.shutdownDoneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower)) != 0U;

    struct Milestone {
        const uint8_t *icon;
        bool ok;
        bool deg;
    };
    const std::array<Milestone, 3U> items{{
        {icons::kRowSave7x8.data(), saveOk, saveDeg},
        {icons::kRowRf7x8.data(), rfOk, rfDeg},
        {icons::kRowPwr7x8.data(), safeOk && cutOk && !safeDeg, safeDeg},
    }};
    constexpr int16_t kStepW = 26;
    for (size_t i = 0U; i < items.size(); ++i) {
        const auto bx = static_cast<int16_t>(kShutRightX + (static_cast<int16_t>(i) * kStepW));
        WouoUI_CanvasDrawBMP(&canvas, bx, kShutStepsY, kIcon7W, kIcon8H, items[i].icon, 1U);
        const auto markX = static_cast<int16_t>(bx + 9);
        if (items[i].deg) {
            WouoUI_CanvasDrawBMP(&canvas, markX, kShutStepsY, kIcon7W, kIcon8H,
                                 icons::kCross7x8.data(), 1U);
        } else if (items[i].ok) {
            WouoUI_CanvasDrawBMP(&canvas, markX, kShutStepsY, kIcon7W, kIcon8H,
                                 icons::kCheck7x8.data(), 1U);
        } else {
            WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(markX + 2),
                                  static_cast<int16_t>(kShutStepsY + kShutPendingDotOffY), 2, 2,
                                  0U);
        }
    }
}

/**
 * @brief 渲染关机页（首行原因 + 终态，左侧进度环，右侧阶段名、进度条与三图标勾选里程碑）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆，无浮点。
 */
void renderShutdown(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    const bool usbFinal =
        (vm.shutdownDoneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady)) != 0U;
    const bool batteryFinal =
        (vm.shutdownDoneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kBatteryOffReady)) !=
        0U;
    const char *const reasonStr = shutdownReasonText(vm.shutdownReason);
    const char *const rightTag = usbFinal ? "RESTART" : (batteryFinal ? "POWER OFF" : "SHUTDOWN");
    if (vm.shutdownReason == board::ShutdownReason::kNone) {
        drawCentered(canvas, kHeaderTextY, Font_6_8, rightTag);
    } else {
        drawText(canvas, 1, kHeaderTextY, Font_6_8, reasonStr);
        const uint16_t rw = WouoUI_GetStrWidth(rightTag, Font_6_8);
        const int16_t rx = (rw < kWidth) ? static_cast<int16_t>(kWidth - 1 - rw) : int16_t{0};
        drawText(canvas, rx, kHeaderTextY, Font_6_8, rightTag);
    }
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    const uint8_t pct = std::min(state.shutdownPct, kMaxPercent);
    const uint16_t ringQ10 =
        static_cast<uint16_t>((static_cast<uint32_t>(pct) * kEaseScale) / kMaxPercent);
    drawProgressRing(canvas, kRingCx, kRingCy, ringQ10);

    std::array<char, 8U> pctBuf{};
    static_cast<void>(
        std::snprintf(pctBuf.data(), pctBuf.size(), "%u%%", static_cast<unsigned>(pct)));
    const uint16_t pw = WouoUI_GetStrWidth(pctBuf.data(), Font_6_8);
    const int16_t pctX = static_cast<int16_t>(kRingCx - static_cast<int16_t>(pw / 2U));
    drawText(canvas, pctX, kShutPctY, Font_6_8, viewOf(pctBuf));

    drawText(canvas, kShutRightX, kShutStageY, Font_6_8,
             shutdownStageText(vm.shutdownStage, vm.shutdownDoneMask, vm.shutdownDegradedMask,
                               vm.radioMode));
    WouoUI_CanvasDrawRBoxEmpty(&canvas, kShutRightX, kShutBarY, kShutBarW, 8, 2U);
    const auto barW =
        static_cast<int16_t>((static_cast<uint32_t>(pct) * kShutFillMaxW) / kMaxPercent);
    if (barW > 0) {
        WouoUI_CanvasDrawRBox(&canvas, kShutFillX, kShutFillY, barW, 4, 0U);
    }

    drawShutdownMilestones(canvas, vm);
}

/**
 * @brief 绘制蓝牙配对标题、设备与数字校验码。
 * @param canvas: 目标画布。
 * @param pair: 当前配对快照。
 */
void drawPairPrompt(Canvas &canvas, const OledPairingStatus &pair) noexcept
{
    drawCentered(canvas, kPairModalTitleY, Font_6_8, "PAIR REQUEST");
    std::array<char, kShortBufBytes> peer{};
    static_cast<void>(std::snprintf(
        peer.data(), peer.size(), "%02X:%02X:%02X", static_cast<unsigned>(pair.suffix[0]),
        static_cast<unsigned>(pair.suffix[1]), static_cast<unsigned>(pair.suffix[2])));
    drawCentered(canvas, kPairModalPeerY, Font_6_8,
                 pair.name[0] != '\0' ? viewOf(pair.name) : viewOf(peer));
    if (pair.hasNumber) {
        static_cast<void>(
            std::snprintf(peer.data(), peer.size(), "%06u", static_cast<unsigned>(pair.number)));
        drawCentered(canvas, kPairModalNumberY, Font_8_16, viewOf(peer));
    }
}

/**
 * @brief 画出静态弹窗边框和提示。
 * @param canvas: 目标画布。
 * @param modal: 弹窗类型。
 * @param pair: 配对信息，用于配对类型。
 */
void drawModalPrompt(Canvas &canvas, UiModal modal, const OledPairingStatus &pair) noexcept
{
    WouoUI_CanvasDrawRBoxEmpty(&canvas, 1, 1, static_cast<int16_t>(kCanvasW - 2),
                               static_cast<int16_t>(kCanvasH - 2), kModalCornerR);
    switch (modal) {
    case UiModal::kPowerOffConfirm: {
        constexpr int16_t kTitleGroupW = 7 + 4 + 60; // icon 7 + gap 4 + 10 chars * 6
        const int16_t gx = static_cast<int16_t>((kCanvasW - kTitleGroupW) / 2);
        WouoUI_CanvasDrawBMP(&canvas, gx, kPwrModalTitleY, kIcon7W, kIcon8H,
                             icons::kRowPwr7x8.data(), 1U);
        drawText(canvas, static_cast<int16_t>(gx + kPwrModalTextOffX), kPwrModalTitleY, Font_6_8,
                 "POWER OFF?");
        break;
    }
    case UiModal::kWifiConfirm:
        drawCentered(canvas, kWifiModalTitleY, Font_6_8, "SWITCH TO WIFI?");
        drawCentered(canvas, kWifiModalWarningY, Font_6_8, "BT WILL DISCONNECT");
        break;
    case UiModal::kPair:
        drawPairPrompt(canvas, pair);
        break;
    case UiModal::kNone:
    default:
        break;
    }
}

/**
 * @brief 清空一个弹窗页内指定的像素行，保留外框。
 * @param page: 128 列页缓冲。
 * @param keepMask: 每列应保留的像素位。
 */
void clearModalPage(std::span<uint8_t, kWidth> page, uint8_t keepMask) noexcept
{
    for (auto &byte : page.subspan(kModalInsideX, kWidth - 2U * kModalInsideX)) {
        byte &= keepMask;
    }
}

/**
 * @brief 在选项区画出选项标签及选择框（关机弹窗为单个固定选项，其余为两项随焦点弹簧移动）。
 * @param canvas: 目标画布。
 * @param state: 弹窗类型、焦点像素与挤压量。
 */
void drawModalChoices(Canvas &canvas, const UiRenderState &state) noexcept
{
    if (state.modal == UiModal::kPowerOffConfirm) {
        // OL7：只有一个居中的确认项，选择框固定不动。
        Canvas single = canvas;
        single.start_x = kModalSingleButtonX;
        single.w = kModalSingleButtonW;
        drawCentered(single, kModalButtonLabelY, Font_6_8, "POWER OFF");
        WouoUI_CanvasDrawRBoxEmpty(&canvas, kModalSingleButtonX, kModalButtonY, kModalSingleButtonW,
                                   kModalButtonH, kModalCornerR);
        return;
    }
    const bool pairing = state.modal == UiModal::kPair;
    Canvas label = canvas;
    label.start_x = kModalButtonX;
    label.w = kModalButtonW;
    drawCentered(label, kModalButtonLabelY, Font_6_8, pairing ? "Reject" : "Cancel");
    label.start_x += kModalOptionStepX;
    drawCentered(label, kModalButtonLabelY, Font_6_8, pairing ? "Accept" : "OK");
    const int16_t inset = state.squishInset;
    const int16_t x = static_cast<int16_t>(kModalButtonX + state.modalFocusX + inset);
    WouoUI_CanvasDrawRBoxEmpty(&canvas, x, static_cast<int16_t>(kModalButtonY + inset),
                               static_cast<int16_t>(kModalButtonW - 2 * inset),
                               static_cast<int16_t>(kModalButtonH - 2 * inset), kModalCornerR);
}

/**
 * @brief 绘制关机确认剩余时间的进度条和秒数。
 * @param canvas: 目标画布。
 * @param remainMs: 关机确认剩余时间。
 */
void drawPowerConfirmTimer(Canvas &canvas, uint32_t remainMs) noexcept
{
    WouoUI_CanvasDrawRBoxEmpty(&canvas, kPwrModalBarX, kPwrModalBarY, kPwrModalBarW, kPwrModalBarH,
                               2U);
    const uint32_t rem = std::min(remainMs, kPowerOffConfirmMs);
    const auto fillW = static_cast<int16_t>(rem * (kPwrModalBarW - 4) / kPowerOffConfirmMs);
    if (fillW > 0) {
        WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(kPwrModalBarX + 2),
                              static_cast<int16_t>(kPwrModalBarY + 2), fillW, 4, 0U);
    }
    std::array<char, kShortBufBytes> label{};
    const unsigned seconds = (rem + kMsPerSec - 1U) / kMsPerSec;
    static_cast<void>(std::snprintf(label.data(), label.size(), "%us", seconds));
    drawText(canvas, kPwrModalSecX, kPwrModalBarY, Font_6_8, viewOf(label));
}

/**
 * @brief 判断配对弹窗标题区是否需要重绘。
 * @param a: 上次渲染的配对快照。
 * @param b: 当前配对快照。
 * @retval true 显示所需字段相同。
 */
[[nodiscard]] bool samePairPrompt(const OledPairingStatus &a, const OledPairingStatus &b) noexcept
{
    return a.id == b.id && a.name == b.name && a.suffix == b.suffix && a.hasNumber == b.hasNumber &&
           a.number == b.number;
}

/**
 * @brief 复用弹窗静态区，仅重绘变化的选项页与关机倒计时页。
 * @param canvas: 目标画布。
 * @param state: 渲染状态。
 * @param pair: 当前配对快照。
 * @param fb: 唯一帧缓冲，缓存对应此帧。
 * @param previous: 同一缓冲的弹窗缓存，可为空。
 */
void renderModal(Canvas &canvas, const UiRenderState &state, const OledPairingStatus &pair,
                 ScreenBuff &fb, ModalFrame *previous) noexcept
{
    const bool reuse = previous != nullptr && previous->modal == state.modal &&
                       (state.modal != UiModal::kPair || samePairPrompt(previous->pairing, pair));
    if (reuse) {
        clearModalPage(fb[kModalButtonPage], kModalClearPage);
        clearModalPage(fb[kModalButtonTailPage], kModalButtonTailKeep);
    } else {
        WouoUI_BuffClear();
        drawModalPrompt(canvas, state.modal, pair);
    }
    if (state.modal == UiModal::kPowerOffConfirm) {
        clearModalPage(fb[kModalTimerPage], kModalClearPage);
        drawPowerConfirmTimer(canvas, state.powerOffRemainMs);
    }
    drawModalChoices(canvas, state);
    if (previous != nullptr) {
        previous->modal = state.modal;
        if (state.modal == UiModal::kPair) {
            previous->pairing = pair;
        }
    }
}

/**
 * @brief 渲染软件重启/射频切换过渡提示帧（重启前刷出并静默 I2C，避免中断事务）
 * @param canvas: 目标画布
 * @param banner: 过渡提示类型
 * @note 不分配堆。
 */
void renderRebootBanner(Canvas &canvas, RebootBanner banner) noexcept
{
    WouoUI_CanvasDrawRBoxEmpty(&canvas, 1, 1, static_cast<int16_t>(kCanvasW - 2),
                               static_cast<int16_t>(kCanvasH - 2), kModalCornerR);
    const uint8_t *icon = icons::kDockPwr16x16.data();
    const char *title = "REBOOTING...";
    switch (banner) {
    case RebootBanner::kSwitchingWifi:
        icon = icons::kCardWifi16x16.data();
        title = "SWITCHING WI-FI";
        break;
    case RebootBanner::kSwitchingBt:
        icon = icons::kCardBt16x16.data();
        title = "SWITCHING BT";
        break;
    case RebootBanner::kStarting:
        icon = icons::kDockPwr16x16.data();
        title = "STARTING...";
        break;
    case RebootBanner::kRebooting:
    case RebootBanner::kKeepFrame:
    case RebootBanner::kNone:
    default:
        break;
    }
    WouoUI_CanvasDrawBMP(&canvas, static_cast<int16_t>((kCanvasW - kDockIconSize) / 2),
                         kRebootIconY, kDockIconSize, kDockIconSize, icon, 1U);
    drawCentered(canvas, kRebootTitleY, Font_6_8, title);
    drawCentered(canvas, kRebootSubY, Font_6_8, "PLEASE WAIT");
}

/**
 * @brief 在当前页面上方叠加长按关机进度浮层（600..2500 ms 线性填充，松开 150 ms 收缩收起）
 * @param state: 渲染状态
 * @note 仅 renderUi 调用，不分配堆。
 */
void renderHoldOverlay(const UiRenderState &state) noexcept
{
    int16_t h = kHoldBoxH;
    int16_t w = kHoldBoxW;
    if (state.holdOverlayCollapsing) {
        h = lerpFixed(kHoldBoxH, 4, state.holdCollapseQ10);
        w = lerpFixed(kHoldBoxW, kHoldCollapseMinW, state.holdCollapseQ10);
    }
    const int16_t x = static_cast<int16_t>((kCanvasW - w) / 2);
    const int16_t y = static_cast<int16_t>(kHoldBoxY + ((kHoldBoxH - h) / 2));

    Canvas full{.start_x = 0, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    WouoUI_GraphSetPenColor(0U);
    WouoUI_CanvasDrawRBox(&full, x, y, w, h, kModalCornerR);
    WouoUI_GraphSetPenColor(1U);
    WouoUI_CanvasDrawRBoxEmpty(&full, x, y, w, h, kModalCornerR);

    if (h < kHoldContentMinH) {
        return;
    }
    Canvas box{.start_x = x, .start_y = y, .w = w, .h = h};
    drawCentered(box, kHoldTitleY, Font_6_8, "HOLD TO POWER OFF");
    WouoUI_CanvasDrawBMP(&box, 8, kHoldIconY, kDockIconSize, kDockIconSize,
                         icons::kDockPwr16x16.data(), 1U);
    constexpr int16_t kBarX = 30;
    constexpr int16_t kBarY = 24;
    constexpr int16_t kBarW = 78;
    constexpr int16_t kBarH = 8;
    WouoUI_CanvasDrawRBoxEmpty(&box, kBarX, kBarY, kBarW, kBarH, 2U);
    const auto fillW = static_cast<int16_t>(
        (static_cast<uint32_t>(std::min(state.holdProgressQ10, kEaseScale)) * (kBarW - 4)) /
        kEaseScale);
    if (fillW > 0) {
        WouoUI_CanvasDrawRBox(&box, static_cast<int16_t>(kBarX + 2),
                              static_cast<int16_t>(kBarY + 2), fillW, 4, 0U);
    }
}

/**
 * @brief 将指定页面绘制到任意偏移与剪裁画布
 * @param canvas: 目标画布
 * @param page: 目标页面枚举
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 供静态渲染和过渡动画复用，不分配堆。
 */
void renderPageInto(Canvas &canvas, OledPage page, const UiRenderState &state,
                    const ViewModel &vm) noexcept
{
    switch (page) {
    case OledPage::kSystemCheck:
        renderSystemCheck(canvas, state, vm);
        break;
    case OledPage::kDialplate:
        renderDialplate(canvas, state, vm);
        break;
    case OledPage::kSubSat:
        renderSubSat(canvas, state, vm);
        break;
    case OledPage::kSubPos:
        renderSubPos(canvas, state, vm);
        break;
    case OledPage::kSubComm:
        renderComm(canvas, state, vm);
        break;
    case OledPage::kSubDevice:
        renderSubDevice(canvas, state, vm);
        break;
    case OledPage::kSubPower:
        renderSubPower(canvas, state, vm);
        break;
    case OledPage::kChargeStandby:
        renderChargeStandby(canvas, state, vm);
        break;
    case OledPage::kShutdown:
        renderShutdown(canvas, state, vm);
        break;
    default:
        renderDialplate(canvas, state, vm);
        break;
    }
}

/**
 * @brief 渲染开机滑动视差过渡（旧页左移 1/3 视差，新页从右侧滑入）。
 * @param trans: 过渡动画状态。
 * @param state: 渲染状态。
 * @param vm: 视图数据。
 * @param fb: 当前帧缓冲。
 * @note 仅 renderUi 调用。
 */
void renderSlideLeft(const PageTransition &trans, const UiRenderState &state, const ViewModel &vm,
                     ScreenBuff &fb) noexcept
{
    const uint16_t prog = trans.progressQ10(state.nowMs);
    const auto pose = trans.pose(state.nowMs, {0, 0, kCanvasW, kCanvasH});
    const int16_t newX = pose.viewport.x;
    const int16_t oldX = lerpFixed(0, -kParallaxShiftPx, prog);

    Canvas oldCanvas{.start_x = oldX, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    renderPageInto(oldCanvas, trans.fromPage, state, vm);

    // 新页覆盖整个列高，以页内连续清零代替逐像素填充。
    for (auto &page : fb) {
        // newX 由屏宽插值限定在 0..kWidth。
        std::ranges::fill(std::span{page}.subspan(static_cast<size_t>(newX)), 0U);
    }
    Canvas full{.start_x = 0, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    if (newX > 0) {
        WouoUI_CanvasDrawLine_V(&full, newX, 0, static_cast<int16_t>(kCanvasH - 1));
    }

    Canvas newCanvas{.start_x = newX,
                     .start_y = pose.contentOffsetY,
                     .w = kCanvasW,
                     .h = static_cast<int16_t>(kCanvasH - pose.contentOffsetY)};
    drawStatusBar(newCanvas, state, vm);
    renderPageInto(newCanvas, trans.toPage, state, vm);
}

constexpr size_t kRowsPerPage = 8U;
constexpr size_t kHeaderTailPage = kSubBodyY / kRowsPerPage;
constexpr uint8_t kHeaderTailMask = (1U << (kSubBodyY % kRowsPerPage)) - 1U;
constexpr unsigned kBodyHeadMask = 0xFFU ^ kHeaderTailMask;

/**
 * @brief 将状态栏下方清空，保留顶栏跨页的像素位。
 * @param fb: 当前帧缓冲。
 */
void clearBody(ScreenBuff &fb) noexcept
{
    for (auto &value : fb[kHeaderTailPage]) {
        value &= kHeaderTailMask;
    }
    for (auto &page : std::span{fb}.subspan(kHeaderTailPage + 1U)) {
        std::ranges::fill(page, 0U);
    }
}

/**
 * @brief 在原地将内容下移，在状态栏之下进行裁剪。
 * @param fb: 已渲染的帧缓冲。
 * @param rows: 下移像素行数。
 */
void shiftBodyDown(ScreenBuff &fb, size_t rows) noexcept
{
    const size_t wholePages = rows / kRowsPerPage;
    const size_t bits = rows % kRowsPerPage;
    for (size_t dest = kPages - 1U; dest >= kHeaderTailPage; --dest) {
        for (size_t x = 0U; x < kWidth; ++x) {
            unsigned content = 0U;
            if (dest >= kHeaderTailPage + wholePages) {
                const size_t src = dest - wholePages;
                const unsigned low =
                    src == kHeaderTailPage ? fb[src][x] & kBodyHeadMask : fb[src][x];
                content = low << bits;
                if (bits != 0U && src > kHeaderTailPage) {
                    const unsigned high = src - 1U == kHeaderTailPage
                                              ? fb[src - 1U][x] & kBodyHeadMask
                                              : fb[src - 1U][x];
                    content |= high >> (kRowsPerPage - bits);
                }
            }
            if (dest == kHeaderTailPage) {
                content |= fb[dest][x] & kHeaderTailMask;
            }
            // 位移可能跨页，当前页只保留低 8 位。
            fb[dest][x] = static_cast<uint8_t>(content);
        }
    }
}

/**
 * @brief 从底部收起旧内容，再推入新内容，每帧只绘制当前可见页。
 * @param canvas: 内容绘图所用的全屏坐标系。
 * @param trans: 转场状态。
 * @param state: 渲染状态。
 * @param vm: 视图数据。
 * @param fb: 唯一帧缓冲，顶部 12 行在转场期间保留。
 */
void renderIconMorph(Canvas &canvas, const PageTransition &trans, const UiRenderState &state,
                     const ViewModel &vm, ScreenBuff &fb) noexcept
{
    const uint32_t half = trans.durationMs / 2U;
    const uint32_t elapsed = std::min(state.nowMs - trans.startMs, trans.durationMs);
    const bool entering = elapsed >= half;
    const uint32_t distance = entering ? trans.durationMs - elapsed : elapsed;
    const size_t rows = (kHeight - kSubBodyY) * distance / std::max<uint32_t>(half, 1U);
    if (rows < kHeight - kSubBodyY) {
        renderPageInto(canvas, entering ? trans.toPage : trans.fromPage, state, vm);
        if (rows != 0U) {
            shiftBodyDown(fb, rows);
        }
    }
}

/**
 * @brief 绘制非弹窗页面及页面切换，子页转场复用状态栏。
 * @param canvas: 全屏画布。
 * @param state: 渲染状态。
 * @param vm: 视图数据。
 * @param fb: 当前帧缓冲。
 * @param previousStatusBar: 同一缓冲保有可用的状态栏。
 * @retval 当前帧包含状态栏。
 */
[[nodiscard]] bool renderBaseUi(Canvas &canvas, const UiRenderState &state, const ViewModel &vm,
                                ScreenBuff &fb, bool previousStatusBar) noexcept
{
    const auto *trans = state.transition;
    if (trans != nullptr && trans->kind == TransitionKind::kSlideLeft) {
        WouoUI_BuffClear();
        renderSlideLeft(*trans, state, vm, fb);
        return false;
    }
    const bool showStatusBar =
        state.page >= OledPage::kDialplate && state.page <= OledPage::kSubPower;
    const bool morph = showStatusBar && trans != nullptr &&
                       (trans->kind == TransitionKind::kExpandFromIcon ||
                        trans->kind == TransitionKind::kCollapseToIcon);
    if (morph && previousStatusBar) {
        clearBody(fb);
    } else {
        WouoUI_BuffClear();
        if (showStatusBar) {
            (state.page == OledPage::kSubSat ? drawSatHeader : drawStatusBar)(canvas, state, vm);
        }
    }
    if (morph) {
        renderIconMorph(canvas, *trans, state, vm, fb);
    } else {
        renderPageInto(canvas, state.page, state, vm);
    }
    return showStatusBar;
}

} // namespace

bool renderUi(const UiRenderState &state, const ViewModel &vm, ScreenBuff &fb,
              bool previousStatusBar, ModalFrame *modalFrame) noexcept
{
    Pen pen{.color_mode = PEN_MODE_NORMAL, .color = PEN_COLOR_WHITE, .rev_color_flag = false};
    WouoUI_GraphSetBuff(&fb);
    WouoUI_GraphSetPen(&pen);
    WouoUI_GraphSetPenColor(1U);
    const bool showRebootOverlay = (state.rebootBanner != RebootBanner::kNone) &&
                                   (state.rebootBanner != RebootBanner::kKeepFrame) &&
                                   (state.page != OledPage::kChargeStandby) &&
                                   (state.page != OledPage::kShutdown);
    const bool showModal = !showRebootOverlay && state.modal != UiModal::kNone;
    Canvas full{.start_x = 0, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    bool showStatusBar = false;
    if (showRebootOverlay) {
        WouoUI_BuffClear();
        renderRebootBanner(full, state.rebootBanner);
    } else if (showModal) {
        renderModal(full, state, vm.pairing, fb, modalFrame);
    } else {
        showStatusBar = renderBaseUi(full, state, vm, fb, previousStatusBar);
    }

    if (state.holdOverlayVisible) {
        renderHoldOverlay(state);
    }
    if (modalFrame != nullptr && (!showModal || state.holdOverlayVisible)) {
        modalFrame->modal = UiModal::kNone;
    }
    // 解除栈局部画笔注册，避免 WouoUI 静态 p_cur_pen 悬垂（ROL1-06）。
    WouoUI_GraphSetPen(nullptr);
    return showStatusBar && !state.holdOverlayVisible;
}

void renderPage(OledPage page, const ViewModel &vm, ScreenBuff &fb) noexcept
{
    UiRenderState st{};
    st.page = page;
    st.dockFocusX = kDockStartX;
    st.shutdownPct = shutdownTargetPercent(vm.shutdownStage, vm.shutdownDoneMask,
                                           vm.shutdownDoneCount, vm.shutdownTotalCount);
    static_cast<void>(renderUi(st, vm, fb));
}

void formatDialRightSummary(const ViewModel &vm, std::span<char> topOut,
                            std::span<char> botOut) noexcept
{
    formatDialRightInfo(vm, topOut, botOut);
}

} // namespace app::oled
