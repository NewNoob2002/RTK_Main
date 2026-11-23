// 链路模块独占会话、协议服务与 RX 调度状态。
// init 仅启动前；其余操作仅 kLink；均非 ISR。
#pragma once

#include "PairWindow.hpp"

#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "AppConfig.hpp"
#include "def/RtcmDef.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if RTK_GNSS_ENABLED
#include "core/services/GnssService.hpp"
#include "data/BaseRtcmRing.hpp"
#include "fw/FanoutRing.hpp"
#include "gnss/Types.hpp"
#endif
#include "demux/decoders/Rtcm3.hpp"
#include "demux/StaticDemux.hpp"
#include "spp/SppTransport.hpp"
#include "cli/Console.hpp"
#include "core/AlarmSource.hpp"
#include "core/CommandRouter.hpp"
#include "PhoneCompatService.hpp"
#include "core/LinkManager.hpp"
#include "core/LinkService.hpp"
#include "core/services/AlarmService.hpp"
#include "core/services/ConfigService.hpp"
#include "core/services/DeviceService.hpp"
#include "core/services/PairService.hpp"
#include "proto/DemuxFrame.hpp"
#include "settings/Types.hpp"
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

    /** @brief 安装 GAP 策略。 @note 启动任务在蓝牙初始化前调用。 */
    void installPairingPolicy() noexcept;

    /** @brief 查询配对窗口。 @param nowMs: 当前时间。 @retval 剩余时长及确认模式。 @note 可跨任务读取。 */
    [[nodiscard]] core::PairWindowStatus pairStatus(uint32_t nowMs) const noexcept
    {
        return pair_window_.snapshot(nowMs);
    }
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

    /** @brief 读取当前对外上报的设备 SN（0x01/0x08 共用）。 @retval SN 视图。 @note 仅 kLink。 */
    [[nodiscard]] std::string_view deviceSn() const noexcept;

    /** @brief 更新内存中 DeviceInfo 的 SN（0x08 保存成功后同步）。 @param sn: 新 SN 视图。 @note 仅 kLink。 */
    void updateDeviceSn(std::string_view sn) noexcept;

    /** @brief 安排延迟 >=200 ms 的恢复出厂或重启（0x1E）。 @param factoryReset: 是否擦除用户 NVS。 @param reboot: 是否重启。 @param nowMs: 当前时钟。 @retval true 已安排。 @note 仅 kLink。 */
    [[nodiscard]] bool schedulePhoneReset(bool factoryReset, bool reboot, uint32_t nowMs) noexcept;

    /** @brief 暂停或恢复蓝牙接入并断开已存在的连接。 @param paused: 是否静默。 @note 仅 kLink，非 ISR。 */
    void setPowerPaused(bool paused) noexcept;

    /** @brief 查询已登记连接是否都已关闭。 @retval true 已无 SPP 会话。 @note 仅 kLink，非 ISR。 */
    [[nodiscard]] bool powerPauseDone() const noexcept;

    /**
     * @brief 查询是否仍有在途的手机设置异步凭据（供关机 kSave 阶段等待完成）
     * @retval true 有在途设置凭据
     * @retval false 无在途设置凭据
     * @note 仅 kLink 调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool hasPendingSettings() const noexcept
    {
        return hasPendingTicket(false);
    }

private:
    enum class PendingReset : uint8_t {
        kNone = 0,
        kReboot = 1,
        kFactory = 2,
        kWifiConfig = 3,
    };

    /** @brief 检查到期的 0x1E 恢复出厂或重启请求并执行。 @param nowMs: 当前时钟。 @note 仅 kLink。 */
    void pollPhoneReset(uint32_t nowMs) noexcept;
    /** @brief 0x28 到期：写入 kWifiConfig 单次启动记录并重启（不支持 Wi-Fi 的构建只记日志后返回，由调用方 esp_restart）。 @note 仅 kLink。 */
    static void restartIntoWifiConfig() noexcept;
    /** @brief 检测配置版本号变化并从 settings 同步 DeviceInfo.sn（0x01/0x02）。 @note 仅 kLink。 */
    void syncDeviceSn() noexcept;
    /** @brief 查询是否有手机兼容异步设置凭据在途。 @param localOnly: true 仅查同执行器凭据（height/snpn）。 @retval true 有在途凭据。 @note 仅 kLink。 */
    [[nodiscard]] bool hasPendingTicket(bool localOnly = false) const noexcept;
    /** @brief 构造 PhoneCompatService 回调表，将配置类操作接入 settings::Service。 @retval 手机兼容回调表。 @note 仅 init 调用。 */
    [[nodiscard]] core::services::PhoneCompatOps makePhoneCompatOps() noexcept;
    /** @brief 手机 0x24 读取天线高。 @param ctx: BtLinkModule 指针。 @retval 8 B 小端双精度。 */
    [[nodiscard]] static core::AntennaHeight onReadHeight(const void *ctx) noexcept;
    /** @brief 手机 0x24 异步投递天线高设置。 @param ctx: BtLinkModule 指针。 @param height: 8 B 小端字节。 @retval true 已投递。 */
    [[nodiscard]] static bool onStartHeight(void *ctx, const core::AntennaHeight &height) noexcept;
    /** @brief 手机 0x24 轮询天线高设置结果。 @param ctx: BtLinkModule 指针。 @retval 完成时返回成败。 */
    [[nodiscard]] static std::optional<bool> onHeightResult(const void *ctx) noexcept;
    /** @brief 手机 0x24 取消天线高在途凭据。 @param ctx: BtLinkModule 指针。 */
    static void onCancelHeight(void *ctx) noexcept;
    /** @brief 手机 0x33 查询接收机是否支持 PPP。 @param ctx: BtLinkModule 指针。 @retval true 支持。 */
    [[nodiscard]] static bool onPppSupported(const void *ctx) noexcept;
    /** @brief 手机 0x33 读取当前 PPP 开关。 @param ctx: BtLinkModule 指针。 @retval PPP 状态。 */
    [[nodiscard]] static bool onReadPpp(const void *ctx) noexcept;
    /** @brief 手机 0x33 异步投递 PPP 设置。 @param ctx: BtLinkModule 指针。 @param enabled: 开关值。 @retval true 已投递。 */
    [[nodiscard]] static bool onStartPpp(void *ctx, bool enabled) noexcept;
    /** @brief 手机 0x33 轮询 PPP 设置结果。 @param ctx: BtLinkModule 指针。 @retval 完成时返回成败。 */
    [[nodiscard]] static std::optional<bool> onPppResult(const void *ctx) noexcept;
    /** @brief 手机 0x33 取消 PPP 在途凭据。 @param ctx: BtLinkModule 指针。 */
    static void onCancelPpp(void *ctx) noexcept;
    /** @brief 手机 0x06 读取支持的星座能力掩码。 @param ctx: BtLinkModule 指针。 @retval 能力位图。 */
    [[nodiscard]] static uint8_t onSatCapsMask(const void *ctx) noexcept;
    /** @brief 手机 0x06 读取当前截止角与星座配置。 @param ctx: BtLinkModule 指针。 @retval 卫星配置。 */
    [[nodiscard]] static core::SatTrackConfig onReadSatConfig(const void *ctx) noexcept;
    /** @brief 手机 0x06 异步投递卫星配置。 @param ctx: BtLinkModule 指针。 @param cfg: 合法配置。 @retval true 已投递。 */
    [[nodiscard]] static bool onStartSatConfig(void *ctx, const core::SatTrackConfig &cfg) noexcept;
    /** @brief 手机 0x06 轮询卫星配置结果。 @param ctx: BtLinkModule 指针。 @retval 完成时返回成败。 */
    [[nodiscard]] static std::optional<bool> onSatConfigResult(const void *ctx) noexcept;
    /** @brief 手机 0x06 取消卫星配置在途凭据。 @param ctx: BtLinkModule 指针。 */
    static void onCancelSatConfig(void *ctx) noexcept;
    /** @brief 手机 0x08 读取当前 SN/PN。 @param ctx: BtLinkModule 指针。 @retval 32 B 组合字段。 */
    [[nodiscard]] static core::PhoneSnPn onReadSnPn(const void *ctx) noexcept;
    /** @brief 手机 0x08 异步投递 SN/PN 设置。 @param ctx: BtLinkModule 指针。 @param snPn: 32 B 组合字段。 @retval true 已投递。 */
    [[nodiscard]] static bool onStartSnPn(void *ctx, const core::PhoneSnPn &snPn) noexcept;
    /** @brief 手机 0x08 轮询 SN/PN 设置结果并同步内存 SN。 @param ctx: BtLinkModule 指针。 @retval 完成时返回成败。 */
    [[nodiscard]] static std::optional<bool> onSnPnResult(const void *ctx) noexcept;
    /** @brief 手机 0x08 取消 SN/PN 在途凭据。 @param ctx: BtLinkModule 指针。 */
    static void onCancelSnPn(void *ctx) noexcept;
