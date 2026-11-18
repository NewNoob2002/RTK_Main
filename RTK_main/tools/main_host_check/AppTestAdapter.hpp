// 原有场景的接线适配：调用真实模块，旧测试的断言字段只在此处由主题装配。
#pragma once
// 保留基线白盒场景对会话/预读状态的检查；不额外编译此 cpp，避免重复定义。
// NOLINTBEGIN(bugprone-suspicious-include): 白盒适配需直接包含实现，不再单独编译这些 cpp。
#include "../../main/modules/bt_link/BtLinkModule.cpp"
#include "../../main/modules/bt_link/BtLinkSessions.cpp"
#include "../../main/modules/bt_link/BtLinkGnss.cpp"
// NOLINTEND(bugprone-suspicious-include)
#include "../../main/modules/rtcm/RtcmModule.hpp"
#include "../../main/modules/storage/StorageModule.hpp"
#include "rtos/StaticQueue.hpp"

namespace app {
namespace {
constinit AppContext s_test_app{};
}
AppContext &appContext() noexcept
{
    static bool s_bound = false;
    if (!s_bound) {
        s_bound = true;
        s_test_app.bindContexts();
    }
    return s_test_app;
}

// 原有模块场景直接从运行态接线；真实启动门控由 boot_alloc_check 测试。
bool AppContext::awaitBoot(BootTask, bool) noexcept
{
    return true;
}

// 仅测试接线可访问模块私有状态，保留原场景的逐字段断言。
struct ModuleTestAccess {
    static constexpr size_t kRxBudgetBytes = BtLinkModule::kRxBudgetBytes;
    static auto &manager() noexcept
    {
        return appContext().bt_link.link_manager_;
    }
    static auto &alarm() noexcept
    {
        if (!appContext().bt_link.alarm_.has_value()) {
            appContext().bt_link.alarm_.emplace();
        }
        return *appContext().bt_link.alarm_;
    }
    static auto &service() noexcept
    {
        return appContext().bt_link.linkService();
    }
    static auto &sessions() noexcept
    {
        return appContext().bt_link.sessions_;
    }
    static auto &last_tick_ms() noexcept
    {
        return appContext().bt_link.last_tick_ms_;
    }
    static auto &whitelistPending() noexcept
    {
        return appContext().storage.pending_;
    }
    static auto &whitelist_attempt_ms() noexcept
    {
        return appContext().storage.attempt_ms_;
    }
    static void handleEvent(const spp_server_evt_t &evt, uint32_t now_ms) noexcept
    {
        appContext().bt_link.handleEvent(evt, now_ms);
    }
    static void drainRx(uint32_t now_ms) noexcept
    {
        appContext().bt_link.drainRx(now_ms);
    }
    static void collectWhitelistDirty() noexcept
    {
        appContext().bt_link.collectWhitelistDirty();
    }
    static void applyRtcmReset() noexcept
    {
        appContext().bt_link.applyRtcmReset();
    }
    static void publishLink(uint32_t now_ms) noexcept
    {
        appContext().bt_link.publishLink(now_ms);
    }
    static void flush_rtcm() noexcept
    {
        static_cast<void>(appContext().rtcm.poll(rtos::nowMs()));
    }
};
namespace {
constexpr size_t kRxBudgetBytes = ModuleTestAccess::kRxBudgetBytes;
// 旧白盒断言的名字仅限本测试 TU，产品没有这些二次暴露。
auto &s_link_manager = ModuleTestAccess::manager();
auto &s_alarm = ModuleTestAccess::alarm();
struct LinkServiceProbe {
    core::LinkService *operator->() const noexcept
    {
        return &ModuleTestAccess::service();
    }
};
constinit LinkServiceProbe s_service{};
auto &s_sessions = ModuleTestAccess::sessions();
void handleEvent(const spp_server_evt_t &evt, uint32_t now_ms) noexcept
{
    ModuleTestAccess::handleEvent(evt, now_ms);
}
void drainRx(uint32_t now_ms) noexcept
{
    ModuleTestAccess::drainRx(now_ms);
}
void collectWhitelistDirty() noexcept
{
    ModuleTestAccess::collectWhitelistDirty();
}
void markWhitelistDirty(std::span<const core::BtAddr> list, uint32_t now_ms) noexcept
{
    appContext().storage.markWhitelistDirty(list, now_ms);
}
using WhitelistRaw = std::array<std::array<uint8_t, core::kAddrLen>, kMaxAlarms>;
constexpr uint32_t kWhitelistRetryMs = 1000;
enum class CommandId { kAlarmSet, kAlarmClear, kPair, kWhitelistClear, kSaveSn, kRtcmReset };
enum class CommandStatus { kOk, kInvalidArg, kStoreError };
struct Command {
    CommandId id{};
    uint8_t state = 0;
    uint8_t level = 0;
    uint32_t seq = 0;
};
struct CommandResult {
    uint32_t seq = 0;
    CommandStatus status{};
};
rtos::StaticQueue<Command, 4> s_command_queue{};
rtos::StaticQueue<CommandResult, 1> s_result_queue{};
bool &s_whitelist_pending = ModuleTestAccess::whitelistPending();
uint32_t &s_whitelist_attempt_ms = ModuleTestAccess::whitelist_attempt_ms();
struct StatusSnapshot {
    bool whitelistPending = false;
    esp_err_t whitelistError = ESP_OK;
    bool btReady = false;
    bool linkRunning = false;
    core::Policy policy = policyFromConfig();
    size_t session_count = 0;
    std::array<SessionSnapshot, kMaxSessions> sessions{};
    uint32_t rtcm_frames = 0;
    uint32_t rtcm_bytes = 0;
    uint32_t rtcm_crc32 = 0;
    uint16_t rtcm_last_msg = 0;
    uint32_t rtcm_rejected_role = 0;
    uint32_t demux_unknown_bytes = 0;
    uint32_t demux_product_rejects = 0;
    uint32_t demux_rtcm_rejects = 0;
    uint32_t parserCrcErrors = 0;
    uint32_t parserLengthErrors = 0;
    uint32_t rx_dropped = 0;
};
StatusSnapshot s_snapshot{};
void flush_rtcm()
{
    ModuleTestAccess::flush_rtcm();
}
struct RtcmProbe {
    const RtcmStats &stats() const
    {
        flush_rtcm();
        return appContext().rtcm.stats();
    }
} s_rtcm;
void publish_snapshot(uint32_t now_ms)
{
    ModuleTestAccess::applyRtcmReset();
    ModuleTestAccess::publishLink(now_ms);
    flush_rtcm();
    const auto rtcm = appContext().data.rtcmStats.read();
    const auto demux = appContext().data.demuxStats.read();
    const auto store = appContext().data.storeStatus.read();
    const auto sessions = appContext().data.sessions.read();
    const auto stats = appContext().data.linkStats.read();
    s_snapshot.whitelistPending = store.whitelistPending;
    s_snapshot.whitelistError = store.whitelistError;
    s_snapshot.session_count = sessions.count;
    s_snapshot.sessions = sessions.entries;
    s_snapshot.rtcm_frames = rtcm.frames;
    s_snapshot.rtcm_bytes = rtcm.bytes;
    s_snapshot.rtcm_crc32 = rtcm.crc32;
    s_snapshot.rtcm_last_msg = rtcm.lastMsg;
    s_snapshot.rtcm_rejected_role = rtcm.rejectedRole;
    s_snapshot.demux_unknown_bytes = demux.unknownBytes;
    s_snapshot.demux_product_rejects = demux.productRejects;
    s_snapshot.demux_rtcm_rejects = demux.rtcmRejects;
    s_snapshot.parserCrcErrors = stats.parserCrcErrors;
    s_snapshot.parserLengthErrors = stats.parserLengthErrors;
    s_snapshot.rx_dropped = stats.rxDropped;
}
void note_bt_failure()
{
    s_snapshot.btReady = false;
}
void read_snapshot(StatusSnapshot &out)
{
    out = s_snapshot;
}
void persistWhitelist(uint32_t now_ms, bool force = false)
{
    if (force) {
        static_cast<void>(appContext().storage.flushWhitelist(now_ms));
    } else {
        static_cast<void>(appContext().storage.poll(now_ms));
    }
}
void process_commands(uint32_t /*nowMs*/)
{
    Command queued{};
    while (s_command_queue.receive(queued, 0)) {
        fw::Command command{};
        command.seq = queued.seq;
        fw::CommandResult result{};
        if (queued.id == CommandId::kRtcmReset) {
            command.verb = verb(RtcmVerb::kReset);
            appContext().rtcm.onCommand(command, result);
            ModuleTestAccess::applyRtcmReset();
        } else {
            switch (queued.id) {
            case CommandId::kAlarmSet:
                command.verb = verb(LinkVerb::kAlarmSet);
                command.argLen = 2;
                command.args[0] = queued.state;
                command.args[1] = queued.level;
                break;
            case CommandId::kAlarmClear:
                command.verb = verb(LinkVerb::kAlarmClear);
                break;
            case CommandId::kPair:
                command.verb = verb(LinkVerb::kPair);
                break;
            case CommandId::kWhitelistClear:
                command.verb = verb(LinkVerb::kWhitelistClear);
                break;
            default:
                command.verb = 0xFF;
                break;
            }
            appContext().bt_link.onCommand(command, result);
        }
        const CommandResult response{
            queued.seq, result.status == fw::Result::kOk        ? CommandStatus::kOk
                        : result.status == fw::Result::kIoError ? CommandStatus::kStoreError
                                                                : CommandStatus::kInvalidArg};
        if (!s_result_queue.send(response, 0)) {
            CommandResult stale{};
            static_cast<void>(s_result_queue.receive(stale, 0));
            static_cast<void>(s_result_queue.send(response, 0));
        }
    }
}
void link_round(uint32_t &last_tick_ms)
{
    bool pending = false;
    for (const auto &entry : s_sessions) {
        pending = pending || (entry.used && entry.demuxReady &&
                              (entry.drainPending || entry.rxOffset < entry.rxSize));
    }
    appContext().bt_link.wait(pending ? 0U : 20U);
    const uint32_t begin = rtos::nowMs();
    process_commands(begin);
    ModuleTestAccess::last_tick_ms() = last_tick_ms;
    static_cast<void>(appContext().bt_link.poll(begin));
    last_tick_ms = ModuleTestAccess::last_tick_ms();
    flush_rtcm();
    static_cast<void>(appContext().storage.poll(rtos::nowMs()));
    publish_snapshot(rtos::nowMs());
}
} // namespace
} // namespace app
