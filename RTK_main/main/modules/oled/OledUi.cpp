// OLED 只读页面状态机、纯整数格式化与 WouoUI 帧缓冲渲染实现（ADR-0020）。
// 仅 OLED 任务或主机测试串行调用；无 IDF/硬件依赖，不分配堆，非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/oled/OledUi.hpp"

#include <algorithm>
#include <cstdio>

namespace app::oled {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr char kMinPrintableAscii = ' ';
constexpr char kMaxPrintableAscii = '~';
constexpr int16_t kCanvasW = static_cast<int16_t>(kWidth);
constexpr int16_t kCanvasH = static_cast<int16_t>(kHeight);
constexpr uint8_t kMaxPercent = 100U;
constexpr uint32_t kMvPerVolt = 1000U;
constexpr uint32_t kMvPerCentiVolt = 10U;
constexpr uint16_t kCmPerMeter = 100U;

// 128x64 布局常量（单位：px），避免散落魔数并保证各页对齐一致。
constexpr int16_t kSplashFramePad = 2;
constexpr int16_t kSplashFrameInset = 4;
constexpr uint8_t kFrameRadius = 3U;
constexpr int16_t kSplashTitleY = 12;
constexpr int16_t kSplashRuleMargin = 16;
constexpr int16_t kSplashRuleY = 33;
constexpr int16_t kSplashVerY = 42;

constexpr int16_t kHeaderTextY = 1;
constexpr int16_t kHeaderLineY = 11;
constexpr int16_t kRow1Y = 16;
constexpr int16_t kRow2Y = 28;
constexpr int16_t kRow3Y = 40;
constexpr int16_t kRow4Y = 52;
constexpr int16_t kFooterTextY = 55;
constexpr int16_t kFooterHintX = 68;
constexpr int16_t kPageNumRightX = 108;

constexpr int16_t kStandbyPctX = 14;
constexpr int16_t kStandbyPctY = 17;
constexpr int16_t kStandbyVoltX = 76;
constexpr int16_t kStandbyVoltY = 21;
constexpr int16_t kBarX = 14;
constexpr int16_t kBarY = 39;
constexpr int16_t kBarW = 100;
constexpr int16_t kBarH = 10;
constexpr uint8_t kBarRadius = 2U;
constexpr int16_t kBarInnerPad = 2;
constexpr int16_t kBarInnerH = 6;
constexpr uint32_t kBarInnerMaxW = 96U;
constexpr int16_t kStandbyHintY = 54;

constexpr int16_t kShutdownBoxX = 4;
constexpr int16_t kShutdownBoxY = 6;
constexpr int16_t kShutdownBoxW = kCanvasW - 8;
constexpr int16_t kShutdownBoxH = kCanvasH - 12;
constexpr int16_t kShutdownLowY = 18;
constexpr int16_t kShutdownSubY = 34;
constexpr int16_t kShutdownMidY = 26;
constexpr uint8_t kAllPagesMask = 0xFFU;

/**
 * @brief 在 NUL 结尾字符数组中取得有效文本视图
 * @param buf: 字符数组
 * @retval 不含末尾 NUL 的字符串视图
 * @note 纯函数，防止缺少 NUL 时越界。
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
 * @brief 将可打印 ASCII 字符串转为无符号字节数组供 WouoUI 绘制（避免 reinterpret_cast）
 * @param src: 源文本
 * @param[out] out: NUL 结尾的无符号字节缓冲
 * @note 纯函数，越界字符替换为 '?' 防止字体表越界访问。
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
        // safe 限定在 0x20..0x7E，转为 uint8_t 无符号损失。
        out[i] = static_cast<uint8_t>(safe);
    }
    out[count] = 0U;
}

/**
 * @brief 按指定坐标绘制单行文本
 * @param canvas: 目标画布
 * @param x: 起始 x 坐标
 * @param y: 起始 y 坐标
 * @param font: WouoUI 字体描述
 * @param text: 待绘制文本
 * @note 仅 renderPage 内部调用，不分配堆。
 */
void drawText(Canvas &canvas, int16_t x, int16_t y, const sFONT &font,
              std::string_view text) noexcept
{
    std::array<uint8_t, kMaxLineChars + 1U> bytes{};
    toWouoBytes(text, bytes);
    WouoUI_CanvasDrawStr(&canvas, x, y, font, bytes.data());
}

/**
 * @brief 在画布水平居中位置绘制单行文本
 * @param canvas: 目标画布
 * @param y: 起始 y 坐标
 * @param font: WouoUI 字体描述
 * @param text: 待绘制文本
 * @note 仅 renderPage 内部调用，不分配堆。
 */
void drawCentered(Canvas &canvas, int16_t y, const sFONT &font, std::string_view text) noexcept
{
    std::array<char, kMaxLineChars + 1U> clean{};
    sanitizeCopy(text, clean);
    const uint16_t width = WouoUI_GetStrWidth(clean.data(), font);
    // width < kWidth (128)，差值右移一位结果在 0..63，转为 int16_t 安全。
    const int16_t x = (width < kWidth) ? static_cast<int16_t>((kWidth - width) / 2U) : 0;
    drawText(canvas, x, y, font, viewOf(clean));
}

/**
 * @brief 取得 GNSS 状态的短名
 * @param enabled: GNSS 是否启用
 * @param state: GNSS 控制器状态
 * @retval 静态短名
 * @note 纯函数。
 */
[[nodiscard]] const char *gnssStateShort(bool enabled, gnss::State state) noexcept
{
    if (!enabled) {
        return "OFF";
    }
    switch (state) {
    case gnss::State::kOff:
        return "OFF";
    case gnss::State::kProbing:
        return "PROBE";
    case gnss::State::kConfiguring:
        return "CFG";
    case gnss::State::kRunning:
        return "RUN";
    case gnss::State::kFailed:
        return "FAIL";
    default:
        return "UNK";
    }
}

/**
 * @brief 取得 GGA 定位质量的短名
 * @param fixValid: GGA 解是否有效
 * @param quality: GGA 质量枚举
 * @retval 静态短名
 * @note 纯函数。
 */
[[nodiscard]] const char *fixQualityShort(bool fixValid, gnss::FixQuality quality) noexcept
{
    if (!fixValid) {
        return quality == gnss::FixQuality::kUnknown ? "--" : "NONE";
    }
    switch (quality) {
    case gnss::FixQuality::kSingle:
        return "SINGLE";
    case gnss::FixQuality::kDgps:
        return "DGPS";
    case gnss::FixQuality::kPps:
        return "PPS";
    case gnss::FixQuality::kRtkFixed:
        return "FIXED";
    case gnss::FixQuality::kRtkFloat:
        return "FLOAT";
    case gnss::FixQuality::kDeadReckoning:
        return "DR";
    case gnss::FixQuality::kManual:
        return "MANUAL";
    case gnss::FixQuality::kSimulator:
        return "SIM";
    case gnss::FixQuality::kInvalid:
        return "NONE";
    case gnss::FixQuality::kUnknown:
    default:
        return "--";
    }
}

/**
 * @brief 渲染开机画面（产品短名 + 固件版本）
 * @param canvas: 全屏画布
 * @param vm: 视图数据
 * @note 仅 renderPage 调用。
 */
void renderSplash(Canvas &canvas, const ViewModel &vm) noexcept
{
    WouoUI_CanvasDrawRBoxEmpty(&canvas, kSplashFramePad, kSplashFramePad,
                               static_cast<int16_t>(kCanvasW - kSplashFrameInset),
                               static_cast<int16_t>(kCanvasH - kSplashFrameInset), kFrameRadius);
    const std::string_view prod = viewOf(vm.product);
    const std::string_view fw = viewOf(vm.fwVer);
    drawCentered(canvas, kSplashTitleY, Font_8_16, prod.empty() ? "RTK" : prod);
    WouoUI_CanvasDrawLine_H(&canvas, kSplashRuleMargin,
                            static_cast<int16_t>(kCanvasW - kSplashRuleMargin - 1), kSplashRuleY);
    drawCentered(canvas, kSplashVerY, Font_6_8, fw.empty() ? "UNKNOWN" : fw);
}

/**
 * @brief 渲染只读状态页（射频模式、电量/充电、GNSS 状态/解/卫星数）
 * @param canvas: 全屏画布
 * @param vm: 视图数据
 * @note 仅 renderPage 调用。
 */
void renderStatus(Canvas &canvas, const ViewModel &vm) noexcept
{
    std::array<char, 16U> radio{};
    std::array<char, 16U> bat{};
    formatRadioShort(vm.radioMode, vm.btReady, vm.btSessions, radio);
    formatBatteryShort(vm.batteryPct, vm.usbPresent, vm.charging, bat);

    drawText(canvas, 0, kHeaderTextY, Font_6_8, viewOf(radio));
    const uint16_t batW = WouoUI_GetStrWidth(bat.data(), Font_6_8);
    // batW < kWidth 时右对齐偏移在 0..127，转为 int16_t 安全。
    const int16_t batX = (batW < kWidth) ? static_cast<int16_t>(kWidth - batW) : 0;
    drawText(canvas, batX, kHeaderTextY, Font_6_8, viewOf(bat));
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    std::array<char, kMaxLineChars + 1U> line{};
    formatGnssStateLine(vm.gnssEnabled, vm.gnssState, line);
    drawText(canvas, 0, kRow1Y, Font_6_8, viewOf(line));

    formatFixLine(vm.gnssEnabled, vm.gnssState, vm.fixValid, vm.fixQuality, line);
    drawText(canvas, 0, kRow2Y, Font_6_8, viewOf(line));

    formatSatsAccLine(vm.gnssEnabled, vm.gnssState, vm.fixValid, vm.satsUsed, vm.hAccCm, line);
    drawText(canvas, 0, kRow3Y, Font_6_8, viewOf(line));

    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kRow4Y);
    drawText(canvas, 0, kFooterTextY, Font_6_8, "1/2");
    drawText(canvas, kFooterHintX, kFooterTextY, Font_6_8, "KEY:NEXT");
}

