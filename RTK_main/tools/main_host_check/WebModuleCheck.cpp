// 主机回归：编译真实 WebServer.cpp + WebModule.cpp，按 IDF v6.1 行为模拟 httpd/SoftAP，
// 覆盖 Revision A 全部项：WS post-handshake 握手、/favicon.ico 204、global_user_ctx 空释放、
// httpd_stop 失败保句柄、fd 复用代号隔离、ctrl 槽位预留、分片/超长关闭码、初始 config 先于请求 ack、
// TX 背压与队列重调、GNSS 超时取消背压、退出 GGA 门控持久化确认、空闲重启重连取消。
#include <algorithm>
#include <array>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "AppContext.hpp"
#include "app/SettingsBackend.hpp"
#include "app/PowerRequests.hpp"
#include "board/BoardProfile.hpp"
#include "board/BoardStartup.hpp"
#include "board/Charger.hpp"
#include "driver/gpio.h"
#include "def/StorageDef.hpp"
#include "modules/web/WebModule.hpp"
#include "def/GnssSubDef.hpp"
#include "settings/Registry.hpp"
#include "rtos/EspStatus.hpp"
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
#include "esp_image_format.h"
#include "modules/ota/OtaRuntime.hpp"
#include "modules/web/WebOtaTransfer.hpp"
#endif

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
    httpd_err_handler_func_t err404Handler = nullptr;
    bool running = false;
    bool failStop = false;
    bool failQueueWorkOnce = false;
    int globalCtxFreeCalls = 0;
    int closeCalls = 0;
    int wifiStopCalls = 0;
    wifi_mode_t wifiMode = WIFI_MODE_NULL;
    int netifDestroyCalls = 0;
    int queueWorkCalls = 0;
    httpd_work_fn_t pendingWork = nullptr;
    void *pendingWorkArg = nullptr;

    // DHCP 选项 114 与有序调用序列捕获（F1 / F10）。
    bool dhcpsRunning = false;
    int dhcpsOptionCalls = 0;
    bool dhcpsOptionSetWhileStopped = false;
    const void *captivePortalUriPtr = nullptr;
    std::string captivePortalUri{};
    std::vector<std::string> callOrder{};

    // DNS 套接字替身状态与故障注入（F2 / F3 / F5）。
    int dnsSocketCalls = 0;
    int dnsCloseCalls = 0;
    bool dnsOpen = false;
    bool dnsNonBlock = false;
    uint16_t dnsBoundPort = 0U;
    bool failDnsSocket = false;
    bool failDnsFcntl = false;
    bool failDnsBind = false;
    bool failDnsCloseOnce = false;
    int failDnsRecvErrno = 0;
    std::vector<std::vector<uint8_t>> dnsRxQueue{};
    std::vector<std::vector<uint8_t>> dnsTxSent{};

    esp_event_handler_t wifiHandler = nullptr;
    void *wifiHandlerArg = nullptr;

    // 当前待读取的入站 WS 帧（供 handleWs 中的 httpd_ws_recv_frame 使用）。
    httpd_ws_frame_t nextRxMeta{};
    std::string nextRxPayload{};

    // HTTP 响应捕获。
    std::string lastRespStatus{};
    std::string lastRespType{};
    std::string lastRespLocation{};
    std::string lastRespCacheControl{};
    std::string lastRespBody{};
    int lastRespErrCode = -1;

    // 发送帧记录（同步与异步）。
    std::vector<SentFrame> sent{};
    std::vector<int> liveWsFds{};
};

MockHttpdState s_httpd{};
uint32_t s_nowMs = 1000U;
bool s_powerKeyDriven = false;
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

#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
struct MockOtaState {
    std::span<const char> body{};
    void (*onReceive)() = nullptr;
    int received = 0;
    int aborted = 0;
    size_t written = 0U;
    const esp_partition_t *bootPartition = nullptr;
    bool otherReadable = true;
    esp_ota_img_states_t otherState = ESP_OTA_IMG_VALID;
    esp_err_t otherStateErr = ESP_OK;
    bool backpressure = false;
    long receiveWaitS = 5L;
    std::string wireReply{};
};

MockOtaState s_ota{};
constexpr esp_partition_t kRunningSlot{.address = 0x20000U, .size = 0x1E0000U, .label = "ota_0"};
constexpr esp_partition_t kOtherSlot{.address = 0x200000U, .size = 0x1E0000U, .label = "ota_1"};
alignas(esp_app_desc_t) std::array<std::byte, app::web_ota::kBufferBytes + 1U> s_otaBuffer{};
#endif

/** @brief 为测试服务绑定正常启动提供的工作区。 */
void bindWebStorage()
{
    assert(fw::isOk(s_ctx.web.bindStorage(s_webFrames)));
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    const auto scratch = std::span{s_otaBuffer};
    assert(s_ctx.web.bindOtaStorage(scratch.last(app::web_ota::kBufferBytes)) ==
           fw::Status::kInvalidArg);
    assert(fw::isOk(s_ctx.web.bindOtaStorage(scratch.first(app::web_ota::kBufferBytes))));
#endif
}

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
    app::power::s_power_requests.releaseAction();
    s_httpd = MockHttpdState{};
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    s_ota = {};
    app::ota::clearOtherSlotDirty();
    s_ctx.bootPhase.store(app::BootPhase::kRunning, std::memory_order_relaxed);
#endif
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
    s_ctx.ioExecutor = &s_ioExec;
    app::bindSettings(s_ctx);
    s_ctx.web.bindContext(s_ctx);
    bindWebStorage();
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

/** @brief 主机测试桩：单线程网页回归无当前 FreeRTOS 任务句柄。 @retval nullptr 总是。 */
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return nullptr;
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
/**
 * @brief 模拟停止 DHCP 服务并记录有序调用点
 * @param esp_netif: 网络接口句柄（未使用）
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t esp_netif_dhcps_stop(esp_netif_t * /*esp_netif*/)
{
    s_httpd.dhcpsRunning = false;
    s_httpd.callOrder.emplace_back("dhcps_stop");
    return ESP_OK;
}

/**
 * @brief 模拟设置 SoftAP 静态 IP
 * @param esp_netif: 网络接口句柄（未使用）
 * @param info: IP 信息（未使用）
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t esp_netif_set_ip_info(esp_netif_t * /*esp_netif*/, const esp_netif_ip_info_t * /*info*/)
{
    return ESP_OK;
}

/**
 * @brief 模拟设置 DHCP 选项并记录选项 114 指针、内容与调用顺序
 * @param esp_netif: 网络接口句柄（未使用）
 * @param opt_op: 操作类型
 * @param opt_id: 选项编号
 * @param opt_val: 选项值指针
 * @param opt_len: 选项长度
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t esp_netif_dhcps_option(esp_netif_t * /*esp_netif*/, esp_netif_dhcp_option_mode_t opt_op,
                                 esp_netif_dhcp_option_id_t opt_id, void *opt_val, uint32_t opt_len)
{
    ++s_httpd.dhcpsOptionCalls;
    if ((opt_op == ESP_NETIF_OP_SET) && (opt_id == ESP_NETIF_CAPTIVEPORTAL_URI) &&
        (opt_val != nullptr)) {
        s_httpd.dhcpsOptionSetWhileStopped = !s_httpd.dhcpsRunning;
        s_httpd.captivePortalUriPtr = opt_val;
        s_httpd.captivePortalUri.assign(static_cast<const char *>(opt_val), opt_len);
        s_httpd.callOrder.emplace_back("dhcps_option_114");
    }
    return ESP_OK;
}

