// Wi-Fi 配置模式 SoftAP 与 esp_http_server 适配层（ADR-0017 §2.3）。
// init()/stopBoot() 由启动任务调用；HTTP/WS 回调仅在 httpd 线程（core 0）运行，
// 不直接读写模块业务状态，仅经无锁 SPSC 邮箱与 kLink 交互；不分配堆（IDF/lwIP 内部分配除外）。
// 实时路径（HTTP/WS 回调、TX 排空、Wi-Fi 事件）仅用 fw::trace，禁格式化日志。
#include "modules/web/WebModule.hpp"

#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
#include "modules/ota/OtaRuntime.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <unistd.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
#include "lwip/sockets.h"
#endif

#include "AppContext.hpp"
#include "def/SystemDef.hpp"
#include "fw/Log.hpp"
#include "fw/Trace.hpp"
#include "rtos/EspStatus.hpp"

#if !defined(CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT) ||                                         \
    !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT must be enabled for IDF v6.1 WS handshake"
#endif

namespace app {

// CMake EMBED_FILES 生成的构建期 gzip 单页二进制起止符号（<= 24 KB，外部链接）。
extern const char kWebUiGzStart asm("_binary_index_html_gz_start");
extern const char kWebUiGzEnd asm("_binary_index_html_gz_end");

namespace {

constexpr std::string_view kApPassword = CONFIG_RTK_WIFI_AP_PASSWORD;
constexpr size_t kMinWpa2PasswordLen = 8U;
constexpr size_t kMaxWpa2PasswordLen = 63U;
constexpr uint8_t kMinChannel = 1U;
constexpr uint8_t kMaxChannel = 11U;
constexpr uint32_t kMinIdleExitS = 60U;
constexpr uint32_t kMaxIdleExitS = 3600U;
constexpr uint8_t kMaxStations = 2U;
constexpr uint16_t kMaxHttpSockets = 3U;
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
constexpr uint16_t kBaseUriHandlers = 5U; // /, /favicon.ico, /ws, /ota/info, /ota
#else
constexpr uint16_t kBaseUriHandlers = 3U; // /, /favicon.ico, /ws
#endif
constexpr uint16_t kUriHandlerMargin = 2U;
constexpr uint32_t kMsPerSecond = 1000U;

// SoftAP 固定地址 192.168.10.12/24（用户 2026-11-19 指定，网关同址）。
constexpr uint8_t kApIpA = 192U;
constexpr uint8_t kApIpB = 168U;
constexpr uint8_t kApIpC = 10U;
constexpr uint8_t kApIpD = 12U;
constexpr uint8_t kApMaskHigh = 255U;
constexpr uint8_t kApMaskLow = 0U;

#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
constexpr std::array<uint8_t, 4U> kApIpv4 = {kApIpA, kApIpB, kApIpC, kApIpD};
constexpr std::string_view kApHost = "192.168.10.12";
constexpr std::string_view kApHostPort80 = "192.168.10.12:80";
constexpr const char *kCaptivePortalUrl = "http://192.168.10.12/";
constexpr size_t kCaptivePortalUriBufBytes = 22U; // B，"http://192.168.10.12/" + NUL
// ESP-IDF esp_netif_dhcps_option(ESP_NETIF_CAPTIVEPORTAL_URI) 只保存指针（esp_netif.h:580），须静态存储。
constinit std::array<char, kCaptivePortalUriBufBytes> s_captive_portal_uri = {
    'h', 't', 't', 'p', ':', '/', '/', '1', '9', '2', '.',
    '1', '6', '8', '.', '1', '0', '.', '1', '2', '/', '\0',
};
constexpr uint32_t kCaptivePortalUriLen = static_cast<uint32_t>(s_captive_portal_uri.size() - 1U);
constexpr size_t kMaxDnsPacketsPerPoll = 4U;
constexpr uint32_t kDnsRetryIntervalMs = 1000U; // ms，DNS 异常关闭后在 poll 中的重试间隔
constexpr uint32_t kDnsErrOpRx = 1U;
constexpr uint32_t kDnsErrOpTx = 2U;
constexpr uint32_t kDnsErrOpInit = 3U;
constexpr uint32_t kDnsErrOpClose = 4U;

constexpr std::array<const char *, 7U> kCaptiveProbePaths = {
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/connecttest.txt",
    "/ncsi.txt",
    "/redirect",
};
constexpr uint16_t kMaxUriHandlers =
    kBaseUriHandlers + static_cast<uint16_t>(kCaptiveProbePaths.size()) + kUriHandlerMargin;
#else
constexpr uint16_t kMaxUriHandlers = kBaseUriHandlers + kUriHandlerMargin;
#endif

// RFC 6455 关闭码（大端两字节）：1003 Unsupported Data、1009 Message Too Big。
constexpr uint8_t kWsCloseUnsupportedHi = 0x03U;
constexpr uint8_t kWsCloseUnsupportedLo = 0xEBU;
constexpr uint8_t kWsCloseTooBigHi = 0x03U;
constexpr uint8_t kWsCloseTooBigLo = 0xF1U;
constexpr size_t kBusyBufBytes = 64U;

// WebTrace::kWsReject 原因码。
constexpr uint32_t kRejectUnregistered = 1U;
constexpr uint32_t kRejectFragmented = 2U;
constexpr uint32_t kRejectUnsupportedType = 3U;
constexpr uint32_t kRejectTooLong = 4U;
constexpr uint32_t kRejectCtrlFull = 5U;

static_assert((kApPassword.size() >= kMinWpa2PasswordLen) &&
                  (kApPassword.size() <= kMaxWpa2PasswordLen),
              "RTK_WIFI_AP_PASSWORD must be 8..63 characters");
static_assert((CONFIG_RTK_WIFI_AP_CHANNEL >= kMinChannel) &&
                  (CONFIG_RTK_WIFI_AP_CHANNEL <= kMaxChannel),
              "RTK_WIFI_AP_CHANNEL must be 1..11");
static_assert((CONFIG_RTK_WIFI_IDLE_EXIT_S >= kMinIdleExitS) &&
                  (CONFIG_RTK_WIFI_IDLE_EXIT_S <= kMaxIdleExitS),
              "RTK_WIFI_IDLE_EXIT_S must be 60..3600");

/**
 * @brief 借用全局上下文的空释放回调（防止 httpd_stop 对静态 WebModule 调用 free()）
 * @param ctx: 借用的 WebModule 指针（未使用）
 * @note 仅 httpd_stop 调用，不阻塞、不释放内存。
 */
void noopFreeGlobalCtx(void * /*ctx*/) noexcept {}

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
 * @brief 停机回滚阶段记录单个 IDF 清理错误（仅在非 ESP_OK 时输出 W 日志）
 * @param op: 失败的清理操作短名
 * @param err: IDF 返回的错误码
 * @note 仅启动失败回滚或 stopBoot 调用，非 ISR。
 */
void logTeardownErr(const char *op, esp_err_t err) noexcept
{
    if (err != ESP_OK) {
        FW_LOGW(fw::TraceCat::kSys, "<web> %s failed (%s)", op, esp_err_to_name(err));
    }
}

/**
 * @brief 将 fw::Status 映射为模块初始化使用的 fw::Result
 * @param status: IDF 错误转换后的 fw::Status
 * @retval 对应的 fw::Result
 * @note 纯函数，线程/ISR 安全。
 */
fw::Result toResult(fw::Status status) noexcept
{
    switch (status) {
    case fw::Status::kOk:
        return fw::Result::kOk;
    case fw::Status::kInvalidArg:
        return fw::Result::kInvalidArg;
    case fw::Status::kInvalidState:
        return fw::Result::kInvalidState;
    case fw::Status::kNoSpace:
        return fw::Result::kNoSpace;
    case fw::Status::kBusy:
        return fw::Result::kBusy;
    case fw::Status::kTimeout:
        return fw::Result::kTimeout;
    case fw::Status::kIoError:
    default:
        return fw::Result::kIoError;
    }
}

/**
 * @brief 启动阶段记录单个 IDF 错误并返回对应的 fw::Result
 * @param op: 失败的初始化操作短名
 * @param err: IDF 返回的错误码
 * @retval 映射后的 fw::Result
 * @note 仅启动任务调用，非 ISR。
 */
fw::Result failInit(const char *op, esp_err_t err) noexcept
{
    FW_LOGE(fw::TraceCat::kSys, "<web> %s failed (%s)", op, esp_err_to_name(err));
    return toResult(rtos::fromEspErr(err));
}

/**
 * @brief 计算构建期嵌入 gzip 页面的字节长度
 * @retval 页面字节数
 * @note 纯计算，依赖链接器生成的起止符号。
 */
size_t embeddedPageBytes() noexcept
{
    // 链接器为 EMBED_FILES 生成起止地址符号，转 uintptr_t 计算区间长度避免跨对象指针相减。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto start = reinterpret_cast<uintptr_t>(&kWebUiGzStart);
    const auto end = reinterpret_cast<uintptr_t>(&kWebUiGzEnd);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return (end > start) ? static_cast<size_t>(end - start) : 0U;
}

/**
 * @brief 在 httpd 线程向客户端发送指定 RFC 6455 状态码的关闭帧
 * @param req: 当前 WebSocket 请求
 * @param codeHi: 关闭码高字节（大端）
 * @param codeLo: 关闭码低字节（大端）
 * @note 仅 httpd 线程调用，不访问模块业务状态；失败走 fw::trace。
 */
void sendWsCloseCode(httpd_req_t *req, uint8_t codeHi, uint8_t codeLo) noexcept
{
    std::array<uint8_t, 2U> payload{codeHi, codeLo};
    httpd_ws_frame_t closeFrame{};
    closeFrame.final = true;
    closeFrame.type = HTTPD_WS_TYPE_CLOSE;
    closeFrame.payload = payload.data();
    closeFrame.len = payload.size();
    const esp_err_t err = httpd_ws_send_frame(req, &closeFrame);
    if (err != ESP_OK) {
        const int fd = httpd_req_to_sockfd(req);
        traceWeb(WebTrace::kWsTxErr, static_cast<uint32_t>(fd), static_cast<uint32_t>(err));
    }
}

/**
 * @brief 在 httpd 线程向邮箱已满时的请求回发 `busy` ack
 * @param req: 当前 WebSocket 请求
 * @param reqId: 从原始帧扫描到的请求 ID（未找到为 0）
 * @note 仅 httpd 线程调用，仅使用栈上 64 B 缓冲；失败走 fw::trace。
 */
void sendWsBusyAck(httpd_req_t *req, uint16_t reqId) noexcept
{
    std::array<char, kBusyBufBytes> buf{};
    const int written = std::snprintf(buf.data(), buf.size(),
                                      "{\"t\":\"ack\",\"id\":%u,\"ok\":false,\"err\":\"busy\"}",
                                      static_cast<unsigned>(reqId));
    if ((written <= 0) || (static_cast<size_t>(written) >= buf.size())) {
        return;
    }
    httpd_ws_frame_t frame{};
    frame.final = true;
    frame.type = HTTPD_WS_TYPE_TEXT;
    // esp_http_server C API 使用 uint8_t * 载荷指针，仅读取 buf。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    frame.payload = reinterpret_cast<uint8_t *>(buf.data());
    frame.len = static_cast<size_t>(written);
    const esp_err_t err = httpd_ws_send_frame(req, &frame);
    if (err != ESP_OK) {
        const int fd = httpd_req_to_sockfd(req);
        traceWeb(WebTrace::kWsTxErr, static_cast<uint32_t>(fd), static_cast<uint32_t>(err));
    }
}

/**
 * @brief 配置 SoftAP 静态 IP 192.168.10.12/24、DHCP 选项 114 并重启 DHCP 服务
 * @param netif: SoftAP 对应的 esp_netif 句柄
 * @retval ESP_OK 配置成功
 * @retval 其他 IDF 错误码
 * @note 仅启动任务调用，可阻塞，非 ISR。
 */
esp_err_t configureApIpAndDhcp(esp_netif_t *netif) noexcept
{
    esp_err_t err = esp_netif_dhcps_stop(netif);
    if ((err != ESP_OK) && (err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)) {
        return err;
    }
    esp_netif_ip_info_t ipInfo{};
    esp_netif_set_ip4_addr(&ipInfo.ip, kApIpA, kApIpB, kApIpC, kApIpD);
    esp_netif_set_ip4_addr(&ipInfo.gw, kApIpA, kApIpB, kApIpC, kApIpD);
    esp_netif_set_ip4_addr(&ipInfo.netmask, kApMaskHigh, kApMaskHigh, kApMaskHigh, kApMaskLow);
    err = esp_netif_set_ip_info(netif, &ipInfo);
    if (err != ESP_OK) {
        return err;
    }
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    err = esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 s_captive_portal_uri.data(), kCaptivePortalUriLen);
    if (err != ESP_OK) {
        return err;
    }
#endif
    return esp_netif_dhcps_start(netif);
}

#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
/**
 * @brief 复用 kLink 独占的 Web 工作帧作为 UDP DNS 缓冲
 * @param frame: kLink 工作帧，已由启动池绑定
 * @retval 固定长度 DNS 写入视图
 * @note 仅 kLink 调用，待发送的 Web 帧已经复制到信箱，不与 DNS 共用。
 */
[[nodiscard]] std::span<uint8_t> dnsStorage(web_cfg::WebFrame &frame) noexcept
{
    static_assert(web_cfg::kDnsMaxPacketBytes <= web::kMaxFrameBytes);
    // uint8_t 在 ESP32 上是 unsigned char，可逐字节访问帧存储。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<uint8_t *>(frame.data.data()), web_cfg::kDnsMaxPacketBytes};
}

/**
 * @brief 校验 HTTP 请求的 Host 头是否为规范地址 192.168.10.12 或 192.168.10.12:80
 * @param req: HTTP 请求对象
 * @retval true Host 头为规范地址
 * @retval false Host 头缺失或不匹配
 * @note 仅 httpd 线程调用，不分配堆，仅使用栈上定长缓冲。
 */
[[nodiscard]] bool isCanonicalApHost(httpd_req_t *req) noexcept
{
    const size_t hostLen = httpd_req_get_hdr_value_len(req, "Host");
    if ((hostLen != kApHost.size()) && (hostLen != kApHostPort80.size())) {
        return false;
    }
    std::array<char, kApHostPort80.size() + 1U> hostBuf{};
    if (httpd_req_get_hdr_value_str(req, "Host", hostBuf.data(), hostBuf.size()) != ESP_OK) {
        return false;
    }
    const std::string_view host{hostBuf.data(), hostLen};
    return (host == kApHost) || (host == kApHostPort80);
}

/**
 * @brief 发送 302 Found 强制门户重定向响应（空体 + Cache-Control: no-store）
 * @param req: HTTP 请求对象
 * @retval ESP_OK 响应已发送
 * @retval 其他 发送错误
 * @note 仅 httpd 线程调用，不访问模块可变业务状态。
 */
esp_err_t sendCaptiveRedirect(httpd_req_t *req) noexcept
{
    esp_err_t err = httpd_resp_set_status(req, "302 Found");
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_set_hdr(req, "Location", kCaptivePortalUrl);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send(req, nullptr, 0);
}

using HttpdUriHandlerFn = esp_err_t (*)(httpd_req_t *);

/**
 * @brief 注册强制门户系统探测路径与 404 重定向处理器
 * @param httpd: 已启动的 httpd 句柄
 * @param self: WebModule 实例指针
 * @param probeHandler: 系统探测路径的 302 处理回调
 * @param notFoundHandler: 404 错误处理回调
 * @retval ESP_OK 全部注册成功
 * @retval 其他 注册失败错误码
 * @note 仅启动任务调用，非 ISR。
 */
esp_err_t registerCaptivePortalHandlers(httpd_handle_t httpd, WebModule *self,
                                        HttpdUriHandlerFn probeHandler,
                                        httpd_err_handler_func_t notFoundHandler) noexcept
{
    for (const char *path : kCaptiveProbePaths) {
        const httpd_uri_t probeUri{
            .uri = path,
            .method = HTTP_GET,
            .handler = probeHandler,
            .user_ctx = self,
            .is_websocket = false,
            .handle_ws_control_frames = false,
            .supported_subprotocol = nullptr,
            .ws_post_handshake_cb = nullptr,
        };
        const esp_err_t err = httpd_register_uri_handler(httpd, &probeUri);
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, notFoundHandler);
}
#endif

} // namespace

