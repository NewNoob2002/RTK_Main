// 链路单一所有者；RX 调度保持 512 B 默认读缓冲、字节/20 ms 预算、每轮一个产品帧，RTCM 直推管道。
// 启动期由组合根初始化一次；poll 与命令只在 kLink 执行器任务；蓝牙 API 可阻塞，非 ISR 安全。
#include "modules/bt_link/BtLinkModule.hpp"
#include "modules/bt_link/BtLinkModulePrivate.hpp"
#include "AppContext.hpp"
#include "board/BoardProfile.hpp"
#include "app/PowerRequests.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <cstring>
#include "fw/Atomic.hpp"
#include "fw/Log.hpp"
#include "fw/Tlog.hpp"
#include "core/LinkService.hpp"
#include "esp_system.h"
#include "SppServerPairing.h"
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#include "proto/Frame.hpp"
#include "rtos/EspStatus.hpp"

// 回复信用按整帧预留；协议帧长一旦超过预留量，commit 会拒绝并永久占住该会话的预留，
// 使会话再也不能恢复。在此处绑定两者，避免只改一侧。
static_assert(proto::kMaxFrame <= SPP_SERVER_REPLY_RESERVE,
              "reply reserve must cover a full protocol frame");
static_assert(fw::kAtomicUsable<TaskHandle_t>, "wake_task_ must be lock-free");
static_assert(core::kSatCapGps == settings::const_bit::kGps &&
                  core::kSatCapBds == settings::const_bit::kBds &&
                  core::kSatCapGlo == settings::const_bit::kGlo &&
                  core::kSatCapGal == settings::const_bit::kGal &&
                  core::kSatCapSbas == settings::const_bit::kSbas,
              "PhoneCompatService constellation cap bits must match settings::const_bit");

namespace app {
namespace {

constexpr uint32_t kPhoneResetDelayMs = 250U; // >= 200 ms，确保 0x1E 应答发出后再执行复位
constexpr uint32_t kPhoneResetPollMs = 50U;
constexpr size_t kSatTrackTextBuf = 24U;
constexpr uint8_t kHexNibbleMask = 0x0FU;
constexpr unsigned kHexHighShift = 4U;

/**
 * @brief 从板级配置与系统身份填充手机设备信息
 * @param profile: 启动时识别的板型，nullptr 表示未知
 * @param sn: 当前系统序列号
 * @param[out] info: 写入产品名、硬件版本、固件版本与序列号
 * @note 仅启动任务调用，不阻塞，非 ISR。
 */
void setDeviceIdentity(const board::Profile *profile, std::string_view sn,
                       core::DeviceInfo &info) noexcept
{
    info.product.assign(board::productName(profile));
    info.hwVer.assign(profile != nullptr ? profile->hardwareVersion
                                         : board::Profile{}.hardwareVersion);
    info.sn.assign(sn);
    info.fwVer.assign(appVersion());
}

/** @brief 解析单个十六进制字符。 @param ch: ASCII 字符。 @param[out] out: 0..15。 @retval true 合法。 */
bool parseHexNibble(char ch, uint8_t &out) noexcept
{
    constexpr uint8_t kHexAlphaBase = 10U;
    if ((ch >= '0') && (ch <= '9')) {
        out = static_cast<uint8_t>(ch - '0');
        return true;
    }
    if ((ch >= 'A') && (ch <= 'F')) {
        out = static_cast<uint8_t>(kHexAlphaBase + (ch - 'A'));
        return true;
    }
    if ((ch >= 'a') && (ch <= 'f')) {
        out = static_cast<uint8_t>(kHexAlphaBase + (ch - 'a'));
        return true;
    }
    return false;
}

/** @brief 将定长字节编码为大写十六进制文本。 @param bytes: 输入字节。 @param[out] out: 输出缓冲。 @retval 编码视图。 */
std::string_view encodeHexUpper(std::span<const uint8_t> bytes, std::span<char> out) noexcept
{
    constexpr std::string_view kHex = "0123456789ABCDEF";
    if (bytes.size() * 2U > out.size()) {
        return {};
    }
    size_t pos = 0U;
    for (const uint8_t byte : bytes) {
        out[pos++] = kHex[static_cast<size_t>((byte >> kHexHighShift) & kHexNibbleMask)];
        out[pos++] = kHex[static_cast<size_t>(byte & kHexNibbleMask)];
    }
    return std::string_view{out.data(), pos};
}

/** @brief 将定长十六进制文本解码到字节缓冲。 @param text: 十六进制串。 @param[out] out: 输出字节。 @retval true 长度匹配且字符合法。 */
bool decodeHexFixed(std::string_view text, std::span<uint8_t> out) noexcept
{
    if (text.size() != out.size() * 2U) {
        return false;
    }
    for (size_t i = 0U; i < out.size(); ++i) {
        uint8_t hi = 0U;
        uint8_t lo = 0U;
        if (!parseHexNibble(text[i * 2U], hi) || !parseHexNibble(text[(i * 2U) + 1U], lo)) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << kHexHighShift) | lo);
    }
    return true;
}

