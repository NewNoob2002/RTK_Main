// Wi-Fi 配置模式网页服务模块（ADR-0017 W2/W5/S2，登记为 kWifiOnly / kLink）。
// init()/stopBoot() 由启动任务调用；poll() 仅由 kLink 任务调用，不阻塞、不分配堆，非 ISR。
// httpd 回调线程仅通过无锁 SPSC 邮箱（rxMailbox_/ctrlMailbox_/txMailbox_）与本模块交互。
// 堆档位：禁止区；大帧槽位由 BootArena 仅在 kWifiConfig 模式下外供，成员全零初值落入 .bss。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#include "app/RadioMode.hpp"
#include "fw/Atomic.hpp"
#include "fw/Result.hpp"
#include "fw/Status.hpp"
#include "modules/web/WebDns.hpp"
#include "modules/web/WebIdleTimer.hpp"
#include "modules/web/WebLogic.hpp"
#include "modules/web/WebMailbox.hpp"
#include "web_proto/Request.hpp"

namespace app {
struct AppContext;

/** Wi-Fi 配置模式网页服务模块：拥有 SoftAP、HTTP/WS 服务器句柄、外供帧邮箱与协议状态机。 */
class WebModule final {
public:
    static constexpr size_t kMaxWsClients = 3U;
    static constexpr size_t kRxMailboxCap = 4U;
    static constexpr size_t kTxMailboxCap = 8U;
    static constexpr size_t kTxReservedCtrlSlots = 4U;
    static constexpr size_t kScratchFrames = 4U;
    static constexpr size_t kTotalFrames = kRxMailboxCap + kTxMailboxCap + kScratchFrames;
    static constexpr size_t kCtrlMailboxCap = 16U;
    static constexpr uint32_t kStatusPeriodMs = 1000U;

    constexpr WebModule() noexcept = default;
    ~WebModule() = default;
    WebModule(const WebModule &) = delete;
    WebModule &operator=(const WebModule &) = delete;
    WebModule(WebModule &&) = delete;
    WebModule &operator=(WebModule &&) = delete;

    /**
     * @brief 绑定所属应用上下文
     * @param context: 固件唯一应用上下文，生命周期覆盖模块
     * @note 仅启动前单线程调用，不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }

    /**
     * @brief 绑定启动期由 BootArena 外供的帧缓冲数组（仅 kWifiConfig 模式分配）
     * @param frames: 外供帧数组，长度须 >= kTotalFrames，生命周期覆盖整个运行期
     * @retval fw::Status::kOk 绑定成功
     * @retval fw::Status::kInvalidState 模块已初始化或 httpd 句柄仍存活
     * @retval fw::Status::kInvalidArg 缓冲为空或长度不足
     * @note 仅启动前单线程调用，不阻塞、不分配堆，非 ISR。
     */
    [[nodiscard]] fw::Status bindStorage(std::span<web_cfg::WebFrame> frames) noexcept;

    /**
     * @brief 启动绑定回滚时解除外供帧缓冲引用（仅消费者尚未启动且 httpd 未存活时调用）
     * @note 仅启动任务回滚阶段调用，不阻塞，非 ISR。
     */
    void unbindStorage() noexcept;

    /**
     * @brief 启动 SoftAP（192.168.10.12/24 + DHCP）与 HTTP/WS 服务器（core 0，最多 3 套接字）
     * @retval fw::Result::kOk SoftAP 与 HTTP/WS 就绪
     * @retval fw::Result::kInvalidState 上下文/存储未绑定、模式不符或 SSID 为空
     * @retval fw::Result::kIoError Wi-Fi/netif/httpd 启动失败（已回滚）
     * @note 仅启动任务调用一次（仅 kWifiConfig 模式）；可阻塞于 IDF 初始化，非 ISR。
     */
    [[nodiscard]] fw::Result init() noexcept;

    /**
     * @brief 启动失败降级时停止 httpd、Wi-Fi 与 netif
     * @note 仅启动任务在消费者停稳后调用；可阻塞于 IDF 停机，非 ISR。
     */
    void stopBoot() noexcept;

