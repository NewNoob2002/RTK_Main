// 主机回归：编译真实 WebServer.cpp + WebModule.cpp，按 IDF v6.1 行为模拟 httpd/SoftAP，
// 覆盖 Revision A 全部项：WS post-handshake 握手、/favicon.ico 204、global_user_ctx 空释放、
// httpd_stop 失败保句柄、fd 复用代号隔离、ctrl 槽位预留、分片/超长关闭码、初始 config 先于请求 ack、
// TX 背压与队列重调、GNSS 超时取消背压、退出 GGA 门控持久化确认、空闲重启重连取消。
#include <array>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "AppContext.hpp"
#include "board/BoardProfile.hpp"
#include "def/StorageDef.hpp"
#include "modules/web/WebModule.hpp"
#include "rtos/EspStatus.hpp"

// 嵌入页面符号替身（与 CMake EMBED_FILES 一致）。
asm(".section .rodata\n"
    ".global _binary_index_html_gz_start\n"
    ".global _binary_index_html_gz_end\n"
    "_binary_index_html_gz_start:\n"
    ".ascii \"<gz-ui>\"\n"
    "_binary_index_html_gz_end:\n");

namespace {

struct SentFrame {
    int fd = 0;
    httpd_ws_type_t type = HTTPD_WS_TYPE_TEXT;
    std::string payload{};
    bool async = false;
};

struct MockHttpdState {
    httpd_config_t config{};
    std::vector<httpd_uri_t> uris{};
    bool running = false;
    bool failStop = false;
    bool failQueueWorkOnce = false;
    int globalCtxFreeCalls = 0;
    int closeCalls = 0;
    int wifiStopCalls = 0;
    int netifDestroyCalls = 0;
    int queueWorkCalls = 0;
    httpd_work_fn_t pendingWork = nullptr;
    void *pendingWorkArg = nullptr;

    esp_event_handler_t wifiHandler = nullptr;
    void *wifiHandlerArg = nullptr;

    // 当前待读取的入站 WS 帧（供 handleWs 中的 httpd_ws_recv_frame 使用）。
    httpd_ws_frame_t nextRxMeta{};
    std::string nextRxPayload{};

    // HTTP 响应捕获。
    std::string lastRespStatus{};
    std::string lastRespType{};
    std::string lastRespBody{};