/** @brief 解析逗号分隔的 6 项卫星跟踪配置文本。 @param text: "<cutoff>,<gps>,<glo>,<bds>,<gal>,<sbas>"。 @param[out] out: 配置结果。 @retval true 解析成功。 */
bool parseSatTrackText(std::string_view text, core::SatTrackConfig &out) noexcept
{
    constexpr size_t kFieldCount = 6U;
    constexpr uint32_t kDecimalBase = 10U;
    std::array<uint8_t, kFieldCount> fields{};
    std::string_view rest = text;
    for (size_t idx = 0U; idx < kFieldCount; ++idx) {
        const size_t sep = rest.find(',');
        const bool isLast = (idx + 1U) == kFieldCount;
        if (isLast != (sep == std::string_view::npos)) {
            return false;
        }
        const std::string_view part = isLast ? rest : rest.substr(0U, sep);
        if (part.empty()) {
            return false;
        }
        uint32_t val = 0U;
        for (const char ch : part) {
            if ((ch < '0') || (ch > '9')) {
                return false;
            }
            val = (val * kDecimalBase) + static_cast<uint32_t>(ch - '0');
            if (val > UINT8_MAX) {
                return false;
            }
        }
        // 已检查 val <= UINT8_MAX，缩窄到 uint8_t 无损。
        fields[idx] = static_cast<uint8_t>(val);
        if (!isLast) {
            rest.remove_prefix(sep + 1U);
        }
    }
    const auto decoded = core::decodeSatTrackConfig(fields);
    if (!decoded.has_value()) {
        return false;
    }
    out = *decoded;
    return true;
}

/** @brief 轮询一条 settings::Service 异步凭据。 @param settings: 设置服务。 @param[in,out] ticket: 凭据（完成时清零）。 @retval 空表示进行中，布尔表示最终成败。 */
std::optional<bool> pollTicket(const settings::Service &settings, settings::Ticket &ticket) noexcept
{
    if (ticket.seq == 0U) {
        return false;
    }
    fw::CommandResult res{};
    const auto state = settings.poll(ticket, res);
    if (state == settings::PollState::kPending) {
        return std::nullopt;
    }
    ticket = {};
    return (state == settings::PollState::kDone) && fw::isOk(res.status);
}

/** @brief 取消一条在途 settings::Service 异步凭据。 @param settings: 设置服务。 @param[in,out] ticket: 凭据。 */
void cancelTicket(const settings::Service &settings, settings::Ticket &ticket) noexcept
{
    if (ticket.seq != 0U) {
        settings.cancel(ticket);
        ticket = {};
    }
}

} // namespace

#if RTK_GNSS_ENABLED
size_t BtLinkModule::onReadLatestGsv(const void *ctx, std::span<uint8_t> dst) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr)) {
        return 0U;
    }
    const auto gsv = self->context_->data.gnssLatestGsv.read();
    if ((gsv.len == 0U) || (gsv.len > dst.size()) || (gsv.len > gsv.bytes.size())) {
        return 0U;
    }
    std::copy_n(gsv.bytes.begin(), gsv.len, dst.begin());
    return gsv.len;
}
#endif

core::PhoneNtripConfig BtLinkModule::onReadNtrip(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    return ((self != nullptr) && (self->context_ != nullptr)) ? self->context_->storage.phoneNtrip()
                                                              : core::PhoneNtripConfig{};
}

bool BtLinkModule::onSetNtrip(
    void *ctx, // NOLINT(misc-const-correctness): PhoneCompatOps 固定为 void *ctx
    const core::PhoneNtripConfig &cfg, uint32_t nowMs) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    const power::SettingAdmission admission;
    return admission.admitted() && (self != nullptr) && (self->context_ != nullptr) &&
           self->context_->storage.setPhoneNtrip(cfg, nowMs);
}

core::PhoneJsonServerConfig BtLinkModule::onReadJsonServer(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    return ((self != nullptr) && (self->context_ != nullptr))
               ? self->context_->storage.phoneJsonServer()
               : core::PhoneJsonServerConfig{};
}

bool BtLinkModule::onSetJsonServer(
    void *ctx, // NOLINT(misc-const-correctness): PhoneCompatOps 固定为 void *ctx
    const core::PhoneJsonServerConfig &cfg, uint32_t nowMs) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    const power::SettingAdmission admission;
    return admission.admitted() && (self != nullptr) && (self->context_ != nullptr) &&
           self->context_->storage.setPhoneJsonServer(cfg, nowMs);
}

core::PhoneThingsboardConfig BtLinkModule::onReadThingsboard(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    return ((self != nullptr) && (self->context_ != nullptr))
               ? self->context_->storage.phoneThingsboard()
               : core::PhoneThingsboardConfig{};
}