    /** @brief 更新关机期 Web 暂停请求。 @param paused: 是否静默。 @note 仅 kLink，非阻塞。 */
    void setPowerPaused(bool paused) noexcept;

    /** @brief 查询 Web 是否已静默。 @retval true SoftAP 已关闭。 @note 仅 kLink，非阻塞。 */
    [[nodiscard]] bool powerPauseDone() const noexcept;

    /**
     * @brief kLink 非阻塞轮询：消费控制/请求邮箱、推进 GNSS 异步命令、1 Hz 状态推送与空闲回落
     * @param nowMs: 当前单调时间（ms）
     * @retval true 仍有待处理请求、必达响应或同步帧需要尽快续轮
     * @retval false 本轮已空闲
     * @note 仅 kLink 任务调用，不阻塞、不分配堆，非 ISR；实时路径仅用 fw::trace。
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;

    /**
     * @brief 查询是否仍有在途的网页设置异步请求（供关机 kSave 阶段等待完成）
     * @retval true 有在途设置凭据
     * @retval false 无在途设置凭据
     * @note 仅 kLink 任务调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool hasPendingSettings() const noexcept
    {
        return inflight_.ticket.seq != 0U;
    }

private:
    enum class SyncStage : uint8_t {
        kReady = 0,
        kSendHello,
        kSendConfig,
        kSendStatus,
    };

    struct HttpdSession {
        bool active = false;
        int fd = 0;
        uint32_t gen = 0U;
    };

    struct ClientSlot {
        bool active = false;
        int fd = 0;
        uint32_t gen = 0U;
        bool unlocked = false;
        SyncStage syncStage = SyncStage::kReady;
        bool hasLastStatus = false;
        web_cfg::StatusSnapshot lastStatus{};
        bool pendingAck = false;
        uint16_t pendingAckId = 0U;
        web::ErrorCode pendingAckErr = web::ErrorCode::kNone;
        bool pendingConfig = false;
        uint32_t capsVersion = 0U;
        bool pendingBye = false;
    };

    enum class RequestKind : uint8_t { kSet, kSave, kExit, kFreset };

    struct ExitRequest {
        int fd = -1;
        uint32_t gen = 0U;
        uint16_t reqId = 0U;
    };

    struct InflightRequest {
        settings::Ticket ticket{};
        int fd = 0;
        uint32_t gen = 0U;
        uint16_t reqId = 0U;
        uint32_t startedMs = 0U;
        RequestKind kind = RequestKind::kSet;
    };

    enum class RebootKind : uint8_t {
        kNone = 0,
        kExit,
        kIdle,
        kFreset,
    };

    static_assert(fw::kAtomicUsable<uint32_t>, "Wi-Fi station and TX counters must be lock-free");
    static_assert(fw::kAtomicUsable<bool>, "HTTPD TX work flag must be lock-free");

    /**
     * @brief 取得已绑定的应用上下文引用
     * @retval 上下文引用
     * @note 仅 init/poll 路径在已绑定后调用，非 ISR。
     */
    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    // ---- WebServer.cpp（SoftAP + httpd 适配层）----

    /**
     * @brief 在 httpd 线程独占会话表中按套接字查找活跃 WebSocket 会话
     * @param fd: 套接字描述符
     * @retval 活跃会话槽位指针；未找到返回 nullptr
     * @note 仅 httpd 线程调用，不阻塞。
     */
    [[nodiscard]] HttpdSession *findHttpdSession(int fd) noexcept;

    /**
     * @brief 统计 httpd 线程当前活跃 WebSocket 会话数
     * @retval 活跃会话数（0..kMaxWsClients）
     * @note 仅 httpd 线程调用，不阻塞。
     */
    [[nodiscard]] size_t countHttpdSessions() const noexcept;