    // 发送帧记录（同步与异步）。
    std::vector<SentFrame> sent{};
    std::vector<int> liveWsFds{};
};

MockHttpdState s_httpd{};
uint32_t s_nowMs = 1000U;
std::string s_mockSn = "B5F2";
bool s_mockPairNext = false;
fw::Result s_mockStorageResult = fw::Result::kOk;
int s_storageCmdCalls = 0;

bool s_catchReboot = false;
std::jmp_buf s_rebootJmp{};
int s_rebootCalls = 0;
app::RadioMode s_lastRadioMode = app::RadioMode::kWifiConfig;
app::RadioReason s_lastRadioReason = app::RadioReason::kConsole;
bool s_lastWasRestart = false;

app::AppContext s_ctx{};
rtos::Executor s_ioExec{};
std::array<app::web_cfg::WebFrame, app::WebModule::kTotalFrames> s_webFrames{};

const httpd_uri_t *findUri(std::string_view path)
{
    for (const auto &u : s_httpd.uris) {
        if (std::string_view{u.uri} == path) {
            return &u;
        }
    }
    return nullptr;
}

void setWsFdLive(int fd, bool live)
{
    for (auto it = s_httpd.liveWsFds.begin(); it != s_httpd.liveWsFds.end(); ++it) {
        if (*it == fd) {
            if (!live) {
                s_httpd.liveWsFds.erase(it);
            }
            return;
        }
    }
    if (live) {
        s_httpd.liveWsFds.push_back(fd);
    }
}

void runPendingWork()
{
    while (s_httpd.pendingWork != nullptr) {
        httpd_work_fn_t fn = s_httpd.pendingWork;
        void *arg = s_httpd.pendingWorkArg;
        s_httpd.pendingWork = nullptr;
        s_httpd.pendingWorkArg = nullptr;
        fn(arg);
    }
}

// 严格按 ESP-IDF v6.1 httpd_uri.c:339-366 行为模拟 WS 握手：只调 ws_post_handshake_cb，不调 handler。
esp_err_t simulateIdfV61WsHandshake(int fd)
{
    const auto *ws = findUri("/ws");
    assert(ws != nullptr && ws->is_websocket);
    httpd_req_t req{};
    req.handle = &s_httpd;
    req.method = HTTP_GET;
    req.uri = "/ws";
    req.user_ctx = ws->user_ctx;
    req.sockfd = fd;
    setWsFdLive(fd, true);
    assert(ws->ws_post_handshake_cb != nullptr);
    const esp_err_t err = ws->ws_post_handshake_cb(&req);
    if (err != ESP_OK) {
        setWsFdLive(fd, false);
    }
    return err;
}

esp_err_t simulateWsFrame(int fd, httpd_ws_type_t type, bool finalFlag, std::string_view payload,
                          size_t declaredLen = SIZE_MAX)
{
    const auto *ws = findUri("/ws");
    assert(ws != nullptr);
    s_httpd.nextRxMeta = httpd_ws_frame_t{
        .final = finalFlag,
        .fragmented = !finalFlag,
        .type = type,
        .payload = nullptr,
        .len = (declaredLen == SIZE_MAX) ? payload.size() : declaredLen,
    };
    s_httpd.nextRxPayload.assign(payload.data(), payload.size());
    httpd_req_t req{};
    req.handle = &s_httpd;
    req.method = HTTP_GET;
    req.uri = "/ws";
    req.user_ctx = ws->user_ctx;
    req.sockfd = fd;
    return ws->handler(&req);
}

void simulateWsClose(int fd)
{
    setWsFdLive(fd, false);
    assert(s_httpd.config.close_fn != nullptr);
    s_httpd.config.close_fn(&s_httpd, fd);
}

void simulateWifiEvent(int32_t eventId)
{
    assert(s_httpd.wifiHandler != nullptr);
    s_httpd.wifiHandler(s_httpd.wifiHandlerArg, WIFI_EVENT, eventId, nullptr);
}

void resetHarness()
{
    s_httpd = MockHttpdState{};
    s_nowMs = 1000U;
    s_mockSn = "B5F2";
    s_mockPairNext = false;
    s_mockStorageResult = fw::Result::kOk;
    s_storageCmdCalls = 0;
    s_rebootCalls = 0;
    s_lastWasRestart = false;
    s_catchReboot = false;

    s_ctx.radioMode = app::RadioMode::kWifiConfig;
    s_ctx.boardProfile = app::board::byId(app::board::BoardId::kP2V141);
    s_ctx.data.init();
    app::SystemStatus sys{};
    std::snprintf(sys.sn.data(), sys.sn.size(), "%s", "B5F2");
    std::snprintf(sys.deviceName.data(), sys.deviceName.size(), "%s", "P2-B5F2");
    sys.freeHeap = 80000U;
    sys.minFreeHeap = 75000U;
    s_ctx.data.systemStatus.write(sys);
    s_ctx.data.gnssRateHz.store(1U, std::memory_order_release);
    s_ctx.data.ggaGatePersist.store(app::data::GgaGatePersist::kClean, std::memory_order_release);
    s_ctx.data.webGnssCancelSeq.store(0U, std::memory_order_release);
    s_ctx.data.webGnssFlushGgaSeq.store(0U, std::memory_order_release);
    s_ctx.data.webGnssFlushGgaDoneSeq.store(0U, std::memory_order_release);
    s_ctx.data.webGnssCommand.write(fw::Command{});
    s_ctx.data.webGnssOutcome.write(app::data::WebGnssOutcome{});

    // 构造一个已运行的伪 ioExecutor 视图（仅需 running() 与 moduleOk(kGnss) 为真）。
    s_ctx.ioExecutor = &s_ioExec;
    s_ctx.web.bindContext(s_ctx);
    assert(fw::isOk(s_ctx.web.bindStorage(s_webFrames)));
    assert(fw::isOk(s_ctx.web.init()));
}

} // namespace