bool BtLinkModule::onSetThingsboard(
    void *ctx, // NOLINT(misc-const-correctness): PhoneCompatOps 固定为 void *ctx
    const core::PhoneThingsboardConfig &cfg, uint32_t nowMs) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    const power::SettingAdmission admission;
    return admission.admitted() && (self != nullptr) && (self->context_ != nullptr) &&
           self->context_->storage.setPhoneThingsboard(cfg, nowMs);
}

bool BtLinkModule::onScheduleReset(void *ctx, bool factoryReset, bool reboot,
                                   uint32_t nowMs) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    return (self != nullptr) && self->schedulePhoneReset(factoryReset, reboot, nowMs);
}

bool BtLinkModule::onWifiConfigSupported([[maybe_unused]] const void *ctx) noexcept
{
#if defined(CONFIG_RTK_WIFI_CONFIG_ENABLE)
    return true;
#else
    return false;
#endif
}

bool BtLinkModule::onScheduleWifiConfig(void *ctx, uint32_t nowMs) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self == nullptr) || !onWifiConfigSupported(ctx) ||
        (self->pending_reset_ != PendingReset::kNone) ||
        !power::s_power_requests.canScheduleRestart()) {
        return false;
    }
    // 与 0x1E 共用延迟，先让 01 01 应答经 SPP 发出再重启（ADR-0017 §2.2）。
    self->pending_reset_ = PendingReset::kWifiConfig;
    self->pending_reset_at_ms_ = nowMs + kPhoneResetDelayMs;
    return true;
}

core::AntennaHeight BtLinkModule::onReadHeight(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    core::AntennaHeight height{};
    settings::Value value{};
    if ((self != nullptr) && (self->context_ != nullptr) &&
        self->context_->settings.get("phone.height", value)) {
        static_cast<void>(decodeHexFixed(value.textView(), height));
    }
    return height;
}

bool BtLinkModule::onStartHeight(void *ctx, const core::AntennaHeight &height) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr) || (self->height_ticket_.seq != 0U)) {
        return false;
    }
    std::array<char, core::kAntennaHeightBytes * 2U> hex{};
    const auto text = encodeHexUpper(height, hex);
    const auto err = self->context_->settings.set("phone.height", text, settings::Caller::kPhone,
                                                  self->height_ticket_);
    return err == settings::Error::kOk;
}

std::optional<bool> BtLinkModule::onHeightResult(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr)) {
        return false;
    }
    return pollTicket(self->context_->settings, self->height_ticket_);
}

void BtLinkModule::onCancelHeight(void *ctx) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self != nullptr) && (self->context_ != nullptr)) {
        cancelTicket(self->context_->settings, self->height_ticket_);
    }
}

bool BtLinkModule::onPppSupported(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    settings::Domain dom{};
    return (self != nullptr) && (self->context_ != nullptr) &&
           self->context_->settings.domain("gnss.ppp", dom) && dom.supported;
}

bool BtLinkModule::onReadPpp(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    settings::Value value{};
    return (self != nullptr) && (self->context_ != nullptr) &&
           self->context_->settings.get("gnss.ppp", value) && value.boolVal;
}

bool BtLinkModule::onStartPpp(void *ctx, bool enabled) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr) || (self->ppp_ticket_.seq != 0U)) {
        return false;
    }
    const auto err = self->context_->settings.set("gnss.ppp", enabled ? "1" : "0",
                                                  settings::Caller::kPhone, self->ppp_ticket_);
    return err == settings::Error::kOk;
}

std::optional<bool> BtLinkModule::onPppResult(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr)) {
        return false;
    }
    return pollTicket(self->context_->settings, self->ppp_ticket_);
}

void BtLinkModule::onCancelPpp(void *ctx) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self != nullptr) && (self->context_ != nullptr)) {
        cancelTicket(self->context_->settings, self->ppp_ticket_);
    }
}

uint8_t BtLinkModule::onSatCapsMask(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    settings::Domain dom{};
    if ((self == nullptr) || (self->context_ == nullptr) ||
        !self->context_->settings.domain("gnss.sat_track", dom) || !dom.supported) {
        return 0U;
    }
    return dom.mask;
}

core::SatTrackConfig BtLinkModule::onReadSatConfig(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    settings::Value value{};
    core::SatTrackConfig cfg = core::SatTrackConfig::defaults();
    if ((self != nullptr) && (self->context_ != nullptr) &&
        self->context_->settings.get("gnss.sat_track", value)) {
        static_cast<void>(parseSatTrackText(value.textView(), cfg));
    }
    return cfg;
}