    /**
     * @brief 初始化 netif、静态 IP 192.168.10.12/24 与 DHCP 服务
     * @retval fw::Result::kOk SoftAP 网络接口与 DHCP 已就绪
     * @retval 其他 IDF 错误映射结果
     * @note 仅启动任务调用，可阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result initSoftApNetif() noexcept;

    /**
     * @brief 初始化 Wi-Fi 驱动、注册站点事件、配置 WPA2-PSK SoftAP 并启动发射
     * @param ssid: SoftAP SSID（来自 systemStatus.deviceName）
     * @retval fw::Result::kOk SoftAP 已启动
     * @retval 其他 IDF 错误映射结果
     * @note 仅启动任务调用，须先调用 initSoftApNetif()，可阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result startSoftApWifi(std::string_view ssid) noexcept;

    /**
     * @brief 启动 esp_http_server（core 0，max_open_sockets=3）并注册 /、/favicon.ico、/ws 及门户路由
     * @retval fw::Result::kOk httpd 已启动
     * @retval 其他 IDF 错误映射结果
     * @note 仅启动任务调用，可阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result startHttpd() noexcept;

#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    /**
     * @brief 创建并绑定非阻塞 UDP 53 强制门户 DNS 套接字（不输出格式化日志，可用于启动与运行期重试）
     * @param[out] errNo: 失败时写入对应 errno；成功时写 0
     * @retval fw::Result::kOk DNS 套接字就绪
     * @retval fw::Result::kIoError 套接字创建、非阻塞设置或绑定失败
     * @note 启动任务或 kLink 任务均可调用，不阻塞、不分配堆，非 ISR。
     */
    [[nodiscard]] fw::Result startDns(int &errNo) noexcept;

    /**
     * @brief 关闭强制门户 DNS 套接字并按 lwip_close 结果维护句柄状态
     * @note 启动任务或 kLink 任务调用，不阻塞、不分配堆，非 ISR；失败仅走 fw::trace。
     */
    void closeDnsSocket() noexcept;

    /**
     * @brief 非阻塞轮询 UDP 53 强制门户 DNS 套接字（每轮最多处理 4 包，错误后按 >=1 s 间隔重试）
     * @param nowMs: 当前单调时间（ms）
     * @retval true 本轮打满 4 包预算，可能仍有待处理查询
     * @retval false 当前无更多可读查询或发生错误
     * @note 仅 kLink 任务调用，不阻塞、不分配堆，非 ISR；异常仅走 fw::trace。
     */
    [[nodiscard]] bool pollDns(uint32_t nowMs) noexcept;
#endif

    /**
     * @brief 复位邮箱、会话、原子计数器与协议状态，为 init 启动网络服务准备干净初态
     * @note 仅启动任务在 init() 阶段调用，不阻塞、不分配堆，非 ISR。
     */
    void resetRuntimeState() noexcept;

    /**
     * @brief 按顺序启动 SoftAP netif、httpd、可选门户 DNS（失败降级）与 SoftAP 发射
     * @param ssid: SoftAP SSID
     * @retval fw::Result::kOk 网络栈就绪
     * @retval 其他 启动失败（已调用 teardownServer 回滚）
     * @note 仅启动任务在 init() 阶段调用，可阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result startNetworkStack(std::string_view ssid) noexcept;

    /**
     * @brief 停止并回收 httpd、DNS 套接字、Wi-Fi 事件与 SoftAP 资源（httpd 停机失败时保留句柄与资源）
     * @note 仅启动失败回滚或 stopBoot 调用，可阻塞，非 ISR。
     */
    void teardownServer() noexcept;

    /**
     * @brief 将一帧已编码 JSON 推入外供 TX 邮箱并唤醒 httpd 异步发送任务
     * @param fd: 目标 WebSocket 套接字
     * @param gen: 目标会话代号
     * @param frame: 已编码 UTF-8 JSON 视图
     * @retval true 已入队并递增 txEnqueuedSeq_
     * @retval false 邮箱满或长度超限
     * @note 仅 kLink 任务调用，不阻塞，非 ISR。
     */
    [[nodiscard]] bool enqueueTx(int fd, uint32_t gen, std::span<const char> frame) noexcept;

    /**
     * @brief 通过 httpd_queue_work 调度一次 TX 邮箱排空
     * @note 仅 kLink 或 httpd 线程在原子去重后调用，不阻塞；失败走 fw::trace。
     */
    void scheduleTxDrain() noexcept;