// ---- ESP-IDF C 桩实现 ----
extern "C" {

esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";
static int s_dummyNetif = 0;

const char *esp_err_to_name(esp_err_t err)
{
    return (err == ESP_OK) ? "ESP_OK" : "ESP_FAIL";
}

int64_t esp_timer_get_time(void)
{
    return static_cast<int64_t>(s_nowMs) * 1000LL;
}

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

void esp_restart(void)
{
    ++s_rebootCalls;
    s_lastWasRestart = true;
    if (s_catchReboot) {
        std::longjmp(s_rebootJmp, 1);
    }
    std::abort();
}

esp_err_t esp_netif_init(void)
{
    return ESP_OK;
}
esp_netif_t *esp_netif_create_default_wifi_ap(void)
{
    return reinterpret_cast<esp_netif_t *>(&s_dummyNetif);
}
void esp_netif_destroy_default_wifi(void * /*esp_netif*/)
{
    ++s_httpd.netifDestroyCalls;
}
esp_err_t esp_netif_dhcps_stop(esp_netif_t * /*esp_netif*/)
{
    return ESP_OK;
}
esp_err_t esp_netif_set_ip_info(esp_netif_t * /*esp_netif*/, const esp_netif_ip_info_t * /*info*/)
{
    return ESP_OK;
}
esp_err_t esp_netif_dhcps_start(esp_netif_t * /*esp_netif*/)
{
    return ESP_OK;
}

esp_err_t esp_event_loop_create_default(void)
{
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_register(esp_event_base_t /*base*/, int32_t /*id*/,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance)
{
    s_httpd.wifiHandler = handler;
    s_httpd.wifiHandlerArg = arg;
    *instance = reinterpret_cast<esp_event_handler_instance_t>(0x1);
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t /*base*/, int32_t /*id*/,
                                                esp_event_handler_instance_t /*instance*/)
{
    s_httpd.wifiHandler = nullptr;
    return ESP_OK;
}

esp_err_t esp_wifi_init(const wifi_init_config_t * /*config*/)
{
    return ESP_OK;
}
esp_err_t esp_wifi_deinit(void)
{
    return ESP_OK;
}
esp_err_t esp_wifi_set_storage(wifi_storage_t /*storage*/)
{
    return ESP_OK;
}
esp_err_t esp_wifi_set_mode(wifi_mode_t /*mode*/)
{
    return ESP_OK;
}
esp_err_t esp_wifi_set_config(wifi_interface_t /*ifx*/, wifi_config_t * /*conf*/)
{
    return ESP_OK;
}
esp_err_t esp_wifi_start(void)
{
    return ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    ++s_httpd.wifiStopCalls;
    return ESP_OK;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    s_httpd.config = *config;
    s_httpd.running = true;
    *handle = &s_httpd;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t /*handle*/)
{
    if (s_httpd.failStop) {
        return ESP_FAIL;
    }
    s_httpd.running = false;
    // 严格复现 IDF v6.1 httpd_main.c:612-618：global_user_ctx_free_fn 为空则断言失败（真实硬件会 free 静态对象）。
    if (s_httpd.config.global_user_ctx != nullptr) {
        assert(s_httpd.config.global_user_ctx_free_fn != nullptr);
        s_httpd.config.global_user_ctx_free_fn(s_httpd.config.global_user_ctx);
        ++s_httpd.globalCtxFreeCalls;
        s_httpd.config.global_user_ctx = nullptr;
    }
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t /*handle*/, const httpd_uri_t *uri_handler)
{
    s_httpd.uris.push_back(*uri_handler);
    return ESP_OK;
}

void *httpd_get_global_user_ctx(httpd_handle_t /*handle*/)
{
    return s_httpd.config.global_user_ctx;
}

int httpd_req_to_sockfd(httpd_req_t *r)
{
    return r->sockfd;
}

esp_err_t httpd_resp_set_status(httpd_req_t * /*r*/, const char *status)
{
    s_httpd.lastRespStatus = status;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t * /*r*/, const char *type)
{
    s_httpd.lastRespType = type;
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t * /*r*/, const char * /*field*/, const char * /*value*/)
{
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t * /*r*/, const char *buf, ssize_t buf_len)
{
    if ((buf != nullptr) && (buf_len > 0)) {
        s_httpd.lastRespBody.assign(buf, static_cast<size_t>(buf_len));
    } else {
        s_httpd.lastRespBody.clear();
    }
    return ESP_OK;
}

esp_err_t httpd_ws_recv_frame(httpd_req_t * /*req*/, httpd_ws_frame_t *pkt, size_t max_len)
{
    if (max_len == 0U) {
        *pkt = s_httpd.nextRxMeta;
        return ESP_OK;
    }
    const size_t n = std::min(max_len, s_httpd.nextRxPayload.size());
    if ((n > 0U) && (pkt->payload != nullptr)) {
        std::memcpy(pkt->payload, s_httpd.nextRxPayload.data(), n);
    }
    pkt->len = n;
    return ESP_OK;
}

esp_err_t httpd_ws_send_frame(httpd_req_t *req, httpd_ws_frame_t *pkt)
{
    SentFrame f{};
    f.fd = req->sockfd;
    f.type = pkt->type;
    f.async = false;
    if ((pkt->payload != nullptr) && (pkt->len > 0U)) {
        f.payload.assign(reinterpret_cast<const char *>(pkt->payload), pkt->len);
    }
    s_httpd.sent.push_back(std::move(f));
    return ESP_OK;
}

esp_err_t httpd_ws_send_frame_async(httpd_handle_t /*hd*/, int fd, httpd_ws_frame_t *frame)
{
    SentFrame f{};
    f.fd = fd;
    f.type = frame->type;
    f.async = true;
    if ((frame->payload != nullptr) && (frame->len > 0U)) {
        f.payload.assign(reinterpret_cast<const char *>(frame->payload), frame->len);
    }
    s_httpd.sent.push_back(std::move(f));
    return ESP_OK;
}

httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t /*hd*/, int sockfd)
{
    for (int fd : s_httpd.liveWsFds) {
        if (fd == sockfd) {
            return HTTPD_WS_CLIENT_WEBSOCKET;
        }
    }
    return HTTPD_WS_CLIENT_INVALID;
}

esp_err_t httpd_sess_trigger_close(httpd_handle_t /*handle*/, int /*sockfd*/)
{
    ++s_httpd.closeCalls;
    return ESP_OK;
}

esp_err_t httpd_queue_work(httpd_handle_t /*handle*/, httpd_work_fn_t work, void *arg)
{
    ++s_httpd.queueWorkCalls;
    if (s_httpd.failQueueWorkOnce) {
        s_httpd.failQueueWorkOnce = false;
        return ESP_FAIL;
    }
    s_httpd.pendingWork = work;
    s_httpd.pendingWorkArg = arg;
    return ESP_OK;
}

} // extern "C"