/**
 * @brief 渲染只读设备信息页（板型、SN、硬件版本、固件版本）
 * @param canvas: 全屏画布
 * @param vm: 视图数据
 * @note 仅 renderPage 调用。
 */
void renderDeviceInfo(Canvas &canvas, const ViewModel &vm) noexcept
{
    drawText(canvas, 0, kHeaderTextY, Font_6_8, "DEVICE INFO");
    drawText(canvas, kPageNumRightX, kHeaderTextY, Font_6_8, "2/2");
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    std::array<char, kMaxLineChars + 1U> line{};
    formatLabelValue("DEV:", viewOf(vm.boardName), line);
    drawText(canvas, 0, kRow1Y, Font_6_8, viewOf(line));

    formatLabelValue("SN :", viewOf(vm.sn), line);
    drawText(canvas, 0, kRow2Y, Font_6_8, viewOf(line));

    formatLabelValue("HW :", viewOf(vm.hwVer), line);
    drawText(canvas, 0, kRow3Y, Font_6_8, viewOf(line));

    formatLabelValue("FW :", viewOf(vm.fwVer), line);
    drawText(canvas, 0, kRow4Y, Font_6_8, viewOf(line));
}

/**
 * @brief 渲染充电待机页（仅电量百分比、电压、充电态与进度条）
 * @param canvas: 全屏画布
 * @param vm: 视图数据
 * @note 仅 renderPage 调用。
 */