    /**
     * @brief httpd 工作队列回调：在 httpd 线程弹出 TX 邮箱、核对会话代号并异步发送
     * @param arg: WebModule 实例指针
     * @note 仅 httpd 线程调用，不得读写模块业务状态；发送/丢弃后推进 txCompletedSeq_。
     */
    static void onTxDrainWork(void *arg) noexcept;

    /**
     * @brief `GET /` 处理器：返回构建期 gzip 压缩并嵌入的静态单页
     * @param req: HTTP 请求对象
     * @retval ESP_OK 响应已发送
     * @retval 其他 发送错误
     * @note 仅 httpd 线程调用，不访问模块可变业务状态。
     */
    static esp_err_t handleRootGet(httpd_req_t *req) noexcept;

    /**
     * @brief `GET /favicon.ico` 处理器：返回 204 No Content，避免浏览器每次请求触发 404 日志
     * @param req: HTTP 请求对象
     * @retval ESP_OK 响应已发送
     * @retval 其他 发送错误
     * @note 仅 httpd 线程调用，不访问模块可变业务状态。
     */
    static esp_err_t handleFaviconGet(httpd_req_t *req) noexcept;

#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    /**
     * @brief 强制门户系统探测路径处理器：返回 302 Found 重定向至 http://192.168.10.12/
     * @param req: HTTP 请求对象
     * @retval ESP_OK 重定向响应已发送
     * @retval 其他 发送错误
     * @note 仅 httpd 线程调用，不访问模块可变业务状态。
     */
    static esp_err_t handleCaptiveRedirect(httpd_req_t *req) noexcept;

    /**
     * @brief 404 错误处理器：未匹配 GET 请求 302 重定向至配置页，非 GET 返回 404
     * @param req: HTTP 请求对象
     * @param err: httpd 错误码（HTTPD_404_NOT_FOUND）
     * @retval ESP_OK 响应已发送
     * @retval 其他 发送错误或关闭连接
     * @note 仅 httpd 线程调用，不访问模块可变业务状态。
     */
    static esp_err_t handleNotFound(httpd_req_t *req, httpd_err_code_t err) noexcept;
#endif

    /**
     * @brief `/ws` 握手完成回调（IDF v6.1 唯一连接通知）：分配会话代号并投递 kConnect
     * @param req: 已完成 101 升级的请求对象
     * @retval ESP_OK 已登记会话并投递 kConnect
     * @retval ESP_FAIL 会话满或控制队列无法为断开预留空间（httpd 将关闭连接）
     * @note 仅 httpd 线程调用，不访问模块业务状态。
     */
    static esp_err_t handleWsPostHandshake(httpd_req_t *req) noexcept;

    /**
     * @brief `/ws` 数据帧处理器：校验活跃会话、拒收分片/二进制/超长帧并入队 RX 邮箱
     * @param req: WebSocket 帧请求对象（IDF v6.1 握手不调用本函数）
     * @retval ESP_OK 已处理
     * @retval 其他 连接错误、未登记会话或协议违规关闭
     * @note 仅 httpd 线程调用，不直接读写模块业务状态。
     */
    static esp_err_t handleWs(httpd_req_t *req) noexcept;

    /**
     * @brief httpd 会话关闭回调：从 httpd 会话表移除并无损投递 kDisconnect，随后关闭套接字
     * @param hd: httpd 句柄
     * @param sockfd: 被关闭的套接字描述符
     * @note 仅 httpd 线程调用，不访问模块可变业务状态。
     */
    static void onHttpdClose(httpd_handle_t hd, int sockfd) noexcept;

    /**
     * @brief ESP 默认事件循环回调：以单个 32 位原子 CAS 更新 SoftAP 终端数与活动序号一致快照
     * @param arg: WebModule 实例指针
     * @param base: 事件基类（WIFI_EVENT）
     * @param eventId: 事件 ID
     * @param data: 事件载荷（未使用）
     * @note 在 sys_evt 任务调用，仅操作无锁原子变量。
     */
    static void onWifiEvent(void *arg, esp_event_base_t base, int32_t eventId, void *data) noexcept;

    // ---- WebModule.cpp（kLink 协议处理与状态机）----

