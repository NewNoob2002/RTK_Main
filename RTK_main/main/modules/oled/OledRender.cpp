// OLED WouoUI 图形渲染器（OL2）：Logo 展开、自检列表、表盘、四子菜单、关机进度环与页面过渡动画。
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
constexpr uint16_t kCmPerMeter = 100U;
constexpr uint16_t kExpandRevealHalf = kEaseScale / 2U;

// 通用顶栏/布局像素常量。
constexpr int16_t kHeaderTextY = 1;
constexpr int16_t kHeaderLineY = 11;
constexpr int16_t kDockStartX = 2;
constexpr int16_t kDockStepX = 32;
constexpr int16_t kDockIconOffsetX = 7; // (28 - 14) / 2
constexpr int16_t kDockY = 44;
constexpr int16_t kDockW = 28;
constexpr int16_t kDockH = 19;
constexpr int16_t kDockIconY = 46;
constexpr int16_t kDockIconSize = 14;
constexpr uint8_t kDockCornerR = 3U;
constexpr int16_t kParallaxShiftPx = 42;
constexpr size_t kRollBufBytes = 12U;
constexpr size_t kShortBufBytes = 16U;
constexpr size_t kGnssTagBufBytes = 13U;
constexpr size_t kWaitBufBytes = 12U;
constexpr size_t kSubSatHdrBufBytes = 16U;

// 电池图标与状态栏像素常量。
constexpr int16_t kBatBoxW = 16;
constexpr int16_t kBatBoxH = 8;
constexpr int16_t kBatTipX1 = 16;
constexpr int16_t kBatTipX2 = 17;
constexpr int16_t kBatTipY0 = 2;
constexpr int16_t kBatTipY1 = 5;
constexpr uint32_t kBatFillMaxW = 12U;
constexpr int16_t kBatFillH = 4;
constexpr int16_t kStatusSatTextX = 10;
constexpr int16_t kStatusWarnX = 28;
constexpr int16_t kIcon7W = 7;
constexpr int16_t kIcon8H = 8;
constexpr int16_t kStatusWifiX = 44;
constexpr int16_t kStatusWifiApX = 54;
constexpr int16_t kStatusBtX = 46;
constexpr int16_t kStatusBoltX = 76;
constexpr int16_t kBoltW = 5;
constexpr int16_t kStatusPctUnknownX = 84;
constexpr int16_t kStatusPct3DigitX = 83;
constexpr int16_t kStatusPct2DigitX = 89;
constexpr int16_t kStatusPct1DigitX = 95;
constexpr uint8_t kTwoDigitMin = 10U;
constexpr int16_t kStatusBatIconX = 109;

// 自检页像素常量。
constexpr uint8_t kLogoBoxR = 3U;
constexpr int16_t kLogoTitleY = 10;
constexpr int16_t kLogoLineX0 = 18;
constexpr int16_t kLogoLineInset = 19;
constexpr int16_t kLogoLineY = 38;
constexpr int16_t kLogoFwY = 44;
constexpr int16_t kCheckFracX = 106;
constexpr int16_t kCheckRowStartY = 13;
constexpr int16_t kCheckRowStepY = 8;
constexpr int16_t kCheckLabelX = 4;
constexpr int16_t kCheckGnssInfoX = 44;
constexpr int16_t kCheckFailX = 82;
constexpr int16_t kCheckWaitX = 96;
constexpr int16_t kCheckIconX = 118;
constexpr int16_t kCheckSpinX = 119;
constexpr int16_t kCheckBarY = 55;
constexpr int16_t kCheckBarW = 124;
constexpr int16_t kCheckBarH = 7;
constexpr int16_t kCheckBarFillY = 57;
constexpr int16_t kCheckBarFillH = 3;
constexpr uint32_t kCheckBarFillMaxW = 120U;
constexpr uint32_t kSpinStepMs = 100U;
constexpr uint32_t kMsPerSec = 1000U;
constexpr size_t kCheckIdxGnss = 4U;