WebModule::HttpdSession *WebModule::findHttpdSession(int fd) noexcept
{
    for (auto &slot : httpdSessions_) {
        if (slot.active && (slot.fd == fd)) {
            return &slot;
        }
    }
    return nullptr;
}

size_t WebModule::countHttpdSessions() const noexcept
{
    size_t count = 0U;
    for (const auto &slot : httpdSessions_) {
        if (slot.active) {
            ++count;
        }
    }
    return count;
}

void WebModule::onWifiEvent(void *arg, esp_event_base_t /*base*/, int32_t eventId,
                            void * /*data*/) noexcept
{
    // arg 在事件注册时固定传入 WebModule 实例指针。
    auto *self = static_cast<WebModule *>(arg);
    if (self == nullptr) {
        return;
    }
    if ((eventId != WIFI_EVENT_AP_STACONNECTED) && (eventId != WIFI_EVENT_AP_STADISCONNECTED)) {
        return;
    }
    uint32_t cur = self->staState_.load(std::memory_order_acquire);
    while (true) {
        uint32_t count = web_cfg::staStateCount(cur);
        const uint32_t seq = (web_cfg::staStateSeq(cur) + 1U) & web_cfg::kStaSeqMask;
        if (eventId == WIFI_EVENT_AP_STACONNECTED) {
            if (count < web_cfg::kStaCountMask) {
                ++count;
            }
        } else if (count > 0U) {
            --count;
        }
        const uint32_t next = web_cfg::packStaState(count, seq);
        if (self->staState_.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            return;
        }
    }
}