/**
 * @brief 模拟启动 DHCP 服务并记录有序调用点
 * @param esp_netif: 网络接口句柄（未使用）
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t esp_netif_dhcps_start(esp_netif_t * /*esp_netif*/)
{
    s_httpd.dhcpsRunning = true;
    s_httpd.callOrder.emplace_back("dhcps_start");
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
esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    s_httpd.wifiMode = mode;
    return ESP_OK;
}
esp_err_t esp_wifi_set_config(wifi_interface_t /*ifx*/, wifi_config_t * /*conf*/)
{
    return ESP_OK;
}
/**
 * @brief 模拟启动 Wi-Fi SoftAP 发射并记录启动顺序
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t esp_wifi_start(void)
{
    s_httpd.callOrder.emplace_back("wifi_start");
    return ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    ++s_httpd.wifiStopCalls;
    return ESP_OK;
}

/**
 * @brief 模拟启动 httpd 并记录调用顺序
 * @param[out] handle: 返回替身句柄
 * @param config: httpd 配置
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    s_httpd.config = *config;
    s_httpd.running = true;
    s_httpd.callOrder.emplace_back("httpd_start");
    *handle = &s_httpd;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t /*handle*/)
{
    if (s_httpd.failStop) {
        return ESP_FAIL;
    }
    s_httpd.running = false;
    s_httpd.uris.clear();
    s_httpd.err404Handler = nullptr;
    // 严格复现 IDF v6.1 httpd_main.c:612-618：global_user_ctx_free_fn 为空则断言失败（真实硬件会 free 静态对象）。
    if (s_httpd.config.global_user_ctx != nullptr) {
        assert(s_httpd.config.global_user_ctx_free_fn != nullptr);
        s_httpd.config.global_user_ctx_free_fn(s_httpd.config.global_user_ctx);
        ++s_httpd.globalCtxFreeCalls;
        s_httpd.config.global_user_ctx = nullptr;
    }
    return ESP_OK;
}

/**
 * @brief 模拟注册 URI 处理器并断言未超出 max_uri_handlers
 * @param handle: httpd 句柄（未使用）
 * @param uri_handler: 待注册的 URI 配置
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t httpd_register_uri_handler(httpd_handle_t /*handle*/, const httpd_uri_t *uri_handler)
{
    assert(s_httpd.uris.size() < s_httpd.config.max_uri_handlers);
    s_httpd.uris.push_back(*uri_handler);
    s_httpd.callOrder.emplace_back("httpd_register_uri");
    return ESP_OK;
}

/**
 * @brief 模拟注册 HTTP 错误处理器
 * @param handle: httpd 句柄（未使用）
 * @param error: HTTP 错误码
 * @param handler_fn: 处理回调
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t httpd_register_err_handler(httpd_handle_t /*handle*/, httpd_err_code_t error,
                                     httpd_err_handler_func_t handler_fn)
{
    if (error == HTTPD_404_NOT_FOUND) {
        s_httpd.err404Handler = handler_fn;
        s_httpd.callOrder.emplace_back("httpd_register_err");
    }
    return ESP_OK;
}

/**
 * @brief 返回 httpd 全局上下文指针
 * @param handle: httpd 句柄（未使用）
 * @retval 全局上下文指针
 * @note 主机测试桩。
 */
void *httpd_get_global_user_ctx(httpd_handle_t /*handle*/)
{
    return s_httpd.config.global_user_ctx;
}

/**
 * @brief 取得请求对应的套接字描述符
 * @param r: HTTP 请求对象
 * @retval 套接字描述符
 * @note 主机测试桩。
 */
int httpd_req_to_sockfd(httpd_req_t *r)
{
    return r->sockfd;
}

#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
/** @brief 模拟本次 HTTP 接收并触发注入事件。 @param buf: 输出数据。 @param len: 最大字节数。 @retval 字节数或 HTTPD_SOCK_ERR_TIMEOUT。 */
int httpd_req_recv(httpd_req_t *, char *buf, size_t len)
{
    ++s_ota.received;
    if (s_ota.body.empty() && s_ota.onReceive != nullptr) {
        const auto event = std::exchange(s_ota.onReceive, nullptr);
        event();
        s_nowMs += static_cast<uint32_t>(s_ota.receiveWaitS * 1000L);
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    const auto part = s_ota.body.first(std::min(len, s_ota.body.size()));
    if (!part.empty()) {
        std::ranges::copy(part, buf);
    }
    s_ota.body = s_ota.body.subspan(part.size());
    return static_cast<int>(part.size());
}

/** @brief 提供测试运行镜像描述符。 @retval 静态描述符。 */
const esp_app_desc_t *esp_app_get_description()
{
    static constexpr esp_app_desc_t kDescription{
        .magic_word = ESP_APP_DESC_MAGIC_WORD,
        .unused_prefix = {},
        .version = "host-test",
        .project_name = "rtk_main",
        .unused_suffix = {},
    };
    return &kDescription;
}

/** @brief 返回测试运行槽。 @retval 第一槽。 */
const esp_partition_t *esp_ota_get_running_partition()
{
    return &kRunningSlot;
}

/** @brief 返回更新槽。 @retval 第二槽。 */
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *)
{
    return &kOtherSlot;
}

/** @brief 模拟运行槽与另一槽状态。 @param part: 查询分区。 @param state: 输出状态。 @retval ESP_OK 已返回，或返回注入错误。 */
esp_err_t esp_ota_get_state_partition(const esp_partition_t *part, esp_ota_img_states_t *state)
{
    if (part == &kOtherSlot) {
        if (s_ota.otherStateErr != ESP_OK) {
            return s_ota.otherStateErr;
        }
        *state = s_ota.otherState;
        return ESP_OK;
    }
    *state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}

/** @brief 模拟另一槽描述符。 @param desc: 输出描述符。 @retval ESP_OK 已读取。 @retval ESP_ERR_NOT_FOUND 模拟无镜像。 */
esp_err_t esp_ota_get_partition_description(const esp_partition_t *, esp_app_desc_t *desc)
{
    if (!s_ota.otherReadable) {
        return ESP_ERR_NOT_FOUND;
    }
    *desc = *esp_app_get_description();
    std::memcpy(desc->version, "6.0", sizeof("6.0"));
    return ESP_OK;
}

/** @brief 验证 OTA 以顺序写入模式开启。 @param partition: 写入槽。 @param size: 开启模式。 @param handle: 输出测试句柄。 @retval ESP_OK 已开启。 */
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t size, esp_ota_handle_t *handle)
{
    assert(partition == &kOtherSlot && size == OTA_WITH_SEQUENTIAL_WRITES);
    *handle = 1U;
    return ESP_OK;
}

/** @brief 记录写入字节数。 @param handle: 测试句柄。 @param size: 数据量。 @retval ESP_OK 已写入。 */
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *, size_t size)
{
    assert(handle == 1U);
    s_ota.written += size;
    return ESP_OK;
}

/** @brief 模拟写入结束。 @param handle: 测试句柄。 @retval ESP_OK 校验通过。 */
esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(handle == 1U);
    return ESP_OK;
}

/** @brief 记录失败清理。 @param handle: 测试句柄。 @retval ESP_OK 已释放。 @retval ESP_ERR_NOT_FOUND 尚未创建。 */
esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    ++s_ota.aborted;
    return handle == 0U ? ESP_ERR_NOT_FOUND : ESP_OK;
}

/** @brief 记下提交的启动槽。 @param partition: 新启动槽。 @retval ESP_OK 已提交。 */
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    s_ota.bootPartition = partition;
    return ESP_OK;
}

/** @brief 模拟实际镜像长度。 @param pos: 声明范围。 @param image: 输出镜像长度。 @retval ESP_OK 读取成功。 */
esp_err_t esp_image_get_metadata(const esp_partition_pos_t *pos, esp_image_metadata_t *image)
{
    assert(pos->size == s_ota.written);
    image->image_len = pos->size;
    return ESP_OK;
}
#endif

/** @brief 取得 HTTP 头替身中的字段。 @param r: HTTP 请求。 @param field: 字段名。 @retval 字段值，缺失时为空指针。 */
[[nodiscard]] static const char *requestHeader(const httpd_req_t &r, std::string_view field)
{
    if (field == "Host") {
        return r.host;
    }
    if (field == "Content-Length") {
        return r.content_length;
    }
    return field == "Content-Type" ? r.content_type : nullptr;
}

/**
 * @brief 读取请求头字段长度
 * @param r: HTTP 请求对象
 * @param field: 请求头字段名
 * @retval 字段值字节长度；未找到返回 0
 * @note 主机测试桩。
 */
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    if ((r == nullptr) || (field == nullptr)) {
        return 0U;
    }
    const char *value = requestHeader(*r, field);
    return value != nullptr ? std::strlen(value) : 0U;
}