    /**
     * @brief 消费 httpd 投递的连接/断开控制事件
     * @note 仅 kLink 任务调用，不阻塞。
     */
    void drainCtrlEvents() noexcept;

    /**
     * @brief 按套接字与会话代号精确查找已激活客户端槽位
     * @param fd: 套接字描述符
     * @param gen: 会话代号
     * @retval 槽位指针；未找到或代号不匹配返回 nullptr
     * @note 仅 kLink 任务调用。
     */
    [[nodiscard]] ClientSlot *findClient(int fd, uint32_t gen) noexcept;

    /**
     * @brief 为新连接的 WebSocket 客户端分配槽位（清理同 fd 旧槽位，初始化同步阶段）
     * @param fd: 新连接套接字描述符
     * @param gen: 新会话代号
     * @note 仅 kLink 任务调用。
     */
    void registerClient(int fd, uint32_t gen) noexcept;

    /**
     * @brief 注销已断开的 WebSocket 客户端并清空其工厂解锁状态与待发状态
     * @param fd: 已关闭套接字描述符
     * @param gen: 已关闭会话代号
     * @note 仅 kLink 任务调用。
     */
    void unregisterClient(int fd, uint32_t gen) noexcept;

    /**
     * @brief 优先冲刷全部客户端因发送背压暂存的必达帧（bye / ack / config）
     * @retval true 仍有必达帧等待发送
     * @retval false 所有必达帧均已入队
     * @note 仅 kLink 任务调用，不阻塞。
     */
    [[nodiscard]] bool pollPendingDeliveries() noexcept;

    /**
     * @brief 判断是否仍有任意客户端尚未完成初始 hello/config 排入或尚有未发出 ack
     * @retval true 应延后消费 RX 请求帧，保证 config 先于请求 ack 且不覆盖待发 ack
     * @retval false 可正常消费 RX 请求帧
     * @note 仅 kLink 任务调用。
     */
    [[nodiscard]] bool hasPendingCtrlBeforeRx() const noexcept;

    /**
     * @brief 判断是否仍有任意活跃客户端存在未入队的 ack 或 bye
     * @retval true 仍有待入队响应
     * @retval false 全部已入队
     * @note 仅 kLink 任务调用。
     */
    [[nodiscard]] bool hasPendingAckOrBye() const noexcept;

    /**
     * @brief 推进新连接客户端的 hello -> config -> 首次全量 status 推送
     * @param nowMs: 当前单调时间（ms）
     * @retval true 仍有客户端处于待同步阶段
     * @retval false 所有活跃客户端均已完成初始同步
     * @note 仅 kLink 任务调用，不阻塞。
     */
    [[nodiscard]] bool pollInitialSync(uint32_t nowMs) noexcept;

    /**
     * @brief 从 RX 邮箱取出一条帧并按 ws-protocol.md 解析分派
     * @param nowMs: 当前单调时间（ms）
     * @retval true 本轮消费了一条帧（可能仍有积压）
     * @retval false RX 邮箱为空或因同步/背压暂缓消费
     * @note 仅 kLink 任务调用，不阻塞。
     */
    [[nodiscard]] bool pollRxFrame(uint32_t nowMs) noexcept;

    /**
     * @brief 处理已解析的 `unlock` 请求
     * @param client: 请求来源客户端槽位
     * @param req: 解析后的请求
     * @note 仅 kLink 任务调用。
     */
    void handleUnlockReq(ClientSlot &client, const web::Request &req) noexcept;

    /**
     * @brief 处理已解析的 `cmd` 请求（save / exit / freset）
     * @param client: 请求来源客户端槽位
     * @param req: 解析后的请求
     * @param nowMs: 当前单调时间（ms）
     * @note 仅 kLink 任务调用。
     */
    void handleCmdReq(ClientSlot &client, const web::Request &req, uint32_t nowMs) noexcept;

    /**
     * @brief 处理已解析的 `set` 请求
     * @param client: 请求来源客户端槽位
     * @param req: 解析后的请求
     * @param nowMs: 当前单调时间（ms）
     * @note 仅 kLink 任务调用。
     */
    void handleSetReq(ClientSlot &client, const web::Request &req, uint32_t nowMs) noexcept;