bool BtLinkModule::onStartSatConfig(void *ctx, const core::SatTrackConfig &cfg) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr) || (self->sat_ticket_.seq != 0U)) {
        return false;
    }
    std::array<char, kSatTrackTextBuf> buf{};
    const int written =
        std::snprintf(buf.data(), buf.size(), "%u,%u,%u,%u,%u,%u",
                      static_cast<unsigned>(cfg.cutoffDeg), static_cast<unsigned>(cfg.gps),
                      static_cast<unsigned>(cfg.glo), static_cast<unsigned>(cfg.bds),
                      static_cast<unsigned>(cfg.gal), static_cast<unsigned>(cfg.sbas));
    if (written <= 0 || static_cast<size_t>(written) >= buf.size()) {
        return false;
    }
    const std::string_view text{buf.data(), static_cast<size_t>(written)};
    const auto err = self->context_->settings.set("gnss.sat_track", text, settings::Caller::kPhone,
                                                  self->sat_ticket_);
    return err == settings::Error::kOk;
}

std::optional<bool> BtLinkModule::onSatConfigResult(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr)) {
        return false;
    }
    return pollTicket(self->context_->settings, self->sat_ticket_);
}

void BtLinkModule::onCancelSatConfig(void *ctx) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self != nullptr) && (self->context_ != nullptr)) {
        cancelTicket(self->context_->settings, self->sat_ticket_);
    }
}

core::PhoneSnPn BtLinkModule::onReadSnPn(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    core::PhoneSnPn out{};
    if ((self == nullptr) || (self->context_ == nullptr)) {
        return out;
    }
    settings::Value value{};
    if (self->context_->settings.get("phone.sn_pn", value) &&
        (value.kind == settings::ValueKind::kBytes) && (value.textLen == core::kPhoneSnPnBytes)) {
        std::memcpy(out.data(), value.text.data(), core::kPhoneSnPnBytes);
    }
    if (out[0U] == 0U) {
        const auto sn = self->deviceSn();
        const size_t copyLen = std::min(sn.size(), core::kPhoneSnBytes);
        std::memcpy(out.data(), sn.data(), copyLen);
    }
    return out;
}

bool BtLinkModule::onStartSnPn(void *ctx, const core::PhoneSnPn &snPn) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr) || (self->snpn_ticket_.seq != 0U)) {
        return false;
    }
    std::array<char, core::kPhoneSnPnBytes * 2U> hex{};
    const auto text = encodeHexUpper(snPn, hex);
    const auto err = self->context_->settings.set("phone.sn_pn", text, settings::Caller::kPhone,
                                                  self->snpn_ticket_);
    return err == settings::Error::kOk;
}

std::optional<bool> BtLinkModule::onSnPnResult(const void *ctx) noexcept
{
    const auto *self = static_cast<const BtLinkModule *>(ctx);
    if ((self == nullptr) || (self->context_ == nullptr)) {
        return false;
    }
    const auto res = pollTicket(self->context_->settings, self->snpn_ticket_);
    if (res.has_value() && *res) {
        settings::Value snVal{};
        if (self->context_->settings.get("factory.sn", snVal)) {
            self->context_->bt_link.updateDeviceSn(snVal.textView());
        }
    }
    return res;
}

void BtLinkModule::onCancelSnPn(void *ctx) noexcept
{
    auto *self = static_cast<BtLinkModule *>(ctx);
    if ((self != nullptr) && (self->context_ != nullptr)) {
        cancelTicket(self->context_->settings, self->snpn_ticket_);
    }
}

core::services::PhoneCompatOps BtLinkModule::makePhoneCompatOps() noexcept
{
    return {
        .ctx = this,
        .readHeight = &BtLinkModule::onReadHeight,
        .startHeight = &BtLinkModule::onStartHeight,
        .heightResult = &BtLinkModule::onHeightResult,
        .cancelHeight = &BtLinkModule::onCancelHeight,
        .pppSupported = &BtLinkModule::onPppSupported,
        .readPpp = &BtLinkModule::onReadPpp,
        .startPpp = &BtLinkModule::onStartPpp,
        .pppResult = &BtLinkModule::onPppResult,
        .cancelPpp = &BtLinkModule::onCancelPpp,
        .satCapsMask = &BtLinkModule::onSatCapsMask,
        .readSatConfig = &BtLinkModule::onReadSatConfig,
        .startSatConfig = &BtLinkModule::onStartSatConfig,
        .satConfigResult = &BtLinkModule::onSatConfigResult,
        .cancelSatConfig = &BtLinkModule::onCancelSatConfig,
        .readSnPn = &BtLinkModule::onReadSnPn,
        .startSnPn = &BtLinkModule::onStartSnPn,
        .snPnResult = &BtLinkModule::onSnPnResult,
        .cancelSnPn = &BtLinkModule::onCancelSnPn,
#if RTK_GNSS_ENABLED
        .readLatestGsv = &BtLinkModule::onReadLatestGsv,
#endif
        .readNtrip = &BtLinkModule::onReadNtrip,
        .setNtrip = &BtLinkModule::onSetNtrip,
        .readJsonServer = &BtLinkModule::onReadJsonServer,
        .setJsonServer = &BtLinkModule::onSetJsonServer,
        .readThingsboard = &BtLinkModule::onReadThingsboard,
        .setThingsboard = &BtLinkModule::onSetThingsboard,
        .scheduleReset = &BtLinkModule::onScheduleReset,
        .wifiConfigSupported = &BtLinkModule::onWifiConfigSupported,
        .scheduleWifiConfig = &BtLinkModule::onScheduleWifiConfig,
    };
}

