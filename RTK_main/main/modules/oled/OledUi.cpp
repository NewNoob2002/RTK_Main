// OLED 自检状态机、表盘/子菜单/关机控制器、纯整数格式化与卫星摘要实现（OL2）。
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
constexpr uint8_t kMaxPercent = 100U;
constexpr uint8_t kAllPagesMask = 0xFFU;
constexpr uint32_t kMvPerVolt = 1000U;
constexpr uint32_t kMvPerCentiVolt = 10U;
constexpr int64_t kE9Scale = 1000000000LL;
constexpr int64_t kE9ToMicroDeg = 1000LL; // 1e-9 度 -> 1e-6 度（保留 6 位小数）
constexpr int32_t kMmPerMeter = 1000;
constexpr int32_t kMmPerCentiMeter = 10;
constexpr uint16_t kHdopScale = 100U;
constexpr uint16_t kDiffAgeScale = 10U;
constexpr uint32_t kMsPerSec = 1000U;
constexpr uint32_t kSecPerMin = 60U;
constexpr uint32_t kSecPerHour = 3600U;
constexpr uint32_t kSecPerDay = 86400U;

constexpr size_t kSlotGps = 0U;
constexpr size_t kSlotBeidou = 1U;
constexpr size_t kSlotGlonass = 2U;
constexpr size_t kSlotGalileo = 3U;
constexpr size_t kCheckIdxPower = 0U;
constexpr size_t kCheckIdxNvs = 1U;
constexpr size_t kCheckIdxRadio = 2U;
constexpr size_t kCheckIdxCharger = 3U;
constexpr size_t kCheckIdxGnss = 4U;
constexpr uint8_t kDockIdxSat = 0U;
constexpr uint8_t kDockIdxPos = 1U;
constexpr uint8_t kDockIdxComm = 2U;
constexpr uint8_t kDockIdxDevice = 3U;
constexpr uint8_t kDockIdxPower = 4U;

// 关机进度条目标步进百分比（与真实阶段和 fence 一一对应）。
constexpr uint8_t kPctAnnounce = 15U;
constexpr uint8_t kPctSaveStart = 30U;
constexpr uint8_t kPctSaveFences = 45U;
constexpr uint8_t kPctSaveDone = 55U;
constexpr uint8_t kPctQuiesceStart = 65U;
constexpr uint8_t kPctRadioDone = 80U;
constexpr uint8_t kPctCutPower = 92U;
constexpr uint8_t kPctCutPowerDone = 100U;
constexpr uint8_t kShutdownStepPerTick = 5U;

/**
 * @brief 将卫星星座枚举映射为柱状图槽位下标（0:GPS 1:BDS 2:GLO 3:GAL，其他返回 kConstellationSlots）
 * @param sys: 星座枚举
 * @retval 槽位下标或越界哨兵
 * @note 纯函数。
 */
[[nodiscard]] size_t constellationSlot(gnss::Constellation sys) noexcept
{
    switch (sys) {
    case gnss::Constellation::kGps:
        return kSlotGps;
    case gnss::Constellation::kBeidou:
        return kSlotBeidou;
    case gnss::Constellation::kGlonass:
        return kSlotGlonass;
    case gnss::Constellation::kGalileo:
        return kSlotGalileo;
    case gnss::Constellation::kQzss:
    case gnss::Constellation::kNavic:
    case gnss::Constellation::kSbas:
    case gnss::Constellation::kOther:
    case gnss::Constellation::kCount:
    default:
        return kConstellationSlots;
    }
}

/**
 * @brief 将卫星星座枚举映射为单字母缩写（G/C/R/E/J/I/S/O）
 * @param sys: 星座枚举
 * @retval 单字母缩写
 * @note 纯函数。
 */
[[nodiscard]] char constellationLetter(gnss::Constellation sys) noexcept
{
    switch (sys) {
    case gnss::Constellation::kGps:
        return 'G';
    case gnss::Constellation::kBeidou:
        return 'C';
    case gnss::Constellation::kGlonass:
        return 'R';
    case gnss::Constellation::kGalileo:
        return 'E';
    case gnss::Constellation::kQzss:
        return 'J';
    case gnss::Constellation::kNavic:
        return 'I';
    case gnss::Constellation::kSbas:
        return 'S';
    case gnss::Constellation::kOther:
    case gnss::Constellation::kCount:
    default:
        return 'O';
    }
}

/**
 * @brief 判断指定页面是否为可双击返回表盘的子菜单
 * @param page: 页面枚举
 * @retval true 处于子菜单
 * @retval false 非子菜单
 * @note 纯函数。
 */
