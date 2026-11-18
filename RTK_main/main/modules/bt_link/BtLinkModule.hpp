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
    /**
     * @brief 记录所属上下文，不初始化硬件或 RTOS
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅对象构造时调用，不分配、不阻塞；非 ISR。
     */
    constexpr explicit BtLinkModule(AppContext &context) noexcept : context_(&context) {}
    /**
     * @brief 绑定模块所属上下文
     * @param context: 所属应用上下文，生命周期须覆盖模块
     * @note 仅消费者启动前单线程调用；不分配、不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~BtLinkModule() = default;
    BtLinkModule(const BtLinkModule &) = delete;
    BtLinkModule &operator=(const BtLinkModule &) = delete;
    BtLinkModule(BtLinkModule &&) = delete;
    BtLinkModule &operator=(BtLinkModule &&) = delete;
    /**
     * @brief  原地构造链路服务并安装接收唤醒钩子
     * @retval fw::Result::kOk 成功；其他为失败原因（已记 E 级日志）
     * @note   启动前单线程，非 ISR，不分配堆。
     */
    [[nodiscard]] fw::Result init() noexcept;
    /**
     * @brief init 成功后仅 kLink 调用，非 ISR、非阻塞；发布快照，返回是否仍有 RX 积压。
     * @param nowMs: 当前单调时间（毫秒）
     * @retval true 尚有可处理积压
     * @retval false 无立即积压或模块未就绪
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;
    /**
     * @brief init 成功后仅 kLink 调用，非 ISR；白名单清除可同步写 NVS，结果写入 result。
     * @param cmd: 待处理指令
     * @param[out] result: 执行结果输出
     * @note 仅所属模块执行器任务调用，非 ISR；涉及存储时可同步等待 NVS。
     */
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /**
     * @brief 仅 kLink，非 ISR；至多等待 maxMs，先处理生命周期事件。
     * @param maxMs: 最长等待时间（毫秒）
     * @note 仅 kLink 调用；可等待任务通知至 maxMs，非 ISR。
     */
    void wait(uint32_t maxMs) noexcept;
    /**
     * @brief 仅 kLink 或启动前，非 ISR；返回直推 RTCM 累计统计快照。
     * @retval 仅 kLink 写入的 RTCM 累计统计引用
     * @note 仅 kLink 或启动前读取，非 ISR，不阻塞。
     */
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
        /**
         * @brief 把本 sink 绑定到模块实例
         * @param owner: owner 见调用点
         * @param handle: 会话句柄
         * @retval 无返回值（嵌套类方法的返回类型解析限制，此处显式标注 void）
         * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
         */
        void bind(BtLinkModule &owner, core::SessionHandle handle) noexcept;
        void onFrame(const demux::Decoder &decoder, uint8_t index,
                     std::span<const uint8_t> wire) noexcept override;
        /**
         * @brief 查询本会话分流 sink 是否仍可用
         * @retval true 尚未标记异常
         * @retval false 已标记异常
         * @note 仅 kLink，非 ISR，不阻塞。
         */
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

    /**
     * @brief 按句柄查找会话槽
     * @param handle: 会话句柄
     * @retval 返回对象指针，可能为空
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
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
    /**
     * @brief 取得已原地构造的链路服务
     * @retval 当前链路服务引用
     * @note 仅 kLink；须 init 成功，不阻塞，非 ISR。
     */
    [[nodiscard]] core::LinkService &linkService() noexcept;
    /**
     * @brief 把白名单变更标记落库
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void collectWhitelistDirty() noexcept;
    /**
     * @brief 处理一条 SPP 事件（按类型分发）
     * @param evt: SPP 事件
     * @param nowMs: 单调毫秒时间戳
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void handleEvent(const spp_server_evt_t &evt, uint32_t nowMs) noexcept;
    /**
     * @brief 处理 SPP 会话打开事件
     * @param evt: SPP 事件
     * @param nowMs: 单调毫秒时间戳
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void handleOpen(const spp_server_evt_t &evt, uint32_t nowMs) noexcept;
    /**
     * @brief 处理 SPP 会话关闭事件
     * @param handle: 会话句柄
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void handleClose(core::SessionHandle handle) noexcept;
    /**
     * @brief 处理 SPP 收到的数据事件
     * @param handle: 会话句柄
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void handleData(core::SessionHandle handle) noexcept;
    /**
     * @brief 处理一帧 RTCM（转发给会话与统计）
     * @param handle: 会话句柄
     * @param wire: wire 见调用点
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void handleRtcmFrame(core::SessionHandle handle, std::span<const uint8_t> wire) noexcept;
    /**
     * @brief 处理生命周期事件并检查会话是否可继续读取
     * @param nowMs: 当前时间（毫秒）
     * @retval true 至少一个会话有可处理的 RX 或待发送应答
     * @retval false 没有就绪会话
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    [[nodiscard]] bool pollAndCheckReady(uint32_t nowMs) noexcept;
    /**
     * @brief 把 SPP 接收通知转发给链路任务
     * @param ctx: 指向 BtLinkModule 的回调上下文
     * @note SPP 任务回调，非 ISR；只发任务通知，不解析、不阻塞。
     */
    static void onRxWake(void *ctx) noexcept;
    /**
     * @brief 按每轮预算轮询会话并消费 SPP 接收数据
     * @param roundStartMs: roundStartMs 见调用点
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void drainRx(uint32_t roundStartMs) noexcept;
    /**
     * @brief 按本轮预算消费一个会话
     * @param entry: 当前会话状态
     * @param index: 会话槽下标
     * @param budget: 本轮剩余字节预算，按消费扣减
     * @param roundStartMs: 本轮开始时间（毫秒）
     * @retval true 仍有待消费数据
     * @retval false 本会话读空、异常或暂停
     * @note 仅 kLink，非 ISR，不阻塞。
     */
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
    /**
     * @brief 把会话与 RTCM 流通道对齐
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void syncStream() noexcept;
    /**
     * @brief 发布链路、会话、报警与统计快照（每轮一次，顺序固定）
     * @param nowMs: 本次快照时间
     * @note 仅 kLink 任务；非阻塞、非 ISR。
     */
    void publishLink(uint32_t nowMs) noexcept;
    /**
     * @brief 发布链路策略/配对/白名单快照
     * @param nowMs: 本次快照时间
     * @note 仅 kLink 任务；非阻塞、不分配堆。
     */
    void publishLinkStatus(uint32_t nowMs) noexcept;
    /**
     * @brief 发布当前会话表（句柄、角色、对端地址）
     * @note 仅 kLink 任务；只读会话表，非阻塞。
     */
    void publishSessions() noexcept;
    /**
     * @brief 发布报警状态；无报警时写空快照
     * @note 仅 kLink 任务；非阻塞。
     */
    void publishAlarmState() noexcept;
    /**
     * @brief 发布发送、心跳与分流统计（顺序与基线一致）
     * @note 仅 kLink 任务；spp_server_*_stats 只读快照，非阻塞。
     */
    void publishStats() noexcept;
    /**
     * @brief 汇总各会话分流器统计并在增量变化时上报重同步/未知字节
     * @note 仅 kLink 任务；每轮取一次增量，避免在逐字节 feed 路径上读统计。
     */
    void publishDemuxStats() noexcept;
    /**
     * @brief 处理 RTCM 复位请求（版本号变化时清空转发器与解复用器）
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void applyRtcmReset() noexcept;
#if RTK_GNSS_ENABLED
    /**
     * @brief 把 GNSS 定位状态同步到 DataCenter（供对端查询）
     * @param nowMs: 单调毫秒时间戳
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void syncGnssInfo(uint32_t nowMs) noexcept;
    /**
     * @brief 把 GNSS 流绑定会话标记为待重绑
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void resetGnssStreamSession() noexcept;
    /**
     * @brief 把 App 环记录提交给当前手机会话
     * @retval true 本轮已处理满预算，可能仍有记录
     * @retval false 无会话、读者或记录，或发送背压/提交失败
     * @note 仅 kLink，非 ISR，不阻塞；发送失败取消当前租约，保留待发记录。
     */
    [[nodiscard]] bool drainGnssStream() noexcept;
    /**
     * @brief 读取一次 GNSS 最新语句查询
     * @param ctx: BtLinkModule 实例
     * @param msgId: 协议消息号
     * @param[out] dst: 字节输出；失败不变
     * @retval 空结果 上下文为空、消息未知或没有缓存
     * @retval ok=true 已复制最新帧
     * @retval oversize=true 帧超过查询或目标容量
     * @note 仅 kLink，缓存内部加锁，非 ISR，不阻塞。
     */
    static core::services::GnssQueryResult onGnssQuery(void *ctx, uint16_t msgId,
                                                       std::span<uint8_t> dst) noexcept;
    /**
     * @brief 处理对端的 GNSS 输出掩码设置
     * @param ctx: cli 上下文（未用）
     * @param mask: mask 见调用点
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    static void onGnssMask(void *ctx, uint32_t mask) noexcept;
#endif
    /**
     * @brief 执行 alarm set 命令
     * @param cmd: cmd 见调用点
     * @param[out] result: result 见调用点
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void setAlarm(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /**
     * @brief 执行 whitelist clear 命令
     * @param[out] result: result 见调用点
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void clearWhitelist(fw::CommandResult &result) noexcept;
    /**
     * @brief 执行白名单增删命令
     * @param cmd: cmd 见调用点
     * @param add: add 见调用点
     * @param[out] result: result 见调用点
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void editWhitelist(const fw::Command &cmd, bool add, fw::CommandResult &result) noexcept;
    /**
     * @brief 把白名单写入 NVS
     * @param[out] result: result 见调用点
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void persistWhitelist(fw::CommandResult &result) noexcept;

    static_assert(std::is_trivially_destructible_v<core::AlarmSource>,
                  "AlarmSource must be trivially destructible");

    /**
     * @brief 取得已绑定的应用上下文
     * @retval 模块所属上下文引用
     * @note 仅模块所属任务调用；须已 bindContext，非 ISR，不阻塞。
     */
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
/**
 * @brief 取得链路命令表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取；不分配、不阻塞，非 ISR。
 * @note 链路模块的只读控制台命令表（bt/alarm）；静态存储，进程期有效。
 */
[[nodiscard]] std::span<const cli::Cmd> linkCmds() noexcept;
} // namespace app