// 表盘与子菜单像素常量。
constexpr int16_t kDialBadgeY = 15;
constexpr int16_t kDialInfoX = 72;
constexpr int16_t kDialInfoTopY = 17;
constexpr int16_t kDialInfoBotY = 29;
constexpr int16_t kSubSatTitleX = 11;
constexpr int16_t kSubSatColStartX = 6;
constexpr int16_t kSubSatColStepX = 31;
constexpr int16_t kSubSatCountY = 14;
constexpr int16_t kSubSatVisBarW = 14;
constexpr int16_t kSubSatUsedBarOffX = 6;
constexpr int16_t kSubSatUsedBarW = 10;
constexpr int16_t kSubSatLabelY = 55;
constexpr int16_t kSubSatNoDataY = 30;
constexpr int16_t kSubSnrStartX = 4;
constexpr int16_t kSubSnrStepX = 15;
constexpr int16_t kSubSnrBarW = 10;
constexpr int16_t kSubSnrLetterOffX = 3;
constexpr int16_t kSubLine0Y = 15;
constexpr int16_t kSubLine1Y = 27;
constexpr int16_t kSubLine2Y = 39;
constexpr int16_t kSubLine3Y = 51;
constexpr int16_t kSubPwrUsbX = 86;
constexpr int16_t kSubPwrVoltX = 74;
constexpr int16_t kSubPwrVoltY = 20;
constexpr int16_t kSubPwrBarY = 36;
constexpr int16_t kSubPwrBarW = 112;
constexpr int16_t kSubPwrBarH = 10;
constexpr int16_t kSubPwrFillX = 10;
constexpr int16_t kSubPwrFillY = 38;
constexpr int16_t kSubPwrFillH = 6;
constexpr uint32_t kSubPwrFillMaxW = 108U;
constexpr int16_t kSubPwrChgY = 52;

// 充电待机与关机页像素常量。
constexpr int16_t kStandbyPctX = 14;
constexpr int16_t kStandbyPctY = 17;
constexpr int16_t kStandbyVoltX = 76;
constexpr int16_t kStandbyVoltY = 21;
constexpr int16_t kStandbyBarX = 14;
constexpr int16_t kStandbyBarY = 39;
constexpr int16_t kStandbyBarW = 100;
constexpr int16_t kStandbyBarH = 10;
constexpr int16_t kStandbyFillY = 41;
constexpr int16_t kStandbyFillH = 6;
constexpr uint32_t kStandbyFillMaxW = 96U;
constexpr int16_t kStandbyHintY = 54;
constexpr int16_t kRingCx = 24;
constexpr int16_t kRingCy = 38;
constexpr int16_t kRingDotSize = 3;
constexpr int16_t kShutPctY = 34;
constexpr int16_t kShutRightX = 48;
constexpr int16_t kShutStageY = 20;
constexpr int16_t kShutBarY = 34;
constexpr int16_t kShutBarW = 76;
constexpr int16_t kShutFillX = 50;
constexpr int16_t kShutFillY = 36;
constexpr uint32_t kShutFillMaxW = 72U;
constexpr int16_t kShutStepsY = 48;

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
 * @param roll: 数字滚动器指针（允许 nullptr）
 * @param fallbackValue: 无滚动器时显示的数值
 * @param suffix: 紧随数值的后缀（如 "%" 或 ""）
 * @param nowMs: 当前单调时间（ms）
 * @note 纯整数定点计算，不分配堆。
 */