esp_err_t WebModule::handleRootGet(httpd_req_t *req) noexcept
{
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    if (!isCanonicalApHost(req)) {
        return sendCaptiveRedirect(req);
    }
#endif
    esp_err_t err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (err != ESP_OK) {
        return err;
    }
    const size_t bodyLen = embeddedPageBytes();
    // 嵌入页面由构建期 pack.py 限制 <= 24 KB，转为 ssize_t 无损。
    return httpd_resp_send(req, &kWebUiGzStart, static_cast<ssize_t>(bodyLen));
}

esp_err_t WebModule::handleFaviconGet(httpd_req_t *req) noexcept
{
    const esp_err_t err = httpd_resp_set_status(req, "204 No Content");
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send(req, nullptr, 0);
}

#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
esp_err_t WebModule::handleCaptiveRedirect(httpd_req_t *req) noexcept
{
    return sendCaptiveRedirect(req);
}

esp_err_t WebModule::handleNotFound(httpd_req_t *req, httpd_err_code_t /*err*/) noexcept
{
    if (req->method == HTTP_GET) {
        return sendCaptiveRedirect(req);
    }
    // 非 GET 保持 IDF 原生 404 关闭连接语义（返回非 ESP_OK 触发底层关连接）。
    const esp_err_t sendErr = httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return (sendErr != ESP_OK) ? sendErr : ESP_FAIL;
}
#endif