[[nodiscard]] bool isSubmenuPage(OledPage page) noexcept
{
    return (page == OledPage::kSubSat) || (page == OledPage::kSubPos) ||
           (page == OledPage::kSubComm) || (page == OledPage::kSubDevice) ||
           (page == OledPage::kSubPower);
}

} // namespace

/**
 * @brief 复位开机自检状态机至 Logo 展开阶段
 * @param nowMs: 当前单调时间（ms）
 */
void SystemCheckSm::reset(uint32_t nowMs) noexcept
{
    phase_ = CheckPhase::kLogo;
    items_.fill(CheckStatus::kPending);
    activeIdx_ = 0U;
    phaseStartMs_ = nowMs;
    stepStartMs_ = nowMs;
    stepResolvedMs_ = nowMs;
    stepResolved_ = false;
    hasWarning_ = false;
}

CheckStatus SystemCheckSm::evaluateItem(size_t idx, const ViewModel &vm,
                                        uint32_t elapsedMs) noexcept
{
    switch (idx) {
    case kCheckIdxPower: // 电源/电池
        return (vm.powerActive && vm.batteryAdcReady) ? CheckStatus::kOk : CheckStatus::kFail;
    case kCheckIdxNvs: // 存储 NVS
        return vm.nvsReady ? CheckStatus::kOk : CheckStatus::kFail;
    case kCheckIdxRadio: // 蓝牙或 Wi-Fi 射频
        if (vm.radioMode == RadioMode::kWifiConfig) {
            return vm.wifiReady ? CheckStatus::kOk : CheckStatus::kFail;
        }
        return vm.btReady ? CheckStatus::kOk : CheckStatus::kFail;
    case kCheckIdxCharger: // 充电芯片（无芯片显示 "-" 不算失败）
        return (vm.chargerPresent || vm.chargerOk) ? CheckStatus::kOk : CheckStatus::kAbsent;
    case kCheckIdxGnss: // GNSS 探测（型号/固件）
        if (!vm.gnssEnabled) {
            return CheckStatus::kAbsent;
        }
        if (isGnssRunningLike(vm.gnssState)) {
            return CheckStatus::kOk;
        }
        if (vm.gnssState == gnss::State::kFailed) {
            return CheckStatus::kFail;
        }
        if (elapsedMs >= kGnssWaitCapMs) {
            return CheckStatus::kFail;
        }
        return CheckStatus::kPending;
    default:
        return CheckStatus::kFail;
    }
}

void SystemCheckSm::skip(const ViewModel &vm) noexcept
{
    for (size_t i = 0U; i < kCheckItemCount; ++i) {
        if (items_[i] == CheckStatus::kPending) {
            items_[i] = evaluateItem(i, vm, 0U);
        }
        if (items_[i] == CheckStatus::kFail) {
            hasWarning_ = true;
        }
    }
    phase_ = CheckPhase::kDone;
}

/**
 * @brief 按当前只读快照推进自检一步（非阻塞）
 * @param nowMs: 当前单调时间（ms）
 * @param vm: 当前只读视图模型
 * @retval true 本轮刚进入 kDone
 * @retval false 仍在进行或此前已完成
 */
bool SystemCheckSm::step(uint32_t nowMs, const ViewModel &vm) noexcept
{
    switch (phase_) {
    case CheckPhase::kLogo:
        if ((nowMs - phaseStartMs_) >= (kLogoRevealMs + kLogoHoldMs)) {
            phase_ = CheckPhase::kChecking;
            activeIdx_ = 0U;
            stepStartMs_ = nowMs;
            stepResolved_ = false;
        }
        return false;
    case CheckPhase::kChecking: {
        const uint32_t elapsed = nowMs - stepStartMs_;
        if (!stepResolved_) {
            const CheckStatus st = evaluateItem(activeIdx_, vm, elapsed);
            items_[activeIdx_] = st;
            if (st == CheckStatus::kPending) {
                return false;
            }
            stepResolved_ = true;
            stepResolvedMs_ = nowMs;
            if (st == CheckStatus::kFail) {
                hasWarning_ = true;
            }
        }
        if ((elapsed < kMinStepMs) || ((nowMs - stepResolvedMs_) < kResultHoldMs)) {
            return false;
        }
        ++activeIdx_;
        if (activeIdx_ >= kCheckItemCount) {
            phase_ = CheckPhase::kSummary;
            phaseStartMs_ = nowMs;
            return false;
        }
        stepStartMs_ = nowMs;
        stepResolved_ = false;
        return false;
    }
    case CheckPhase::kSummary:
        if ((nowMs - phaseStartMs_) >= kSummaryHoldMs) {
            phase_ = CheckPhase::kDone;
            return true;
        }
        return false;
    case CheckPhase::kDone:
    default:
        return false;
    }
}