void drawRollingValue(const Canvas &parent, int16_t x, int16_t y, const sFONT &font,
                      const NumberRoll *roll, int32_t fallbackValue, std::string_view suffix,
                      uint32_t nowMs) noexcept
{
    std::array<char, kRollBufBytes> curBuf{};
    const int32_t curVal = (roll != nullptr) ? roll->current() : fallbackValue;
    static_cast<void>(std::snprintf(curBuf.data(), curBuf.size(), "%d%.*s",
                                    static_cast<int>(curVal), static_cast<int>(suffix.size()),
                                    suffix.data()));

    const int16_t clipH = static_cast<int16_t>(font.Height);
    const uint16_t textW = WouoUI_GetStrWidth(curBuf.data(), font);
    // 画布最大宽度不超过 128，窄化安全。
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
 * @brief 绘制 18x8 单色电池图形图标（含右端正极凸起与内部填充条）
 * @param canvas: 目标画布
 * @param x: 左上角 x
 * @param y: 左上角 y
 * @param pct: 电量百分比（0..100 或 kBatteryPctUnknown）
 * @note 不分配堆。
 */
void drawBatteryIcon(Canvas &canvas, int16_t x, int16_t y, uint8_t pct) noexcept
{
    WouoUI_CanvasDrawRBoxEmpty(&canvas, x, y, kBatBoxW, kBatBoxH, 1U);
    WouoUI_CanvasDrawLine_V(&canvas, static_cast<int16_t>(x + kBatTipX1),
                            static_cast<int16_t>(y + kBatTipY0),
                            static_cast<int16_t>(y + kBatTipY1));
    WouoUI_CanvasDrawLine_V(&canvas, static_cast<int16_t>(x + kBatTipX2),
                            static_cast<int16_t>(y + kBatTipY0),
                            static_cast<int16_t>(y + kBatTipY1));
    if (pct == board::kBatteryPctUnknown) {
        return;
    }
    const uint8_t clamped = std::min(pct, kMaxPercent);
    // 内宽 12 px，clamped <= 100，乘法与窄化均安全。
    const auto fillW =
        static_cast<int16_t>((static_cast<uint32_t>(clamped) * kBatFillMaxW) / kMaxPercent);
    if (fillW > 0) {
        WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(x + 2), static_cast<int16_t>(y + 2),
                              fillW, kBatFillH, 0U);
    }
}

/**
 * @brief 绘制表盘顶部状态栏（卫星图标+数、自检告警、蓝牙/Wi-Fi、充电闪电、电量百分比与电池图标）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 仅 renderDialplate 调用。
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

    // 射频状态图标：Wi-Fi 模式画 Wi-Fi 扇形，蓝牙模式区分已连接/待机。
    if (vm.radioMode == RadioMode::kWifiConfig) {
        WouoUI_CanvasDrawBMP(&canvas, kStatusWifiX, kHeaderTextY, kIcon8H, kIcon8H,
                             icons::kWifi8x8.data(), 1U);
        drawText(canvas, kStatusWifiApX, kHeaderTextY, Font_6_8, "AP");
    } else if (vm.btReady) {
        const uint8_t *btBmp =
            (vm.btSessions > 0U) ? icons::kBtConn8x8.data() : icons::kBtWait8x8.data();
        WouoUI_CanvasDrawBMP(&canvas, kStatusBtX, kHeaderTextY, kIcon8H, kIcon8H, btBmp, 1U);
    }

    if (vm.charging) {
        WouoUI_CanvasDrawBMP(&canvas, kStatusBoltX, kHeaderTextY, kBoltW, kIcon8H,
                             icons::kBolt5x8.data(), 1U);
    }
    if (vm.batteryPct == board::kBatteryPctUnknown) {
        drawText(canvas, kStatusPctUnknownX, kHeaderTextY, Font_6_8, "--%");
    } else {
        const uint8_t clamped = std::min(vm.batteryPct, kMaxPercent);
        const int16_t pctX =
            (clamped >= kMaxPercent)
                ? kStatusPct3DigitX
                : ((clamped >= kTwoDigitMin) ? kStatusPct2DigitX : kStatusPct1DigitX);
        drawRollingValue(canvas, pctX, kHeaderTextY, Font_6_8, state.batRoll,
                         static_cast<int32_t>(clamped), "%", state.nowMs);
    }
    drawBatteryIcon(canvas, kStatusBatIconX, kHeaderTextY, vm.batteryPct);
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
 * @brief 渲染自检列表单行（名称、GNSS 型号/固件、状态图标或 Pending 旋转动画）
 * @param canvas: 目标画布
 * @param idx: 自检项下标（0..4）
 * @param label: 左侧名称
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 仅 renderSystemCheck 调用。
 */
