// HIL 专用经典 SPP 对端：不驱动功放/板级 GPIO，所有 SDK 投递由单任务执行。
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spp_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "reconnect_bridge.h"
#include "dev_proto_alarm.h"
#include "dev_proto_frame.h"

#define PEER_QUEUE              24U
#define PEER_STACK              6144U
#define PEER_LINE               640U
#define PEER_POLL_MS            20U
#define PEER_HEARTBEAT_MS       1000U
#define PEER_NVS_NS             "peer"
#define PEER_NVS_KEY            "target"
#define PEER_CONNECT_PREFIX_LEN 8U
#define PEER_QUERY_PREFIX_LEN   6U
/* 复位后首连推迟：RTK 端 LSTO 为 5 s（supv_to 8000 × 0.625 ms），旧 ACL 未释放前发起的连接
 * 会卡约 20 s 才以 HCI 0x08 失败（J5 §3）。推迟到 LSTO 之后首连即可成功。 */
#define PEER_BOOT_HOLDOFF_MS 5500U
/* Page 超时 4096 × 0.625 ms = 2.56 s（默认 5.12 s），对方不在时更快失败进入退避。 */
#define PEER_PAGE_TIMEOUT_SLOTS 4096U

static const char *s_tag = "hil_peer";
typedef enum { EV_INIT, EV_DISC, EV_OPEN, EV_CLOSE, EV_DATA, EV_PIN, EV_CONFIRM } peer_event_type_t;
typedef struct {
    peer_event_type_t type;
    uint32_t handle;
    int status;
    size_t length;
    uint8_t data[DEV_PROTO_MAX_FRAME];
} peer_event_t;
static StaticQueue_t s_queue_control;
static uint8_t s_queue_storage[PEER_QUEUE * sizeof(peer_event_t)];
static QueueHandle_t s_queue;
static StaticTask_t s_task_control;
static StackType_t s_task_stack[PEER_STACK / sizeof(StackType_t)];
static uint32_t s_handle;
static bool s_spp_ready;
static bool s_pending_autostart;
static bool s_alarm;
static bool s_heartbeat = true;
static uint32_t s_last_heartbeat;
static uint16_t s_sequence;
static dev_proto_parser_t s_parser;
static uint32_t s_frames;
static uint32_t s_pushes;
static uint32_t s_replies;
static uint32_t s_errors;
static uint32_t s_last_push;

static uint32_t now_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000U);
}

static bool checked(esp_err_t err, const char *operation)
{
    if (err == ESP_OK) {
        return true;
    }
    ESP_LOGE(s_tag, "%s: %s", operation, esp_err_to_name(err));
    return false;
}

// BTC 上下文：只做非阻塞复制投递，不在回调调用可能阻塞的蓝牙 SDK。
static void post(const peer_event_t *event)
{
    if (xQueueSend(s_queue, event, 0) != pdPASS) {
        abort();
    }
}

static void spp_callback(esp_spp_cb_event_t type, esp_spp_cb_param_t *parameter)
{
    peer_event_t event = {0};
    switch (type) {
    case ESP_SPP_INIT_EVT:
        event.type = EV_INIT;
        event.status = parameter->init.status;
        post(&event);
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        event.type = EV_DISC;
        event.status = parameter->disc_comp.status;
        event.handle = parameter->disc_comp.scn_num > 0 ? parameter->disc_comp.scn[0] : 0;
        post(&event);
        break;
    case ESP_SPP_OPEN_EVT:
        event.type = EV_OPEN;
        event.status = parameter->open.status;
        event.handle = parameter->open.handle;
        post(&event);
        break;
    case ESP_SPP_CLOSE_EVT:
        event.type = EV_CLOSE;
        event.handle = parameter->close.handle;
        event.status = parameter->close.status;
        post(&event);
        break;
    case ESP_SPP_DATA_IND_EVT:
        event.type = EV_DATA;
        event.handle = parameter->data_ind.handle;
        for (size_t offset = 0; offset < parameter->data_ind.len;) {
            size_t count = parameter->data_ind.len - offset;
            if (count > sizeof(event.data)) {
                count = sizeof(event.data);
            }
            event.length = count;
            memcpy(event.data, parameter->data_ind.data + offset, count);
            post(&event);
            offset += count;
        }
        break;
    default:
        break;
    }
}