    /** @brief 记录本会话在途命令。 @param client: 会话。 @param reqId: 请求号。 @param ticket: 凭据。 @param kind: 请求类型。 @param nowMs: 时钟。 @note 仅 kLink。 */
    void trackRequest(const ClientSlot &client, uint16_t reqId, settings::Ticket ticket,
                      RequestKind kind, uint32_t nowMs) noexcept;

    /** @brief 投递维护命令并跟踪结果。 @param client: 会话。 @param reqId: 请求号。 @param owner: 目标模块。 @param verb: 命令字。 @param kind: 请求类型。 @param nowMs: 时钟。 @note 仅 kLink。 */
    void postMaintenance(ClientSlot &client, uint16_t reqId, settings::Owner owner, uint8_t verb,
                         RequestKind kind, uint32_t nowMs) noexcept;

    /** @brief 收取异步结果或取消超时请求。 @param nowMs: 时钟。 @note 仅 kLink。 */
    void pollInflight(uint32_t nowMs) noexcept;

    /**
     * @brief 每 1 s 采样状态并向已完成初始同步的客户端推送变化分组（为控制帧预留 TX 槽位）
     * @param nowMs: 当前单调时间（ms）
     * @param forceNow: true 时立即采样推送（用于白名单变更即时刷新）
     * @note 仅 kLink 任务调用。
     */
    void pollStatusPush(uint32_t nowMs, bool forceNow) noexcept;

    /** @brief 向已同步会话推送能力变化。 @retval 邮箱尚有待发能力帧。 @note 仅 kLink。 */
    [[nodiscard]] bool pollCapsPush() noexcept;

    /**
     * @brief 采样当前 `status` 快照（含 1 Hz GGA 实际频率估算）
     * @param nowMs: 当前单调时间（ms）
     * @retval 填充好的 StatusSnapshot
     * @note 仅 kLink 任务调用。
     */
    [[nodiscard]] web_cfg::StatusSnapshot sampleStatus(uint32_t nowMs) noexcept;

    /**
     * @brief 发送单条 `ack` 帧到指定客户端；邮箱满时保留在槽位 pendingAck 中重试，绝不丢弃
     * @param client: 目标客户端槽位
     * @param reqId: 回显的请求 ID
     * @param error: 协议错误码
     * @note 仅 kLink 任务调用。
     */
    void sendAck(ClientSlot &client, uint16_t reqId, web::ErrorCode error) noexcept;

    /**
     * @brief 向全部已完成初始同步的客户端广播最新 `config` 帧；邮箱满时保留 pendingConfig 重试
     * @note 仅 kLink 任务调用。
     */
    void broadcastConfig() noexcept;

    /**
     * @brief 向全部活跃客户端安排 `bye` 帧并启动有界重启等待
     * @param kind: 重启类型（exit / idle / freset）
     * @param reasonText: 协议 `bye.reason` 文本
     * @param nowMs: 当前单调时间（ms）
     * @param idleActSeq: kind==kIdle 时记录排定时刻的站点活动序号，供提交前复核
     * @note 仅 kLink 任务调用。
     */
    void scheduleReboot(RebootKind kind, std::string_view reasonText, uint32_t nowMs,
                        uint32_t idleActSeq = 0U) noexcept;

    /** @brief 仲裁排定的主动退出，先取得重启权再确认。 @param nowMs: 单调时钟。 @note 仅 kLink。 */
    void commitPendingExit(uint32_t nowMs) noexcept;

    /**
     * @brief 复核空闲重启条件；若期间终端重连或活动序号前进则中止本次空闲重启
     * @param nowMs: 当前单调时间（ms）
     * @retval true 已中止空闲重启
     * @retval false 非空闲重启或仍满足空闲条件
     * @note 仅 kLink 任务调用。
     */
    [[nodiscard]] bool tryAbortIdleReboot(uint32_t nowMs) noexcept;

