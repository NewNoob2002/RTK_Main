// Wi-Fi 配置模式网页服务模块（kLink 侧）：会话管理、请求分派、GNSS 异步落地、1 Hz 状态推送与空闲回落。
// 仅 kLink 执行器任务调用（poll()），不阻塞、不分配堆，非 ISR 安全；实时路径仅用 fw::trace。
// 堆档位：禁止区；全部帧缓冲由 BootArena 仅在 kWifiConfig 模式下外供。
#include "modules/web/WebModule.hpp"

#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
#include "modules/ota/OtaRuntime.hpp"
#include "modules/web/WebOtaTransfer.hpp"
#include "rtos/EspStatus.hpp"
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>

#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "app/PowerRequests.hpp"
#include "app/ConsoleProfile.hpp"
#include "app/SettingsBackend.hpp"
#include "def/GnssSubDef.hpp"
#include "board/BoardProfile.hpp"
#include "def/StorageDef.hpp"
#include "def/SystemDef.hpp"
#include "fw/Trace.hpp"
#include "modules/gnss/GnssCmdPack.hpp"
#include "modules/gnss/GnssMaintenance.hpp"
#include "modules/storage/StorageModule.hpp"
#include "support/ResetReason.hpp"
#include "web_proto/Error.hpp"

namespace app {
namespace {

constexpr uint32_t kSetTimeoutMs = 2000U;
constexpr uint32_t kSaveTimeoutMs = 6000U;
constexpr uint32_t kExitFlushTimeoutMs = 1500U;
constexpr uint32_t kRebootSocketGraceMs = 100U;
constexpr uint32_t kRebootMaxWaitMs = 500U;
constexpr int64_t kUsPerSecond = 1000000;
constexpr uint32_t kMsPerSecond = 1000U;
constexpr int32_t kMmPerCm = 10;

/**
 * @brief 发出一条网页模块 kSys 实时追踪事件
 * @param code: WebTrace 事件码
 * @param a: 参数 a
 * @param b: 参数 b
 * @note 实时路径安全，不格式化、不分配堆，非 ISR。
 */
void traceWeb(WebTrace code, uint32_t a, uint32_t b) noexcept
{
    fw::trace(fw::TraceCat::kSys, static_cast<uint8_t>(code), 0U, a, b);
}

/**
 * @brief 触发软件重启（ESP 平台先通知 OLED 刷过渡页并静默 I2C）
 * @note 仅 WebModule 重启提交路径调用（ESP 平台不返回，主机测试桩可返回），非 ISR。
 */
void commitRebootNow() noexcept
{
#if defined(ESP_PLATFORM)
    restartWithOledQuiesce(RebootBanner::kRebooting);
#else
    esp_restart();
#endif
}

/**
 * @brief 将 uint32_t 字节数安全钳制到 int32_t 范围供 JSON 输出
 * @param value: 无符号 32 位数值
 * @retval 钳制后的 int32_t 数值
 * @note 纯函数，线程/ISR 安全。
 */
int32_t clampToI32(uint32_t value) noexcept
{
    constexpr uint32_t kMaxI32 = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    // 钳制在 [0, INT32_MAX] 后转有符号无损。
    return static_cast<int32_t>(std::min(value, kMaxI32));
}

/**
 * @brief 取定长 C 字符串数组的有效视图
 * @param buf: 含终止符或定长字符数组
 * @retval 截至首个 '\0' 的视图
 * @note 纯函数，线程/ISR 安全。
 */
std::string_view cstrView(std::span<const char> buf) noexcept
{
    size_t len = 0U;
    while ((len < buf.size()) && (buf[len] != '\0')) {
        ++len;
    }
    return std::string_view{buf.data(), len};
}

/**
 * @brief 复制短字符串到定长 char 数组并保证 NUL 结尾
 * @param src: 源文本视图
 * @param[out] dst: 目标定长数组
 * @note 纯内存复制，不阻塞。
 */
void copyBounded(std::string_view src, std::span<char> dst) noexcept
{
    if (dst.empty()) {
        return;
    }
    const size_t n = std::min(src.size(), dst.size() - 1U);
    if (n > 0U) {
        std::memcpy(dst.data(), src.data(), n);
    }
    dst[n] = '\0';
}

} // namespace

WebModule::ClientSlot *WebModule::findClient(int fd, uint32_t gen) noexcept
{
    for (auto &slot : clients_) {
        if (slot.active && (slot.fd == fd) && (slot.gen == gen)) {
            return &slot;
        }
    }
    return nullptr;
}

void WebModule::registerClient(int fd, uint32_t gen) noexcept
{
    // 清理同 fd 的历史残余槽位，避免 fd 复用后旧解锁/同步态串联。
    for (auto &slot : clients_) {
        if (slot.active && (slot.fd == fd)) {
            slot = ClientSlot{};
        }
    }
    for (auto &slot : clients_) {
        if (!slot.active) {
            slot = ClientSlot{};
            slot.active = true;
            slot.fd = fd;
            slot.gen = gen;
            slot.syncStage = SyncStage::kSendHello;
            traceWeb(WebTrace::kWsConnect, static_cast<uint32_t>(fd), gen);
            return;
        }
    }
}

void WebModule::unregisterClient(int fd, uint32_t gen) noexcept
{
    auto *slot = findClient(fd, gen);
    if (slot == nullptr) {
        return;
    }
    traceWeb(WebTrace::kWsDisconnect, static_cast<uint32_t>(fd), gen);
    *slot = ClientSlot{};
}

void WebModule::drainCtrlEvents() noexcept
{
    web_cfg::WebCtrlEvent evt{};
    while (ctrlMailbox_.tryPop(evt)) {
        if (evt.kind == web_cfg::WebCtrlEvent::Kind::kConnect) {
            registerClient(evt.fd, evt.gen);
        } else if (evt.kind == web_cfg::WebCtrlEvent::Kind::kDisconnect) {
            unregisterClient(evt.fd, evt.gen);
        }
    }
}

web_cfg::StatusSnapshot WebModule::sampleStatus(uint32_t nowMs) noexcept
{
    web_cfg::StatusSnapshot snap{};
    const auto sys = ctx().data.systemStatus.read();
    const int64_t upSec = esp_timer_get_time() / kUsPerSecond;
    // 单次运行秒数远小于 INT32_MAX（约 68 年），转换安全。
    snap.dev.uptimeS = (upSec > 0) ? static_cast<int32_t>(upSec) : 0;
    snap.dev.heapFree = clampToI32(sys.freeHeap);
    snap.dev.heapMin = clampToI32(sys.minFreeHeap);
    snap.dev.reset = resetReasonName(esp_reset_reason());

    const auto link = ctx().data.linkStatus.read();
    snap.bt.whitelistCount = std::min(link.whitelistCount, kMaxAlarms);
    std::copy_n(link.whitelist.begin(), snap.bt.whitelistCount, snap.bt.whitelist.begin());

#if RTK_GNSS_ENABLED
    const auto gnss = ctx().data.gnssStatus.read();
    const auto fix = ctx().data.gnssFix.read();
    snap.gnss.state = web_cfg::gnssStateName(gnss.state);
    if (gnss.model != ::gnss::Model::kUnknown) {
        copyBounded(::gnss::modelName(gnss.model), snap.gnss.model);
        snap.gnss.hasModel = true;
    }
    const auto fwView = cstrView(gnss.fwVersion);
    if (!fwView.empty()) {
        copyBounded(fwView, snap.gnss.fw);
        snap.gnss.hasFw = true;
    }
    snap.gnss.fix = web_cfg::gnssFixName(fix);
    if (fix.valid || (fix.updateMs != 0U)) {
        snap.gnss.sats = static_cast<int32_t>(fix.satsUsed);
        snap.gnss.hasSats = true;
    }
    // F9P 无定位时 GST 仍输出巨大标准差（解析器饱和为 65534 cm），仅有效定位时才上报精度。
    if (fix.valid && (fix.hAccCm != ::gnss::kU16Unknown)) {
        snap.gnss.haccMm = static_cast<int32_t>(fix.hAccCm) * kMmPerCm;
        snap.gnss.hasHacc = true;
    }
    if (gnss.state == ::gnss::State::kRunning) {
        const uint32_t elapsedMs = nowMs - lastGgaSampleMs_;
        if (ggaSampleValid_ && (elapsedMs >= (kStatusPeriodMs / 2U))) {
            const uint32_t deltaGga = gnss.nmea.gga - lastGgaCount_;
            const uint32_t hz = ((deltaGga * kMsPerSecond) + (elapsedMs / 2U)) / elapsedMs;
            snap.gnss.hz = clampToI32(hz);
            snap.gnss.hasHz = true;
            lastGgaCount_ = gnss.nmea.gga;
            lastGgaSampleMs_ = nowMs;
        } else if (!ggaSampleValid_) {
            lastGgaCount_ = gnss.nmea.gga;
            lastGgaSampleMs_ = nowMs;
            ggaSampleValid_ = true;
        }
    } else {
        ggaSampleValid_ = false;
    }
#else
    static_cast<void>(nowMs);
    snap.gnss.state = "off";
    snap.gnss.fix = "none";
#endif
    return snap;
}

bool WebModule::pollPendingDeliveries() noexcept
{
    bool stillPending = false;
    auto &encodeBuf = klinkEncodeFrame_->data;
    for (auto &slot : clients_) {
        if (!slot.active) {
            continue;
        }
        if (slot.pendingAck) {
            const size_t len =
                web_cfg::buildAckFrame(slot.pendingAckId, slot.pendingAckErr, encodeBuf);
            if ((len > 0U) &&
                enqueueTx(slot.fd, slot.gen, std::span<const char>{encodeBuf}.first(len))) {
                slot.pendingAck = false;
            } else {
                stillPending = true;
                continue;
            }
        }
        if (slot.pendingConfig) {
            const size_t len = web_cfg::buildConfigFrame(ctx().settings, encodeBuf);
            if ((len > 0U) &&
                enqueueTx(slot.fd, slot.gen, std::span<const char>{encodeBuf}.first(len))) {
                slot.pendingConfig = false;
            } else {
                stillPending = true;
                continue;
            }
        }
        if (slot.pendingBye) {
            const size_t len = web_cfg::buildByeFrame(rebootByeReason_, encodeBuf);
            if ((len > 0U) &&
                enqueueTx(slot.fd, slot.gen, std::span<const char>{encodeBuf}.first(len))) {
                slot.pendingBye = false;
            } else {
                stillPending = true;
            }
        }
    }
    return stillPending;
}

bool WebModule::hasPendingCtrlBeforeRx() const noexcept
{
    for (const auto &slot : clients_) {
        if (!slot.active) {
            continue;
        }
        if ((slot.syncStage == SyncStage::kSendHello) ||
            (slot.syncStage == SyncStage::kSendConfig) || slot.pendingAck) {
            return true;
        }
    }
    return false;
}

bool WebModule::hasPendingAckOrBye() const noexcept
{
    for (const auto &slot : clients_) {
        if (slot.active && (slot.pendingAck || slot.pendingBye)) {
            return true;
        }
    }
    return false;
}

bool WebModule::pollInitialSync(uint32_t nowMs) noexcept
{
    bool morePending = false;
    auto &encodeBuf = klinkEncodeFrame_->data;
    for (auto &slot : clients_) {
        if (!slot.active || (slot.syncStage == SyncStage::kReady)) {
            continue;
        }
        if (slot.syncStage == SyncStage::kSendHello) {
            const auto sys = ctx().data.systemStatus.read();
            const web_cfg::HelloSnapshot hello{
                .board = board::current().name,
                .fw = appVersion(),
                .btName = cstrView(sys.deviceName),
                .console = kConsoleProfileName,
                .factoryUnlocked = slot.unlocked,
            };
            uint32_t version = 0U;
            const auto caps = ctx().data.gnssCaps.read(version);
            const size_t len =
                web_cfg::buildHelloFrame(hello, ctx().settings, caps, caps.enabled, encodeBuf);
            if ((len > 0U) &&
                enqueueTx(slot.fd, slot.gen, std::span<const char>{encodeBuf}.first(len))) {
                slot.syncStage = SyncStage::kSendConfig;
                slot.capsVersion = version;
            } else {
                morePending = true;
                continue;
            }
        }
        if (slot.syncStage == SyncStage::kSendConfig) {
            const size_t len = web_cfg::buildConfigFrame(ctx().settings, encodeBuf);
            if ((len > 0U) &&
                enqueueTx(slot.fd, slot.gen, std::span<const char>{encodeBuf}.first(len))) {
                slot.syncStage = SyncStage::kSendStatus;
            } else {
                morePending = true;
                continue;
            }
        }
        if (slot.syncStage == SyncStage::kSendStatus) {
            const auto status = sampleStatus(nowMs);
            const size_t len = web_cfg::buildStatusFrame(status, nullptr, encodeBuf);
            if ((len > 0U) &&
                enqueueTx(slot.fd, slot.gen, std::span<const char>{encodeBuf}.first(len))) {
                slot.lastStatus = status;
                slot.hasLastStatus = true;
                slot.syncStage = SyncStage::kReady;
            } else {
                morePending = true;
            }
        }
    }
    return morePending;
}

void WebModule::sendAck(ClientSlot &client, uint16_t reqId, web::ErrorCode error) noexcept
{
    auto &encodeBuf = klinkEncodeFrame_->data;
    const size_t len = web_cfg::buildAckFrame(reqId, error, encodeBuf);
    if ((len > 0U) && !client.pendingAck &&
        enqueueTx(client.fd, client.gen, std::span<const char>{encodeBuf}.first(len))) {
        return;
    }
    client.pendingAck = true;
    client.pendingAckId = reqId;
    client.pendingAckErr = error;
}

void WebModule::broadcastConfig() noexcept
{
    auto &encodeBuf = klinkEncodeFrame_->data;
    const size_t len = web_cfg::buildConfigFrame(ctx().settings, encodeBuf);
    if (len == 0U) {
        return;
    }
    const auto frame = std::span<const char>{encodeBuf}.first(len);
    for (auto &slot : clients_) {
        if (!slot.active || (slot.syncStage == SyncStage::kSendHello) ||
            (slot.syncStage == SyncStage::kSendConfig)) {
            continue;
        }
        if (slot.pendingAck || !enqueueTx(slot.fd, slot.gen, frame)) {
            slot.pendingConfig = true;
        }
    }
}

void WebModule::scheduleReboot(RebootKind kind, std::string_view reasonText, uint32_t nowMs,
                               uint32_t idleActSeq) noexcept
{
    if (pendingReboot_ != RebootKind::kNone ||
        (kind == RebootKind::kIdle && !power::s_power_requests.canScheduleRestart())) {
        return;
    }
    pendingReboot_ = kind;
    rebootByeReason_ = reasonText;
    rebootRequestedMs_ = nowMs;
    if (kind == RebootKind::kOta) {
        return; // HTTP 已发回升级结果，无需改动 WebSocket bye 协议。
    }
    rebootDrainDoneMs_ = 0U;
    idleRebootActSeq_ = idleActSeq;
    rebootTxCommitted_ = false;
    rebootDrainDone_ = false;
    for (auto &slot : clients_) {
        if (slot.active) {
            slot.pendingBye = true;
        }
    }
    static_cast<void>(pollPendingDeliveries());
    if (!hasPendingAckOrBye()) {
        rebootWaitTxSeq_ = txEnqueuedSeq_.load(std::memory_order_acquire);
        rebootTxCommitted_ = true;
    }
}

bool WebModule::requestOledExit([[maybe_unused]] uint32_t nowMs) noexcept
{
    if (!initialized_ || pendingExit_.fd >= 0 || pendingExit_.local ||
        pendingReboot_ != RebootKind::kNone || inflight_.ticket.seq != 0U ||
        !power::s_power_requests.canScheduleRestart()) {
        return false;
    }
#if RTK_GNSS_ENABLED
    if (ctx().data.ggaGatePersist.load(std::memory_order_acquire) != data::GgaGatePersist::kClean) {
        const fw::Command cmd{.verb = static_cast<uint8_t>(GnssSubVerb::kFlushGgaGate)};
        settings::Ticket ticket{};
        if (!fw::isOk(postSettingCommand(&ctx(), settings::Owner::kGnss, cmd,
                                         settings::Caller::kWeb, ticket))) {
            return false;
        }
        inflight_ = {.ticket = ticket,
                     .fd = -1,
                     .startedMs = nowMs,
                     .kind = RequestKind::kExit,
                     .local = true};
        return true;
    }
#endif
    pendingExit_.local = true;
    return true;
}

void WebModule::commitPendingExit(uint32_t nowMs) noexcept
{
    const auto request = pendingExit_;
    if (request.fd < 0 && !request.local) {
        return;
    }
    pendingExit_ = {};
    auto *client = findClient(request.fd, request.gen);
    if (pendingReboot_ != RebootKind::kNone ||
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
        ota::transferBusy() ||
#endif
        !power::s_power_requests.claimRestart()) {
        if (client != nullptr) {
            sendAck(*client, request.reqId, web::ErrorCode::kBusy);
        }
        return;
    }
    if (client != nullptr) {
        sendAck(*client, request.reqId, web::ErrorCode::kNone);
    }
    scheduleReboot(RebootKind::kExit, "exit", nowMs);
}

bool WebModule::tryAbortIdleReboot(uint32_t nowMs) noexcept
{
    if (pendingReboot_ != RebootKind::kIdle) {
        return false;
    }
    const uint32_t staSnap = staState_.load(std::memory_order_acquire);
    if (web_cfg::canCommitIdleExit(staSnap, idleRebootActSeq_)
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
        && !ota::transferBusy()
#endif
    ) {
        return false;
    }
    traceWeb(WebTrace::kIdleAbort, web_cfg::staStateCount(staSnap), web_cfg::staStateSeq(staSnap));
    pendingReboot_ = RebootKind::kNone;
    rebootTxCommitted_ = false;
    rebootDrainDone_ = false;
    for (auto &slot : clients_) {
        slot.pendingBye = false;
    }
    static_cast<void>(
        idleTimer_.tick(web_cfg::staStateCount(staSnap), web_cfg::staStateSeq(staSnap), nowMs));
    return true;
}

bool WebModule::pollPendingReboot(uint32_t nowMs) noexcept
{
    if (pendingReboot_ == RebootKind::kNone) {
        return false;
    }
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    if (pendingReboot_ == RebootKind::kOta) {
        if (nowMs - rebootRequestedMs_ >= web_ota::kRebootMs) {
            traceWeb(WebTrace::kRebootCommit, static_cast<uint32_t>(pendingReboot_), 0U);
            commitRebootNow();
        }
        static_cast<void>(pollPendingDeliveries());
        static_cast<void>(pollRxFrame(nowMs));
        return true;
    }
#endif
    if (pendingReboot_ == RebootKind::kIdle && !power::s_power_requests.canScheduleRestart()) {
        pendingReboot_ = RebootKind::kNone;
        for (auto &slot : clients_) {
            slot.pendingBye = false;
        }
        return false;
    }
    if (tryAbortIdleReboot(nowMs)) {
        return false;
    }
    static_cast<void>(pollPendingDeliveries());
    if (!txMailbox_.empty() && !txWorkQueued_.load(std::memory_order_acquire)) {
        scheduleTxDrain();
    }
    if (!rebootTxCommitted_ && !hasPendingAckOrBye()) {
        rebootWaitTxSeq_ = txEnqueuedSeq_.load(std::memory_order_acquire);
        rebootTxCommitted_ = true;
    }
    const uint32_t elapsed = nowMs - rebootRequestedMs_;
    const bool txDone = rebootTxCommitted_ &&
                        (static_cast<int32_t>(txCompletedSeq_.load(std::memory_order_acquire) -
                                              rebootWaitTxSeq_) >= 0);
    if (txDone && !rebootDrainDone_) {
        rebootDrainDone_ = true;
        rebootDrainDoneMs_ = nowMs;
    }
    const bool graceDone =
        rebootDrainDone_ && ((nowMs - rebootDrainDoneMs_) >= kRebootSocketGraceMs);
    if (!graceDone && (elapsed < kRebootMaxWaitMs)) {
        return true;
    }
    if (tryAbortIdleReboot(nowMs) ||
        (pendingReboot_ == RebootKind::kIdle && !power::s_power_requests.claimRestart())) {
        return false;
    }
    if (pendingReboot_ == RebootKind::kFreset) {
        traceWeb(WebTrace::kRebootCommit, static_cast<uint32_t>(pendingReboot_), 0U);
        commitRebootNow();
    }
    const RadioReason reason =
        (pendingReboot_ == RebootKind::kIdle) ? RadioReason::kIdle : RadioReason::kWebExit;
    traceWeb(WebTrace::kRebootCommit, static_cast<uint32_t>(pendingReboot_),
             static_cast<uint32_t>(reason));
    requestRadioMode(RadioMode::kBluetooth, reason);
}

void WebModule::handleUnlockReq(ClientSlot &client, const web::Request &req) noexcept
{
    if (req.token == std::string_view{CONFIG_RTK_FACTORY_KEY}) {
        client.unlocked = true;
        sendAck(client, req.id, web::ErrorCode::kNone);
        return;
    }
    client.unlocked = false;
    sendAck(client, req.id, web::ErrorCode::kBadValue);
}

void WebModule::handleCmdReq(ClientSlot &client, const web::Request &req, uint32_t nowMs) noexcept
{
    if (req.name == "save") {
        postMaintenance(client, req.id, settings::Owner::kGnss,
                        static_cast<uint8_t>(GnssSubVerb::kSaveReceiver), RequestKind::kSave,
                        nowMs);
        return;
    }
    if (req.name == "exit") {
        if (pendingExit_.fd >= 0 || pendingExit_.local ||
            !power::s_power_requests.canScheduleRestart()) {
            sendAck(client, req.id, web::ErrorCode::kBusy);
            return;
        }
#if RTK_GNSS_ENABLED
        if (ctx().data.ggaGatePersist.load(std::memory_order_acquire) !=
            data::GgaGatePersist::kClean) {
            postMaintenance(client, req.id, settings::Owner::kGnss,
                            static_cast<uint8_t>(GnssSubVerb::kFlushGgaGate), RequestKind::kExit,
                            nowMs);
            return;
        }
#endif
        pendingExit_ = {.fd = client.fd, .gen = client.gen, .reqId = req.id};
        return;
    }
    if (req.name == "freset") {
        if (!client.unlocked) {
            sendAck(client, req.id, web::ErrorCode::kLocked);
            return;
        }
        postMaintenance(client, req.id, settings::Owner::kStorage, StorageModule::kEraseUserForWeb,
                        RequestKind::kFreset, nowMs);
        return;
    }
    sendAck(client, req.id, web::ErrorCode::kUnsupported);
}

void WebModule::handleSetReq(ClientSlot &client, const web::Request &req, uint32_t nowMs) noexcept
{
    settings::Ticket ticket{};
    const auto error =
        ctx().settings.set(req.key, req.value, settings::Caller::kWeb, ticket, client.unlocked);
    if (error != settings::Error::kOk) {
        sendAck(client, req.id, web_cfg::fromSettingsError(error));
        return;
    }
    trackRequest(client, req.id, ticket, RequestKind::kSet, nowMs);
}

void WebModule::trackRequest(const ClientSlot &client, uint16_t reqId, settings::Ticket ticket,
                             RequestKind kind, uint32_t nowMs) noexcept
{
    inflight_ = {.ticket = ticket,
                 .fd = client.fd,
                 .gen = client.gen,
                 .reqId = reqId,
                 .startedMs = nowMs,
                 .kind = kind};
}

void WebModule::postMaintenance(ClientSlot &client, uint16_t reqId, settings::Owner owner,
                                uint8_t verb, RequestKind kind, uint32_t nowMs) noexcept
{
    const fw::Command cmd{.verb = verb};
    settings::Ticket ticket{};
    const auto posted = postSettingCommand(&ctx(), owner, cmd, settings::Caller::kWeb, ticket);
    if (!fw::isOk(posted)) {
        sendAck(client, reqId, web::fromResult(posted));
        return;
    }
    trackRequest(client, reqId, ticket, kind, nowMs);
}

void WebModule::pollInflight(uint32_t nowMs) noexcept
{
    const auto request = inflight_;
    if (request.ticket.seq == 0U) {
        return;
    }
    if (request.kind == RequestKind::kExit && !power::s_power_requests.canScheduleRestart()) {
        ctx().settings.cancel(request.ticket);
        inflight_ = {};
        if (auto *client = findClient(request.fd, request.gen); client != nullptr) {
            sendAck(*client, request.reqId, web::ErrorCode::kBusy);
        }
        return;
    }
    fw::CommandResult result{};
    const auto state = ctx().settings.poll(request.ticket, result);
    auto error = web::fromResult(result.status);
    if (state == settings::PollState::kPending) {
        uint32_t timeout = kSetTimeoutMs;
        if (request.kind == RequestKind::kSave) {
            timeout = kSaveTimeoutMs;
        } else if (request.kind == RequestKind::kExit) {
            timeout = kExitFlushTimeoutMs;
        }
        if ((nowMs - request.startedMs) < timeout) {
            return;
        }
        ctx().settings.cancel(request.ticket);
        error =
            request.kind == RequestKind::kExit ? web::ErrorCode::kFailed : web::ErrorCode::kTimeout;
    } else if (state != settings::PollState::kDone) {
        error = web::ErrorCode::kFailed;
    }
    inflight_ = {};
    if (request.kind == RequestKind::kExit && error == web::ErrorCode::kNone) {
        pendingExit_ = {
            .fd = request.fd, .gen = request.gen, .reqId = request.reqId, .local = request.local};
        return;
    }
    if (auto *client = findClient(request.fd, request.gen); client != nullptr) {
        sendAck(*client, request.reqId, error);
    }
    if (error != web::ErrorCode::kNone) {
        return;
    }
    if (request.kind == RequestKind::kFreset) {
        scheduleReboot(RebootKind::kFreset, "freset", nowMs);
    } else if (request.kind == RequestKind::kSet &&
               request.ticket.owner == settings::Owner::kStorage) {
        pollStatusPush(nowMs, true);
    }
}

bool WebModule::pollCapsPush() noexcept
{
    uint32_t version = 0U;
    const auto caps = ctx().data.gnssCaps.read(version);
    bool more = false;
    size_t len = 0U;
    auto &buf = klinkEncodeFrame_->data;
    for (auto &slot : clients_) {
        if (!slot.active || slot.syncStage != SyncStage::kReady || slot.capsVersion == version) {
            continue;
        }
        if (len == 0U) {
            len = web_cfg::buildCapsFrame(ctx().settings, caps, caps.enabled, buf);
        }
        if (len == 0U) {
            // 编码失败属于该版本的固定结果，只在能力更新后重试。
            slot.capsVersion = version;
            continue;
        }
        if (!slot.pendingAck &&
            enqueueTx(slot.fd, slot.gen, std::span<const char>{buf}.first(len))) {
            slot.capsVersion = version;
        } else {
            more = true;
        }
    }
    return more;
}

bool WebModule::pollRxFrame(uint32_t nowMs) noexcept
{
    if (hasPendingCtrlBeforeRx()) {
        return !rxMailbox_.empty();
    }
    auto &rxFrame = *klinkRxFrame_;
    if (!rxMailbox_.tryPop(rxFrame)) {
        return false;
    }
    auto *client = findClient(rxFrame.fd, rxFrame.gen);
    if (client == nullptr) {
        return true;
    }
    const auto raw = std::span<const char>{rxFrame.data}.first(rxFrame.len);
    const web::ErrorCode parseErr = web::parseRequest(raw, parsedReq_);
    if (parseErr != web::ErrorCode::kNone) {
        sendAck(*client, parsedReq_.id, parseErr);
        return true;
    }
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    if ((parsedReq_.type == web::RequestType::kSet || parsedReq_.type == web::RequestType::kCmd) &&
        ota::transferBusy()) {
        sendAck(*client, parsedReq_.id, web::ErrorCode::kBusy);
        return true;
    }
#endif
    if (inflight_.ticket.seq != 0U) {
        sendAck(*client, parsedReq_.id, web::ErrorCode::kBusy);
        return true;
    }
    switch (parsedReq_.type) {
    case web::RequestType::kUnlock:
        handleUnlockReq(*client, parsedReq_);
        break;
    case web::RequestType::kCmd:
        handleCmdReq(*client, parsedReq_, nowMs);
        break;
    case web::RequestType::kSet:
        handleSetReq(*client, parsedReq_, nowMs);
        break;
    default:
        sendAck(*client, parsedReq_.id, web::ErrorCode::kBadJson);
        break;
    }
    return true;
}

void WebModule::pollStatusPush(uint32_t nowMs, bool forceNow) noexcept
{
    if (!forceNow && ((nowMs - lastStatusMs_) < kStatusPeriodMs)) {
        return;
    }
    if (!forceNow) {
        lastStatusMs_ = nowMs;
    }
    bool anyReady = false;
    for (const auto &slot : clients_) {
        if (slot.active && (slot.syncStage == SyncStage::kReady)) {
            anyReady = true;
            break;
        }
    }
    if (!anyReady) {
        return;
    }
    const auto cur = sampleStatus(nowMs);
    auto &encodeBuf = klinkEncodeFrame_->data;
    for (auto &slot : clients_) {
        if (!slot.active || (slot.syncStage != SyncStage::kReady)) {
            continue;
        }
        // 为 ack/config/bye 预留发送槽位，防止 1 Hz 状态推送挤占必达响应。
        if (txMailbox_.freeSlots() <= kTxReservedCtrlSlots) {
            break;
        }
        const web_cfg::StatusSnapshot *prev = slot.hasLastStatus ? &slot.lastStatus : nullptr;
        const size_t len = web_cfg::buildStatusFrame(cur, prev, encodeBuf);
        if (len == 0U) {
            continue;
        }
        if (enqueueTx(slot.fd, slot.gen, std::span<const char>{encodeBuf}.first(len))) {
            slot.lastStatus = cur;
            slot.hasLastStatus = true;
        }
    }
}

bool WebModule::pollIdleExit(uint32_t nowMs) noexcept
{
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    if (ota::transferBusy()) {
        return false;
    }
    if (otaIdleReset_.exchange(false, std::memory_order_acq_rel)) {
        nowMs = rtos::nowMs();
        idleTimer_.start(nowMs, static_cast<uint32_t>(CONFIG_RTK_WIFI_IDLE_EXIT_S) * kMsPerSecond);
    }
#endif
    if (pendingReboot_ != RebootKind::kNone || pendingExit_.fd >= 0) {
        return false;
    }
    const uint32_t staSnap = staState_.load(std::memory_order_acquire);
    const uint32_t actSeq = web_cfg::staStateSeq(staSnap);
    if (!idleTimer_.tick(web_cfg::staStateCount(staSnap), actSeq, nowMs)) {
        return false;
    }
    traceWeb(WebTrace::kIdleTrigger, static_cast<uint32_t>(CONFIG_RTK_WIFI_IDLE_EXIT_S), actSeq);
    scheduleReboot(RebootKind::kIdle, "idle", nowMs, actSeq);
    return true;
}

bool WebModule::poll(uint32_t nowMs) noexcept
{
    if (!initialized_) {
        return false;
    }
    commitPendingExit(nowMs);
    schedulePowerMode();
    if (powerPauseRequested_.load(std::memory_order_acquire) ||
        powerPaused_.load(std::memory_order_acquire)) {
        return false;
    }
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    const bool dnsMore = pollDns(nowMs);
#else
    constexpr bool dnsMore = false;
#endif
    drainCtrlEvents();
    if (!txMailbox_.empty() && !txWorkQueued_.load(std::memory_order_acquire)) {
        scheduleTxDrain();
    }
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    if (pendingReboot_ == RebootKind::kNone &&
        otaRebootRequested_.exchange(false, std::memory_order_acq_rel)) {
        // 从观察到应答完成后开始计时，同一轮检查也使用这个时刻。
        nowMs = rtos::nowMs();
        scheduleReboot(RebootKind::kOta, {}, nowMs);
    }
#endif
    if (pollPendingReboot(nowMs)) {
        return false;
    }
    const bool deliveryPending = pollPendingDeliveries();
    pollInflight(nowMs);
    const uint32_t configRev = ctx().data.configRev.load(std::memory_order_acquire);
    if (configVersion_ != configRev) {
        configVersion_ = configRev;
        broadcastConfig();
    }
    const bool syncMore = pollInitialSync(nowMs);
    const bool capsPending = pollCapsPush();
    const bool rxConsumed = pollRxFrame(nowMs);
    pollStatusPush(nowMs, false);

    if (pollIdleExit(nowMs)) {
        return false;
    }
    return dnsMore || deliveryPending || syncMore || capsPending ||
           (rxConsumed && !rxMailbox_.empty());
}

} // namespace app