void renderSystemCheckRow(Canvas &canvas, size_t idx, const char *label, const UiRenderState &state,
                          const ViewModel &vm) noexcept
{
    constexpr std::array<char, 4U> kSpinner{'|', '/', '-', '\\'};
    const SystemCheckSm *sm = state.checkSm;
    // idx < 5，行坐标在 13..45，转 int16_t 安全。
    const auto rowY =
        static_cast<int16_t>(kCheckRowStartY + (static_cast<int16_t>(idx) * kCheckRowStepY));
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
        WouoUI_CanvasDrawBMP(&canvas, kCheckIconX, rowY, kIcon7W, kIcon8H, icons::kCheck7x8.data(),
                             1U);
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
 * @brief 渲染开机自检页（Logo 逐列展开 -> 5 项实时状态列表与底部进度条）
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
    for (size_t i = 0U; i < kCheckItemCount; ++i) {
        renderSystemCheckRow(canvas, i, kNames[i], state, vm);
    }

    WouoUI_CanvasDrawRBoxEmpty(&canvas, 2, kCheckBarY, kCheckBarW, kCheckBarH, 1U);
    const auto fillW = static_cast<int16_t>((static_cast<uint32_t>(doneCount) * kCheckBarFillMaxW) /
                                            kCheckItemCount);
    if (fillW > 0) {
        WouoUI_CanvasDrawRBox(&canvas, 4, kCheckBarFillY, fillW, kCheckBarFillH, 0U);
    }
}

/**
 * @brief 渲染主表盘（顶栏 + 中部大号解状态/精度 + 底部 4 图标入口与弹簧选中框）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderDialplate(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    drawStatusBar(canvas, state, vm);

    const char *badge = dialHeroBadge(vm);
    drawText(canvas, 2, kDialBadgeY, Font_12_24, badge);

    std::array<char, kShortBufBytes> rightTop{};
    std::array<char, kShortBufBytes> rightBot{};
    if (vm.workModeActive && (vm.workMode != WorkModeKind::kRover)) {
        static_cast<void>(std::snprintf(rightTop.data(), rightTop.size(), "%us",
                                        static_cast<unsigned>(vm.workModeElapsedS)));
        sanitizeCopy(vm.workModeState == WorkModeState::kBaseTx ? "TX RTCM" : "BASE", rightBot);
    } else if (vm.gnssEnabled && isGnssRunningLike(vm.gnssState)) {
        if (vm.hAccCm != gnss::kU16Unknown) {
            static_cast<void>(std::snprintf(rightTop.data(), rightTop.size(), "%u.%02um",
                                            static_cast<unsigned>(vm.hAccCm / kCmPerMeter),
                                            static_cast<unsigned>(vm.hAccCm % kCmPerMeter)));
        } else {
            sanitizeCopy(vm.fixValid ? "ACC --" : "NO FIX", rightTop);
        }
        const unsigned used = vm.fixValid ? static_cast<unsigned>(vm.satsUsed) : 0U;
        if (vm.satsVisible > 0U) {
            static_cast<void>(std::snprintf(rightBot.data(), rightBot.size(), "SAT %u/%u", used,
                                            static_cast<unsigned>(vm.satsVisible)));
        } else {
            static_cast<void>(std::snprintf(rightBot.data(), rightBot.size(), "SAT %u", used));
        }
    } else {
        const DialGnssDetail detail = dialGnssDetail(vm);
        sanitizeCopy(detail.top, rightTop);
        sanitizeCopy(detail.bot, rightBot);
    }
    drawText(canvas, kDialInfoX, kDialInfoTopY, Font_6_8, viewOf(rightTop));
    drawText(canvas, kDialInfoX, kDialInfoBotY, Font_6_8, viewOf(rightBot));

    const std::array<const uint8_t *, kDockItemCount> kDockIcons{
        icons::kDockSat14x14.data(),
        icons::kDockPos14x14.data(),
        icons::kDockDev14x14.data(),
        icons::kDockPwr14x14.data(),
    };
    for (uint8_t i = 0U; i < kDockItemCount; ++i) {
        // i < 4，坐标位于 9..105，转 int16_t 安全。
        const auto iconX = static_cast<int16_t>(
            kDockStartX + (static_cast<int16_t>(i) * kDockStepX) + kDockIconOffsetX);
        WouoUI_CanvasDrawBMP(&canvas, iconX, kDockIconY, kDockIconSize, kDockIconSize,
                             kDockIcons[i], 1U);
    }
    WouoUI_CanvasDrawRBoxEmpty(&canvas, state.dockFocusX, kDockY, kDockW, kDockH, kDockCornerR);
    WouoUI_CanvasDrawBoxRightAngle(
        &canvas, static_cast<int16_t>(state.dockFocusX + 1), static_cast<int16_t>(kDockY + 1),
        static_cast<int16_t>(kDockW - 2), static_cast<int16_t>(kDockH - 2), kDockCornerR);
}

/**
 * @brief 渲染卫星子菜单子页 0（GPS/BDS/GLO/GAL 可见与参与解算柱状图）
 * @param canvas: 目标画布
 * @param vm: 视图数据
 * @note 仅 renderSubSat 调用。
 */