void renderChargeStandby(Canvas &canvas, const ViewModel &vm) noexcept
{
    const char *title =
        vm.charging
            ? "CHARGING"
            : (((vm.batteryPct != board::kBatteryPctUnknown) && (vm.batteryPct >= kMaxPercent))
                   ? "BATTERY FULL"
                   : "USB STANDBY");
    drawCentered(canvas, kHeaderTextY, Font_6_8, title);
    WouoUI_CanvasDrawLine_H(&canvas, 0, static_cast<int16_t>(kCanvasW - 1), kHeaderLineY);

    std::array<char, 16U> pctBuf{};
    if (vm.batteryPct == board::kBatteryPctUnknown) {
        sanitizeCopy("--%", pctBuf);
    } else {
        const uint8_t clamped = std::min(vm.batteryPct, kMaxPercent);
        static_cast<void>(
            std::snprintf(pctBuf.data(), pctBuf.size(), "%u%%", static_cast<unsigned>(clamped)));
    }
    std::array<char, 16U> voltBuf{};
    formatBatteryVoltage(vm.batteryMv, voltBuf);

    drawText(canvas, kStandbyPctX, kStandbyPctY, Font_8_16, viewOf(pctBuf));
    drawText(canvas, kStandbyVoltX, kStandbyVoltY, Font_6_8, viewOf(voltBuf));

    WouoUI_CanvasDrawRBoxEmpty(&canvas, kBarX, kBarY, kBarW, kBarH, kBarRadius);
    if (vm.batteryPct != board::kBatteryPctUnknown) {
        const uint8_t clamped = std::min(vm.batteryPct, kMaxPercent);
        // 内框最大宽度 96 px，clamped <= 100，乘法和窄化均安全。
        const auto fillW = static_cast<int16_t>((static_cast<uint32_t>(clamped) * kBarInnerMaxW) /
                                                static_cast<uint32_t>(kMaxPercent));
        if (fillW > 0) {
            WouoUI_CanvasDrawRBox(&canvas, static_cast<int16_t>(kBarX + kBarInnerPad),
                                  static_cast<int16_t>(kBarY + kBarInnerPad), fillW, kBarInnerH,
                                  1U);
        }
    }
    drawCentered(canvas, kStandbyHintY, Font_6_8, "HOLD KEY TO BOOT");
}