static void gap_callback(esp_bt_gap_cb_event_t type, esp_bt_gap_cb_param_t *parameter)
{
    peer_event_t event = {0};
    switch (type) {
    case ESP_BT_GAP_PIN_REQ_EVT:
        event.type = EV_PIN;
        event.status = parameter->pin_req.min_16_digit;
        memcpy(event.data, parameter->pin_req.bda, PEER_RC_ADDR_LEN);
        post(&event);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        event.type = EV_CONFIRM;
        memcpy(event.data, parameter->cfm_req.bda, PEER_RC_ADDR_LEN);
        post(&event);
        break;
    default:
        break;
    }
}

static bool save_target_nvs(const uint8_t addr[PEER_RC_ADDR_LEN])
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(PEER_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return checked(err, "nvs_open");
    }
    err = nvs_set_blob(handle, PEER_NVS_KEY, addr, PEER_RC_ADDR_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return checked(err, "nvs_set_blob");
}

static bool load_target_nvs(uint8_t out_addr[PEER_RC_ADDR_LEN])
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(PEER_NVS_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        checked(err, "nvs_open");
        return false;
    }
    size_t len = PEER_RC_ADDR_LEN;
    err = nvs_get_blob(handle, PEER_NVS_KEY, out_addr, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        checked(err, "nvs_get_blob");
        return false;
    }
    if (len != PEER_RC_ADDR_LEN) {
        ESP_LOGW(s_tag, "nvs target bad len=%zu", len);
        return false;
    }
    return true;
}

static void execute_action(const peer_rc_action_t *act)
{
    uint8_t target[PEER_RC_ADDR_LEN] = {0};
    if ((act == NULL) || (act->type == PEER_RC_ACT_NONE) || (!s_spp_ready)) {
        return;
    }
    if (!peer_rc_get_target(target)) {
        return;
    }
    if (act->type == PEER_RC_ACT_START_SDP) {
        if (!checked(esp_spp_start_discovery(target), "discover")) {
            (void)peer_rc_on_sdp_result(false, 0U, now_ms());
        }
    } else if (act->type == PEER_RC_ACT_CONNECT) {
        if (!checked(esp_spp_connect(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER, act->scn, target), "connect")) {
            (void)peer_rc_on_connect_failed(now_ms());
        }
    }
}

static void start_reconnect_now(void)
{
    peer_rc_action_t act = {0};
    if (!peer_rc_has_target()) {
        ESP_LOGW(s_tag, "no target");
        return;
    }
    if (!s_spp_ready) {
        s_pending_autostart = true;
        return;
    }
    s_pending_autostart = false;
    if (peer_rc_start(now_ms(), &act)) {
        execute_action(&act);
    }
}

static void send_frame(uint16_t id, uint8_t sender, uint8_t type, const uint8_t *payload, size_t length)
{
    uint8_t bytes[DEV_PROTO_MAX_FRAME] = {0};
    size_t count = 0;
    if (s_handle == 0U) {
        ESP_LOGW(s_tag, "not connected");
        return;
    }
    if (dev_proto_frame_build(bytes, sizeof(bytes), id, sender, type, payload, length, &count) != DEV_PROTO_OK) {
        ESP_LOGE(s_tag, "frame build failed");
        return;
    }
    // count <= 280，因此转为 SDK 的 int 长度安全。
    if (checked(esp_spp_write(s_handle, (int)count, bytes), "write")) {
        ESP_LOGI(s_tag, "TX id=%04x length=%zu", id, count);
    }
}

static void heartbeat(void)
{
    const dev_proto_alarm_heartbeat_t hb = {.seq = s_sequence++, .battery_pct = 80U, .local_state = 0U};
    uint8_t payload[DEV_PROTO_ALARM_HEARTBEAT_LEN] = {0};
    const size_t count = dev_proto_alarm_encode_heartbeat(&hb, payload, sizeof(payload));
    send_frame(0x41U, DEV_PROTO_SENDER_ALARM, DEV_PROTO_TYPE_PUSH, payload, count);
    s_last_heartbeat = now_ms();
}