uint16_t SystemCheckSm::logoRevealQ10(uint32_t nowMs) const noexcept
{
    if (phase_ != CheckPhase::kLogo) {
        return kEaseScale;
    }
    return easeOutCubic(nowMs - phaseStartMs_, kLogoRevealMs);
}

Rect UiController::dockSlotRect(uint8_t idx) noexcept
{
    const uint8_t clamped = (idx < kDockItemCount) ? idx : 0U;
    // clamped <= 4，计算结果在 2..102，转 int16_t 安全。
    const auto x = static_cast<int16_t>(kDockStartX + (static_cast<int16_t>(clamped) * kDockStepX));
    return Rect{.x = x, .y = kDockY, .w = kDockW, .h = kDockH};
}

OledPage UiController::dockTargetPage(uint8_t idx) noexcept
{
    switch (idx) {
    case kDockIdxSat:
        return OledPage::kSubSat;
    case kDockIdxPos:
        return OledPage::kSubPos;
    case kDockIdxComm:
        return OledPage::kSubComm;
    case kDockIdxDevice:
        return OledPage::kSubDevice;
    case kDockIdxPower:
        return OledPage::kSubPower;
    default:
        return OledPage::kSubSat;
    }
}

/**
 * @brief 按启动运行档复位 UI 控制器
 * @param bootMode: 正常启动或充电待机
 * @param nowMs: 当前单调时间（ms）
 */
void UiController::reset(board::BootMode bootMode, uint32_t nowMs) noexcept
{
    bootMode_ = bootMode;
    dockSelected_ = 0U;
    commSelected_ = 0U;
    pairDecisionSent_ = false;
    satSubTab_ = 0U;
    shutdownPct_ = 0U;
    lastStepMs_ = nowMs;
    inShutdown_ = false;
    hasWarning_ = false;
    click_.reset();
    transition_.clear();
    wifiConfirm_ = false;
    pairDialogId_ = 0U;
    dockSpring_.snap(dockSlotRect(0U).x);
    satsRoll_.reset();
    batRoll_.reset();
    if (bootMode_ == board::BootMode::kChargeStandby) {
        page_ = OledPage::kChargeStandby;
    } else {
        page_ = OledPage::kSystemCheck;
        checkSm_.reset(nowMs);
    }
}

UiDecision UiController::stepShutdown(uint32_t nowMs, const ViewModel &vm) noexcept
{
    static_cast<void>(nowMs);
    UiDecision out{};
    click_.reset();
    transition_.clear();
    wifiConfirm_ = false;
    pairDialogId_ = 0U;
    inShutdown_ = true;
    page_ = OledPage::kShutdown;
    out.page = page_;

    const uint8_t finalBits = board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady) |
                              board::shutdownDoneBit(board::ShutdownDoneBit::kBatteryOffReady);
    const bool final = (vm.shutdownDoneMask & finalBits) != 0U;
    const uint8_t target = shutdownTargetPercent(vm.shutdownStage, vm.shutdownDoneMask);
    if (final || (target <= shutdownPct_)) {
        shutdownPct_ = target;
    } else {
        const uint8_t stepAmt = static_cast<uint8_t>(
            std::min<unsigned>(kShutdownStepPerTick, static_cast<unsigned>(target - shutdownPct_)));
        shutdownPct_ = static_cast<uint8_t>(shutdownPct_ + stepAmt);
    }
    out.markShutdownFinalFrame = final;
    out.animating = (shutdownPct_ < target);
    out.nextWaitMs = out.animating ? kAnimFramePeriodMs : kIdlePollPeriodMs;
    return out;
}