esp_err_t WebModule::handleWsPostHandshake(httpd_req_t *req) noexcept
{
    // user_ctx 在 URI 注册时固定传入 WebModule 实例指针。
    auto *self = static_cast<WebModule *>(req->user_ctx);
    if (self == nullptr || self->powerPauseRequested_.load(std::memory_order_acquire)) {
        return ESP_FAIL;
    }
    const int fd = httpd_req_to_sockfd(req);
    // 若同 fd 仍有未清理旧槽位，先无损投递其断开事件再登记新会话。
    if (auto *stale = self->findHttpdSession(fd); stale != nullptr) {
        const uint32_t staleGen = stale->gen;
        *stale = HttpdSession{};
        static_cast<void>(
            self->ctrlMailbox_.tryPush({web_cfg::WebCtrlEvent::Kind::kDisconnect, fd, staleGen}));
    }
    const size_t active = self->countHttpdSessions();
    // 数学不变量：接受新连接前必须保证剩余槽位 >= active + 2（1 槽给本次 kConnect，active+1 槽预留给全部活跃会话未来的 kDisconnect）。
    if ((active >= kMaxWsClients) || (self->ctrlMailbox_.freeSlots() < (active + 2U))) {
        traceWeb(WebTrace::kWsReject, kRejectCtrlFull, static_cast<uint32_t>(fd));
        return ESP_FAIL;
    }
    self->nextSessionGen_ = (self->nextSessionGen_ == std::numeric_limits<uint32_t>::max())
                                ? 1U
                                : (self->nextSessionGen_ + 1U);
    const uint32_t gen = self->nextSessionGen_;
    if (!self->ctrlMailbox_.tryPush({web_cfg::WebCtrlEvent::Kind::kConnect, fd, gen})) {
        traceWeb(WebTrace::kWsReject, kRejectCtrlFull, static_cast<uint32_t>(fd));
        return ESP_FAIL;
    }
    for (auto &slot : self->httpdSessions_) {
        if (!slot.active) {
            slot = HttpdSession{.active = true, .fd = fd, .gen = gen};
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t WebModule::handleWs(httpd_req_t *req) noexcept
{
    // user_ctx 在 URI 注册时固定传入 WebModule 实例指针。
    auto *self = static_cast<WebModule *>(req->user_ctx);
    if (self == nullptr || self->powerPauseRequested_.load(std::memory_order_acquire)) {
        return ESP_FAIL;
    }
    const int fd = httpd_req_to_sockfd(req);
    const auto *session = self->findHttpdSession(fd);
    if (session == nullptr) {
        traceWeb(WebTrace::kWsReject, kRejectUnregistered, static_cast<uint32_t>(fd));
        return ESP_FAIL;
    }
    httpd_ws_frame_t wsPkt{};
    esp_err_t err = httpd_ws_recv_frame(req, &wsPkt, 0U);
    if (err != ESP_OK) {
        return err;
    }
    if (wsPkt.type == HTTPD_WS_TYPE_CLOSE) {
        return ESP_OK;
    }
    if ((wsPkt.type == HTTPD_WS_TYPE_CONTINUE) || !wsPkt.final) {
        traceWeb(WebTrace::kWsReject, kRejectFragmented, static_cast<uint32_t>(wsPkt.type));
        sendWsCloseCode(req, kWsCloseUnsupportedHi, kWsCloseUnsupportedLo);
        return ESP_FAIL;
    }
    if (wsPkt.type != HTTPD_WS_TYPE_TEXT) {
        traceWeb(WebTrace::kWsReject, kRejectUnsupportedType, static_cast<uint32_t>(wsPkt.type));
        sendWsCloseCode(req, kWsCloseUnsupportedHi, kWsCloseUnsupportedLo);
        return ESP_FAIL;
    }
    if (wsPkt.len > web::kMaxFrameBytes) {
        traceWeb(WebTrace::kWsReject, kRejectTooLong, static_cast<uint32_t>(wsPkt.len));
        sendWsCloseCode(req, kWsCloseTooBigHi, kWsCloseTooBigLo);
        return ESP_FAIL;
    }
    // httpdRxFrame_ 由 httpd 线程独占；esp_http_server C API 需要 uint8_t * 载荷指针。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    wsPkt.payload = reinterpret_cast<uint8_t *>(self->httpdRxFrame_->data.data());
    if (wsPkt.len > 0U) {
        err = httpd_ws_recv_frame(req, &wsPkt, wsPkt.len);
        if (err != ESP_OK) {
            return err;
        }
    }
    const auto text = std::span<const char>{self->httpdRxFrame_->data}.first(wsPkt.len);
    const auto pushed = self->rxMailbox_.tryPush(fd, session->gen, text);
    if (pushed == web_cfg::MailboxPush::kFull) {
        const uint16_t reqId = web_cfg::extractRequestId(text);
        traceWeb(WebTrace::kWsBusy, static_cast<uint32_t>(fd), static_cast<uint32_t>(reqId));
        sendWsBusyAck(req, reqId);
    } else if (pushed == web_cfg::MailboxPush::kTooLong) {
        traceWeb(WebTrace::kWsReject, kRejectTooLong, static_cast<uint32_t>(wsPkt.len));
        sendWsCloseCode(req, kWsCloseTooBigHi, kWsCloseTooBigLo);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void WebModule::onHttpdClose(httpd_handle_t hd, int sockfd) noexcept
{
    // global_user_ctx 在 httpd_start 前固定设为 WebModule 实例指针。
    auto *self = static_cast<WebModule *>(httpd_get_global_user_ctx(hd));
    if (self != nullptr) {
        if (auto *session = self->findHttpdSession(sockfd); session != nullptr) {
            const uint32_t gen = session->gen;
            *session = HttpdSession{};
            // 由升级时的槽位预留不变量保证此处必有空槽，断开事件永不丢失。
            static_cast<void>(self->ctrlMailbox_.tryPush(
                {web_cfg::WebCtrlEvent::Kind::kDisconnect, sockfd, gen}));
        }
    }
    static_cast<void>(close(sockfd));
}

void WebModule::onTxDrainWork(void *arg) noexcept
{
    // arg 在 httpd_queue_work 时固定传入 WebModule 实例指针。
    auto *self = static_cast<WebModule *>(arg);
    if ((self == nullptr) || (self->httpd_ == nullptr)) {
        return;
    }
    self->txWorkQueued_.store(false, std::memory_order_release);
    while (self->txMailbox_.tryPop(*self->httpdTxFrame_)) {
        const int fd = self->httpdTxFrame_->fd;
        const uint32_t gen = self->httpdTxFrame_->gen;
        const auto *session = self->findHttpdSession(fd);
        if ((session != nullptr) && (session->gen == gen) &&
            (httpd_ws_get_fd_info(self->httpd_, fd) == HTTPD_WS_CLIENT_WEBSOCKET)) {
            httpd_ws_frame_t frame{};
            frame.final = true;
            frame.type = HTTPD_WS_TYPE_TEXT;
            // httpdTxFrame_ 由 httpd 线程独占弹出，esp_http_server C API 需要 uint8_t *。
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            frame.payload = reinterpret_cast<uint8_t *>(self->httpdTxFrame_->data.data());
            frame.len = self->httpdTxFrame_->len;
            const esp_err_t err = httpd_ws_send_frame_async(self->httpd_, fd, &frame);
            if (err != ESP_OK) {
                traceWeb(WebTrace::kWsTxErr, static_cast<uint32_t>(fd), static_cast<uint32_t>(err));
                const esp_err_t closeErr = httpd_sess_trigger_close(self->httpd_, fd);
                if (closeErr != ESP_OK) {
                    traceWeb(WebTrace::kWsCloseErr, static_cast<uint32_t>(fd),
                             static_cast<uint32_t>(closeErr));
                }
            }
        }
        self->txCompletedSeq_.fetch_add(1U, std::memory_order_release);
    }
    if (!self->txMailbox_.empty()) {
        self->scheduleTxDrain();
    }
}

void WebModule::scheduleTxDrain() noexcept
{
    if (httpd_ == nullptr) {
        return;
    }
    if (txWorkQueued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const esp_err_t err = httpd_queue_work(httpd_, &WebModule::onTxDrainWork, this);
    if (err != ESP_OK) {
        txWorkQueued_.store(false, std::memory_order_release);
        traceWeb(WebTrace::kWsQueueErr, static_cast<uint32_t>(err),
                 static_cast<uint32_t>(txMailbox_.size()));
    }
}

bool WebModule::enqueueTx(int fd, uint32_t gen, std::span<const char> frame) noexcept
{
    if (txMailbox_.tryPush(fd, gen, frame) != web_cfg::MailboxPush::kOk) {
        return false;
    }
    txEnqueuedSeq_.fetch_add(1U, std::memory_order_release);
    scheduleTxDrain();
    return true;
}

fw::Result WebModule::initSoftApNetif() noexcept
{
    esp_err_t err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return failInit("esp_netif_init", err);
    }
    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return failInit("event loop create", err);
    }
    apNetif_ = esp_netif_create_default_wifi_ap();
    if (apNetif_ == nullptr) {
        return failInit("create default wifi ap netif", ESP_ERR_NO_MEM);
    }
    err = configureApIpAndDhcp(apNetif_);
    if (err != ESP_OK) {
        return failInit("SoftAP IP/DHCP config", err);
    }
    return fw::Result::kOk;
}

fw::Result WebModule::startSoftApWifi(std::string_view ssid) noexcept
{
    const wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&initCfg);
    if (err != ESP_OK) {
        return failInit("esp_wifi_init", err);
    }
    wifiInited_ = true;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WebModule::onWifiEvent,
                                              this, &wifiEventHandle_);
    if (err != ESP_OK) {
        return failInit("wifi event register", err);
    }
    eventRegistered_ = true;
    wifi_config_t apCfg{};
    const size_t ssidLen = std::min(ssid.size(), sizeof(apCfg.ap.ssid));
    std::memcpy(&apCfg.ap.ssid[0], ssid.data(), ssidLen);
    // ssidLen <= 32，窄化到 uint8_t 安全。
    apCfg.ap.ssid_len = static_cast<uint8_t>(ssidLen);
    std::memcpy(&apCfg.ap.password[0], kApPassword.data(), kApPassword.size());
    apCfg.ap.channel = static_cast<uint8_t>(CONFIG_RTK_WIFI_AP_CHANNEL);
    apCfg.ap.max_connection = kMaxStations;
    apCfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    apCfg.ap.pmf_cfg.capable = true;
    apCfg.ap.pmf_cfg.required = false;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_AP);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &apCfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        return failInit("SoftAP start", err);
    }
    wifiStarted_ = true;
    return fw::Result::kOk;
}