std::string_view BtLinkModule::deviceSn() const noexcept
{
    return device_service_.has_value() ? device_service_->info().sn.view() : std::string_view{};
}

void BtLinkModule::updateDeviceSn(std::string_view sn) noexcept
{
    if (device_service_.has_value()) {
        device_service_->info().sn.assign(sn);
    }
}

void BtLinkModule::syncDeviceSn() noexcept
{
    if (context_ == nullptr) {
        return;
    }
    const uint32_t rev = ctx().data.configRev.load(std::memory_order_acquire);
    if (rev == last_config_rev_) {
        return;
    }
    last_config_rev_ = rev;
    settings::Value snVal{};
    if (ctx().settings.get("factory.sn", snVal)) {
        const auto sn = snVal.textView();
        if (!sn.empty() && (sn != deviceSn())) {
            updateDeviceSn(sn);
        }
    }
}

bool BtLinkModule::hasPendingTicket(bool localOnly) const noexcept
{
    if ((height_ticket_.seq != 0U) || (snpn_ticket_.seq != 0U)) {
        return true;
    }
    return !localOnly && ((ppp_ticket_.seq != 0U) || (sat_ticket_.seq != 0U));
}

void BtLinkModule::setPowerPaused(bool paused) noexcept
{
    powerPaused_ = paused;
    applyPowerScanMode(rtos::nowMs());
    if (!paused) {
        return;
    }
    for (const SessionEntry &entry : sessions_) {
        if (!entry.used) {
            continue;
        }
        const esp_err_t err = spp_server_disconnect(entry.handle);
        if ((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND)) {
            FW_LOGW(fw::TraceCat::kLink, "shutdown disconnect 0x%08" PRIx32 " failed: %s",
                    entry.handle, esp_err_to_name(err));
        }
    }
}

void BtLinkModule::applyPowerScanMode(uint32_t nowMs) noexcept
{
    constexpr uint32_t kScanRetryMs = 100U;
    if ((!scanPending_ && scanSubmittedPaused_ == powerPaused_) ||
        (scanFailed_ && nowMs - scanFailedMs_ < kScanRetryMs)) {
        return;
    }
    const esp_err_t err = spp_server_scan_pause(powerPaused_);
    scanPending_ = err == ESP_ERR_NOT_FINISHED;
    scanFailed_ = !scanPending_ && err != ESP_OK;
    if (err == ESP_OK) {
        // GAP API 仅确认入队；应用层 OPEN 拒绝与会话 CLOSE 另行生效。
        scanSubmittedPaused_ = powerPaused_;
    } else if (scanFailed_) {
        scanFailedMs_ = nowMs;
        fw::trace(fw::TraceCat::kSys, static_cast<uint8_t>(PowerTrace::kBtScanSubmitError), 0U,
                  static_cast<uint32_t>(err), 0U); // esp_err_t 错误码保留底层位模式。
    }
}

bool BtLinkModule::powerPauseDone() const noexcept
{
    if (!powerPaused_ || !scanSubmittedPaused_ || scanPending_) {
        return false;
    }
    return std::ranges::none_of(sessions_, [](const SessionEntry &entry) { return entry.used; });
}

bool BtLinkModule::schedulePhoneReset(bool factoryReset, bool reboot, uint32_t nowMs) noexcept
{
    if (pending_reset_ != PendingReset::kNone || !power::s_power_requests.canScheduleRestart()) {
        return false;
    }
    if (factoryReset) {
        pending_reset_ = PendingReset::kFactory;
    } else if (reboot) {
        pending_reset_ = PendingReset::kReboot;
    } else {
        return true;
    }
    pending_reset_at_ms_ = nowMs + kPhoneResetDelayMs;
    return true;
}

void BtLinkModule::restartIntoWifiConfig() noexcept
{
    FW_LOGI(fw::TraceCat::kSys, "Phone requested Wi-Fi config, restarting");
#if defined(ESP_PLATFORM) && defined(CONFIG_RTK_WIFI_CONFIG_ENABLE)
    app::requestRadioMode(app::RadioMode::kWifiConfig, app::RadioReason::kPhone);
#endif
}

