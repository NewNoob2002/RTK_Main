// 链路模块独占会话、协议服务与 RX 调度状态。
// init 仅启动前；其余操作仅 kLink；均非 ISR。
#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <type_traits>

#include "AppConfig.hpp"
#include "def/RtcmDef.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if RTK_GNSS_ENABLED
#include "core/services/GnssService.hpp"
#include "fw/FanoutRing.hpp"
#include "gnss/Types.hpp"
#endif
#include "demux/decoders/Rtcm3.hpp"
#include "demux/StaticDemux.hpp"
#include "spp/SppTransport.hpp"
#include "cli/Console.hpp"
#include "core/AlarmSource.hpp"
#include "core/CommandRouter.hpp"
#include "core/LinkManager.hpp"
#include "core/LinkService.hpp"
#include "core/services/AlarmService.hpp"
#include "core/services/ConfigService.hpp"
#include "core/services/DeviceService.hpp"
#include "core/services/PairService.hpp"
#include "proto/DemuxFrame.hpp"
#include "SppServer.h"

namespace app {
struct AppContext;
struct ModuleTestAccess;
/** 链路模块（kLink 执行器）：SPP 事件、分流器、路由与统计的唯一所有者；仅链路任务使用。 */
class BtLinkModule final {
public:
    constexpr BtLinkModule() noexcept = default;
    constexpr explicit BtLinkModule(AppContext &context) noexcept : context_(&context) {}
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~BtLinkModule() = default;
    BtLinkModule(const BtLinkModule &) = delete;
    BtLinkModule &operator=(const BtLinkModule &) = delete;
    BtLinkModule(BtLinkModule &&) = delete;
    BtLinkModule &operator=(BtLinkModule &&) = delete;
    /** 启动前单线程，非 ISR；构造服务且不分配堆，失败返回 kInvalidState。 */
    [[nodiscard]] fw::Result init() noexcept;
    /** init 成功后仅 kLink 调用，非 ISR、非阻塞；发布快照，返回是否仍有 RX 积压。 */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;
    /** init 成功后仅 kLink 调用，非 ISR；白名单清除可同步写 NVS，结果写入 result。 */
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /** 仅 kLink，非 ISR；至多等待 maxMs，先处理生命周期事件。 */
    void wait(uint32_t maxMs) noexcept;
    /** 仅 kLink 或启动前，非 ISR；返回直推 RTCM 累计统计快照。 */
    [[nodiscard]] const RtcmLinkSnapshot &rtcmSnapshot() const noexcept
    {
        return rtcm_snapshot_;
    }

private:
    friend struct ModuleTestAccess;
    using SessionDemux = demux::StaticDemux<proto::demux_frame::kDecoder, demux::rtcm3::kDecoder>;
    static constexpr uint8_t kProductIndex = SessionDemux::indexOf<proto::demux_frame::kDecoder>();
    static constexpr uint8_t kRtcmIndex = SessionDemux::indexOf<demux::rtcm3::kDecoder>();
    static constexpr size_t kRxChunkBytes = kLinkRxBufBytes;
    static constexpr size_t kRxBudgetBytes = kLinkRxBufBytes;
    class SessionSink final : public demux::Sink {
    public:
        constexpr SessionSink() noexcept = default;
        // 仅 kLink，非 ISR；OPEN/CLOSE 时复位元数据，owner 覆盖本会话生命周期。
        void bind(BtLinkModule &owner, core::SessionHandle handle) noexcept;
        void onFrame(const demux::Decoder &decoder, uint8_t index,
                     std::span<const uint8_t> wire) noexcept override;
        [[nodiscard]] bool healthy() const noexcept
        {
            return !unhealthy_;
        }

    private:
        BtLinkModule *owner_ = nullptr;
        core::SessionHandle handle_ = 0;
        bool unhealthy_ = false;
    };
    struct SessionEntry {
        bool used = false;
        core::SessionHandle handle = 0;
        core::BtAddr addr{};
        bool drainPending = false;
        std::array<uint8_t, kRxChunkBytes> rx{};
        size_t rxOffset = 0;
        size_t rxSize = 0;
        std::optional<SessionSink> sink{};
        std::optional<SessionDemux> demux{};
        bool demuxReady = false;
        // 分流器累计统计的上报基线，仅 kLink 读写；会话复用时归零。
        uint32_t seenResyncs = 0;
        uint32_t seenUnknown = 0;
    };
    static_assert(std::is_trivially_destructible_v<SessionSink>,
                  "SessionSink must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<SessionDemux>,
                  "SessionDemux must be trivially destructible");
    static_assert(std::is_trivially_destructible_v<spp::SppTransport>,
                  "SppTransport must be trivially destructible");