void renderSubSatSys(Canvas &canvas, const ViewModel &vm) noexcept
{
    constexpr std::array<const char *, kConstellationSlots> kLabels{"GPS", "BDS", "GLO", "GAL"};
    constexpr int16_t kBarBottomY = 52;
    constexpr int16_t kBarMaxH = 28;
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
 * @brief 渲染卫星子菜单（顶栏 + 子页 0 星座柱状图 / 子页 1 Top-8 CN0 载噪比条）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderSubSat(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
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

    if (state.satSubTab == 0U) {
        renderSubSatSys(canvas, vm);
    } else {
        renderSubSatCn0(canvas, vm);
    }
}

/**
 * @brief 渲染位置子菜单（经纬度、海拔、HDOP 与差分龄期，纯定点无浮点）
 * @param canvas: 目标画布
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderSubPos(Canvas &canvas, const ViewModel &vm) noexcept
{
    drawText(canvas, 0, kHeaderTextY, Font_6_8, "POSITION");
    const char *badge = dialHeroBadge(vm);
    const uint16_t bw = WouoUI_GetStrWidth(badge, Font_6_8);
    const int16_t bx = (bw < kWidth) ? static_cast<int16_t>(kWidth - bw) : int16_t{0};
    drawText(canvas, bx, kHeaderTextY, Font_6_8, badge);
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    std::array<char, kMaxLineChars + 1U> val{};
    std::array<char, kMaxLineChars + 1U> line{};
    formatCoordE9(vm.latE9, 'N', 'S', vm.fixValid, val);
    formatLabelValue("LAT:", viewOf(val), line);
    drawText(canvas, 0, kSubLine0Y, Font_6_8, viewOf(line));

    formatCoordE9(vm.lonE9, 'E', 'W', vm.fixValid, val);
    formatLabelValue("LON:", viewOf(val), line);
    drawText(canvas, 0, kSubLine1Y, Font_6_8, viewOf(line));

    formatAltMm(vm.altMm, vm.fixValid, val);
    formatLabelValue("ALT:", viewOf(val), line);
    drawText(canvas, 0, kSubLine2Y, Font_6_8, viewOf(line));

    formatHdopAge(vm.hdopX100, vm.diffAgeDs, vm.fixValid, line);
    drawText(canvas, 0, kSubLine3Y, Font_6_8, viewOf(line));
}

/**
 * @brief 渲染设备信息子菜单（固件版本、SN、蓝牙/AP 名、运行时长）
 * @param canvas: 目标画布
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderSubDevice(Canvas &canvas, const ViewModel &vm) noexcept
{
    drawText(canvas, 0, kHeaderTextY, Font_6_8, "DEVICE");
    const std::string_view hw = viewOf(vm.hwVer);
    if (!hw.empty()) {
        std::array<char, kHwVerBufBytes> hwClean{};
        sanitizeCopy(hw, hwClean);
        const uint16_t w = WouoUI_GetStrWidth(hwClean.data(), Font_6_8);
        const int16_t x = (w < kWidth) ? static_cast<int16_t>(kWidth - w) : int16_t{0};
        drawText(canvas, x, kHeaderTextY, Font_6_8, viewOf(hwClean));
    }
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    std::array<char, kMaxLineChars + 1U> line{};
    formatLabelValue("FW :", viewOf(vm.fwVer), line);
    drawText(canvas, 0, kSubLine0Y, Font_6_8, viewOf(line));

    formatLabelValue("SN :", viewOf(vm.sn), line);
    drawText(canvas, 0, kSubLine1Y, Font_6_8, viewOf(line));

    const char *nameTag = (vm.radioMode == RadioMode::kWifiConfig) ? "AP :" : "BT :";
    formatLabelValue(nameTag, viewOf(vm.deviceName), line);
    drawText(canvas, 0, kSubLine2Y, Font_6_8, viewOf(line));

    std::array<char, kShortBufBytes> upBuf{};
    formatUptime(vm.uptimeMs, upBuf);
    formatLabelValue("UP :", viewOf(upBuf), line);
    drawText(canvas, 0, kSubLine3Y, Font_6_8, viewOf(line));
}

/**
 * @brief 渲染电源子菜单（电量滚动数字、电压、进度条、充电态与 USB 状态）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderSubPower(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    drawText(canvas, 0, kHeaderTextY, Font_6_8, "POWER");
    drawText(canvas, kSubPwrUsbX, kHeaderTextY, Font_6_8, vm.usbPresent ? "USB:ON" : "USB:--");
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    if (vm.batteryPct == board::kBatteryPctUnknown) {
        drawText(canvas, 8, 16, Font_8_16, "--%");
    } else {
        drawRollingValue(canvas, 8, 16, Font_8_16, state.batRoll,
                         static_cast<int32_t>(std::min(vm.batteryPct, kMaxPercent)), "%",
                         state.nowMs);
    }
    std::array<char, kShortBufBytes> voltBuf{};
    formatBatteryVoltage(vm.batteryMv, voltBuf);
    drawText(canvas, kSubPwrVoltX, kSubPwrVoltY, Font_6_8, viewOf(voltBuf));

    WouoUI_CanvasDrawRBoxEmpty(&canvas, 8, kSubPwrBarY, kSubPwrBarW, kSubPwrBarH, 2U);
    if (vm.batteryPct != board::kBatteryPctUnknown) {
        const uint8_t clamped = std::min(vm.batteryPct, kMaxPercent);
        const auto fillW =
            static_cast<int16_t>((static_cast<uint32_t>(clamped) * kSubPwrFillMaxW) / kMaxPercent);
        if (fillW > 0) {
            WouoUI_CanvasDrawRBox(&canvas, kSubPwrFillX, kSubPwrFillY, fillW, kSubPwrFillH, 1U);
        }
    }

    const char *chgText =
        vm.charging
            ? "CHG:CHARGING"
            : (((vm.batteryPct != board::kBatteryPctUnknown) && (vm.batteryPct >= kMaxPercent))
                   ? "CHG:FULL"
                   : (vm.chargerPresent ? "CHG:IDLE" : "CHG:NO IC"));
    drawText(canvas, 8, kSubPwrChgY, Font_6_8, chgText);
}

/**
 * @brief 渲染充电待机页（仅电量百分比、电压、进度条与长按开机提示）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆。
 */
void renderChargeStandby(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    const char *title =
        vm.charging
            ? "CHARGING"
            : (((vm.batteryPct != board::kBatteryPctUnknown) && (vm.batteryPct >= kMaxPercent))
                   ? "BATTERY FULL"
                   : "USB STANDBY");
    drawCentered(canvas, kHeaderTextY, Font_6_8, title);
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    if (vm.batteryPct == board::kBatteryPctUnknown) {
        drawText(canvas, kStandbyPctX, kStandbyPctY, Font_8_16, "--%");
    } else {
        drawRollingValue(canvas, kStandbyPctX, kStandbyPctY, Font_8_16, state.batRoll,
                         static_cast<int32_t>(std::min(vm.batteryPct, kMaxPercent)), "%",
                         state.nowMs);
    }
    std::array<char, kShortBufBytes> voltBuf{};
    formatBatteryVoltage(vm.batteryMv, voltBuf);
    drawText(canvas, kStandbyVoltX, kStandbyVoltY, Font_6_8, viewOf(voltBuf));

    WouoUI_CanvasDrawRBoxEmpty(&canvas, kStandbyBarX, kStandbyBarY, kStandbyBarW, kStandbyBarH, 2U);
    if (vm.batteryPct != board::kBatteryPctUnknown) {
        const uint8_t clamped = std::min(vm.batteryPct, kMaxPercent);
        const auto fillW =
            static_cast<int16_t>((static_cast<uint32_t>(clamped) * kStandbyFillMaxW) / kMaxPercent);
        if (fillW > 0) {
            WouoUI_CanvasDrawRBox(&canvas, 16, kStandbyFillY, fillW, kStandbyFillH, 1U);
        }
    }
    drawCentered(canvas, kStandbyHintY, Font_6_8, "HOLD KEY TO BOOT");
}

/**
 * @brief 绘制关机收尾项状态行（S/R/P 分别区分已完成 +、超时/失败降级 ! 与未完成 .）
 * @param canvas: 目标画布
 * @param vm: 视图数据
 * @note 仅 renderShutdown 调用。
 */
void drawShutdownSteps(Canvas &canvas, const ViewModel &vm) noexcept
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
    const char sMark = saveOk ? '+' : (saveDeg ? '!' : '.');
    const char rMark = rfOk ? '+' : (rfDeg ? '!' : '.');
    const char pMark = safeDeg ? '!' : ((safeOk && cutOk) ? '+' : '.');
    std::array<char, kShortBufBytes> steps{};
    static_cast<void>(
        std::snprintf(steps.data(), steps.size(), "S:%c R:%c P:%c", sMark, rMark, pMark));
    drawText(canvas, kShutRightX, kShutStepsY, Font_6_8, viewOf(steps));
}
/**
 * @brief 渲染关机进度页（左侧定点圆环进度 + 右侧阶段文案、进度条与 USB 终帧文案）
 * @param canvas: 目标画布
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 不分配堆，无浮点。
 */