fw::Result WebModule::startHttpd() noexcept
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 0; // ADR-0017 §2.3：固定 core 0，与 core 1 的 kLink 隔离。
    cfg.max_open_sockets = kMaxHttpSockets;
    cfg.max_uri_handlers = kMaxUriHandlers;
    cfg.lru_purge_enable = true;
    cfg.close_fn = &WebModule::onHttpdClose;
    cfg.global_user_ctx = this;
    cfg.global_user_ctx_free_fn = &noopFreeGlobalCtx;
    esp_err_t err = httpd_start(&httpd_, &cfg);
    if (err != ESP_OK) {
        return failInit("httpd_start", err);
    }
    const std::array handlers{
        httpd_uri_t{.uri = "/",
                    .method = HTTP_GET,
                    .handler = &WebModule::handleRootGet,
                    .user_ctx = this,
                    .is_websocket = false,
                    .handle_ws_control_frames = false,
                    .supported_subprotocol = nullptr,
                    .ws_post_handshake_cb = nullptr},
        httpd_uri_t{.uri = "/favicon.ico",
                    .method = HTTP_GET,
                    .handler = &WebModule::handleFaviconGet,
                    .user_ctx = this,
                    .is_websocket = false,
                    .handle_ws_control_frames = false,
                    .supported_subprotocol = nullptr,
                    .ws_post_handshake_cb = nullptr},
        httpd_uri_t{.uri = "/ws",
                    .method = HTTP_GET,
                    .handler = &WebModule::handleWs,
                    .user_ctx = this,
                    .is_websocket = true,
                    .handle_ws_control_frames = false,
                    .supported_subprotocol = nullptr,
                    .ws_post_handshake_cb = &WebModule::handleWsPostHandshake},
    };
    for (const auto &handler : handlers) {
        err = httpd_register_uri_handler(httpd_, &handler);
        if (err != ESP_OK) {
            return failInit("URI register", err);
        }
    }
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    if (err == ESP_OK) {
        err = registerOtaHandlers();
    }