    [[nodiscard]] SessionEntry *sessionFind(core::SessionHandle handle) noexcept;
    /**
     * @brief  会话关闭：中止该会话的分流器并清除记录
     * @param  handle: 会话句柄；未记录时只忽略
     * @note   仅 kLink 调用。
     */
    void sessionForget(core::SessionHandle handle) noexcept;
    /**
     * @brief  会话打开：分配表项、记录对端地址并初始化分流器
     * @param  handle: 会话句柄
     * @param  addr: 对端蓝牙地址
     * @note   仅 kLink 调用；表满或分流器初始化失败记 W 级日志，该会话不解析。
     */
    void sessionRecord(core::SessionHandle handle, const core::BtAddr &addr) noexcept;
    [[nodiscard]] core::LinkService &linkService() noexcept;
    void collectWhitelistDirty() noexcept;
    void handleEvent(const spp_server_evt_t &evt, uint32_t nowMs) noexcept;
    void handleOpen(const spp_server_evt_t &evt, uint32_t nowMs) noexcept;
    void handleClose(core::SessionHandle handle) noexcept;
    void handleData(core::SessionHandle handle) noexcept;
    void handleRtcmFrame(core::SessionHandle handle, std::span<const uint8_t> wire) noexcept;
    [[nodiscard]] bool pollAndCheckReady(uint32_t nowMs) noexcept;
    static void onRxWake(void *ctx) noexcept;
    void drainRx(uint32_t roundStartMs) noexcept;
    [[nodiscard]] bool drainSession(SessionEntry &entry, size_t index, size_t &budget,
                                    uint32_t roundStartMs) noexcept;
    /**
     * @brief  从会话 RX 取一块字节喂给分流器
     * @param  entry: 会话表项
     * @param[in,out] budget: 本轮剩余字节预算，按实际消费扣减
     * @param  roundStartMs: 本轮开始时间（ms），用于超时判断
     * @retval true 还有积压可继续读
     * @retval false 已读空或分流器已停止
     */
    [[nodiscard]] bool feedOne(SessionEntry &entry, size_t &budget, uint32_t roundStartMs) noexcept;
    void syncStream() noexcept;
    void publishLink(uint32_t nowMs) noexcept;
    /** 发布链路策略/配对/白名单快照（publishLink 第一节）；仅 kLink，非阻塞。 */
    void publishLinkStatus(uint32_t nowMs) noexcept;
    /** 发布当前会话表快照；仅 kLink，非阻塞。 */
    void publishSessions() noexcept;
    /** 发布报警状态（无报警时写空快照）；仅 kLink，非阻塞。 */
    void publishAlarmState() noexcept;
    /** 发布发送/心跳/分流统计；仅 kLink，非阻塞。 */
    void publishStats() noexcept;
    /** 汇总各会话分流器增量并上报重同步/未知字节；仅 kLink，非阻塞。 */
    void publishDemuxStats() noexcept;
    void applyRtcmReset() noexcept;
#if RTK_GNSS_ENABLED
    void syncGnssInfo(uint32_t nowMs) noexcept;
    void resetGnssStreamSession() noexcept;
    [[nodiscard]] bool drainGnssStream() noexcept;
    static core::services::GnssQueryResult onGnssQuery(void *ctx, uint16_t msgId,
                                                       std::span<uint8_t> dst) noexcept;
    static void onGnssMask(void *ctx, uint32_t mask) noexcept;
#endif
    void setAlarm(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    void clearWhitelist(fw::CommandResult &result) noexcept;
    void editWhitelist(const fw::Command &cmd, bool add, fw::CommandResult &result) noexcept;
    void persistWhitelist(fw::CommandResult &result) noexcept;

    static_assert(std::is_trivially_destructible_v<core::AlarmSource>,
                  "AlarmSource must be trivially destructible");

    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    AppContext *context_ = nullptr;
    uint32_t last_tick_ms_ = 0;
    uint32_t reset_version_ = 0;
    uint32_t round_start_ms_ = 0;
    DemuxStats retired_demux_{};
    core::LinkManager link_manager_{};
    std::optional<core::AlarmSource> alarm_{};
    std::optional<spp::SppTransport> transport_{};
    core::SessionHandle stream_handle_ = 0;
    // optional 在 init 按依赖次序原地构造，生命周期覆盖整个运行期，不分配堆。
    std::optional<core::services::DeviceService> device_service_{};
    std::optional<core::services::ConfigService> config_service_{};
    std::optional<core::services::AlarmService> alarm_service_{};
    std::optional<core::services::PairService> pair_service_{};
#if RTK_GNSS_ENABLED
    std::optional<core::services::GnssService> gnss_service_{};
    uint8_t gnss_reader_id_ = 0U;
    bool gnss_reader_attached_ = false;
    // App 环单条记录上限；跨环尾记录经 spp_server_tx_commit2 两段提交。
    static constexpr size_t kGnssStreamMaxRecord = kGnssAppRingBytes - fw::kFanoutTypedHeaderBytes;
#endif
    core::CommandRouter router_{};
    std::optional<core::LinkService> service_{};
    std::array<SessionEntry, kMaxSessions> sessions_{};
    size_t rx_cursor_ = 0;
    RtcmLinkSnapshot rtcm_snapshot_{};
    std::atomic<TaskHandle_t> wake_task_{nullptr};
#if RTK_GNSS_ENABLED
    static_assert(std::is_trivially_destructible_v<::gnss::Fix>,
                  "gnss::Fix must be trivially destructible");
    uint32_t gnss_fix_version_ = 0;
    uint32_t gnss_status_version_ = 0;
    std::optional<::gnss::Fix> cached_gnss_fix_{};
#endif
};
/** 链路模块的只读控制台命令表（bt/alarm）；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> linkCmds() noexcept;
} // namespace app