void renderShutdown(Canvas &canvas, const UiRenderState &state, const ViewModel &vm) noexcept
{
    const bool usbFinal =
        (vm.shutdownDoneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady)) != 0U;
    const char *title =
        usbFinal ? "POWERED OFF"
                 : ((vm.shutdownReason == board::ShutdownReason::kLowBattery) ? "BATTERY LOW!"
                                                                              : "SHUTTING DOWN");
    drawCentered(canvas, kHeaderTextY, Font_6_8, title);
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    const uint8_t pct = std::min(state.shutdownPct, kMaxPercent);
    const size_t litCount =
        (static_cast<size_t>(pct) * icons::kRingPts32.size()) / static_cast<size_t>(kMaxPercent);
    for (size_t i = 0U; i < icons::kRingPts32.size(); ++i) {
        const int16_t px = static_cast<int16_t>(kRingCx + icons::kRingPts32[i].dx);
        const int16_t py = static_cast<int16_t>(kRingCy + icons::kRingPts32[i].dy);
        if (i < litCount) {
            WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(px - 1),
                                  static_cast<int16_t>(py - 1), kRingDotSize, kRingDotSize, 0U);
        } else if ((i & 1U) == 0U) {
            WouoUI_CanvasDrawPoint(&canvas, px, py);
        }
    }
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

    drawShutdownSteps(canvas, vm);
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
        renderSubPos(canvas, vm);
        break;
    case OledPage::kSubDevice:
        renderSubDevice(canvas, vm);
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
 * @brief 渲染滑动视差过渡（旧页左移 1/3 视差，新页从右侧完整滑入）
 * @param trans: 过渡动画状态
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 仅 renderUi 调用。
 */