void BtLinkModule::pollPhoneReset(uint32_t nowMs) noexcept
{
    if (pending_reset_ == PendingReset::kNone) {
        return;
    }
    if (!power::s_power_requests.canScheduleRestart()) {
        pending_reset_ = PendingReset::kNone;
        return;
    }
    // 有符号差值判断到期，容忍 32 位毫秒回绕。
    if (static_cast<int32_t>(nowMs - pending_reset_at_ms_) < 0) {
        return;
    }
    const PendingReset action = pending_reset_;
    pending_reset_ = PendingReset::kNone;
    if (!power::s_power_requests.claimRestart()) {
        return;
    }
    switch (action) {
    case PendingReset::kFactory: {
        fw::Command cmd{};
        cmd.target = id(ModuleKind::kStorage);
        cmd.verb = StorageModule::kEraseUser;
        fw::CommandResult result{};
        ctx().storage.onCommand(cmd, result);
        if (!fw::isOk(result.status)) {
            power::s_power_requests.releaseAction();
            FW_LOGW(fw::TraceCat::kStore, "Phone factory reset failed (%s)",
                    fw::toString(result.status));
            return;
        }
        FW_LOGI(fw::TraceCat::kStore, "Factory reset done, restarting");
        break;
    }
    case PendingReset::kWifiConfig:
        restartIntoWifiConfig();
        break;
    case PendingReset::kReboot:
    case PendingReset::kNone:
    default:
        FW_LOGI(fw::TraceCat::kSys, "Phone reboot requested, restarting");
        break;
    }
    esp_restart();
}

void BtLinkModule::publishLinkStatus(uint32_t nowMs) noexcept
{

    /* 按基线顺序发布链路、会话与报警状态。 */
    LinkStatus link{};
    link.policy = policyFromConfig();
    link.pairing = pairStatus(nowMs).remainingMs != 0U;
    link.snapshotMs = nowMs;
    const auto list = link_manager_.whitelist();
    link.whitelistCount = std::min(list.size(), kMaxAlarms);
    std::copy_n(list.begin(), link.whitelistCount, link.whitelist.begin());
    ctx().data.linkStatus.write(link);
}

void BtLinkModule::publishSessions() noexcept
{
    Sessions sessions{};
    for (const auto &entry : sessions_) {
        const auto role = link_manager_.role(entry.handle);
        if (entry.used && role != core::Role::kNone && sessions.count < kMaxSessions) {
            sessions.entries[sessions.count++] = {true, entry.handle, role, entry.addr};
        }
    }
    ctx().data.sessions.write(sessions);
}

void BtLinkModule::publishAlarmState() noexcept
{
    if (alarm_.has_value()) {
        const AlarmState snapshot{alarm_->state(), alarm_->level(), alarm_->isOverride()};
        ctx().data.alarmState.write(snapshot);
    } else {
        ctx().data.alarmState.write({});
    }
}

void BtLinkModule::publishStats() noexcept
{
    /* 继续发布发送、心跳与分流统计，顺序不变。 */
    LinkStats stats{};
    static_cast<core::LinkStats &>(stats) = linkService().stats(); // 派生结构的基类赋值，无窄化。
    stats.parserCrcErrors = linkService().parserCrcErrors();
    stats.parserLengthErrors = linkService().parserLengthErrors();
    spp_server_tx_stats_t tx{};
    spp_server_tx_stats(&tx);
    stats.txAccepted = tx.accepted;
    stats.txCompleted = tx.completed;
    stats.txCancelled = tx.cancelled;
    stats.txErrors = tx.errors;
    stats.txCong = tx.cong;
    stats.txHighWater = tx.high_water;
    stats.txPaused = tx.paused;
    stats.txAnonWriteFail = tx.anon_write_fail;
    stats.txStrayWrite = tx.stray_write;
    stats.txInflight = tx.inflight;
    spp_server_rx_stats_t rx{};
    spp_server_rx_stats(&rx);
    stats.evtDropped = rx.evt_dropped;
    stats.rxWakeCalls = rx.wake_calls;
    for (size_t idx = 0U; idx < kMaxSessions; ++idx) {
        stats.rxUsed[idx] = rx.sessions[idx].used;
        stats.rxHighWater[idx] = rx.sessions[idx].high_water;
        stats.rxDroppedBytes[idx] = rx.sessions[idx].dropped_bytes;
        stats.rxEvents[idx] = rx.sessions[idx].rx_events;
        stats.rxDropEvents[idx] = rx.sessions[idx].drop_events;
    }
    ctx().data.linkStats.write(stats);
    const auto heartbeat = linkService().lastHeartbeat();
    ctx().data.heartbeat.write(
        {heartbeat.has_value(), heartbeat.value_or(proto::alarm::Heartbeat{})});
}

void BtLinkModule::publishDemuxStats() noexcept
{
    DemuxStats demux = retired_demux_;
    for (auto &entry : sessions_) {
        if (!entry.used || !entry.demux.has_value()) {
            continue;
        }
        const auto &counts = entry.demux->stats();
        demux.unknownBytes += counts.unknownBytes;
        demux.productRejects += counts.decoder[kProductIndex].rejects;
        demux.rtcmRejects += counts.decoder[kRtcmIndex].rejects;
        if (counts.resyncs > entry.seenResyncs) {
            traceRtcm(RtcmTrace::kResync, entry.handle, counts.resyncs - entry.seenResyncs);
            entry.seenResyncs = counts.resyncs;
        }
        if (counts.unknownBytes > entry.seenUnknown) {
            traceRtcm(RtcmTrace::kUnknown, entry.handle, counts.unknownBytes - entry.seenUnknown);
            entry.seenUnknown = counts.unknownBytes;
        }
    }
    ctx().data.demuxStats.write(demux);
}