void UiController::handleClick(ClickEvent click, uint32_t nowMs, const ViewModel &vm) noexcept
{
    if (click == ClickEvent::kNone) {
        return;
    }
    if (page_ == OledPage::kSystemCheck) {
        if (click == ClickEvent::kDouble) {
            checkSm_.skip(vm);
            hasWarning_ = checkSm_.hasWarning();
            page_ = OledPage::kDialplate;
            dockSpring_.snap(dockSlotRect(dockSelected_).x);
            transition_.start(TransitionKind::kSlideLeft, OledPage::kSystemCheck,
                              OledPage::kDialplate, {}, nowMs);
        }
        return;
    }
    if (page_ == OledPage::kDialplate) {
        if (click == ClickEvent::kSingle) {
            dockSelected_ = static_cast<uint8_t>((dockSelected_ + 1U) % kDockItemCount);
            dockSpring_.retarget(dockSlotRect(dockSelected_).x);
        } else if (click == ClickEvent::kDouble) {
            const OledPage target = dockTargetPage(dockSelected_);
            satSubTab_ = 0U;
            commSelected_ = 0U;
            transition_.start(TransitionKind::kExpandFromIcon, OledPage::kDialplate, target,
                              dockSlotRect(dockSelected_), nowMs);
            page_ = target;
        }
        return;
    }
    if (isSubmenuPage(page_)) {
        if (click == ClickEvent::kDouble) {
            leaveSubmenu(nowMs);
        } else if ((click == ClickEvent::kSingle) && (page_ == OledPage::kSubSat)) {
            satSubTab_ = static_cast<uint8_t>(satSubTab_ ^ 1U);
        }
    }
}

void UiController::leaveSubmenu(uint32_t nowMs) noexcept
{
    const OledPage from = page_;
    page_ = OledPage::kDialplate;
    dockSpring_.snap(dockSlotRect(dockSelected_).x);
    transition_.start(TransitionKind::kCollapseToIcon, from, OledPage::kDialplate,
                      dockSlotRect(dockSelected_), nowMs);
}

OledActionRequest UiController::handleCommClick(ClickEvent click, uint32_t nowMs,
                                                const ViewModel &vm) noexcept
{
    if (click == ClickEvent::kNone) {
        return {};
    }
    if (pairDialogId_ != 0U) {
        if (pairDecisionSent_) {
            return {};
        }
        pairDecisionSent_ = true;
        return {click == ClickEvent::kDouble ? OledAction::kPairAccept : OledAction::kPairReject,
                pairDialogId_};
    }
    if (wifiConfirm_) {
        wifiConfirm_ = false;
        return {click == ClickEvent::kDouble ? OledAction::kWifi : OledAction::kNone};
    }
    const bool wifi = vm.radioMode == RadioMode::kWifiConfig;
    const uint8_t back = wifi ? 1U : 2U;
    if (click == ClickEvent::kSingle) {
        commSelected_ = commSelected_ < back ? static_cast<uint8_t>(commSelected_ + 1U) : 0U;
        return {};
    }
    if (commSelected_ >= back) {
        leaveSubmenu(nowMs);
    } else if (commSelected_ == 1U) {
        return {OledAction::kTogglePair};
    } else if (wifi) {
        return {OledAction::kBluetooth};
    } else {
        wifiConfirm_ = vm.wifiSupported;
    }
    return {};
}

OledActionRequest UiController::handleInput(uint32_t nowMs, std::span<const uint32_t> times,
                                            const ViewModel &vm) noexcept
{
    // 配对截止最多相隔 30 s，差值符号在 uint32_t 回绕时仍有效。
    const uint32_t id =
        static_cast<int32_t>(vm.pairing.requestDeadlineMs - nowMs) > 0 ? vm.pairing.id : 0U;
    if (id != pairDialogId_) {
        pairDialogId_ = id;
        pairDecisionSent_ = false;
        click_.reset();
        transition_.clear();
        return {}; // 不把弹窗出现前的短按用于确认或拒绝。
    }
    const auto consume = [&](ClickEvent click) noexcept -> OledActionRequest {
        if (pairDialogId_ != 0U || wifiConfirm_ || page_ == OledPage::kSubComm) {
            return handleCommClick(click, nowMs, vm);
        }
        handleClick(click, nowMs, vm);
        return {};
    };
    for (const uint32_t pressMs : times) {
        if (const auto action = consume(click_.onPress(pressMs));
            action.action != OledAction::kNone) {
            return action;
        }
    }
    return consume(click_.pollTimeout(nowMs));
}

UiDecision UiController::recoverFromShutdown() noexcept
{
    UiDecision out{};
    inShutdown_ = false;
    shutdownPct_ = 0U;
    click_.reset();
    page_ = (bootMode_ == board::BootMode::kChargeStandby) ? OledPage::kChargeStandby
                                                           : OledPage::kDialplate;
    dockSpring_.snap(dockSlotRect(dockSelected_).x);
    out.page = page_;
    out.nextWaitMs = kIdlePollPeriodMs;
    return out;
}