static void receive_data(const peer_event_t *event)
{
    if (event->handle != s_handle) {
        return;
    }
    for (size_t i = 0; i < event->length; i++) {
        dev_proto_frame_t frame = {0};
        const dev_proto_parse_result_t result = dev_proto_parser_feed(&s_parser, event->data[i], &frame);
        if (result == DEV_PROTO_PARSE_FRAME) {
            s_frames++;
            const uint32_t now = now_ms();
            if ((frame.msg_id == 0x40U) && (frame.type == DEV_PROTO_TYPE_PUSH) && (frame.payload_len >= 16U)) {
                const uint16_t seq = (uint16_t)((uint16_t)frame.payload[0] | ((uint16_t)frame.payload[1] << 8U));
                s_pushes++;
                ESP_LOGI(s_tag, "PUSH seq=%u state=%u level=%u gap_ms=%" PRIu32, seq, frame.payload[2],
                         frame.payload[3], s_last_push == 0U ? 0U : now - s_last_push);
                s_last_push = now;
            } else {
                s_replies++;
                ESP_LOGI(s_tag, "RX id=%04x type=%u payload=%u", frame.msg_id, frame.type, frame.payload_len);
            }
        } else if ((result == DEV_PROTO_PARSE_CRC_ERR) || (result == DEV_PROTO_PARSE_LEN_ERR)) {
            s_errors++;
        }
    }
}

static void process_event(const peer_event_t *event)
{
    switch (event->type) {
    case EV_INIT:
        ESP_LOGI(s_tag, "READY status=%d", event->status);
        if (event->status == ESP_SPP_SUCCESS) {
            s_spp_ready = true;
            checked(esp_bt_gap_set_page_timeout(PEER_PAGE_TIMEOUT_SLOTS), "page_to");
            /* 自动首连由 worker 循环在 PEER_BOOT_HOLDOFF_MS 后发起 */
        }
        break;
    case EV_DISC: {
        const bool ok = (event->status == ESP_SPP_SUCCESS) && (event->handle > 0U);
        // SCN 来自 SDK 的 uint8_t，记录到 uint32_t 后再转回，无损。
        const peer_rc_action_t act = peer_rc_on_sdp_result(ok, ok ? (uint8_t)event->handle : 0U, now_ms());
        execute_action(&act);
        break;
    }
    case EV_OPEN:
        if (event->status == ESP_SPP_SUCCESS) {
            peer_rc_status_t st = {0};
            s_handle = event->handle;
            dev_proto_parser_init(&s_parser);
            s_last_push = 0U;
            ESP_LOGI(s_tag, "OPEN handle=%" PRIu32, s_handle);
            (void)peer_rc_on_open(now_ms());
            peer_rc_get_status(now_ms(), &st);
            if (st.state != PEER_RC_STATE_CONNECTED) {
                if (!checked(esp_spp_disconnect(s_handle), "disconnect")) {
                    s_handle = 0U;
                }
            } else if (s_alarm && s_heartbeat) {
                heartbeat();
            }
        } else {
            const peer_rc_action_t act = peer_rc_on_connect_failed(now_ms());
            execute_action(&act);
        }
        break;
    case EV_CLOSE:
        ESP_LOGI(s_tag, "CLOSE handle=%" PRIu32, event->handle);
        if ((s_handle != 0U) && (event->handle == s_handle)) {
            s_handle = 0U;
            const peer_rc_action_t act = peer_rc_on_close(now_ms());
            execute_action(&act);
        }
        if (s_pending_autostart && (s_handle == 0U) && peer_rc_has_target()) {
            start_reconnect_now();
        }
        break;
    case EV_DATA:
        receive_data(event);
        break;
    case EV_PIN: {
        esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
        checked(esp_bt_gap_pin_reply((uint8_t *)event->data, event->status == 0, 4, pin), "pin reply");
        break;
    }
    case EV_CONFIRM:
        checked(esp_bt_gap_ssp_confirm_reply((uint8_t *)event->data, true), "confirm");
        break;
    default:
        break;
    }
}

static void print_reconnect_status(void)
{
    peer_rc_status_t st = {0};
    peer_rc_get_status(now_ms(), &st);
    if (st.has_target) {
        ESP_LOGI(s_tag,
                 "RECONNECT state=%s target=%02x:%02x:%02x:%02x:%02x:%02x next_attempt_in=%" PRIu32
                 "ms attempts=%" PRIu32 " sdp=%" PRIu32 " failures=%" PRIu32 " connects=%" PRIu32
                 " disconnects=%" PRIu32 " last_delay=%" PRIu32 "ms",
                 peer_rc_state_name(st.state), st.target[0], st.target[1], st.target[2], st.target[3], st.target[4],
                 st.target[5], st.next_attempt_in_ms, st.attempts, st.sdp_queries, st.failures, st.connects,
                 st.disconnects, st.last_delay_ms);
    } else {
        ESP_LOGI(s_tag,
                 "RECONNECT state=%s target=none next_attempt_in=%" PRIu32 "ms attempts=%" PRIu32 " sdp=%" PRIu32
                 " failures=%" PRIu32 " connects=%" PRIu32 " disconnects=%" PRIu32 " last_delay=%" PRIu32 "ms",
                 peer_rc_state_name(st.state), st.next_attempt_in_ms, st.attempts, st.sdp_queries, st.failures,
                 st.connects, st.disconnects, st.last_delay_ms);
    }
}