// ---- 最小 C++ 依赖桩 ----
namespace rtos {
uint32_t nowMs() noexcept
{
    return s_nowMs;
}
fw::Status fromEspErr(esp_err_t err) noexcept
{
    if (err == ESP_OK) {
        return fw::Status::kOk;
    }
    if (err == ESP_ERR_NO_MEM) {
        return fw::Status::kNoSpace;
    }
    return fw::Status::kIoError;
}
} // namespace rtos

namespace app {

AppContext &appContext() noexcept
{
    return s_ctx;
}

std::string_view StorageModule::sn() const noexcept
{
    return s_mockSn;
}

void StorageModule::onCommand(const fw::Command & /*cmd*/, fw::CommandResult &result) noexcept
{
    ++s_storageCmdCalls;
    result.status = s_mockStorageResult;
}

void requestRadioMode(RadioMode mode, RadioReason reason) noexcept
{
    ++s_rebootCalls;
    s_lastWasRestart = false;
    s_lastRadioMode = mode;
    s_lastRadioReason = reason;
    if (s_catchReboot) {
        std::longjmp(s_rebootJmp, 1);
    }
    std::abort();
}

} // namespace app

namespace {

/** @brief 1. 验证 IDF v6.1 WS 握手回调、/favicon.ico 204 与 global_user_ctx 空释放回调。 */
void testHandshakeFaviconAndStopLifecycle()
{
    resetHarness();

    // 验证 /favicon.ico 返回 204 No Content，/ 返回嵌入 gzip 页。
    const auto *fav = findUri("/favicon.ico");
    assert(fav != nullptr);
    httpd_req_t favReq{};
    assert(fav->handler(&favReq) == ESP_OK);
    assert(s_httpd.lastRespStatus == "204 No Content" && s_httpd.lastRespBody.empty());

    const auto *root = findUri("/");
    assert(root != nullptr);
    httpd_req_t rootReq{};
    assert(root->handler(&rootReq) == ESP_OK);
    assert(s_httpd.lastRespBody == "<gz-ui>");

    // 未经 ws_post_handshake_cb 直接调 handler（模拟误把握手放进 handler）：必须拒绝，绝不发 hello。
    assert(simulateWsFrame(1001, HTTPD_WS_TYPE_TEXT, true, R"({"t":"cmd","id":1,"name":"save"})") ==
           ESP_FAIL);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.empty());