UiDecision UiController::finishActiveStep(uint32_t nowMs, uint32_t dtMs,
                                          const ViewModel &vm) noexcept
{
    if ((page_ == OledPage::kSystemCheck) && pairDialogId_ == 0U && checkSm_.step(nowMs, vm)) {
        hasWarning_ = checkSm_.hasWarning();
        page_ = OledPage::kDialplate;
        dockSpring_.snap(dockSlotRect(dockSelected_).x);
        transition_.start(TransitionKind::kSlideLeft, OledPage::kSystemCheck, OledPage::kDialplate,
                          {}, nowMs);
    }
    if ((page_ != OledPage::kSystemCheck) && vm.gnssEnabled &&
        (vm.gnssState == gnss::State::kFailed)) {
        hasWarning_ = true;
    }
    dockSpring_.step(dtMs);
    const bool transActive = transition_.pollActive(nowMs);
    const bool springActive = (page_ == OledPage::kDialplate) && !dockSpring_.settled();
    const bool checkAnim = (page_ == OledPage::kSystemCheck) && checkSm_.animating();
    const bool rollAnim =
        ((page_ == OledPage::kDialplate) && (satsRoll_.active(nowMs) || batRoll_.active(nowMs))) ||
        ((page_ == OledPage::kSubSat) && satsRoll_.active(nowMs)) ||
        ((page_ == OledPage::kSubPower) && batRoll_.active(nowMs));

    UiDecision out{};
    out.page = page_;
    out.animating = transActive || springActive || checkAnim || rollAnim;
    uint32_t waitMs = out.animating ? kAnimFramePeriodMs : kIdlePollPeriodMs;
    if (click_.pending()) {
        out.hasClickDeadline = true;
        out.clickDeadlineMs = click_.deadlineMs();
        const uint32_t rem = click_.remainingMs(nowMs);
        waitMs = std::min(waitMs, rem);
    }
    out.nextWaitMs = waitMs;
    return out;
}

/**
 * @brief 推进一轮 UI 状态机、按生产时间戳消歧双击、动画物理与帧周期调度
 * @param nowMs: 当前单调时间（ms）
 * @param shortKeyTimes: 本轮自 UiEventBus 弹出的消抖短按生产时间戳序列
 * @param vm: 当前只读视图模型
 * @retval 本轮渲染页、USB 终帧通知、双击绝对截止与下一帧精确等待时长
 */
UiDecision UiController::step(uint32_t nowMs, std::span<const uint32_t> shortKeyTimes,
                              const ViewModel &vm) noexcept
{
    const uint32_t dtMs = nowMs - lastStepMs_;
    lastStepMs_ = nowMs;

    if (vm.shutdownStage != board::ShutdownStage::kIdle) {
        return stepShutdown(nowMs, vm);
    }
    if (inShutdown_) {
        return recoverFromShutdown();
    }

    if (vm.batteryPct != board::kBatteryPctUnknown) {
        batRoll_.update(static_cast<int32_t>(std::min(vm.batteryPct, kMaxPercent)), nowMs);
    }
    if (vm.gnssEnabled && isGnssRunningLike(vm.gnssState)) {
        satsRoll_.update(vm.fixValid ? static_cast<int32_t>(vm.satsUsed) : 0, nowMs);
    }
    if (bootMode_ == board::BootMode::kChargeStandby) {
        click_.reset();
        page_ = OledPage::kChargeStandby;
        const bool anim = batRoll_.active(nowMs);
        return UiDecision{
            .page = page_,
            .animating = anim,
            .nextWaitMs = anim ? kAnimFramePeriodMs : kIdlePollPeriodMs,
        };
    }

    // 必须先按生产顺序消费队列内全部消抖短按时间戳，再检查绝对截止，避免 OLED 刷屏延迟误拆双击（R2OL2-03）。
    const auto action = handleInput(nowMs, shortKeyTimes, vm);
    auto out = finishActiveStep(nowMs, dtMs, vm);
    out.action = action;
    return out;
}

UiRenderState UiController::renderState(uint32_t nowMs) const noexcept
{
    return UiRenderState{
        .page = page_,
        .dockSelected = dockSelected_,
        .dockFocusX = dockSpring_.position(),
        .satSubTab = satSubTab_,
        .hasCheckWarning = hasWarning_,
        .shutdownPct = shutdownPct_,
        .commSelected = commSelected_,
        .modal = pairDialogId_ != 0U ? UiModal::kPair
                                     : (wifiConfirm_ ? UiModal::kWifiConfirm : UiModal::kNone),
        .nowMs = nowMs,
        .checkSm = &checkSm_,
        .transition = &transition_,
        .satsRoll = &satsRoll_,
        .batRoll = &batRoll_,
    };
}