#if RTK_GNSS_ENABLED
    /** @brief 手机 0x0A Nav 读取最近单句 GSV 原文。 @param ctx: BtLinkModule 指针。 @param[out] dst: 输出缓冲。 @retval 有效字节数。 */
    [[nodiscard]] static size_t onReadLatestGsv(const void *ctx, std::span<uint8_t> dst) noexcept;
#endif
    /** @brief 手机 0x12 读取 NTRIP 配置。 @param ctx: BtLinkModule 指针。 @retval 112 B 配置块。 */
    [[nodiscard]] static core::PhoneNtripConfig onReadNtrip(const void *ctx) noexcept;
    /** @brief 手机 0x12 保存 NTRIP 配置。 @param ctx: BtLinkModule 指针。 @param cfg: 112 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 */
    [[nodiscard]] static bool onSetNtrip(void *ctx, const core::PhoneNtripConfig &cfg,
                                         uint32_t nowMs) noexcept;
    /** @brief 手机 0x32 Nav 读取 JSON 服务器配置。 @param ctx: BtLinkModule 指针。 @retval 42 B 配置块。 */
    [[nodiscard]] static core::PhoneJsonServerConfig onReadJsonServer(const void *ctx) noexcept;
    /** @brief 手机 0x32 Nav 保存 JSON 服务器配置。 @param ctx: BtLinkModule 指针。 @param cfg: 42 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 */
    [[nodiscard]] static bool onSetJsonServer(void *ctx, const core::PhoneJsonServerConfig &cfg,
                                              uint32_t nowMs) noexcept;
    /** @brief 手机 0x33 Nav 读取 Thingsboard 配置。 @param ctx: BtLinkModule 指针。 @retval 86 B 配置块。 */
    [[nodiscard]] static core::PhoneThingsboardConfig onReadThingsboard(const void *ctx) noexcept;
    /** @brief 手机 0x33 Nav 保存 Thingsboard 配置。 @param ctx: BtLinkModule 指针。 @param cfg: 86 B 配置块。 @param nowMs: 去抖起点。 @retval true 已登记。 */
    [[nodiscard]] static bool onSetThingsboard(void *ctx, const core::PhoneThingsboardConfig &cfg,
                                               uint32_t nowMs) noexcept;
    /** @brief 手机 0x1E 登记延迟恢复出厂或重启。 @param ctx: BtLinkModule 指针。 @param factoryReset: 是否擦除用户 NVS。 @param reboot: 是否重启。 @param nowMs: 当前时钟。 @retval true 已受理。 */
    [[nodiscard]] static bool onScheduleReset(void *ctx, bool factoryReset, bool reboot,
                                              uint32_t nowMs) noexcept;
    /** @brief 手机 0x28 查询本构建是否带 Wi-Fi 网页配置。 @param ctx: 未用。 @retval true 支持。 */
    [[nodiscard]] static bool onWifiConfigSupported(const void *ctx) noexcept;
    /** @brief 手机 0x28 登记延迟重启进入 Wi-Fi 网页配置。 @param ctx: BtLinkModule 指针。 @param nowMs: 当前时钟。 @retval true 已受理。 */
    [[nodiscard]] static bool onScheduleWifiConfig(void *ctx, uint32_t nowMs) noexcept;
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

    /** @brief 恢复链路策略和白名单，绑定配对与写入准入。 @note 仅启动，非 ISR。 */
    void initLinkManager() noexcept;

    /** @brief 登记协议白名单写入至交给 Storage。 @param context: 当前模块实例。 @retval true 已获准。 @note 仅 kLink。 */
    [[nodiscard]] static bool admitWhitelistChange(void *context) noexcept;
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
     * @brief 把 GNSS 流绑定会话标记为待重绑并清零默认直传与显式订阅
     * @note 仅启动期或 kLink 串行路径，非 ISR；不分配堆。
     */
    void resetGnssStreamSession() noexcept;
    /**
     * @brief 返回当前生效的接收机支持语句位图（未探测前按默认能力集回退）
     * @retval bit 0..6 对应 gnss::OutputMsg；不修改状态。
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    [[nodiscard]] uint32_t effectiveSupportedMask() const noexcept;
    /**
     * @brief 汇总默认直传与显式订阅位图并同步到 App 环与跨任务订阅状态
     * @note 仅 kLink，非 ISR，不阻塞；仅在有效掩码变化时调用 gnssAppRing.setMask。
     */
    void applyGnssStreamMask() noexcept;
    /**
     * @brief 新 PHONE 会话建立时立即开启默认 NMEA 直传（无需等待首帧）
     * @param handle: 新建立的 PHONE 会话句柄
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    void onPhoneSessionOpen(core::SessionHandle handle) noexcept;
    /**
     * @brief PHONE 会话收到产品帧时检查 Nav 首帧（sender==0x00），若未显式订阅则关闭默认直传
     * @param handle: 来源会话句柄
     * @param wire: 已通过分流校验的完整产品帧
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    void onPhoneProductFrame(core::SessionHandle handle, std::span<const uint8_t> wire) noexcept;
    /**
     * @brief 帧派发后复核当前 PHONE 会话角色；若已转为 ALARM（HELLO 配对）则关闭直传
     * @param handle: 来源会话句柄
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    void recheckPhoneSessionRole(core::SessionHandle handle) noexcept;
    /**
     * @brief 把 App 环记录提交给当前手机会话
     * @retval true 本轮已处理满预算，可能仍有记录
     * @retval false 无会话、读者或记录，或发送背压/提交失败
     * @note 仅 kLink，非 ISR，不阻塞；发送失败取消当前租约，保留待发记录。
     */
    [[nodiscard]] bool drainGnssStream() noexcept;
    /**
     * @brief 从 BaseRtcmRing 弹出基站 RTCM3 帧并经 PHONE 流通道下发；失败丢整帧并计数。
     * @retval true 本轮达到批量上限，可能仍有待发帧
     * @retval false 环空或无有效 PHONE 会话
     * @note 仅 kLink，非 ISR，不阻塞。
     */
    [[nodiscard]] bool drainBaseRtcm() noexcept;
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
    core::PairWindow pair_window_{};
    unsigned whitelistAdmissions_ = 0U;
    portMUX_TYPE pair_mux_{};

    /** @brief 处理控制台开窗请求。 @param cmd: 窗口参数。 @param result: 操作结果。 @note 仅 kLink。 */
    void openPairing(const fw::Command &cmd, fw::CommandResult &result) noexcept;
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
    core::SessionHandle phone_session_handle_ = 0U;
    bool gnss_default_push_ = false;
    bool gnss_explicit_sub_ = false;
    bool gnss_supported_known_ = false;
    uint32_t gnss_supported_mask_ = 0U;
    uint32_t gnss_applied_mask_ = 0U;
    // App 环单条记录上限；单段连续提交至 SPP TX.stream。
    static constexpr size_t kGnssStreamMaxRecord = kGnssAppRingBytes - fw::kFanoutTypedHeaderBytes;
#endif
    core::services::PhoneCompatService phone_compat_{};
    // PhoneCompatOps 异步设置凭据，仅 kLink 读写；poll 回调仅读 const void*，此处按逻辑状态管理。
    mutable settings::Ticket height_ticket_{};
    mutable settings::Ticket ppp_ticket_{};
    mutable settings::Ticket sat_ticket_{};
    mutable settings::Ticket snpn_ticket_{};
    bool powerPaused_ = false;
    /** @brief 按请求投递或轮询扫描模式，失败时限频重试。 @param nowMs: 单调时钟。 @note 仅 kLink。 */
    void applyPowerScanMode(uint32_t nowMs) noexcept;

    bool scanSubmittedPaused_ = false;
    bool scanPending_ = false;
    bool scanFailed_ = false;
    uint32_t scanFailedMs_ = 0U;
    PendingReset pending_reset_ = PendingReset::kNone;
    uint32_t pending_reset_at_ms_ = 0U;
    uint32_t last_config_rev_ = 0U;
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
    uint32_t base_rtcm_tx_drop_ = 0U;
    uint32_t base_rtcm_ring_drop_base_ = 0U;
    std::array<uint8_t, data::BaseRtcmRing::kMaxFrameBytes> base_rtcm_scratch_{};
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