#endif
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    if (err == ESP_OK) {
        err = registerCaptivePortalHandlers(httpd_, this, &WebModule::handleCaptiveRedirect,
                                            &WebModule::handleNotFound);
    }
#endif
    if (err != ESP_OK) {
        return failInit("URI register", err);
    }
    return fw::Result::kOk;
}

#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
fw::Result WebModule::startDns(int &errNo) noexcept
{
    errNo = 0;
    const int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        errNo = errno;
        return fw::Result::kIoError;
    }
    const int reuse = 1;
    static_cast<void>(lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    const int flags = lwip_fcntl(fd, F_GETFL, 0);
    if ((flags < 0) || (lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)) {
        errNo = errno;
        if (lwip_close(fd) < 0) {
            traceWeb(WebTrace::kDnsErr, kDnsErrOpClose, static_cast<uint32_t>(errno));
        }
        return fw::Result::kIoError;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(web_cfg::kDnsPort);
    // BSD/lwIP socket API 需要 const sockaddr *，sockaddr_in 为标准 IPv4 地址类型。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (lwip_bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) < 0) {
        errNo = errno;
        if (lwip_close(fd) < 0) {
            traceWeb(WebTrace::kDnsErr, kDnsErrOpClose, static_cast<uint32_t>(errno));
        }
        return fw::Result::kIoError;
    }
    dnsSock_ = fd;
    dnsOpen_ = true;
    return fw::Result::kOk;
}

void WebModule::closeDnsSocket() noexcept
{
    if (!dnsOpen_) {
        return;
    }
    if (lwip_close(dnsSock_) < 0) {
        const int errNo = errno;
        traceWeb(WebTrace::kDnsErr, kDnsErrOpClose, static_cast<uint32_t>(errNo));
        // 仅当句柄已无效（EBADF）时视作已关闭；其他错误保留句柄以便后续再次关闭。
        if (errNo != EBADF) {
            return;
        }
    }
    dnsSock_ = 0;
    dnsOpen_ = false;
}