void FrameTracker::invalidate() noexcept
{
    validMask_ = 0U;
}

void FrameTracker::invalidatePage(uint8_t page) noexcept
{
    if (page < kPages) {
        // 页号小于 8，反码只保留低 8 位对应的页有效位。
        validMask_ &= static_cast<uint8_t>(~(1U << page));
    }
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

void formatCoordE9(int64_t degE9, char posHemisphere, char negHemisphere, bool valid,
                   std::span<char> out) noexcept
{
    if (out.empty()) {
        return;
    }
    if (!valid) {
        sanitizeCopy("--.------", out);
        return;
    }
    const bool neg = degE9 < 0;
    const uint64_t mag = neg ? (0ULL - static_cast<uint64_t>(degE9)) : static_cast<uint64_t>(degE9);
    const uint64_t deg = mag / static_cast<uint64_t>(kE9Scale);
    const uint64_t fracMicro =
        (mag % static_cast<uint64_t>(kE9Scale)) / static_cast<uint64_t>(kE9ToMicroDeg);
    const char hemi = neg ? negHemisphere : posHemisphere;
    static_cast<void>(std::snprintf(out.data(), out.size(), "%u.%06u%c", static_cast<unsigned>(deg),
                                    static_cast<unsigned>(fracMicro), hemi));
}

void formatAltMm(int32_t altMm, bool valid, std::span<char> out) noexcept
{
    if (out.empty()) {
        return;
    }
    if (!valid) {
        sanitizeCopy("--.--m", out);
        return;
    }
    const bool neg = altMm < 0;
    const uint32_t mag =
        neg ? static_cast<uint32_t>(-static_cast<int64_t>(altMm)) : static_cast<uint32_t>(altMm);
    const uint32_t meters = mag / static_cast<uint32_t>(kMmPerMeter);
    const uint32_t cm =
        (mag % static_cast<uint32_t>(kMmPerMeter)) / static_cast<uint32_t>(kMmPerCentiMeter);
    static_cast<void>(std::snprintf(out.data(), out.size(), "%s%u.%02um", neg ? "-" : "",
                                    static_cast<unsigned>(meters), static_cast<unsigned>(cm)));
}

void formatHdopAge(uint16_t hdopX100, uint16_t diffAgeDs, bool valid, std::span<char> out) noexcept
{
    if (out.empty()) {
        return;
    }
    std::array<char, 8U> hdopBuf{};
    std::array<char, 8U> ageBuf{};
    if (!valid || (hdopX100 == gnss::kU16Unknown)) {
        sanitizeCopy("--", hdopBuf);
    } else {
        static_cast<void>(std::snprintf(hdopBuf.data(), hdopBuf.size(), "%u.%02u",
                                        static_cast<unsigned>(hdopX100 / kHdopScale),
                                        static_cast<unsigned>(hdopX100 % kHdopScale)));
    }
    if (!valid || (diffAgeDs == gnss::kU16Unknown)) {
        sanitizeCopy("--", ageBuf);
    } else {
        static_cast<void>(std::snprintf(ageBuf.data(), ageBuf.size(), "%u.%us",
                                        static_cast<unsigned>(diffAgeDs / kDiffAgeScale),
                                        static_cast<unsigned>(diffAgeDs % kDiffAgeScale)));
    }
    static_cast<void>(
        std::snprintf(out.data(), out.size(), "HDOP %s AGE %s", hdopBuf.data(), ageBuf.data()));
}

void formatUptime(uint32_t uptimeMs, std::span<char> out) noexcept
{
    if (out.empty()) {
        return;
    }
    const uint32_t totalSec = uptimeMs / kMsPerSec;
    const uint32_t days = totalSec / kSecPerDay;
    const uint32_t remDay = totalSec % kSecPerDay;
    const uint32_t hours = remDay / kSecPerHour;
    const uint32_t mins = (remDay % kSecPerHour) / kSecPerMin;
    const uint32_t secs = remDay % kSecPerMin;
    if (days > 0U) {
        static_cast<void>(std::snprintf(out.data(), out.size(), "%ud %02u:%02u",
                                        static_cast<unsigned>(days), static_cast<unsigned>(hours),
                                        static_cast<unsigned>(mins)));
        return;
    }
    static_cast<void>(std::snprintf(out.data(), out.size(), "%02u:%02u:%02u",
                                    static_cast<unsigned>(hours), static_cast<unsigned>(mins),
                                    static_cast<unsigned>(secs)));
}

const char *dialHeroBadge(const ViewModel &vm) noexcept
{
    if (!vm.gnssEnabled) {
        return "OFF";
    }
    switch (vm.gnssState) {
    case gnss::State::kOff:
        return "OFF";
    case gnss::State::kPowerUp:
        return "BOOT";
    case gnss::State::kProbing:
        return "PROBE";
    case gnss::State::kConfiguring:
    case gnss::State::kOutput:
    case gnss::State::kMaintenance:
        return "CFG";
    case gnss::State::kFailed:
        return "FAIL";
    case gnss::State::kRunning:
    case gnss::State::kReadback:
        break;
    default:
        return "OFF";
    }
    if (vm.workModeActive && (vm.workMode != WorkModeKind::kRover)) {
        return (vm.workModeState == WorkModeState::kSurvey) ? "SVY" : "BASE";
    }
    if (!vm.fixValid) {
        return "NONE";
    }
    switch (vm.fixQuality) {
    case gnss::FixQuality::kSingle:
        return "SPS";
    case gnss::FixQuality::kDgps:
    case gnss::FixQuality::kPps:
        return "DGPS";
    case gnss::FixQuality::kRtkFloat:
        return "FLOAT";
    case gnss::FixQuality::kRtkFixed:
        return "FIX";
    case gnss::FixQuality::kDeadReckoning:
        return "DR";
    case gnss::FixQuality::kManual:
        return "BASE";
    case gnss::FixQuality::kSimulator:
        return "SIM";
    case gnss::FixQuality::kInvalid:
    case gnss::FixQuality::kUnknown:
    default:
        return "NONE";
    }
}

DialGnssDetail dialGnssDetail(const ViewModel &vm) noexcept
{
    if (!vm.gnssEnabled) {
        return DialGnssDetail{.top = "GNSS", .bot = "DISABLED"};
    }
    switch (vm.gnssState) {
    case gnss::State::kOff:
        return DialGnssDetail{.top = "GNSS", .bot = "DISABLED"};
    case gnss::State::kPowerUp:
        return DialGnssDetail{.top = "GNSS", .bot = "POWER UP"};
    case gnss::State::kProbing:
        return DialGnssDetail{.top = "GNSS", .bot = "PROBING"};
    case gnss::State::kConfiguring:
        return DialGnssDetail{.top = "GNSS", .bot = "CONFIG"};
    case gnss::State::kOutput:
        return DialGnssDetail{.top = "GNSS", .bot = "SET OUT"};
    case gnss::State::kMaintenance:
        return DialGnssDetail{.top = "GNSS", .bot = "MAINT"};
    case gnss::State::kFailed:
        return DialGnssDetail{.top = "GNSS", .bot = "FAULT"};
    case gnss::State::kRunning:
    case gnss::State::kReadback:
    default:
        return DialGnssDetail{.top = "GNSS", .bot = "READY"};
    }
}

PostFlushDecision settleFrameWait(const UiDecision &dec, FlushOutcome outcome, uint32_t frameUs,
                                  uint32_t postFlushNowMs) noexcept
{
    if (outcome == FlushOutcome::kInterrupted) {
        return PostFlushDecision{.nextWaitMs = 0U};
    }
    PostFlushDecision out{};
    if (dec.markShutdownFinalFrame) {
        if ((outcome == FlushOutcome::kWritten) || (outcome == FlushOutcome::kSkipped)) {
            out.markFinalFlushed = true;
        } else if ((outcome == FlushOutcome::kError) || (outcome == FlushOutcome::kBackoff)) {
            out.markFinalFlushed = false;
            out.nextWaitMs = kShutdownFinalRetryMs;
            return out;
        }
    }
    uint32_t waitMs = dec.nextWaitMs;
    if (dec.animating) {
        const uint32_t spentMs = frameUs / kMsPerSec;
        waitMs = (waitMs > spentMs) ? (waitMs - spentMs) : 1U;
    }
    if (dec.hasClickDeadline) {
        const int32_t rem = static_cast<int32_t>(dec.clickDeadlineMs - postFlushNowMs);
        const uint32_t clickWait = (rem <= 0) ? 0U : static_cast<uint32_t>(rem);
        waitMs = std::min(waitMs, clickWait);
    }
    out.nextWaitMs = waitMs;
    return out;
}

uint8_t shutdownTargetPercent(board::ShutdownStage stage, uint8_t doneMask) noexcept
{
    const uint8_t finalBits = board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady) |
                              board::shutdownDoneBit(board::ShutdownDoneBit::kBatteryOffReady);
    if ((doneMask & finalBits) != 0U) {
        return kPctCutPowerDone;
    }
    switch (stage) {
    case board::ShutdownStage::kAnnounce:
        return kPctAnnounce;
    case board::ShutdownStage::kSave:
        if ((doneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed)) != 0U) {
            return kPctSaveDone;
        }
        if ((doneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFences)) != 0U) {
            return kPctSaveFences;
        }
        return kPctSaveStart;
    case board::ShutdownStage::kQuiesce:
        if ((doneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced)) != 0U) {
            return kPctRadioDone;
        }
        return kPctQuiesceStart;
    case board::ShutdownStage::kCutPower:
        return kPctCutPower;
    case board::ShutdownStage::kIdle:
    default:
        return 0U;
    }
}