void BtLinkModule::publishLink(uint32_t nowMs) noexcept
{
    /* 按基线顺序发布链路、会话与报警状态。 */
    publishLinkStatus(nowMs);
    publishSessions();
    publishAlarmState();
    /* 继续发布发送、心跳与分流统计，顺序不变。 */
    publishStats();
    publishDemuxStats();
}

#if RTK_GNSS_ENABLED

#endif

core::LinkService &BtLinkModule::linkService() noexcept
{
    // 执行器只在 init 成功后调用业务回调；SessionSink 同样仅在该生命周期内分发。
    if (!service_.has_value()) {
        std::abort();
    }
    return *service_;
}

/**
 * @brief  原地构造手机与报警器链路服务并安装接收唤醒钩子
 * @retval fw::Result::kOk 成功；其他为失败原因（已记 E 级日志）
 * @note   启动前单线程，非 ISR，不分配堆。
 */
fw::Result BtLinkModule::init() noexcept
{
    if (context_ == nullptr) {
        return fw::Result::kInvalidState;
    }
    const auto system = ctx().data.systemStatus.read();
    if (!system.btReady) {
        return fw::Result::kInvalidState;
    }
    /* 从持久化快照恢复策略，再按依赖顺序原地构造服务。 */
    initLinkManager();
    auto &alarm = alarm_.emplace();
    auto &deviceService = device_service_.emplace();
    setDeviceIdentity(ctx().boardProfile, system.sn.data(), deviceService.info());
    last_config_rev_ = ctx().data.configRev.load(std::memory_order_acquire);
    deviceService.setSatTrackSurveyHook(&core::services::PhoneCompatService::handleSurveySatTrack,
                                        &phone_compat_);
    auto &configService = config_service_.emplace();
    phone_compat_.ops = makePhoneCompatOps();
    auto &alarmService = alarm_service_.emplace(alarm, link_manager_, deviceService.info());
    auto &pairService = pair_service_.emplace(link_manager_);
#if RTK_GNSS_ENABLED
    cached_gnss_fix_.emplace();
    auto &gnssService = gnss_service_.emplace();
    gnssService.setQueryHook(&BtLinkModule::onGnssQuery, this);
    gnssService.setMaskHook(&BtLinkModule::onGnssMask, this);
    gnss_reader_attached_ = fw::isOk(ctx().data.gnssAppRing.attach(0U, gnss_reader_id_));
    if (!gnss_reader_attached_) {
        FW_LOGE(fw::TraceCat::kLink, "<gnss> app ring attach failed");
        return fw::Result::kInvalidState;
    }
    /* 路由表校验成功后才构造 LinkService，失败由执行器隔离。 */
    const std::array<core::RouteTable, 6> tables = {deviceService.table(), configService.table(),
                                                    alarmService.table(),  pairService.table(),
                                                    gnssService.table(),   phone_compat_.table()};
#else
    /* 路由表校验成功后才构造 LinkService，失败由执行器隔离。 */
    const std::array<core::RouteTable, 5> tables = {deviceService.table(), configService.table(),
                                                    alarmService.table(), pairService.table(),
                                                    phone_compat_.table()};
#endif
    const fw::Status routeStatus = router_.init(tables);
    if (!isOk(routeStatus)) {
        FW_LOGE(fw::TraceCat::kLink, "command router init failed: %s", toString(routeStatus));
        return fw::Result::kInvalidState;
    }
    transport_.emplace();
    if (!transport_->supportsCredit()) {
        return fw::Result::kInvalidState;
    }
    service_.emplace(*transport_, link_manager_, router_, alarmService);
    spp_server_set_rx_wake_hook(&BtLinkModule::onRxWake, this);
    if (ctx().storage.consumeBtPairNext()) {
        link_manager_.pairOpen(rtos::nowMs());
        FW_LOGI(fw::TraceCat::kLink, "bt.pair_next consumed, pairing window opened");
    }
    return fw::Result::kOk;
}

void BtLinkModule::onRxWake(void *ctx) noexcept
{
    const auto *self = static_cast<BtLinkModule *>(ctx);
    if (self == nullptr) {
        return;
    }
    const TaskHandle_t task = self->wake_task_.load(std::memory_order_acquire);
    if (task != nullptr) {
        static_cast<void>(xTaskNotifyGive(task));
    }
}

bool BtLinkModule::pollAndCheckReady(uint32_t nowMs) noexcept
{
    spp_server_evt_t evt{};
    for (size_t i = 0; i < SPP_SERVER_EVENT_QUEUE_LEN; ++i) {
        if (!spp_server_poll_event(&evt, 0)) {
            break;
        }
        handleEvent(evt, nowMs);
    }
    bool ready = false;
    for (auto &entry : sessions_) {
        if (!entry.used) {
            continue;
        }
        if (linkService().paused(entry.handle)) {
            if (transport_.has_value() && transport_->replyReady(entry.handle)) {
                ready = true;
            }
            continue;
        }
        if (entry.demuxReady) {
            if (spp_server_rx_ready(entry.handle)) {
                entry.drainPending = true;
            }
            if (entry.drainPending || (entry.rxOffset < entry.rxSize)) {
                ready = true;
            }
        }
    }
    return ready;
}