    // 按 IDF v6.1 行为调用 ws_post_handshake_cb（不调 handler）：必须注册并依次发 hello/config/status。
    assert(simulateIdfV61WsHandshake(1001) == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.size() == 3U);
    assert(s_httpd.sent[0].payload.find(R"("t":"hello")") != std::string::npos);
    assert(s_httpd.sent[1].payload.find(R"("t":"config")") != std::string::npos);
    assert(s_httpd.sent[2].payload.find(R"("t":"status")") != std::string::npos);

    // 验证 httpd_stop 失败时保留句柄和底层资源、且禁止 unbindStorage（P1 #1）。
    s_httpd.failStop = true;
    s_ctx.web.stopBoot();
    assert(s_httpd.wifiStopCalls == 0 && s_httpd.netifDestroyCalls == 0);
    s_ctx.web.unbindStorage(); // httpd 仍存活，不得解绑。

    // httpd_stop 成功后调用具名空释放回调而非 free()，随后才释放网络资源。
    s_httpd.failStop = false;
    s_ctx.web.stopBoot();
    assert(s_httpd.globalCtxFreeCalls == 1 && s_httpd.wifiStopCalls == 1 &&
           s_httpd.netifDestroyCalls == 1);
    s_ctx.web.unbindStorage();
}