const char *shutdownStageText(board::ShutdownStage stage, uint8_t doneMask, uint8_t degradedMask,
                              RadioMode radioMode) noexcept
{
    const bool saveTimeout =
        (degradedMask & board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout)) != 0U;
    const bool radioTimeout =
        (degradedMask & board::shutdownDegradedBit(board::ShutdownDegradedBit::kRadioTimeout)) !=
        0U;
    const bool safeFailed =
        (degradedMask &
         board::shutdownDegradedBit(board::ShutdownDegradedBit::kPeripheralsFailed)) != 0U;
    const auto degradedText = [&]() noexcept -> const char * {
        if (saveTimeout) {
            return "SAVE TIMEOUT";
        }
        if (radioTimeout) {
            return "RF TIMEOUT";
        }
        if (safeFailed) {
            return "SAFE IO ERR";
        }
        return nullptr;
    };
    if ((doneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady)) != 0U) {
        if (const char *deg = degradedText(); deg != nullptr) {
            return deg;
        }
        return "CHARGE STBY";
    }
    switch (stage) {
    case board::ShutdownStage::kAnnounce:
        return "PREPARING";
    case board::ShutdownStage::kSave:
        return "SAVING CFG";
    case board::ShutdownStage::kQuiesce:
        if (safeFailed) {
            return "SAFE IO ERR";
        }
        if (saveTimeout) {
            return "SAVE TIMEOUT";
        }
        if ((doneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced)) != 0U) {
            return radioMode == RadioMode::kBluetooth ? "SCAN REQ OK" : "AP MODE OFF";
        }
        return radioMode == RadioMode::kBluetooth ? "CLOSING SPP" : "CLOSING AP";
    case board::ShutdownStage::kCutPower:
        if (const char *deg = degradedText(); deg != nullptr) {
            return deg;
        }
        return "CUTTING PWR";
    case board::ShutdownStage::kIdle:
    default:
        return "READY";
    }
}