/**
 * @brief 渲染关机提示页（低电关机附加 BATTERY LOW 行）
 * @param canvas: 全屏画布
 * @param vm: 视图数据
 * @note 仅 renderPage 调用。
 */
void renderShutdown(Canvas &canvas, const ViewModel &vm) noexcept
{
    WouoUI_CanvasDrawRBoxEmpty(&canvas, kShutdownBoxX, kShutdownBoxY, kShutdownBoxW, kShutdownBoxH,
                               kFrameRadius);
    if (vm.shutdownReason == board::ShutdownReason::kLowBattery) {
        drawCentered(canvas, kShutdownLowY, Font_6_8, "BATTERY LOW");
        drawCentered(canvas, kShutdownSubY, Font_6_8, "Shutting down...");
    } else {
        drawCentered(canvas, kShutdownMidY, Font_6_8, "Shutting down...");
    }
}

} // namespace

void PageSm::reset(board::BootMode bootMode, uint32_t nowMs, uint32_t initialShortKeys) noexcept
{
    bootMode_ = bootMode;
    splashStartMs_ = nowMs;
    lastShortKeys_ = initialShortKeys;
    shutdownNoticeSinceMs_ = 0U;
    wakeFailedAtMs_ = 0U;
    inShutdown_ = false;
    poweredOff_ = false;
    wakeRetryActive_ = false;
    page_ = (bootMode_ == board::BootMode::kChargeStandby) ? OledPage::kChargeStandby
                                                           : OledPage::kSplash;
}

void PageSm::onWakeFailed(uint32_t nowMs) noexcept
{
    poweredOff_ = true;
    wakeRetryActive_ = true;
    wakeFailedAtMs_ = nowMs;
}

PageDecision PageSm::stepShutdown(uint32_t nowMs, board::ShutdownStage shutdownStage) noexcept
{
    PageDecision out{};
    wakeRetryActive_ = false;
    if (!inShutdown_) {
        inShutdown_ = true;
        shutdownNoticeSinceMs_ = nowMs;
    }
    page_ = OledPage::kShutdown;
    out.page = page_;
    if ((shutdownStage == board::ShutdownStage::kCutPower) && !poweredOff_) {
        const uint32_t elapsed = nowMs - shutdownNoticeSinceMs_;
        if (elapsed >= kShutdownNoticeMinMs) {
            poweredOff_ = true;
            out.shouldPowerOff = true;
        } else {
            out.nextWaitMs = kShutdownNoticeMinMs - elapsed;
        }
    }
    return out;
}