static void handle_stop(void)
{
    s_pending_autostart = false;
    peer_rc_stop();
    if (s_handle != 0U) {
        checked(esp_spp_disconnect(s_handle), "disconnect");
    }
}

static void handle_connect_cmd(const char *arg)
{
    unsigned int bytes[PEER_RC_ADDR_LEN] = {0};
    uint8_t mac[PEER_RC_ADDR_LEN] = {0};
    uint8_t old_mac[PEER_RC_ADDR_LEN] = {0};
    if (sscanf(arg, "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6) {
        ESP_LOGE(s_tag, "bad MAC");
        return;
    }
    for (size_t i = 0U; i < PEER_RC_ADDR_LEN; i++) {
        if (bytes[i] > 255U) {
            ESP_LOGE(s_tag, "bad MAC");
            return;
        }
        mac[i] = (uint8_t)bytes[i]; // 已检验单字节范围。
    }

    (void)save_target_nvs(mac);
    const bool had_old = peer_rc_get_target(old_mac);
    const bool target_changed = (!had_old) || (memcmp(old_mac, mac, PEER_RC_ADDR_LEN) != 0);
    peer_rc_set_target(mac);

    if ((s_handle != 0U) && target_changed) {
        s_pending_autostart = true;
        if (!checked(esp_spp_disconnect(s_handle), "disconnect")) {
            s_handle = 0U;
            start_reconnect_now();
        }
    } else if (s_handle == 0U) {
        start_reconnect_now();
    }
}

static void command(char *line)
{
    if (strncmp(line, "connect ", PEER_CONNECT_PREFIX_LEN) == 0) {
        handle_connect_cmd(line + PEER_CONNECT_PREFIX_LEN);
    } else if (strcmp(line, "alarm") == 0) {
        s_alarm = true;
        s_heartbeat = true;
        if (s_handle != 0U) {
            heartbeat();
        }
    } else if (strcmp(line, "phone") == 0) {
        s_alarm = false;
    } else if (strcmp(line, "hb off") == 0) {
        s_heartbeat = false;
    } else if (strcmp(line, "hb on") == 0) {
        s_heartbeat = true;
    } else if ((strcmp(line, "stop") == 0) || (strcmp(line, "reconnect stop") == 0)) {
        handle_stop();
    } else if (strcmp(line, "reconnect start") == 0) {
        start_reconnect_now();
    } else if (strcmp(line, "reconnect status") == 0) {
        print_reconnect_status();
    } else if (strcmp(line, "reconnect") == 0) {
        if (!peer_rc_has_target()) {
            ESP_LOGW(s_tag, "no target");
        } else if (s_handle != 0U) {
            s_pending_autostart = true;
            peer_rc_stop();
            if (!checked(esp_spp_disconnect(s_handle), "disconnect")) {
                s_handle = 0U;
                start_reconnect_now();
            }
        } else {
            start_reconnect_now();
        }
    } else if (strncmp(line, "query ", PEER_QUERY_PREFIX_LEN) == 0) {
        char *end = NULL;
        unsigned long id = strtoul(line + PEER_QUERY_PREFIX_LEN, &end, 16);
        if ((end != (line + PEER_QUERY_PREFIX_LEN)) && (*end == '\0') && (id <= UINT16_MAX)) {
            send_frame((uint16_t)id, s_alarm ? DEV_PROTO_SENDER_ALARM : DEV_PROTO_SENDER_APP, DEV_PROTO_TYPE_QUERY,
                       NULL, 0U); // 已检验 uint16_t 范围。
        }
    } else if (strcmp(line, "stats") == 0) {
        ESP_LOGI(s_tag,
                 "STATS handle=%" PRIu32 " frames=%" PRIu32 " pushes=%" PRIu32 " replies=%" PRIu32 " errors=%" PRIu32,
                 s_handle, s_frames, s_pushes, s_replies, s_errors);
    } else {
        ESP_LOGW(
            s_tag,
            "commands: connect MAC | phone | alarm | hb on/off | query HEX | stop | reconnect [stop|start|status] | "
            "stats");
    }
}

static void worker(void *unused)
{
    (void)unused;
    char line[PEER_LINE] = {0};
    size_t length = 0U;

    peer_rc_init(esp_random());
    uint8_t saved_mac[PEER_RC_ADDR_LEN] = {0};
    if (load_target_nvs(saved_mac)) {
        peer_rc_set_target(saved_mac);
        s_pending_autostart = true;
        s_alarm = true;
        ESP_LOGI(s_tag, "loaded target %02x:%02x:%02x:%02x:%02x:%02x", saved_mac[0], saved_mac[1], saved_mac[2],
                 saved_mac[3], saved_mac[4], saved_mac[5]);
    }

    for (;;) {
        peer_event_t event = {0};
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(PEER_POLL_MS)) == pdTRUE) {
            process_event(&event);
        }
        uint8_t byte = 0U;
        while (uart_read_bytes(UART_NUM_0, &byte, 1, 0) == 1) {
            if ((byte == '\r') || (byte == '\n')) {
                if (length > 0U) {
                    line[length] = '\0';
                    command(line);
                    length = 0U;
                }
            } else if ((length + 1U) < sizeof(line)) {
                line[length++] = (char)byte;
            } else {
                length = 0U;
                ESP_LOGE(s_tag, "command too long");
            }
        }
        const uint32_t now = now_ms();
        if (s_spp_ready && s_pending_autostart && (now >= PEER_BOOT_HOLDOFF_MS) && peer_rc_has_target()) {
            ESP_LOGI(s_tag, "autostart after holdoff");
            start_reconnect_now();
        }
        if (s_spp_ready) {
            const peer_rc_action_t act = peer_rc_poll(now);
            execute_action(&act);
        }
        if ((s_handle != 0U) && s_alarm && s_heartbeat && ((now - s_last_heartbeat) >= PEER_HEARTBEAT_MS)) {
            heartbeat();
        }
    }
}