void summarizeSatTable(const gnss::SatTable &table, ViewModel &vm) noexcept
{
    vm.satGroups = {};
    vm.topSnr = {};
    vm.topSnrCount = 0U;
    const size_t total = std::min<size_t>(table.count, gnss::kMaxSats);
    // total <= 63，窄化到 uint8_t 安全。
    vm.satsVisible = static_cast<uint8_t>(total);

    for (size_t i = 0U; i < total; ++i) {
        const gnss::SatInfo &sat = table.sats[i];
        const size_t slot = constellationSlot(sat.system);
        if (slot < kConstellationSlots) {
            ++vm.satGroups[slot].visible;
            if (sat.used) {
                ++vm.satGroups[slot].used;
            }
        }
        if (sat.snrDbhz == 0U) {
            continue;
        }
        const SatSnrItem cand{
            .sysLetter = constellationLetter(sat.system),
            .prn = sat.prn,
            .snrDbhz = sat.snrDbhz,
            .used = sat.used,
        };
        // 按 SNR 降序插入前 8 槽（零堆分配）。
        size_t pos = vm.topSnrCount;
        while ((pos > 0U) && (vm.topSnr[pos - 1U].snrDbhz < cand.snrDbhz)) {
            if (pos < kTopSnrBars) {
                vm.topSnr[pos] = vm.topSnr[pos - 1U];
            }
            --pos;
        }
        if (pos < kTopSnrBars) {
            vm.topSnr[pos] = cand;
            if (vm.topSnrCount < kTopSnrBars) {
                ++vm.topSnrCount;
            }
        }
    }
}

} // namespace app::oled