void renderSlideLeft(const PageTransition &trans, const UiRenderState &state,
                     const ViewModel &vm) noexcept
{
    const uint16_t prog = trans.progressQ10(state.nowMs);
    const int16_t newX = lerpFixed(kCanvasW, 0, prog);
    const int16_t oldX = lerpFixed(0, -kParallaxShiftPx, prog);

    Canvas oldCanvas{.start_x = oldX, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    renderPageInto(oldCanvas, trans.fromPage, state, vm);

    // 清空新页覆盖区（黑画笔）再切回白画笔绘制新页，避免旧页残影透出。
    Canvas full{.start_x = 0, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    WouoUI_GraphSetPenColor(0U);
    WouoUI_CanvasDrawRBox(&full, newX, 0, static_cast<int16_t>(kCanvasW - newX), kCanvasH, 0U);
    WouoUI_GraphSetPenColor(1U);
    if (newX > 0) {
        WouoUI_CanvasDrawLine_V(&full, newX, 0, static_cast<int16_t>(kCanvasH - 1));
    }

    Canvas newCanvas{.start_x = newX, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    renderPageInto(newCanvas, trans.toPage, state, vm);
}

/**
 * @brief 渲染图标展开/收回过渡（从表盘图标矩形展开至全屏或反向收回）
 * @param trans: 过渡动画状态
 * @param state: 渲染状态
 * @param vm: 视图数据
 * @note 仅 renderUi 调用。
 */
void renderIconMorph(const PageTransition &trans, const UiRenderState &state,
                     const ViewModel &vm) noexcept
{
    const bool expanding = (trans.kind == TransitionKind::kExpandFromIcon);
    const uint16_t rawProg = trans.progressQ10(state.nowMs);
    const uint16_t openProg = expanding ? rawProg : static_cast<uint16_t>(kEaseScale - rawProg);

    Canvas full{.start_x = 0, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    renderPageInto(full, OledPage::kDialplate, state, vm);

    const int16_t x = lerpFixed(trans.iconRect.x, 0, openProg);
    const int16_t y = lerpFixed(trans.iconRect.y, 0, openProg);
    const int16_t w = lerpFixed(trans.iconRect.w, kCanvasW, openProg);
    const int16_t h = lerpFixed(trans.iconRect.h, kCanvasH, openProg);

    WouoUI_GraphSetPenColor(0U);
    WouoUI_CanvasDrawRBox(&full, x, y, w, h, 2U);
    WouoUI_GraphSetPenColor(1U);

    if (openProg >= kExpandRevealHalf) {
        Canvas subCanvas{.start_x = x, .start_y = y, .w = w, .h = h};
        const OledPage subPage = expanding ? trans.toPage : trans.fromPage;
        renderPageInto(subCanvas, subPage, state, vm);
    }
    WouoUI_CanvasDrawRBoxEmpty(&full, x, y, w, h, 2U);
}

} // namespace

void renderUi(const UiRenderState &state, const ViewModel &vm, ScreenBuff &fb) noexcept
{
    Pen pen{.color_mode = PEN_MODE_NORMAL, .color = PEN_COLOR_WHITE, .rev_color_flag = false};
    WouoUI_GraphSetBuff(&fb);
    WouoUI_GraphSetPen(&pen);
    WouoUI_GraphSetPenColor(1U);
    WouoUI_BuffClear();

    const PageTransition *trans = state.transition;
    if ((trans != nullptr) && (trans->kind == TransitionKind::kSlideLeft)) {
        renderSlideLeft(*trans, state, vm);
    } else if ((trans != nullptr) && ((trans->kind == TransitionKind::kExpandFromIcon) ||
                                      (trans->kind == TransitionKind::kCollapseToIcon))) {
        renderIconMorph(*trans, state, vm);
    } else {
        Canvas full{.start_x = 0, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
        renderPageInto(full, state.page, state, vm);
    }
    // 解除栈局部画笔注册，避免 WouoUI 静态 p_cur_pen 悬垂（ROL1-06）。
    WouoUI_GraphSetPen(nullptr);
}

void renderPage(OledPage page, const ViewModel &vm, ScreenBuff &fb) noexcept
{
    UiRenderState st{};
    st.page = page;
    st.dockFocusX = kDockStartX;
    st.shutdownPct = shutdownTargetPercent(vm.shutdownStage, vm.shutdownDoneMask);
    renderUi(st, vm, fb);
}

} // namespace app::oled