PageDecision PageSm::step(uint32_t nowMs, uint32_t shortKeyCount,
                          board::ShutdownStage shutdownStage) noexcept
{
    const uint32_t shortDelta = shortKeyCount - lastShortKeys_;
    lastShortKeys_ = shortKeyCount;

    if (shutdownStage != board::ShutdownStage::kIdle) {
        return stepShutdown(nowMs, shutdownStage);
    }

    PageDecision out{};
    if (inShutdown_) {
        inShutdown_ = false;
        wakeRetryActive_ = false;
        if (poweredOff_) {
            poweredOff_ = false;
            out.shouldWake = true;
        }
        page_ = (bootMode_ == board::BootMode::kChargeStandby) ? OledPage::kChargeStandby
                                                               : OledPage::kStatus;
        out.page = page_;
        return out;
    }

    if (wakeRetryActive_ && poweredOff_ && ((nowMs - wakeFailedAtMs_) >= kWakeRetryBackoffMs)) {
        poweredOff_ = false;
        wakeRetryActive_ = false;
        out.shouldWake = true;
    }

    if (bootMode_ == board::BootMode::kChargeStandby) {
        page_ = OledPage::kChargeStandby;
        out.page = page_;
        return out;
    }

    uint32_t remainingPresses = shortDelta;
    if ((page_ == OledPage::kSplash) &&
        ((remainingPresses > 0U) || ((nowMs - splashStartMs_) >= kSplashDurationMs))) {
        page_ = OledPage::kStatus;
        if (remainingPresses > 0U) {
            --remainingPresses;
        }
    }
    if ((remainingPresses & 1U) != 0U) {
        page_ = (page_ == OledPage::kStatus) ? OledPage::kDeviceInfo : OledPage::kStatus;
    }
    out.page = page_;
    return out;
}

void FrameTracker::invalidate() noexcept
{
    validMask_ = 0U;
}

uint8_t FrameTracker::computeDirtyMask(const ScreenBuff &fb,
                                       std::array<uint64_t, kPages> &hashesOut,
                                       bool forceFull) const noexcept
{
    uint8_t dirty = 0U;
    for (size_t page = 0U; page < kPages; ++page) {
        hashesOut[page] = fnv1a64(std::span<const uint8_t>{fb[page]});
        // page < 8，位运算结果在 uint8_t 范围内。
        const auto bit = static_cast<uint8_t>(1U << page);
        const bool known = (validMask_ & bit) != 0U;
        if (forceFull || !known || (committedHashes_[page] != hashesOut[page])) {
            dirty |= bit;
        }
    }
    return forceFull ? kAllPagesMask : dirty;
}

void FrameTracker::commitPage(uint8_t page, uint64_t hash) noexcept
{
    if (page >= kPages) {
        return;
    }
    committedHashes_[page] = hash;
    // page < 8，位掩码转换安全。
    validMask_ |= static_cast<uint8_t>(1U << page);
}