/**
 * @brief 读取请求头字段值字符串
 * @param r: HTTP 请求对象
 * @param field: 请求头字段名
 * @param[out] val: 输出缓冲
 * @param val_size: 输出缓冲容量
 * @retval ESP_OK 读取成功
 * @retval ESP_ERR_NOT_FOUND 字段不存在
 * @retval ESP_ERR_INVALID_ARG 参数非法
 * @retval ESP_FAIL 缓冲不足发生截断
 * @note 主机测试桩。
 */
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    if ((r == nullptr) || (field == nullptr) || (val == nullptr) || (val_size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *value = requestHeader(*r, field);
    if (value == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t len = std::strlen(value);
    const size_t copied = std::min(len, val_size - 1U);
    std::memcpy(val, value, copied);
    val[copied] = '\0';
    // 模拟 IDF 6.1 的边界行为，len == val_size 时截断但仍报成功。
    return len > val_size ? ESP_FAIL : ESP_OK;
}

/**
 * @brief 记录 HTTP 响应状态行
 * @param r: HTTP 请求对象（未使用）
 * @param status: 状态字符串
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t httpd_resp_set_status(httpd_req_t * /*r*/, const char *status)
{
    s_httpd.lastRespStatus = status;
    return ESP_OK;
}

/**
 * @brief 记录 HTTP 响应 Content-Type
 * @param r: HTTP 请求对象（未使用）
 * @param type: MIME 类型字符串
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t httpd_resp_set_type(httpd_req_t * /*r*/, const char *type)
{
    s_httpd.lastRespType = type;
    return ESP_OK;
}

/**
 * @brief 记录 HTTP 响应扩展头（Location / Cache-Control）
 * @param r: HTTP 请求对象（未使用）
 * @param field: 头字段名
 * @param value: 头字段值
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t httpd_resp_set_hdr(httpd_req_t * /*r*/, const char *field, const char *value)
{
    if ((field != nullptr) && (value != nullptr)) {
        if (std::strcmp(field, "Location") == 0) {
            s_httpd.lastRespLocation = value;
        } else if (std::strcmp(field, "Cache-Control") == 0) {
            s_httpd.lastRespCacheControl = value;
        }
    }
    return ESP_OK;
}

/**
 * @brief 记录 HTTP 响应体
 * @param r: HTTP 请求对象（未使用）
 * @param buf: 响应体指针
 * @param buf_len: 响应体长度
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t httpd_resp_send(httpd_req_t * /*r*/, const char *buf, ssize_t buf_len)
{
    if ((buf != nullptr) && (buf_len > 0)) {
        s_httpd.lastRespBody.assign(buf, static_cast<size_t>(buf_len));
    } else {
        s_httpd.lastRespBody.clear();
    }
    return ESP_OK;
}

/**
 * @brief 记录 HTTP 错误响应码与消息
 * @param req: HTTP 请求对象（未使用）
 * @param error: HTTP 错误码
 * @param msg: 错误文本
 * @retval ESP_OK 始终成功
 * @note 主机测试桩。
 */
esp_err_t httpd_resp_send_err(httpd_req_t * /*req*/, httpd_err_code_t error, const char *msg)
{
    s_httpd.lastRespErrCode = static_cast<int>(error);
    s_httpd.lastRespBody = (msg != nullptr) ? msg : "";
    return ESP_OK;
}

/**
 * @brief 模拟创建 lwIP UDP 套接字（支持故障注入）
 * @param domain: 地址族（未使用）
 * @param type: 套接字类型（未使用）
 * @param protocol: 协议号（未使用）
 * @retval 42 创建成功
 * @retval -1 注入失败（errno=ENOMEM）
 * @note 主机测试桩。
 */
int lwip_socket(int /*domain*/, int /*type*/, int /*protocol*/)
{
    ++s_httpd.dnsSocketCalls;
    if (s_httpd.failDnsSocket) {
        errno = ENOMEM;
        return -1;
    }
    s_httpd.dnsOpen = true;
    return 42;
}

/**
 * @brief 模拟设置 lwIP 套接字选项
 * @param s: 套接字描述符（未使用）
 * @param level: 选项层级（未使用）
 * @param optname: 选项名（未使用）
 * @param optval: 选项值（未使用）
 * @param optlen: 选项长度（未使用）
 * @retval 0 始终成功
 * @note 主机测试桩。
 */
int lwip_setsockopt(int /*s*/, int /*level*/, int optname, const void *optval, socklen_t optlen)
{
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    if (optname == SO_RCVTIMEO) {
        assert(optlen == sizeof(timeval));
        s_ota.receiveWaitS = static_cast<const timeval *>(optval)->tv_sec;
    }
#else
    static_cast<void>(optname);
    static_cast<void>(optval);
    static_cast<void>(optlen);
#endif
    return 0;
}

#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
/** @brief 返回模拟的接收等待选项。 @param optname: 选项名。 @param optval: 输出值。 @param optlen: 输出容量。 @retval 0 已读取。 */
int lwip_getsockopt(int, int, int optname, void *optval, socklen_t *optlen)
{
    assert(optname == SO_RCVTIMEO && *optlen == sizeof(timeval));
    *static_cast<timeval *>(optval) = {.tv_sec = s_ota.receiveWaitS, .tv_usec = 0};
    return 0;
}

/** @brief 记录原始 HTTP 回包或模拟发送窗口已满。 @param dataptr: 数据。 @param size: 长度。 @param flags: 发送标志。 @retval 完整发送的字节数，或 -1。 */
ssize_t lwip_send(int, const void *dataptr, size_t size, int flags)
{
    assert(flags == MSG_DONTWAIT);
    if (s_ota.backpressure) {
        errno = EWOULDBLOCK;
        return -1;
    }
    s_ota.wireReply.assign(static_cast<const char *>(dataptr), size);
    return static_cast<ssize_t>(size);
}
#endif

/**
 * @brief 模拟设置 lwIP 套接字非阻塞标志（支持故障注入）
 * @param s: 套接字描述符（未使用）
 * @param cmd: fcntl 命令
 * @param val: 标志值
 * @retval 0 成功
 * @retval -1 注入失败（errno=EIO）
 * @note 主机测试桩。
 */
int lwip_fcntl(int /*s*/, int cmd, int val)
{
    if (s_httpd.failDnsFcntl) {
        errno = EIO;
        return -1;
    }
    if (cmd == F_GETFL) {
        return 0;
    }
    if ((cmd == F_SETFL) && ((val & O_NONBLOCK) != 0)) {
        s_httpd.dnsNonBlock = true;
    }
    return 0;
}

/**
 * @brief 模拟绑定 lwIP UDP 端口并记录有序调用点（支持故障注入）
 * @param s: 套接字描述符（未使用）
 * @param name: 绑定地址
 * @param namelen: 地址长度（未使用）
 * @retval 0 成功
 * @retval -1 注入失败（errno=EADDRINUSE）
 * @note 主机测试桩。
 */
int lwip_bind(int /*s*/, const struct sockaddr *name, socklen_t /*namelen*/)
{
    if (s_httpd.failDnsBind) {
        errno = EADDRINUSE;
        return -1;
    }
    const auto *in = reinterpret_cast<const struct sockaddr_in *>(name);
    s_httpd.dnsBoundPort = ntohs(in->sin_port);
    s_httpd.callOrder.emplace_back("dns_bind");
    return 0;
}

/**
 * @brief 模拟非阻塞 lwip_recvmsg（超长时按 lwIP 行为置位 MSG_TRUNC，支持接收错误注入）
 * @param s: 套接字描述符（未使用）
 * @param[inout] message: 消息头（接收对端地址、数据与 msg_flags）
 * @param flags: 接收标志（未使用）
 * @retval >=0 实际拷贝字节数
 * @retval -1 无数据（EAGAIN）或注入错误
 * @note 主机测试桩。
 */
ssize_t lwip_recvmsg(int /*s*/, struct msghdr *message, int /*flags*/)
{
    if (s_httpd.failDnsRecvErrno != 0) {
        errno = s_httpd.failDnsRecvErrno;
        s_httpd.failDnsRecvErrno = 0;
        return -1;
    }
    if (s_httpd.dnsRxQueue.empty()) {
        errno = EAGAIN;
        return -1;
    }
    auto pkt = std::move(s_httpd.dnsRxQueue.front());
    s_httpd.dnsRxQueue.erase(s_httpd.dnsRxQueue.begin());
    message->msg_flags = 0;
    const size_t cap = message->msg_iov[0].iov_len;
    const size_t n = std::min(cap, pkt.size());
    std::memcpy(message->msg_iov[0].iov_base, pkt.data(), n);
    if (pkt.size() > cap) {
        message->msg_flags |= MSG_TRUNC;
    }
    if ((message->msg_name != nullptr) && (message->msg_namelen >= sizeof(struct sockaddr_in))) {
        auto *in = reinterpret_cast<struct sockaddr_in *>(message->msg_name);
        *in = {};
        in->sin_family = AF_INET;
        in->sin_port = htons(53000U);
        in->sin_addr.s_addr = htonl(0xC0A80A02U);
        message->msg_namelen = sizeof(struct sockaddr_in);
    }
    return static_cast<ssize_t>(n);
}