// 仅 IDF main 任务调用一次；ISR 不可调用。
void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if ((nvs_err == ESP_ERR_NVS_NO_FREE_PAGES) || (nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_LOGW(s_tag, "nvs init 0x%x, erase and retry", nvs_err);
        if (checked(nvs_flash_erase(), "NVS erase")) {
            nvs_err = nvs_flash_init();
        }
    }
    if (!checked(nvs_err, "NVS init") || !checked(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0), "UART")) {
        return;
    }
    s_queue = xQueueCreateStatic(PEER_QUEUE, sizeof(peer_event_t), s_queue_storage, &s_queue_control);
    if (s_queue == NULL) {
        ESP_LOGE(s_tag, "queue failed");
        return;
    }
    if (!checked(esp_bt_controller_mem_release(ESP_BT_MODE_BLE), "release BLE")) {
        return;
    }
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (!checked(esp_bt_controller_init(&config), "BT init") ||
        !checked(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), "BT enable")) {
        return;
    }
    esp_bluedroid_config_t bluedroid = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if (!checked(esp_bluedroid_init_with_cfg(&bluedroid), "Bluedroid init") ||
        !checked(esp_bluedroid_enable(), "Bluedroid enable") ||
        !checked(esp_bt_gap_register_callback(gap_callback), "GAP callback") ||
        !checked(esp_spp_register_callback(spp_callback), "SPP callback")) {
        return;
    }
    esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
    if (!checked(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability)), "SSP") ||
        !checked(esp_bt_gap_set_device_name("RTK-HIL-Peer"), "name")) {
        return;
    }
    esp_spp_cfg_t spp = {.mode = ESP_SPP_MODE_CB, .enable_l2cap_ertm = true, .tx_buffer_size = 0};
    if (!checked(esp_spp_enhanced_init(&spp), "SPP init")) {
        return;
    }
    // core1 运行测试调度，避开 core0 的 BTC 回调；IDF 栈单位为字节。
    if (xTaskCreateStaticPinnedToCore(worker, "hil_peer", PEER_STACK, NULL, 5, s_task_stack, &s_task_control, 1) ==
        NULL) {
        ESP_LOGE(s_tag, "worker failed");
    }
}