    /**
     * @brief 推进延迟重启：空闲回落前复核站点快照、等待 httpd 完成发送后切回蓝牙或重启
     * @param nowMs: 当前单调时间（ms）
     * @retval true 正在等待重启，调用方应跳过普通业务
     * @retval false 无待执行重启（或空闲重启因新连接被取消）
     * @note 仅 kLink 任务调用。
     */
    [[nodiscard]] bool pollPendingReboot(uint32_t nowMs) noexcept;

    AppContext *context_ = nullptr;
    /**
     * @brief 在 httpd 任务切换 SoftAP，保留服务器供关机失败恢复
     * @param arg: WebModule 实例
     * @note 仅 httpd 线程，可在 Wi-Fi SDK 中阻塞。
     */
    static void onPowerModeWork(void *arg) noexcept;

    /** @brief 按需投递 SoftAP 切换请求。 @note 仅 kLink，非阻塞。 */
    void schedulePowerMode() noexcept;

    std::atomic<bool> powerPauseRequested_{false};
    std::atomic<bool> powerPaused_{false};
    std::atomic<bool> powerModeQueued_{false};
    uint32_t powerModeAttemptMs_ = 0U;
    bool powerModeAttempted_ = false;
    bool initialized_ = false;
    bool wifiStarted_ = false;
    bool wifiInited_ = false;
    bool eventRegistered_ = false;
    esp_netif_t *apNetif_ = nullptr;
    esp_event_handler_instance_t wifiEventHandle_ = nullptr;
    httpd_handle_t httpd_ = nullptr;
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    bool dnsOpen_ = false;
    bool dnsRetryArmed_ = false;
    int dnsSock_ = 0;
    uint32_t lastDnsRetryMs_ = 0U;
    std::array<uint8_t, web_cfg::kDnsMaxPacketBytes> dnsRxBuf_{};
    std::array<uint8_t, web_cfg::kDnsMaxPacketBytes> dnsTxBuf_{};
#endif

    // 跨任务通信状态（零初值；帧槽位由 BootArena 仅在 kWifiConfig 模式下外供）。
    std::atomic<uint32_t> staState_{0U}; // 低 4 位为终端数，高 28 位为活动序号（一致快照）。
    std::atomic<bool> txWorkQueued_{false};
    std::atomic<uint32_t> txEnqueuedSeq_{0U};
    std::atomic<uint32_t> txCompletedSeq_{0U};
    web_cfg::WebFrameMailboxView rxMailbox_{};
    web_cfg::WebFrameMailboxView txMailbox_{};
    web_cfg::WebCtrlMailbox<kCtrlMailboxCap> ctrlMailbox_{};
    bool storageBound_ = false;

    // httpd 线程独占会话表与外供收发帧指针。
    std::array<HttpdSession, kMaxWsClients> httpdSessions_{};
    uint32_t nextSessionGen_ = 0U;
    web_cfg::WebFrame *httpdRxFrame_ = nullptr;
    web_cfg::WebFrame *httpdTxFrame_ = nullptr;

    // kLink 独占工作区指针与协议状态。
    web_cfg::WebFrame *klinkRxFrame_ = nullptr;
    web_cfg::WebFrame *klinkEncodeFrame_ = nullptr;
    std::array<ClientSlot, kMaxWsClients> clients_{};
    web::Request parsedReq_{};
    InflightRequest inflight_{};
    uint32_t configVersion_ = 0U;
    uint32_t lastStatusMs_ = 0U;
    uint32_t lastGgaCount_ = 0U;
    uint32_t lastGgaSampleMs_ = 0U;
    bool ggaSampleValid_ = false;
    web_cfg::WebIdleTimer idleTimer_{};
    RebootKind pendingReboot_ = RebootKind::kNone;
    ExitRequest pendingExit_{}; // 留到下一轮与关机仲裁。
    std::string_view rebootByeReason_{};
    uint32_t rebootRequestedMs_ = 0U;
    uint32_t rebootWaitTxSeq_ = 0U;
    uint32_t rebootDrainDoneMs_ = 0U;
    uint32_t idleRebootActSeq_ = 0U;
    bool rebootTxCommitted_ = false;
    bool rebootDrainDone_ = false;
};

} // namespace app