uint64_t fnv1a64(std::span<const uint8_t> bytes) noexcept
{
    uint64_t hash = kFnvOffsetBasis;
    for (const uint8_t byte : bytes) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

void sanitizeCopy(std::string_view src, std::span<char> dst) noexcept
{
    if (dst.empty()) {
        return;
    }
    const size_t maxChars = dst.size() - 1U;
    const size_t count = std::min(src.size(), maxChars);
    for (size_t i = 0U; i < count; ++i) {
        const char ch = src[i];
        dst[i] = ((ch >= kMinPrintableAscii) && (ch <= kMaxPrintableAscii)) ? ch : '?';
    }
    dst[count] = '\0';
}

void formatLabelValue(std::string_view label, std::string_view value, std::span<char> out) noexcept
{
    if (out.empty()) {
        return;
    }
    const std::string_view effective = value.empty() ? std::string_view{"--"} : value;
    const size_t maxChars = out.size() - 1U;
    size_t pos = 0U;
    const auto appendClean = [&](std::string_view part) noexcept {
        for (const char ch : part) {
            if (pos >= maxChars) {
                break;
            }
            out[pos++] = ((ch >= kMinPrintableAscii) && (ch <= kMaxPrintableAscii)) ? ch : '?';
        }
    };
    appendClean(label);
    if ((pos < maxChars) && !label.empty() && (label.back() != ' ')) {
        out[pos++] = ' ';
    }
    appendClean(effective);
    out[pos] = '\0';
}

void formatRadioShort(RadioMode mode, bool btReady, uint8_t btSessions,
                      std::span<char> out) noexcept
{
    if (mode == RadioMode::kWifiConfig) {
        sanitizeCopy("WIFI:AP", out);
        return;
    }
    if (!btReady) {
        sanitizeCopy("BT:OFF", out);
        return;
    }
    sanitizeCopy(btSessions > 0U ? "BT:CONN" : "BT:WAIT", out);
}

void formatBatteryShort(uint8_t pct, bool usbPresent, bool charging, std::span<char> out) noexcept
{
    if (out.empty()) {
        return;
    }
    const char *suffix = charging ? "+" : (usbPresent ? "U" : "");
    if (pct == board::kBatteryPctUnknown) {
        static_cast<void>(std::snprintf(out.data(), out.size(), "--%%%s", suffix));
        return;
    }
    const uint8_t clamped = std::min(pct, kMaxPercent);
    static_cast<void>(
        std::snprintf(out.data(), out.size(), "%u%%%s", static_cast<unsigned>(clamped), suffix));
}

void formatBatteryVoltage(uint32_t mv, std::span<char> out) noexcept
{
    if (out.empty()) {
        return;
    }
    if (mv == 0U) {
        sanitizeCopy("--.-V", out);
        return;
    }
    const uint32_t volts = mv / kMvPerVolt;
    const uint32_t centi = (mv % kMvPerVolt) / kMvPerCentiVolt;
    static_cast<void>(std::snprintf(out.data(), out.size(), "%u.%02uV",
                                    static_cast<unsigned>(volts), static_cast<unsigned>(centi)));
}

void formatGnssStateLine(bool enabled, gnss::State state, std::span<char> out) noexcept
{
    formatLabelValue("GNSS:", gnssStateShort(enabled, state), out);
}

void formatFixLine(bool enabled, gnss::State state, bool fixValid, gnss::FixQuality quality,
                   std::span<char> out) noexcept
{
    if (!enabled || (state != gnss::State::kRunning)) {
        formatLabelValue("FIX :", "--", out);
        return;
    }
    formatLabelValue("FIX :", fixQualityShort(fixValid, quality), out);
}

void formatSatsAccLine(bool enabled, gnss::State state, bool fixValid, uint8_t satsUsed,
                       uint16_t hAccCm, std::span<char> out) noexcept
{
    if (out.empty()) {
        return;
    }
    if (!enabled || (state != gnss::State::kRunning)) {
        sanitizeCopy("SAT : --", out);
        return;
    }
    const unsigned sats = fixValid ? static_cast<unsigned>(satsUsed) : 0U;
    if (hAccCm == gnss::kU16Unknown) {
        static_cast<void>(std::snprintf(out.data(), out.size(), "SAT : %u", sats));
        return;
    }
    const unsigned meters = static_cast<unsigned>(hAccCm / kCmPerMeter);
    const unsigned cm = static_cast<unsigned>(hAccCm % kCmPerMeter);
    static_cast<void>(std::snprintf(out.data(), out.size(), "SAT : %u %u.%02um", sats, meters, cm));
}

void renderPage(OledPage page, const ViewModel &vm, ScreenBuff &fb) noexcept
{
    Pen pen{.color_mode = PEN_MODE_NORMAL, .color = PEN_COLOR_WHITE, .rev_color_flag = false};
    WouoUI_GraphSetBuff(&fb);
    WouoUI_GraphSetPen(&pen);
    WouoUI_GraphSetPenColor(1U);
    WouoUI_BuffClear();

    Canvas canvas{.start_x = 0, .start_y = 0, .w = kCanvasW, .h = kCanvasH};
    switch (page) {
    case OledPage::kSplash:
        renderSplash(canvas, vm);
        break;
    case OledPage::kStatus:
        renderStatus(canvas, vm);
        break;
    case OledPage::kDeviceInfo:
        renderDeviceInfo(canvas, vm);
        break;
    case OledPage::kChargeStandby:
        renderChargeStandby(canvas, vm);
        break;
    case OledPage::kShutdown:
        renderShutdown(canvas, vm);
        break;
    default:
        renderStatus(canvas, vm);
        break;
    }
    // 解除栈局部画笔注册，避免 WouoUI 静态 p_cur_pen 悬垂（ROL1-06）。
    WouoUI_GraphSetPen(nullptr);
}

} // namespace app::oled