bool WebModule::pollDns(uint32_t nowMs) noexcept
{
    if (!dnsOpen_) {
        if (dnsRetryArmed_ && ((nowMs - lastDnsRetryMs_) >= kDnsRetryIntervalMs)) {
            lastDnsRetryMs_ = nowMs;
            int retryErr = 0;
            if (fw::isOk(startDns(retryErr))) {
                dnsRetryArmed_ = false;
            }
        }
        return false;
    }
    const auto dnsRx = dnsStorage(*klinkRxFrame_);
    const auto dnsTx = dnsStorage(*klinkEncodeFrame_);
    size_t handled = 0U;
    while (handled < kMaxDnsPacketsPerPoll) {
        sockaddr_in peer{};
        iovec iov{};
        iov.iov_base = dnsRx.data();
        iov.iov_len = dnsRx.size();
        msghdr msg{};
        msg.msg_name = &peer;
        msg.msg_namelen = sizeof(peer);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        const ssize_t rxLen = lwip_recvmsg(dnsSock_, &msg, 0);
        if (rxLen < 0) {
            const int errNo = errno;
            if ((errNo != EAGAIN) && (errNo != EWOULDBLOCK)) {
                traceWeb(WebTrace::kDnsErr, kDnsErrOpRx, static_cast<uint32_t>(errNo));
                closeDnsSocket();
                dnsRetryArmed_ = true;
                lastDnsRetryMs_ = nowMs;
            }
            return false;
        }
        ++handled;
        // 超长（>512 B）报文由 lwIP 置位 MSG_TRUNC，静默丢弃并仍计入本轮 4 包预算。
        if (((msg.msg_flags & MSG_TRUNC) != 0) || (rxLen == 0)) {
            continue;
        }
        // rxLen > 0 且 <= dnsRx.size()（512），转 size_t 无损。
        const auto req = std::span<const uint8_t>{dnsRx.data(), static_cast<size_t>(rxLen)};
        const size_t replyLen = web_cfg::buildDnsReply(req, kApIpv4, dnsTx);
        if (replyLen == 0U) {
            continue;
        }
        // BSD/lwIP sendto 需要 const sockaddr * 指定对端 IPv4 地址。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto *peerAddr = reinterpret_cast<const sockaddr *>(&peer);
        const ssize_t txLen =
            lwip_sendto(dnsSock_, dnsTx.data(), replyLen, 0, peerAddr, msg.msg_namelen);
        if (txLen < 0) {
            traceWeb(WebTrace::kDnsErr, kDnsErrOpTx, static_cast<uint32_t>(errno));
        }
    }
    return true;
}
#endif

void WebModule::teardownServer() noexcept
{
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    // 无论 httpd_stop 成功与否都先关闭 DNS 套接字，避免 UDP 53 泄漏。
    dnsRetryArmed_ = false;
    closeDnsSocket();
#endif
    if (httpd_ != nullptr) {
        const esp_err_t stopErr = httpd_stop(httpd_);
        if (stopErr != ESP_OK) {
            logTeardownErr("httpd_stop", stopErr);
            // httpd 线程可能仍存活并访问本对象与底层网络栈，保留句柄与资源避免悬垂访问。
            return;
        }
        httpd_ = nullptr;
    }
    if (eventRegistered_) {
        logTeardownErr(
            "wifi event unregister",
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandle_));
        wifiEventHandle_ = nullptr;
        eventRegistered_ = false;
    }
    if (wifiStarted_) {
        logTeardownErr("esp_wifi_stop", esp_wifi_stop());
        wifiStarted_ = false;
    }
    if (wifiInited_) {
        logTeardownErr("esp_wifi_deinit", esp_wifi_deinit());
        wifiInited_ = false;
    }
    if (apNetif_ != nullptr) {
        esp_netif_destroy_default_wifi(apNetif_);
        apNetif_ = nullptr;
    }
    httpdSessions_.fill(HttpdSession{});
    txWorkQueued_.store(false, std::memory_order_release);
}

/**
 * @brief 绑定启动期由 BootArena 外供的帧缓冲数组（仅 kWifiConfig 模式分配）
 * @param frames: 外供帧数组，长度须 >= kTotalFrames，生命周期覆盖整个运行期
 * @retval fw::Status::kOk 绑定成功
 * @retval fw::Status::kInvalidState 模块已初始化或 httpd 句柄仍存活
 * @retval fw::Status::kInvalidArg 缓冲为空或长度不足
 * @note 仅启动前单线程调用，不阻塞、不分配堆，非 ISR。
 */
fw::Status WebModule::bindStorage(std::span<web_cfg::WebFrame> frames) noexcept
{
    constexpr size_t kScratchHttpdRx = 0U;
    constexpr size_t kScratchHttpdTx = 1U;
    constexpr size_t kScratchKlinkRx = 2U;
    constexpr size_t kScratchKlinkEncode = 3U;
    if (initialized_ || (httpd_ != nullptr)) {
        return fw::Status::kInvalidState;
    }
    if (frames.size() < kTotalFrames) {
        return fw::Status::kInvalidArg;
    }
    const auto rxSlots = frames.subspan(0U, kRxMailboxCap);
    const auto txSlots = frames.subspan(kRxMailboxCap, kTxMailboxCap);
    const auto scratch = frames.subspan(kRxMailboxCap + kTxMailboxCap, kScratchFrames);
    if (!rxMailbox_.bind(rxSlots) || !txMailbox_.bind(txSlots)) {
        unbindStorage();
        return fw::Status::kInvalidArg;
    }
    httpdRxFrame_ = &scratch[kScratchHttpdRx];
    httpdTxFrame_ = &scratch[kScratchHttpdTx];
    klinkRxFrame_ = &scratch[kScratchKlinkRx];
    klinkEncodeFrame_ = &scratch[kScratchKlinkEncode];
    storageBound_ = true;
    return fw::Status::kOk;
}

void WebModule::unbindStorage() noexcept
{
    if (initialized_ || (httpd_ != nullptr)) {
        return;
    }
    rxMailbox_.unbind();
    txMailbox_.unbind();
    httpdRxFrame_ = nullptr;
    httpdTxFrame_ = nullptr;
    klinkRxFrame_ = nullptr;
    klinkEncodeFrame_ = nullptr;
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    otaBuffer_ = {};
#endif
    storageBound_ = false;
}

void WebModule::resetRuntimeState() noexcept
{
    rxMailbox_.clear();
    txMailbox_.clear();
    ctrlMailbox_.clear();
    httpdSessions_.fill(HttpdSession{});
    nextSessionGen_ = 0U;
    staState_.store(0U, std::memory_order_relaxed);
    txWorkQueued_.store(false, std::memory_order_relaxed);
    powerPauseRequested_.store(false, std::memory_order_relaxed);
    powerPaused_.store(false, std::memory_order_relaxed);
    powerModeQueued_.store(false, std::memory_order_relaxed);
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    otaIdleReset_.store(false, std::memory_order_relaxed);
    otaRebootRequested_.store(false, std::memory_order_relaxed);
#endif
    powerModeAttempted_ = false;
    txEnqueuedSeq_.store(0U, std::memory_order_relaxed);
    txCompletedSeq_.store(0U, std::memory_order_relaxed);
    clients_.fill(ClientSlot{});
    inflight_ = InflightRequest{};
    pendingExit_ = {};
    pendingReboot_ = RebootKind::kNone;
    rebootTxCommitted_ = false;
    rebootDrainDone_ = false;
    ggaSampleValid_ = false;
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    dnsRetryArmed_ = false;
#endif
}