void BtLinkModule::wait(uint32_t maxMs) noexcept
{
    round_start_ms_ = rtos::nowMs();
    if (!service_.has_value()) {
        return;
    }
    wake_task_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    // 先清旧边缘通知，再查队列与水平状态；若仍空闲则复查后阻塞。
    static_cast<void>(ulTaskNotifyTake(pdTRUE, 0U));
    if (!pollAndCheckReady(rtos::nowMs()) && (maxMs > 0U)) {
        if (!pollAndCheckReady(rtos::nowMs())) {
            // 防御性收窄等待上限至 50 ms（独立于当前 kLink idleWaitMs=20 配置，避免未来调大空闲等待时拖慢轮询）。
            const bool fastPoll = (pending_reset_ != PendingReset::kNone) || hasPendingTicket();
            const uint32_t boundedMs = fastPoll ? std::min(maxMs, kPhoneResetPollMs) : maxMs;
            const TickType_t ticks = pdMS_TO_TICKS(boundedMs);
            const TickType_t waitTicks = (ticks == 0U) ? 1U : ticks;
            static_cast<void>(ulTaskNotifyTake(pdTRUE, waitTicks));
            static_cast<void>(pollAndCheckReady(rtos::nowMs()));
        }
    }
    round_start_ms_ = rtos::nowMs();
}

bool BtLinkModule::poll(uint32_t /*nowMs*/) noexcept
{
    /* 先消费复位、SN 变更和白名单变化，再按真实时钟推进 tick。 */
    applyRtcmReset();
    syncDeviceSn();
    if (device_service_.has_value()) {
        auto &device = device_service_->info();
        const auto power = ctx().data.powerStatus.read();
        device.batteryPct = power.batteryPct;
        device.onBattery = !power.usbPresent;
    }
    collectWhitelistDirty();
    const uint32_t now = rtos::nowMs();
    applyPowerScanMode(now);
#if RTK_GNSS_ENABLED
    syncGnssInfo(now);
#endif
    if (now - last_tick_ms_ >= kTickPeriodMs) {
        last_tick_ms_ = now;
        linkService().tick(now);
    } else {
        linkService().pollDeferred(now);
    }
    for (const auto &entry : sessions_) {
        if (entry.used) {
            linkService().resume(entry.handle, now);
        }
    }
    /* 按预算接收并直推 RTCM，再绑定并排空 PHONE GNSS 流，最后发布链路统计。 */
    drainRx(round_start_ms_);
    collectWhitelistDirty();
    syncStream();
#if RTK_GNSS_ENABLED
    const bool gnssMore = drainGnssStream();
    const bool baseMore = drainBaseRtcm();
#else
    const bool gnssMore = false;
    const bool baseMore = false;
#endif
    publishLink(rtos::nowMs());
    pollPhoneReset(rtos::nowMs());
    // 自投递给同执行器 StorageModule 的设置凭据在途时立即返回 true，促使其在下一轮即刻出队；
    // 跨执行器 kIo 凭据（PPP/星座）不在本队列，避免 kLink 空转。
    bool pending = gnssMore || baseMore || hasPendingTicket(true);
    for (const auto &entry : sessions_) {
        pending =
            pending || (entry.used && entry.demuxReady && !linkService().paused(entry.handle) &&
                        (entry.drainPending || entry.rxOffset < entry.rxSize));
    }
    return pending;
}

void BtLinkModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    result.status = fw::Result::kOk;
    if (cmd.argLen > cmd.args.size()) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    /* 校验长度后按动词调用具名处理函数。 */
    // verb 来源为 uint8_t，LinkVerb 底层类型相同；未知值由 default 拒绝。
    switch (static_cast<LinkVerb>(cmd.verb)) {
    case LinkVerb::kAlarmSet:
        setAlarm(cmd, result);
        break;
    case LinkVerb::kAlarmClear:
        if (!alarm_.has_value()) {
            alarm_.emplace();
        }
        alarm_->clearOverride();
        break;
    case LinkVerb::kPair:
        openPairing(cmd, result);
        break;
    case LinkVerb::kPairClose:
        core::closePairWindow(pair_window_, rtos::nowMs());
        break;
    case LinkVerb::kWhitelistClear:
        clearWhitelist(result);
        break;
    case LinkVerb::kWhitelistAdd:
        editWhitelist(cmd, true, result);
        break;
    case LinkVerb::kWhitelistDel:
        editWhitelist(cmd, false, result);
        break;
    default:
        result.status = fw::Result::kInvalidArg;
        break;
    }
}
} // namespace app