/**
 * @brief 模拟发送 UDP 应答并记录发送载荷
 * @param s: 套接字描述符（未使用）
 * @param dataptr: 发送数据指针
 * @param size: 发送字节数
 * @param flags: 发送标志（未使用）
 * @param to: 目标地址（未使用）
 * @param tolen: 目标地址长度（未使用）
 * @retval 实际发送字节数
 * @note 主机测试桩。
 */
ssize_t lwip_sendto(int /*s*/, const void *dataptr, size_t size, int /*flags*/,
                    const struct sockaddr * /*to*/, socklen_t /*tolen*/)
{
    const auto *bytes = static_cast<const uint8_t *>(dataptr);
    s_httpd.dnsTxSent.emplace_back(bytes, bytes + size);
    return static_cast<ssize_t>(size);
}

/**
 * @brief 模拟关闭 lwIP 套接字（支持单次失败注入）
 * @param s: 套接字描述符（未使用）
 * @retval 0 关闭成功
 * @retval -1 注入失败（errno=EIO）
 * @note 主机测试桩。
 */
int lwip_close(int /*s*/)
{
    ++s_httpd.dnsCloseCalls;
    if (s_httpd.failDnsCloseOnce) {
        s_httpd.failDnsCloseOnce = false;
        errno = EIO;
        return -1;
    }
    s_httpd.dnsOpen = false;
    return 0;
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
/** @brief 提供 U3 高、物理按键松开的输入。 @param gpio: 输入引脚。 @retval 数字输入。 */
int gpio_get_level(gpio_num_t gpio)
{
    return gpio == 2 ? 0 : 1;
}

/** @brief 保存 GPIO4 目标。 @param gpio: 输出引脚。 @param level: 数字输出。 @retval SDK 结果。 */
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    assert(gpio == 4);
    s_powerKeyDriven = level != 0U;
    return ESP_OK;
}

/** @brief 模拟 GPIO 输入配置。 @retval SDK 结果。 */
esp_err_t gpio_config(const gpio_config_t *)
{
    return ESP_OK;
}

/** @brief 此替身未配置电池 ADC，无需等待首组采样。 */
extern "C" void vTaskDelay(TickType_t /*ticks*/) {}

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

/** 以测试线程模拟 kIo，手动推进真实执行器队列。 */
struct ExecutorTestAccess {
    /** @brief 绑定测试线程为执行器所有者。 @param exec: 测试执行器。 @param owner: 测试 TCB。 */
    static void bind(Executor &exec, TaskHandle_t owner) noexcept
    {
        exec.task_ = owner;
        fake_current_task = owner;
    }

    /** @brief 消费本轮排队命令。 @param exec: 测试执行器。 */
    static void drain(Executor &exec) noexcept
    {
        fw::Command cmd{};
        uint32_t count = 0U;
        while (xQueueReceive(exec.cmd_q_, &cmd, 0) == pdTRUE) {
            exec.processCommand(cmd, count);
        }
    }
};
} // namespace rtos

namespace app {

AppContext &appContext() noexcept
{
    return s_ctx;
}

/** @brief 模拟蓝牙静默只允许发生在 BT 模式。 @param paused: 暂停请求。 */
void BtLinkModule::setPowerPaused(bool paused) noexcept
{
    static_cast<void>(paused);
    assert(false);
}

/** @brief 提供未启用蓝牙的设置状态。 @param localOnly: 是否仅查询本执行器。 @retval false 蓝牙未运行。 */
bool BtLinkModule::hasPendingTicket(bool localOnly) const noexcept
{
    static_cast<void>(localOnly);
    return false;
}

/** @brief 提供未启用蓝牙的静默结果。 @retval false 蓝牙未运行。 */
bool BtLinkModule::powerPauseDone() const noexcept
{
    return false;
}

/** @brief 提供无延迟写的存储状态。 @retval false 本用例无存储待写。 */
bool StorageModule::hasPendingWrites() const noexcept
{
    return false;
}

/** @brief 本用例没有需要加急的存储写。 */
void StorageModule::expeditePendingWrites() noexcept {}

/** @brief 填写空的待写摘要。 @param[out] out: 摘要缓冲。 */
void StorageModule::formatPendingSummary(std::span<char> out) const noexcept
{
    assert(!out.empty());
    out.front() = '\0';
}

namespace board {

/** @brief 模拟外设进入安全电平。 @retval kOk 成功。 */
fw::Status setSafeOutputLevels(const Profile &) noexcept
{
    return fw::Status::kOk;
}

/** @brief 此用例不使用电池 ADC。 @retval kNotFound 无模拟传感器。 */
fw::Status initBatteryAdc(int /*gpio*/) noexcept
{
    return fw::Status::kNotFound;
}

/** @brief 阻止在未初始化时读取 ADC。 @retval kInvalidState 无模拟传感器。 */
fw::Status readBatteryAdcMv(uint32_t & /*mv*/) noexcept
{
    return fw::Status::kInvalidState;
}

namespace charger {

/** @brief 模拟在线的充电芯片。 @retval true 始终在线。 */
bool isReady() noexcept
{
    return true;
}

/** @brief 提供 USB 在位的有效样本。 @param[out] status: 样本缓存。 @retval kOk 成功。 */
fw::Status refreshInputStatus(ChargerInputStatus &status) noexcept
{
    status = decodeSgmReg08(kSgmReg08PgBit);
    return fw::Status::kOk;
}

} // namespace charger
} // namespace board

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
    rootReq.host = "192.168.10.12";
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

/** @brief OLED 退出共用 Web bye 排空和 BT 重启，并保留持久化前置检查。 */
void testOledExit()
{
    resetHarness();
    assert(simulateIdfV61WsHandshake(1004) == ESP_OK);
    simulateWifiEvent(WIFI_EVENT_AP_STACONNECTED);
    assert(s_ctx.web.wifiStationCount() == 1U);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    s_httpd.sent.clear();
    s_ctx.data.ggaGatePersist.store(app::data::GgaGatePersist::kFailed, std::memory_order_release);
    assert(!s_ctx.web.requestOledExit(s_nowMs)); // 替身未启动 Io 执行器，不能冲刷。
    s_ctx.data.ggaGatePersist.store(app::data::GgaGatePersist::kClean, std::memory_order_release);
    assert(s_ctx.web.requestOledExit(s_nowMs));
    assert(!s_ctx.web.requestOledExit(s_nowMs));
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.back().payload == R"({"t":"bye","reason":"exit"})");
    assert(s_rebootCalls == 0);
    s_catchReboot = true;
    if (setjmp(s_rebootJmp) == 0) {
        s_nowMs += 1000U;
        static_cast<void>(s_ctx.web.poll(s_nowMs));
        assert(false && "expected OLED exit to switch to Bluetooth");
    }
    s_catchReboot = false;
    assert(s_rebootCalls == 1 && s_lastRadioMode == app::RadioMode::kBluetooth);
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/** @brief 关机屏蔽 Web exit，HTTP 任务静默 SoftAP 并在关机回退后恢复。 */
void testShutdownArbitrationAndSoftApResume()
{
    using namespace app;
    resetHarness();
    assert(simulateIdfV61WsHandshake(1005) == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    auto &requests = power::s_power_requests;
    s_httpd.sent.clear();

    assert(simulateWsFrame(1005, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"cmd","id":30,"name":"exit"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.empty());
    s_ctx.ioExecutor = nullptr; // 本场景没有启动 GNSS 执行器。
    s_ctx.power.bindContext(s_ctx);
    assert(fw::isOk(s_ctx.power.init()));
    assert(power::requestShutdown(board::ShutdownReason::kKey));
    s_nowMs += 10U;
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_rebootCalls == 0 && !s_httpd.sent.empty());
    assert(s_httpd.sent.front().payload == R"({"t":"ack","id":30,"ok":false,"err":"busy"})");

    assert(simulateWsFrame(1005, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"cmd","id":31,"name":"exit"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.back().payload == R"({"t":"ack","id":31,"ok":false,"err":"busy"})");

    s_httpd.failQueueWorkOnce = true;
    for (unsigned i = 0U; i < 3U; ++i) {
        s_nowMs += 10U;
        static_cast<void>(s_ctx.power.poll(s_nowMs));
    }
    assert(s_ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kQuiesce);
    assert(!s_ctx.web.powerPauseDone());
    assert(s_httpd.wifiMode == WIFI_MODE_AP);
    s_nowMs += 100U;
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    assert(!s_ctx.web.powerPauseDone());
    runPendingWork();
    assert(s_ctx.web.powerPauseDone());
    assert(s_httpd.wifiMode == WIFI_MODE_NULL && s_httpd.running && s_httpd.wifiStopCalls == 0);

    static_cast<void>(s_ctx.power.poll(s_nowMs));
    assert(s_ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kCutPower);
    assert(s_powerKeyDriven);
    s_nowMs += board::kShutdownCutTimeoutMs;
    static_cast<void>(s_ctx.power.poll(s_nowMs));
    assert(!s_powerKeyDriven && requests.canScheduleRestart());
    runPendingWork();
    assert(!s_ctx.web.powerPauseDone());
    assert(s_httpd.wifiMode == WIFI_MODE_AP && s_httpd.running);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_rebootCalls == 0);
    assert(std::ranges::none_of(s_httpd.sent, [](const auto &frame) {
        return frame.payload == R"({"t":"bye","reason":"exit"})";
    }));
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/** @brief 验证初次同步后只发送变化版本的能力帧。 */
void testCapsUpdates()
{
    resetHarness();
    s_ctx.data.gnssCaps.write({});
    assert(simulateIdfV61WsHandshake(1005) == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent[0].payload.find(R"("gnss.out.gsv")") == std::string::npos);

    settings::GnssCaps caps{.valid = true, .enabled = true};
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGsv)] = {{0, 1000, 5000}, 3U, true};
    s_ctx.data.gnssCaps.write(caps);
    s_httpd.sent.clear();
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.size() == 1U);
    const auto &payload = s_httpd.sent[0].payload;
    assert(payload.starts_with(R"({"t":"caps","caps":{)"));
    assert(payload.find(R"("gnss.out.gsv":[0,1000,5000])") != std::string::npos);