fw::Result WebModule::startNetworkStack(std::string_view ssid) noexcept
{
    // F1：先完成 netif/DHCP、httpd（含全部 URI/404 注册）与 DNS 绑定，最后再启动 SoftAP 发射，
    // 消除 IDF httpd_uri.c 注册表项写入与客户端并发请求之间的竞态。
    fw::Result step = initSoftApNetif();
    if (!fw::isOk(step)) {
        teardownServer();
        return step;
    }
    step = startHttpd();
    if (!fw::isOk(step)) {
        teardownServer();
        return step;
    }
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    int dnsErr = 0;
    if (!fw::isOk(startDns(dnsErr))) {
        // F2：DNS 启动失败仅降级门户 DNS，不拆掉 Wi-Fi 配置模式（httpd/302/option114 继续工作）。
        traceWeb(WebTrace::kDnsErr, kDnsErrOpInit, static_cast<uint32_t>(dnsErr));
        FW_LOGW(fw::TraceCat::kSys,
                "<web> captive DNS start failed (errno=%d); continuing without DNS", dnsErr);
        dnsRetryArmed_ = true;
    }
#endif
    step = startSoftApWifi(ssid);
    if (!fw::isOk(step)) {
        teardownServer();
        return step;
    }
    return fw::Result::kOk;
}

fw::Result WebModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kOk;
    }
    if ((context_ == nullptr) || !storageBound_ || (ctx().radioMode != RadioMode::kWifiConfig)) {
        return fw::Result::kInvalidState;
    }
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    if (otaBuffer_.empty()) {
        return fw::Result::kInvalidState;
    }
#endif
    const auto sys = ctx().data.systemStatus.read();
    const std::string_view ssid{sys.deviceName.data(), std::strlen(sys.deviceName.data())};
    if (ssid.empty()) {
        FW_LOGE(fw::TraceCat::kSys, "<web> empty deviceName, cannot start SoftAP");
        return fw::Result::kInvalidState;
    }
    resetRuntimeState();
    const fw::Result step = startNetworkStack(ssid);
    if (!fw::isOk(step)) {
        return step;
    }
    const uint32_t now = rtos::nowMs();
    lastStatusMs_ = now;
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    lastDnsRetryMs_ = now;
#endif
    idleTimer_.start(now, static_cast<uint32_t>(CONFIG_RTK_WIFI_IDLE_EXIT_S) * kMsPerSecond);
    initialized_ = true;
#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    const char *const portalState = dnsOpen_ ? "on" : "degraded";
#else
    constexpr const char *portalState = "off";
#endif
    FW_LOGI(fw::TraceCat::kSys,
            "<web> SoftAP %.*s up at 192.168.10.12/24, ch=%u, idle=%us, portal=%s, ui=%zuB gz",
            static_cast<int>(ssid.size()), ssid.data(),
            static_cast<unsigned>(CONFIG_RTK_WIFI_AP_CHANNEL),
            static_cast<unsigned>(CONFIG_RTK_WIFI_IDLE_EXIT_S), portalState, embeddedPageBytes());
    return fw::Result::kOk;
}

void WebModule::setPowerPaused(bool paused) noexcept
{
    powerPauseRequested_.store(paused, std::memory_order_release);
    schedulePowerMode();
}

bool WebModule::powerPauseDone() const noexcept
{
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    if (ota::uploadBusy()) {
        return false;
    }
#endif
    return powerPaused_.load(std::memory_order_acquire) &&
           !powerModeQueued_.load(std::memory_order_acquire);
}

void WebModule::schedulePowerMode() noexcept
{
#if defined(CONFIG_HTTPD_QUEUE_WORK_BLOCKING) && CONFIG_HTTPD_QUEUE_WORK_BLOCKING
#error "Power quiesce requires a non-blocking HTTP work queue"
#endif
    constexpr uint32_t kRetryMs = 100U;
    const auto now = rtos::nowMs();
    if (!initialized_ || powerModeQueued_.load(std::memory_order_acquire) ||
        powerPauseRequested_.load(std::memory_order_acquire) ==
            powerPaused_.load(std::memory_order_acquire) ||
        (powerModeAttempted_ && now - powerModeAttemptMs_ < kRetryMs)) {
        return;
    }
    powerModeAttempted_ = true;
    powerModeAttemptMs_ = now;
    powerModeQueued_.store(true, std::memory_order_release);
    if (httpd_queue_work(httpd_, &WebModule::onPowerModeWork, this) != ESP_OK) {
        powerModeQueued_.store(false, std::memory_order_release);
    }
}

void WebModule::onPowerModeWork(void *arg) noexcept
{
    // httpd 原样传回排队时登记的静态模块指针。
    auto &self = *static_cast<WebModule *>(arg);
    const bool paused = self.powerPauseRequested_.load(std::memory_order_acquire);
    if (paused != self.powerPaused_.load(std::memory_order_acquire)) {
        const esp_err_t err = esp_wifi_set_mode(paused ? WIFI_MODE_NULL : WIFI_MODE_AP);
        if (err == ESP_OK) {
            self.powerPaused_.store(paused, std::memory_order_release);
        } else {
            FW_LOGW(fw::TraceCat::kSys, "power SoftAP mode switch failed: %s",
                    esp_err_to_name(err));
        }
    }
    self.powerModeQueued_.store(false, std::memory_order_release);
}

void WebModule::stopBoot() noexcept
{
    teardownServer();
    if (httpd_ == nullptr) {
        initialized_ = false;
    }
}

} // namespace app