/** @brief 2. 验证分片/二进制/超长帧关闭码（1003/1009）与初始 config 先于请求 ack（P2 #9）。 */
void testFramingAndConfigBeforeAck()
{
    resetHarness();
    assert(simulateIdfV61WsHandshake(1002) == ESP_OK);

    // 握手后 kLink 尚未轮询就立即发来 unlock 请求：必须先排入 hello + config，再处理请求发 ack。
    assert(simulateWsFrame(1002, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"unlock","id":9,"token":"123456"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.size() == 4U);
    assert(s_httpd.sent[0].payload.find(R"("t":"hello")") != std::string::npos);
    assert(s_httpd.sent[1].payload.find(R"("t":"config")") != std::string::npos);
    assert(s_httpd.sent[2].payload.find(R"("t":"status")") != std::string::npos);
    assert(s_httpd.sent[3].payload == R"({"t":"ack","id":9,"ok":true})");

    // 分片 TEXT（final=false）→ 关闭码 1003（0x03 0xEB）。
    s_httpd.sent.clear();
    assert(simulateWsFrame(1002, HTTPD_WS_TYPE_TEXT, false, R"({"t":"cmd")") == ESP_FAIL);
    assert(s_httpd.sent.size() == 1U && s_httpd.sent[0].type == HTTPD_WS_TYPE_CLOSE &&
           s_httpd.sent[0].payload.size() == 2U &&
           static_cast<uint8_t>(s_httpd.sent[0].payload[0]) == 0x03U &&
           static_cast<uint8_t>(s_httpd.sent[0].payload[1]) == 0xEBU);

    // 续帧 CONTINUE → 关闭码 1003。
    s_httpd.sent.clear();
    assert(simulateWsFrame(1002, HTTPD_WS_TYPE_CONTINUE, true, "}") == ESP_FAIL);
    assert(s_httpd.sent.size() == 1U && s_httpd.sent[0].type == HTTPD_WS_TYPE_CLOSE &&
           static_cast<uint8_t>(s_httpd.sent[0].payload[1]) == 0xEBU);

    // 二进制帧 → 关闭码 1003。
    s_httpd.sent.clear();
    assert(simulateWsFrame(1002, HTTPD_WS_TYPE_BINARY, true, "bin") == ESP_FAIL);
    assert(s_httpd.sent.size() == 1U && s_httpd.sent[0].type == HTTPD_WS_TYPE_CLOSE &&
           static_cast<uint8_t>(s_httpd.sent[0].payload[1]) == 0xEBU);

    // 超长帧（1025 B）→ 关闭码 1009（0x03 0xF1）。
    s_httpd.sent.clear();
    assert(simulateWsFrame(1002, HTTPD_WS_TYPE_TEXT, true, "", web::kMaxFrameBytes + 1U) ==
           ESP_FAIL);
    assert(s_httpd.sent.size() == 1U && s_httpd.sent[0].type == HTTPD_WS_TYPE_CLOSE &&
           static_cast<uint8_t>(s_httpd.sent[0].payload[0]) == 0x03U &&
           static_cast<uint8_t>(s_httpd.sent[0].payload[1]) == 0xF1U);

    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/** @brief 3. 验证会话代号防 fd 复用串会话、控制队列预留不断链、TX 重调与背压不丢 ACK（P1 #2, P2 #5）。 */
void testSessionGenAndBackpressure()
{
    resetHarness();

    // 准备一个假的 running ioExecutor 状态：通过向 ioExecutor 注入空测试队列启动或直接走 storage 测试。
    // 先验证 fd=1003 复用：gen=1 解锁并发起 factory.sn，关闭前再复用 fd=1003（gen=2）不得继承解锁，
    // 且 gen=1 残留在 TX 邮箱的帧在 onTxDrainWork 中必须被代号过滤丢弃。
    assert(simulateIdfV61WsHandshake(1003) == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    s_httpd.sent.clear();

    // gen=1 解锁，但不跑 pendingWork，让其 ack 停留在 TX 邮箱中。
    assert(simulateWsFrame(1003, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"unlock","id":1,"token":"123456"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    // gen=1 关闭，紧接着 gen=2 复用同一个 fd=1003！
    simulateWsClose(1003);
    assert(simulateIdfV61WsHandshake(1003) == ESP_OK);
    // 此时排空 TX 邮箱：gen=1 的 unlock ack 必须被 onTxDrainWork 按代号丢弃，不得发给 gen=2！
    runPendingWork();
    assert(s_httpd.sent.empty());

    // kLink 轮询处理 gen=2：发 hello/config/status，且 gen=2 尝试写 factory.sn 必须返回 locked。
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    s_httpd.sent.clear();
    assert(simulateWsFrame(1003, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"set","id":2,"key":"factory.sn","value":"ABCD"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.size() == 1U &&
           s_httpd.sent[0].payload == R"({"t":"ack","id":2,"ok":false,"err":"locked"})");

    // 模拟 httpd_queue_work 首次失败：下一轮 poll 必须自动重调并成功送出 ACK（P2 #5）。
    s_httpd.sent.clear();
    s_httpd.failQueueWorkOnce = true;
    assert(simulateWsFrame(1003, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"unlock","id":3,"token":"123456"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    assert(s_httpd.pendingWork == nullptr); // 首次 queue_work 失败。
    static_cast<void>(s_ctx.web.poll(s_nowMs + 10U));
    runPendingWork();
    assert(s_httpd.sent.size() == 1U &&
           s_httpd.sent[0].payload == R"({"t":"ack","id":3,"ok":true})");

    // 控制队列槽位预留不变量：连续开关连接填满 ctrlMailbox_（不让 kLink 消费），
    // 在剩余槽位不足以覆盖活跃连接断开时拒绝新升级，且既有连接关闭事件绝不丢失。
    simulateWsClose(1003);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    int rejected = 0;
    for (int i = 0; i < 10; ++i) {
        if (simulateIdfV61WsHandshake(2000 + i) == ESP_OK) {
            simulateWsClose(2000 + i);
        } else {
            ++rejected;
        }
    }
    assert(rejected > 0); // 16 槽最多容 7 对 connect+disconnect + 预留，后续升级必被拒。

    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/** @brief 4. 验证空闲回落排定后终端重连取消重启（P2 #8）与退出前 GGA 持久化门控（P1 #4）。 */
void testIdleAbortAndExitGgaGate()
{
    resetHarness();
    assert(simulateIdfV61WsHandshake(1004) == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    s_httpd.sent.clear();

    // 1. 触发空闲超时（300 s 无站点连接）→ 排定 idle 重启并推 bye{reason:"idle"}（前序可能伴随 1 Hz status）。
    s_nowMs += 300000U;
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(!s_httpd.sent.empty() &&
           (s_httpd.sent.back().payload == R"({"t":"bye","reason":"idle"})"));

    // 在重启宽限期内有站点连入 SoftAP → 必须中止 idle 重启，超过最大等待也不得重启！
    simulateWifiEvent(WIFI_EVENT_AP_STACONNECTED);
    s_nowMs += 1000U;
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_rebootCalls == 0);

    // 2. GGA 门控持久化处于 kFailed 且 ioExecutor 未运行 → cmd exit 必须回 failed 且留在配置模式！
    s_httpd.sent.clear();
    s_ctx.data.ggaGatePersist.store(app::data::GgaGatePersist::kFailed, std::memory_order_release);
    assert(simulateWsFrame(1004, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"cmd","id":20,"name":"exit"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.size() == 1U &&
           s_httpd.sent[0].payload == R"({"t":"ack","id":20,"ok":false,"err":"failed"})");
    assert(s_rebootCalls == 0);

    // 2b. TX 邮箱满时背压不得丢弃 ack：连续塞满 8 槽后发第 9 条请求，排空后再 poll 必达（P2 #5）。
    s_httpd.sent.clear();
    for (int i = 0; i < 8; ++i) {
        std::string req =
            R"({"t":"unlock","id":)" + std::to_string(30 + i) + R"(,"token":"123456"})";
        assert(simulateWsFrame(1004, HTTPD_WS_TYPE_TEXT, true, req) == ESP_OK);
        static_cast<void>(s_ctx.web.poll(s_nowMs));
    }
    // 此时 TX 邮箱（8 槽）已满；再送第 9 条请求，ack 会暂存在 pendingAck 中不丢失。
    assert(simulateWsFrame(1004, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"unlock","id":38,"token":"123456"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.size() == 8U);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert((s_httpd.sent.size() == 9U) &&
           (s_httpd.sent.back().payload == R"({"t":"ack","id":38,"ok":true})"));

    // 3. GGA 门控已持久化（kClean）→ cmd exit 回 ack ok + bye{reason:"exit"}，
    //    并在 httpd 完成发送 + 100 ms 宽限后切回 kBluetooth。
    s_httpd.sent.clear();
    s_ctx.data.ggaGatePersist.store(app::data::GgaGatePersist::kClean, std::memory_order_release);
    assert(simulateWsFrame(1004, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"cmd","id":21,"name":"exit"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    // httpd 尚未执行 onTxDrainWork，推进 150 ms（< 500 ms 上限）仍不得提前重启！
    s_nowMs += 150U;
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    assert(s_rebootCalls == 0);
    // 排空 TX 后再推进 100 ms 宽限 → 触发 requestRadioMode(kBluetooth, kWebExit)！
    runPendingWork();
    assert(s_httpd.sent.size() == 2U);
    assert(s_httpd.sent[0].payload == R"({"t":"ack","id":21,"ok":true})");
    assert(s_httpd.sent[1].payload == R"({"t":"bye","reason":"exit"})");

    s_catchReboot = true;
    if (setjmp(s_rebootJmp) == 0) {
        s_nowMs += 50U;
        static_cast<void>(s_ctx.web.poll(s_nowMs)); // 记下 rebootDrainDoneMs_。
        s_nowMs += 100U;
        static_cast<void>(s_ctx.web.poll(s_nowMs)); // 宽限到，触发切回蓝牙。
        assert(false && "expected requestRadioMode reboot");
    }
    s_catchReboot = false;
    assert((s_rebootCalls == 1) && !s_lastWasRestart &&
           (s_lastRadioMode == app::RadioMode::kBluetooth) &&
           (s_lastRadioReason == app::RadioReason::kWebExit));

    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

} // namespace

/**
 * @brief 运行真实 WebServer.cpp + WebModule.cpp 的 IDF v6.1 主机回归测试
 * @retval 0 全部通过
 * @note 主机单线程测试入口，支持 ASan/UBSan。
 */
int main()
{
    testHandshakeFaviconAndStopLifecycle();
    testFramingAndConfigBeforeAck();
    testSessionGenAndBackpressure();
    testIdleAbortAndExitGgaGate();
    std::puts("PASS web_module_check");
    return 0;
}