    s_httpd.sent.clear();
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.empty());
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/** @brief 验证真实 SettingsBackend 下 gnss.cutoff 的 get/hello/config/set 端到端链路（NRU1-01）。 */
void testGnssCutoffRealBackend()
{
    resetHarness();
    const settings::KeySpec *const cutoffSpec = settings::findKey("gnss.cutoff");
    assert(cutoffSpec != nullptr);

    // 能力未就绪前，真实 SettingsBackend::getSetting 必须拒绝读取 gnss.cutoff。
    s_ctx.data.gnssCaps.write({});
    settings::Value val{};
    assert(!s_ctx.settings.get(cutoffSpec->id, val));

    // 挂接真实 rtos::Executor 接收 SettingsBackend 派发的 kVerbGnssSetSatTrack 1 字节截止角补丁。
    struct DummyGnss {
        fw::Command lastCmd{};
        unsigned calls = 0U;
    } dummyGnss{};
    rtos::Executor ioExec{};
    app::ExecutorControlStorage<4U> control{};
    std::array<StackType_t, 256U> stack{};
    const auto execId = app::id(app::ExecKind::kIo);
    const auto target = app::id(app::ModuleKind::kGnss);
    fw::ModuleOps ops{};
    ops.onCommand = [](void *self, const fw::Command &cmd, fw::CommandResult &out) noexcept {
        auto *state = static_cast<DummyGnss *>(self);
        state->lastCmd = cmd;
        ++state->calls;
        if ((cmd.verb == static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack)) &&
            (cmd.argLen == 1U)) {
            auto cfg = s_ctx.data.gnssSatTrack.read().value_or(core::SatTrackConfig::defaults());
            cfg.cutoffDeg = cmd.args[0];
            s_ctx.data.gnssSatTrack.write(cfg);
            s_ctx.data.configRev.fetch_add(1U, std::memory_order_release);
            out.status = fw::Result::kOk;
            return;
        }
        out.status = fw::Result::kInvalidArg;
    };
    const std::array modules{fw::Module{
        .name = "gnss", .id = target, .executor = execId, .self = &dummyGnss, .ops = &ops}};
    assert(fw::isOk(ioExec.init({.name = "io", .id = execId, .priority = 9U}, modules,
                                app::viewOf(control, stack))));
    s_ctx.ioExecutor = &ioExec;
    rtos::ExecutorTestAccess::bind(ioExec, &control.tcb);

    // 填充 F9P 能力与当前快照（cutoff=12°）。
    settings::GnssCaps caps{.valid = true, .enabled = true};
    caps.constMask = static_cast<uint8_t>(settings::const_bit::kBds | settings::const_bit::kGlo |
                                          settings::const_bit::kGal | (1U << 7U));
    s_ctx.data.gnssCaps.write(caps);
    core::SatTrackConfig initSat = core::SatTrackConfig::defaults();
    initSat.cutoffDeg = 12U;
    s_ctx.data.gnssSatTrack.write(initSat);

    // 1. 直接经真实 SettingsBackend 读取 gnss.cutoff。
    assert(s_ctx.settings.get(cutoffSpec->id, val));
    assert((val.kind == settings::ValueKind::kInt) && (val.number == 12));

    // 2. WS 握手后 hello.caps 与初始 config 帧必须同时携带 gnss.cutoff。
    assert(simulateIdfV61WsHandshake(1006) == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(s_httpd.sent.size() >= 2U);
    assert(s_httpd.sent[0].payload.find(R"("gnss.cutoff":{"min":0,"max":90})") !=
           std::string::npos);
    assert(s_httpd.sent[1].payload.find(R"("gnss.cutoff":12)") != std::string::npos);

    // 3. 经 WS set 修改 gnss.cutoff=25，走真实 SettingsBackend 派发、更新并回推 config。
    s_httpd.sent.clear();
    assert(simulateWsFrame(1006, HTTPD_WS_TYPE_TEXT, true,
                           R"({"t":"set","id":42,"key":"gnss.cutoff","value":"25"})") == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    rtos::ExecutorTestAccess::drain(ioExec);
    static_cast<void>(s_ctx.web.poll(s_nowMs + 10U));
    runPendingWork();
    assert((dummyGnss.calls == 1U) && (dummyGnss.lastCmd.argLen == 1U) &&
           (dummyGnss.lastCmd.args[0] == 25U));
    assert(s_ctx.settings.get(cutoffSpec->id, val) && (val.number == 25));
    bool sawAckOk = false;
    bool sawUpdatedConfig = false;
    for (const auto &frame : s_httpd.sent) {
        if (frame.payload == R"({"t":"ack","id":42,"ok":true})") {
            sawAckOk = true;
        }
        if (frame.payload.find(R"("gnss.cutoff":25)") != std::string::npos) {
            sawUpdatedConfig = true;
        }
    }
    assert(sawAckOk && sawUpdatedConfig);

    rtos::ExecutorTestAccess::bind(ioExec, nullptr);
    s_ctx.ioExecutor = &s_ioExec;
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/**
 * @brief 6. 验证强制门户：启动顺序（F1）、DHCP 114（F10）、探测 302、404（F7）、Host（F6）、DNS 降级/截断/重连（F2/F3/F5）与 Kconfig 开关
 * @note 分别在 CONFIG_RTK_WIFI_CAPTIVE_PORTAL=1 与 =0 两种编译下验证对应行为。
 */
void testCaptivePortal()
{
    resetHarness();

#if defined(CONFIG_RTK_WIFI_CAPTIVE_PORTAL) && CONFIG_RTK_WIFI_CAPTIVE_PORTAL
    // 1. F1/F10：验证有序调用——dhcps_stop < option_114 < dhcps_start < httpd_start < 全部注册 < dns_bind < wifi_start。
    const auto findCallIdx = [](std::string_view tag, bool last) -> size_t {
        size_t found = SIZE_MAX;
        for (size_t i = 0U; i < s_httpd.callOrder.size(); ++i) {
            if (s_httpd.callOrder[i] == tag) {
                found = i;
                if (!last) {
                    return found;
                }
            }
        }
        return found;
    };
    const size_t idxStop = findCallIdx("dhcps_stop", false);
    const size_t idxOpt114 = findCallIdx("dhcps_option_114", false);
    const size_t idxStart = findCallIdx("dhcps_start", false);
    const size_t idxHttpd = findCallIdx("httpd_start", false);
    const size_t idxLastUri = findCallIdx("httpd_register_uri", true);
    const size_t idxErr404 = findCallIdx("httpd_register_err", false);
    const size_t idxDnsBind = findCallIdx("dns_bind", false);
    const size_t idxWifiStart = findCallIdx("wifi_start", false);
    assert((idxStop < idxOpt114) && (idxOpt114 < idxStart) && (idxStart < idxHttpd));
    assert((idxHttpd < idxLastUri) && (idxLastUri < idxErr404) && (idxErr404 < idxDnsBind));
    assert((idxDnsBind < idxWifiStart) && (idxWifiStart == (s_httpd.callOrder.size() - 1U)));

    // 读回静态 URI 指针，确认 init 返回后仍指向 NUL 结尾的 "http://192.168.10.12/"。
    assert(s_httpd.dhcpsOptionCalls == 1);
    assert(s_httpd.dhcpsOptionSetWhileStopped);
    assert(s_httpd.captivePortalUri == "http://192.168.10.12/");
    assert(s_httpd.captivePortalUriPtr != nullptr);
    assert(std::string_view{static_cast<const char *>(s_httpd.captivePortalUriPtr)} ==
           "http://192.168.10.12/");

    // 2. 3 基础 + 2 OTA + 7 探测，另留 2 个余量。
    assert(s_httpd.uris.size() == 12U);
    assert(s_httpd.config.max_uri_handlers == 14U);
    assert(s_httpd.err404Handler != nullptr);

    // 3. 全部 7 个系统探测路径均回 302 Found + Location + Cache-Control: no-store + 空体。
    const std::array<const char *, 7U> probePaths = {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/connecttest.txt",
        "/ncsi.txt",
        "/redirect",
    };
    for (const char *path : probePaths) {
        const auto *u = findUri(path);
        assert(u != nullptr);
        s_httpd.lastRespStatus.clear();
        s_httpd.lastRespLocation.clear();
        s_httpd.lastRespCacheControl.clear();
        s_httpd.lastRespBody = "dirty";
        httpd_req_t req{};
        req.method = HTTP_GET;
        req.uri = path;
        assert(u->handler(&req) == ESP_OK);
        assert(s_httpd.lastRespStatus == "302 Found");
        assert(s_httpd.lastRespLocation == "http://192.168.10.12/");
        assert(s_httpd.lastRespCacheControl == "no-store");
        assert(s_httpd.lastRespBody.empty());
    }

    // 4. F7：404 错误处理器：未匹配 GET 302 到根页；非 GET 发 404 并返回 ESP_FAIL 保持关闭连接语义。
    {
        s_httpd.lastRespStatus.clear();
        s_httpd.lastRespLocation.clear();
        s_httpd.lastRespCacheControl.clear();
        s_httpd.lastRespBody = "dirty";
        httpd_req_t getReq{};
        getReq.method = HTTP_GET;
        getReq.uri = "/anything";
        assert(s_httpd.err404Handler(&getReq, HTTPD_404_NOT_FOUND) == ESP_OK);
        assert(s_httpd.lastRespStatus == "302 Found");
        assert(s_httpd.lastRespLocation == "http://192.168.10.12/");
        assert(s_httpd.lastRespCacheControl == "no-store");
        assert(s_httpd.lastRespBody.empty());

        s_httpd.lastRespErrCode = -1;
        httpd_req_t postReq{};
        postReq.method = HTTP_POST;
        postReq.uri = "/anything";
        assert(s_httpd.err404Handler(&postReq, HTTPD_404_NOT_FOUND) == ESP_FAIL);
        assert(s_httpd.lastRespErrCode == static_cast<int>(HTTPD_404_NOT_FOUND));
    }

    // 5. F6：GET / Host 校验：缺失或非规范 Host 均 302；192.168.10.12 与 192.168.10.12:80 正常返回页面。
    const auto *root = findUri("/");
    assert(root != nullptr);
    for (const char *badHost : std::array<const char *, 4U>{
             nullptr, "captive.apple.com", "192.168.10.13", "192.168.10.12:8080"}) {
        s_httpd.lastRespStatus.clear();
        s_httpd.lastRespLocation.clear();
        s_httpd.lastRespCacheControl.clear();
        s_httpd.lastRespBody = "dirty";
        httpd_req_t req{};
        req.method = HTTP_GET;
        req.uri = "/";
        req.host = badHost;
        assert(root->handler(&req) == ESP_OK);
        assert(s_httpd.lastRespStatus == "302 Found");
        assert(s_httpd.lastRespLocation == "http://192.168.10.12/");
        assert(s_httpd.lastRespCacheControl == "no-store");
        assert(s_httpd.lastRespBody.empty());
    }
    for (const char *goodHost : std::array<const char *, 2U>{"192.168.10.12", "192.168.10.12:80"}) {
        s_httpd.lastRespBody.clear();
        httpd_req_t goodReq{};
        goodReq.method = HTTP_GET;
        goodReq.uri = "/";
        goodReq.host = goodHost;
        assert(root->handler(&goodReq) == ESP_OK);
        assert(s_httpd.lastRespBody == "<gz-ui>");
    }

    // 6. F3：DNS 套接字：UDP 53 非阻塞，每轮最多 4 包；512 B 合法 EDNS0 接受，513 B（MSG_TRUNC）丢弃且计入 4 包额度。
    assert(s_httpd.dnsSocketCalls == 1 && s_httpd.dnsOpen && s_httpd.dnsNonBlock &&
           s_httpd.dnsBoundPort == 53U);
    const std::vector<uint8_t> aQ = {
        0x11U, 0x22U, 0x01U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x01U, 'a',   0x03U, 'c',   'o',   'm',   0x00U, 0x00U, 0x01U, 0x00U, 0x01U,
    };
    std::vector<uint8_t> aaaaQ = aQ;
    aaaaQ[20] = 28U; // AAAA
    // 恰好 512 B 的合法 EDNS0 查询（23 B 头+问句 + 11 B OPT 头 + 478 B OPT RDATA = 512 B），应答为 23+16+11=50 B。
    std::vector<uint8_t> pkt512 = aQ;
    pkt512[11] = 1U;
    pkt512.insert(pkt512.end(),
                  {0U, 0U, 41U, 0x02U, 0x00U, 0U, 0U, 0U, 0U, 0x01U, 0xDEU}); // RDLENGTH=478
    pkt512.insert(pkt512.end(), 478U, 0x5AU);
    assert(pkt512.size() == 512U);
    // 513 B 超长报文：lwip_recvmsg 置 MSG_TRUNC，接收层丢弃但仍消耗 1 个轮询包额度。
    std::vector<uint8_t> pkt513 = pkt512;
    pkt513.push_back(0xFFU);
    assert(pkt513.size() == 513U);

    s_httpd.dnsRxQueue = {aQ, aaaaQ, pkt513, pkt512, aQ};
    // 第 1 轮 poll 消费前 4 包（发 3 条应答：aQ、aaaaQ、pkt512；pkt513 截断丢弃），打满 4 包返回 true。
    assert(s_ctx.web.poll(s_nowMs));
    assert(s_httpd.dnsRxQueue.size() == 1U);
    assert(s_httpd.dnsTxSent.size() == 3U);
    assert(s_httpd.dnsTxSent[0].size() == (aQ.size() + 16U));
    assert(s_httpd.dnsTxSent[1].size() == aaaaQ.size());
    assert(s_httpd.dnsTxSent[2].size() == (aQ.size() + 16U + 11U));
    // 第 2 轮 poll 消费剩余第 5 包后返回 false。
    assert(!s_ctx.web.poll(s_nowMs));
    assert(s_httpd.dnsRxQueue.empty());
    assert(s_httpd.dnsTxSent.size() == 4U);

    // 7. F5：接收非 EAGAIN 错误关闭 DNS 套接字，1000 ms 内不重试，满 1000 ms 自动重开并恢复服务。
    s_httpd.failDnsRecvErrno = EIO;
    assert(!s_ctx.web.poll(s_nowMs));
    assert(!s_httpd.dnsOpen && (s_httpd.dnsCloseCalls == 1));
    const int callsAfterClose = s_httpd.dnsSocketCalls;
    s_nowMs += 999U;
    assert(!s_ctx.web.poll(s_nowMs));
    assert(!s_httpd.dnsOpen && (s_httpd.dnsSocketCalls == callsAfterClose));
    s_nowMs += 1U;
    assert(!s_ctx.web.poll(s_nowMs));
    assert(s_httpd.dnsOpen && (s_httpd.dnsSocketCalls == (callsAfterClose + 1)));

    // 8. F5：httpd_stop 失败时仍关闭 DNS 套接字；lwip_close 失败一次保留状态，再次停机关闭成功并可重新 init。
    s_httpd.failStop = true;
    s_httpd.failDnsCloseOnce = true;
    s_ctx.web.stopBoot();
    assert(s_httpd.dnsOpen); // close 失败保留句柄状态。
    s_ctx.web.stopBoot();
    assert(!s_httpd.dnsOpen); // 即使 httpd_stop 仍失败，DNS 套接字已关闭。
    s_httpd.failStop = false;
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();

    // 重新 bindStorage + init 验证关停后可再次完整启动。
    bindWebStorage();
    assert(fw::isOk(s_ctx.web.init()));
    assert(s_httpd.dnsOpen);
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();

    // 9. F2：startDns 在 socket / fcntl / bind 任一环节失败时仅降级 DNS，不拆掉 Wi-Fi/httpd/302/option 114。
    for (int mode = 0; mode < 3; ++mode) {
        app::power::s_power_requests.releaseAction();
        s_httpd = MockHttpdState{};
        s_httpd.failDnsSocket = (mode == 0);
        s_httpd.failDnsFcntl = (mode == 1);
        s_httpd.failDnsBind = (mode == 2);
        bindWebStorage();
        assert(fw::isOk(s_ctx.web.init()));
        assert(!s_httpd.dnsOpen);
        assert(s_httpd.running && (s_httpd.dhcpsOptionCalls == 1) &&
               (findUri("/generate_204") != nullptr));
        s_ctx.web.stopBoot();
        s_ctx.web.unbindStorage();
    }
#else
    // Kconfig 关闭门户：不设 option 114、不开 DNS、不注册探测路径与 404 处理器（未知路径回默认 404）。
    assert(s_httpd.dhcpsOptionCalls == 0);
    assert((s_httpd.dnsSocketCalls == 0) && !s_httpd.dnsOpen);
    assert(s_httpd.uris.size() == 3U);
    assert(s_httpd.config.max_uri_handlers == 5U);
    assert(findUri("/ota") == nullptr && findUri("/ota/info") == nullptr);
    assert(s_httpd.err404Handler == nullptr);
    assert(findUri("/generate_204") == nullptr);

    // 关闭门户时 GET / 不做 Host 重定向。
    const auto *root = findUri("/");
    assert(root != nullptr);
    httpd_req_t req{};
    req.method = HTTP_GET;
    req.uri = "/";
    req.host = nullptr;
    assert(root->handler(&req) == ESP_OK);
    assert(s_httpd.lastRespBody == "<gz-ui>");

    s_ctx.web.stopBoot();
    assert(s_httpd.dnsCloseCalls == 0);
    s_ctx.web.unbindStorage();
#endif
}

#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
/** @brief 通过注册路由发起测试 POST。 @param contentType: 原始媒体类型头。 @retval HTTP 回调返回值。 */
[[nodiscard]] esp_err_t postOta(const char *contentType = "application/octet-stream")
{
    const auto *uri = findUri("/ota");
    assert(uri != nullptr && uri->method == HTTP_POST);
    httpd_req_t req{};
    req.handle = &s_httpd;
    req.user_ctx = uri->user_ctx;
    req.sockfd = 1001;
    req.method = HTTP_POST;
    req.content_type = contentType;
    req.content_length = "289";
    req.content_len = app::web_ota::kHeaderBytes + 1U;
    return uri->handler(&req);
}

/** @brief 生成仅供主机替身读取的镜像头及一个正文数据字节。 @retval 测试字节数组。 */
[[nodiscard]] auto otaImage()
{
    std::array<char, app::web_ota::kHeaderBytes + 1U> image{};
    image.front() = static_cast<char>(app::web_ota::kImageMagic);
    const auto offset = app::web_ota::kAppDescOffset;
    const std::array magic{'\x32', '\x54', static_cast<char>(0xcd), static_cast<char>(0xab)};
    std::ranges::copy(magic, std::span{image}.subspan(offset).begin());
    std::memcpy(std::span{image}.subspan(app::web_ota::kVersionOffset).data(), "6.2", 3U);
    std::memcpy(std::span{image}.subspan(app::web_ota::kProjectOffset).data(), "rtk_main", 8U);
    return image;
}

/** @brief 验证 GET 字节协议及 POST 媒体类型边界。 */
void testOtaHttpContract()
{
    resetHarness();
    const auto *uri = findUri("/ota/info");
    assert(uri != nullptr && uri->method == HTTP_GET);
    httpd_req_t req{};
    req.handle = &s_httpd;
    req.user_ctx = uri->user_ctx;
    assert(uri->handler(&req) == ESP_OK);
    assert(s_httpd.lastRespStatus == "200 OK" && s_httpd.lastRespType == "application/json");
    assert(
        s_httpd.lastRespBody ==
        R"({"running":"host-test","project":"rtk_main","part":"ota_0","state":"valid","other":"6.0","max":1966080,"busy":false})");
    s_ota.otherReadable = false;
    assert(uri->handler(&req) == ESP_OK);
    assert(s_httpd.lastRespBody.find(R"("other":null)") != std::string::npos);

    assert(postOta("application/octet-streamX") == ESP_FAIL);
    assert(s_ota.received == 0 && s_ota.wireReply.starts_with("HTTP/1.1 400 Bad Request\r\n"));
    assert(s_ota.wireReply.ends_with("\r\n\r\n"
                                     R"({"ok":false,"err":"bad_image"})"));

    // 空正文在进入接收后模拟 EOF，区分媒体类型是否已获准。
    simulateWifiEvent(WIFI_EVENT_AP_STACONNECTED);
    assert(postOta("\tAPPLICATION/Octet-Stream \t; charset=binary") == ESP_FAIL);
    assert(s_ota.received == 1);
    assert(s_ota.wireReply ==
           "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n"
           "Content-Length: 28\r\nConnection: close\r\n\r\n"
           R"({"ok":false,"err":"aborted"})");
    assert(postOta("application/octet-stream \t") == ESP_FAIL);
    assert(s_ota.received == 2);
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/** @brief 验证 OTA 忙状态暂停空闲回落、拒绝 WS 变更，关机等待清理。 */
void testOtaQuiesceAndWebBusy()
{
    resetHarness();
    assert(simulateIdfV61WsHandshake(1004) == ESP_OK);
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    s_httpd.sent.clear();

    const auto image = otaImage();
    s_ota.body = std::span{image}.first(app::web_ota::kHeaderBytes);
    s_ota.onReceive = [] {
        assert(app::ota::uploadBusy() && s_ota.written == app::web_ota::kHeaderBytes);
        s_nowMs += 300000U; // 无站点且越过 idle 门槛。
        assert(simulateWsFrame(1004, HTTPD_WS_TYPE_TEXT, true,
                               R"({"t":"set","id":51,"key":"gnss.rate","value":"5"})") == ESP_OK);
        assert(simulateWsFrame(1004, HTTPD_WS_TYPE_TEXT, true,
                               R"({"t":"cmd","id":52,"name":"exit"})") == ESP_OK);
        static_cast<void>(s_ctx.web.poll(s_nowMs));
        static_cast<void>(s_ctx.web.poll(s_nowMs));
        runPendingWork();
    };
    assert(postOta() == ESP_FAIL);
    for (const auto &sent : s_httpd.sent) {
        assert(sent.payload != R"({"t":"bye","reason":"idle"})");
    }
    assert(std::ranges::any_of(s_httpd.sent, [](const auto &sent) {
        return sent.payload == R"({"t":"ack","id":51,"ok":false,"err":"busy"})";
    }));
    assert(std::ranges::any_of(s_httpd.sent, [](const auto &sent) {
        return sent.payload == R"({"t":"ack","id":52,"ok":false,"err":"busy"})";
    }));
    // 消费失败回执，确认 idle 从本轮起重新计时。
    s_httpd.sent.clear();
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(std::ranges::none_of(s_httpd.sent, [](const auto &sent) {
        return sent.payload == R"({"t":"bye","reason":"idle"})";
    }));

    simulateWifiEvent(WIFI_EVENT_AP_STACONNECTED);
    s_ota = {};
    s_ota.body = std::span{image}.first(app::web_ota::kHeaderBytes);
    s_ota.onReceive = [] {
        assert(app::ota::uploadBusy() && s_ota.receiveWaitS == 1);
        assert(app::power::s_power_requests.requestShutdown(app::board::BootMode::kNormal,
                                                            app::board::ShutdownReason::kKey));
        s_ctx.web.setPowerPaused(true);
        assert(!s_ctx.web.powerPauseDone() && s_httpd.wifiMode == WIFI_MODE_AP);
    };
    s_ota.backpressure = true;
    const auto before = s_nowMs;
    assert(postOta() == ESP_ERR_HTTPD_RESP_SEND);
    assert(s_nowMs - before == 1000U && s_ota.aborted == 1 && s_ota.bootPartition == nullptr &&
           s_ota.wireReply.empty() && !app::ota::uploadBusy() && !s_ctx.web.powerPauseDone());
    runPendingWork();
    assert(s_ctx.web.powerPauseDone() && s_httpd.wifiMode == WIFI_MODE_NULL);
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();

    resetHarness();
    simulateWifiEvent(WIFI_EVENT_AP_STACONNECTED);
    s_ota.onReceive = [] { simulateWifiEvent(WIFI_EVENT_AP_STADISCONNECTED); };
    s_ota.backpressure = true;
    assert(postOta() == ESP_FAIL && s_ota.aborted == 1 && s_ota.wireReply.empty());
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();

    // 若上次的 OTA idle 标志残留，首轮 poll 会覆盖新服务的初始计时。
    resetHarness();
    assert(simulateIdfV61WsHandshake(1004) == ESP_OK);
    s_nowMs += 300000U;
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    runPendingWork();
    assert(std::ranges::any_of(s_httpd.sent, [](const auto &sent) {
        return sent.payload == R"({"t":"bye","reason":"idle"})";
    }));
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/** @brief 验证 OTA 成功后 1500 ms 直接重启，以及服务重启清除待处理标志。 */
void testOtaReboot()
{
    resetHarness();
    const auto image = otaImage();
    s_ota.body = image;
    assert(postOta() == ESP_OK && s_ota.bootPartition == &kOtherSlot && s_ota.receiveWaitS == 5);
    assert(s_httpd.lastRespBody == R"({"ok":true,"ver":"6.2","reboot_ms":1500})");
    assert(app::ota::uploadBusy());
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    s_nowMs += 1499U;
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    assert(s_rebootCalls == 0);
    s_catchReboot = true;
    if (setjmp(s_rebootJmp) == 0) {
        ++s_nowMs;
        static_cast<void>(s_ctx.web.poll(s_nowMs));
        assert(false && "expected esp_restart");
    }
    s_catchReboot = false;
    assert(s_rebootCalls == 1 && s_lastWasRestart);
    app::ota::endOperation();
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();

    resetHarness();
    s_ota.body = image;
    assert(postOta() == ESP_OK);
    // 停止时尚未由 kLink 消费 httpd 的重启请求。
    app::ota::endOperation();
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
    resetHarness();
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    s_nowMs += 1500U;
    static_cast<void>(s_ctx.web.poll(s_nowMs));
    assert(s_rebootCalls == 0);
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}

/** @brief 验证 F-OTHER 另一槽版本抑制规则与 F-CONC 150 s 总时限清理。 */
void testOtaOtherSlotAndTotalTimeout()
{
    resetHarness();
    const auto *uri = findUri("/ota/info");
    assert(uri != nullptr);
    httpd_req_t req{};
    req.handle = &s_httpd;
    req.user_ctx = uri->user_ctx;
    esp_app_desc_t desc{};

    // 1. 另一槽状态为 UNDEFINED 时，即使描述符可读也抑制版本显示。
    s_ota.otherReadable = true;
    s_ota.otherState = ESP_OTA_IMG_UNDEFINED;
    assert(!app::ota::otherSlotVersion(&kOtherSlot, desc).has_value());
    assert(uri->handler(&req) == ESP_OK);
    assert(s_httpd.lastRespBody.find(R"("other":null)") != std::string::npos);

    // 2. 另一槽状态查询失败时同样抑制版本显示。
    s_ota.otherState = ESP_OTA_IMG_VALID;
    s_ota.otherStateErr = ESP_ERR_NOT_FOUND;
    assert(!app::ota::otherSlotVersion(&kOtherSlot, desc).has_value());
    assert(uri->handler(&req) == ESP_OK);
    assert(s_httpd.lastRespBody.find(R"("other":null)") != std::string::npos);
    s_ota.otherStateErr = ESP_OK;

    // 3. 另一槽有效且干净时正常报告版本。
    assert(app::ota::otherSlotVersion(&kOtherSlot, desc) == std::string_view{"6.0"});
    assert(uri->handler(&req) == ESP_OK);
    assert(s_httpd.lastRespBody.find(R"("other":"6.0")") != std::string::npos);

    // 4. 上传进入 esp_ota_begin 后中止：即使另一槽 otadata 仍为 VALID 且头可读，RAM 脏标志仍抑制版本。
    const auto image = otaImage();
    s_ota.body = std::span{image}.first(app::web_ota::kHeaderBytes);
    assert(postOta() == ESP_FAIL && s_ota.aborted == 1 && !app::ota::uploadBusy());
    assert(!app::ota::otherSlotVersion(&kOtherSlot, desc).has_value());
    assert(uri->handler(&req) == ESP_OK);
    assert(s_httpd.lastRespBody.find(R"("other":null)") != std::string::npos);

    // 5. F-CONC：正文阶段总耗时达到 150 s 返回 408 timeout 并清理句柄与忙标志。
    s_ota.aborted = 0;
    s_ota.written = 0U;
    s_ota.wireReply.clear();
    s_ota.body = std::span{image}.first(app::web_ota::kHeaderBytes);
    s_ota.onReceive = [] { s_nowMs += app::web_ota::kTotalTimeoutMs; };
    assert(postOta() == ESP_FAIL && s_ota.aborted == 1 && !app::ota::uploadBusy());
    assert(s_ota.wireReply.starts_with("HTTP/1.1 408 Request Timeout\r\n"));
    assert(s_ota.wireReply.ends_with("\r\n\r\n"
                                     R"({"ok":false,"err":"timeout"})"));
    assert(std::string_view{app::ota::lastUpload()} == "timeout");
    assert(!app::ota::otherSlotVersion(&kOtherSlot, desc).has_value());

    // 6. 后续上传成功提交后清除脏标志，恢复另一槽版本报告。
    s_ota.body = image;
    s_ota.written = 0U;
    assert(postOta() == ESP_OK && s_ota.bootPartition == &kOtherSlot);
    assert(app::ota::otherSlotVersion(&kOtherSlot, desc) == std::string_view{"6.0"});
    assert(uri->handler(&req) == ESP_OK);
    assert(s_httpd.lastRespBody.find(R"("other":"6.0")") != std::string::npos);

    app::ota::endOperation();
    s_ctx.web.stopBoot();
    s_ctx.web.unbindStorage();
}
#endif

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
    testOledExit();
    testShutdownArbitrationAndSoftApResume();
    testCapsUpdates();
    testGnssCutoffRealBackend();
    testCaptivePortal();
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    testOtaHttpContract();
    testOtaQuiesceAndWebBusy();
    testOtaReboot();
    testOtaOtherSlotAndTotalTimeout();
#endif
    std::puts("PASS web_module_check");
    return 0;
}
