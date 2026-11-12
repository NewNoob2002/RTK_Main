/* SPP 驱动的启动与回滚：控制器/Bluedroid/GAP/SPP 初始化、异步阶段等待、服务端启动与设备名。
 * 只在 main 的初始化上下文调用（spp_server_init / spp_server_get_local_addr）；esp_* 调用可能阻塞在
 * BTC 队列上，因此绝不能在 BTC 回调里调用。等待阶段完成位时会阻塞调用者，直到完成或超时。 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"

#include "spp_server_priv.h"

#define SPP_SERVER_SERVER_NAME "SPP_SERVER"

#define SPP_SERVER_WORKER_PRIORITY (5U)
/* 与 CONFIG_BT_BLUEDROID_PINNED_TO_CORE 相同（sdkconfig=0）：发送任务调用 esp_spp_write 后会
 * 阻塞在 BTC 队列上，同核可直接让高优先级的 BTC 任务接手，避免跨核唤醒延迟。 */
#define SPP_SERVER_WORKER_CORE (0)

/* StreamBuffer 的“可读字节数”触发阈值：只用 poll_event 的阻塞语义，stream_buffer 的 xTriggerLevel 必须非零。 */
#define SPP_SERVER_STREAM_TRIGGER (1U)

/* SPP 初始化/反初始化是异步的（回调只记录状态），这两个预算只用于等确认。 */
#define SPP_SERVER_INIT_TIMEOUT_MS   (5000U)
#define SPP_SERVER_UNINIT_TIMEOUT_MS (2000U)

/* 逆序释放：Bluedroid 先于控制器。 */
static void spp_server_unwind(bool bluedroid_enabled, bool bluedroid_inited, bool controller_enabled,
                              bool controller_inited)
{
    esp_err_t err = ESP_OK;

    if (bluedroid_enabled) {
        err = esp_bluedroid_disable();
        if (err != ESP_OK) {
            ESP_LOGW(SPP_SERVER_TAG, "bluedroid disable: %s", esp_err_to_name(err));
        }
    }
    if (bluedroid_inited) {
        err = esp_bluedroid_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(SPP_SERVER_TAG, "bluedroid deinit: %s", esp_err_to_name(err));
        }
    }
    if (controller_enabled) {
        err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            ESP_LOGW(SPP_SERVER_TAG, "controller disable: %s", esp_err_to_name(err));
        }
    }
    if (controller_inited) {
        err = esp_bt_controller_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(SPP_SERVER_TAG, "controller deinit: %s", esp_err_to_name(err));
        }
    }
}

/* 只在 init 失败路径调用：此时还没有会话，且 SPP/Bluedroid 已关闭，不会再进回调。
 * 发送任务不删除——它只等任务通知，留着可被下一次 init 复用（静态对象不回收）。 */
static void spp_server_objects_delete(void)
{
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        s_spp.sessions[idx].in_use = false;
        s_spp.sessions[idx].reserving = false;
        s_spp.sessions[idx].users = 0U;
        s_spp.sessions[idx].tx_submitting = false;
        s_spp.sessions[idx].tx_state = SPP_SERVER_TX_IDLE;
        s_spp.sessions[idx].handle = 0U;
        if (s_spp.sessions[idx].rx != NULL) {
            vStreamBufferDelete(s_spp.sessions[idx].rx);
            s_spp.sessions[idx].rx = NULL;
        }
    }
    for (uint32_t slot = 0U; slot < SPP_SERVER_CTRL_MAX; slot++) {
        s_spp.ctrl[slot].in_use = false;
        s_spp.ctrl[slot].handle = 0U;
    }
    taskENTER_CRITICAL(&s_spp_mux);
    s_spp_tx_inflight = -1;
    s_spp.tx_inflight_cancelled = false;
    taskEXIT_CRITICAL(&s_spp_mux);
    if (s_spp.rtos.evt_queue != NULL) {
        vQueueDelete(s_spp.rtos.evt_queue);
        s_spp.rtos.evt_queue = NULL;
    }
    if (s_spp.rtos.evt_group != NULL) {
        vEventGroupDelete(s_spp.rtos.evt_group);
        s_spp.rtos.evt_group = NULL;
    }
    s_spp.initialized = false;
}

/* 创建静态 RTOS 对象与发送任务。发送任务跨 init 重试复用。 */
static esp_err_t spp_server_objects_create(void)
{
    /* 重试时发送任务仍在运行：会话/控制表的复位放在同一把锁下，避免与它读到中间状态。 */
    taskENTER_CRITICAL(&s_spp_mux);
    memset(s_spp.sessions, 0, sizeof(s_spp.sessions));
    memset(s_spp.ctrl, 0, sizeof(s_spp.ctrl));
    s_spp.evt_dropped = 0U;
    s_spp_tx_inflight = -1;
    s_spp.tx_inflight_cancelled = false;
    taskEXIT_CRITICAL(&s_spp_mux);

    s_spp.rtos.evt_queue = xQueueCreateStatic(SPP_SERVER_EVENT_QUEUE_LEN, sizeof(spp_server_evt_t),
                                              s_spp.evt_queue_storage, &s_spp.evt_queue_struct);
    s_spp.rtos.evt_group = xEventGroupCreateStatic(&s_spp.evt_group_struct);
    for (uint32_t idx = 0U; idx < SPP_SERVER_MAX_SESSIONS; idx++) {
        /* idx < 2，uint8_t 窄化安全 */
        s_spp.sessions[idx].idx = (uint8_t)idx;
        s_spp.sessions[idx].rx =
            xStreamBufferCreateStatic(SPP_SERVER_RX_BUF_SIZE, SPP_SERVER_STREAM_TRIGGER, s_spp.sessions[idx].rx_storage,
                                      &s_spp.sessions[idx].rx_struct);
    }
    if ((s_spp.rtos.evt_queue == NULL) || (s_spp.rtos.evt_group == NULL) || (s_spp.sessions[0].rx == NULL) ||
        (s_spp.sessions[1].rx == NULL)) {
        return ESP_ERR_NO_MEM;
    }

    /* 只等任务通知/读会话表的常驻任务，栈 4096 字节（调用 esp_spp_write 与日志）。 */
    if (s_spp.rtos.worker == NULL) {
        /* IDF 静态创建接口的栈单位是字节（task.h:519），因此直接给栈字节数。 */
        const uint32_t stack_bytes = (uint32_t)(sizeof(s_spp.worker_stack) / sizeof(StackType_t));

        s_spp.rtos.worker = xTaskCreateStaticPinnedToCore(spp_server_worker, "spp_worker", stack_bytes, NULL,
                                                          SPP_SERVER_WORKER_PRIORITY, s_spp.worker_stack,
                                                          &s_spp.worker_tcb, SPP_SERVER_WORKER_CORE);
        if (s_spp.rtos.worker == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

typedef struct {
    bool controller_inited;
    bool controller_enabled;
    bool bluedroid_inited;
    bool bluedroid_enabled;
} spp_server_hw_state_t;

static void spp_server_hw_state_clear(spp_server_hw_state_t *state)
{
    state->controller_inited = false;
    state->controller_enabled = false;
    state->bluedroid_inited = false;
    state->bluedroid_enabled = false;
}

/* 控制器 + Bluedroid。失败时在内部完成逆序释放。 */
static esp_err_t spp_server_hw_init(spp_server_hw_state_t *state)
{
    esp_err_t err = ESP_OK;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();

    err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "mem release: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "controller init: %s", esp_err_to_name(err));
        return err;
    }
    state->controller_inited = true;

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "controller enable: %s", esp_err_to_name(err));
        spp_server_unwind(state->bluedroid_enabled, state->bluedroid_inited, state->controller_enabled,
                          state->controller_inited);
        spp_server_hw_state_clear(state);
        return err;
    }
    state->controller_enabled = true;

    err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "bluedroid init: %s", esp_err_to_name(err));
        spp_server_unwind(state->bluedroid_enabled, state->bluedroid_inited, state->controller_enabled,
                          state->controller_inited);
        spp_server_hw_state_clear(state);
        return err;
    }
    state->bluedroid_inited = true;

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "bluedroid enable: %s", esp_err_to_name(err));
        spp_server_unwind(state->bluedroid_enabled, state->bluedroid_inited, state->controller_enabled,
                          state->controller_inited);
        spp_server_hw_state_clear(state);
        return err;
    }
    state->bluedroid_enabled = true;

    return ESP_OK;
}

/* GAP/SPP 回调、SSP 参数与 SPP 初始化。失败时由调用方统一 unwind。 */
static esp_err_t spp_server_profile_init(void)
{
    esp_err_t err = ESP_OK;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_pin_code_t pin_code = {0};
    esp_spp_cfg_t spp_cfg = {0};

    err = esp_bt_gap_register_callback(spp_server_gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "gap callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_spp_register_callback(spp_server_cb);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "spp callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "set iocap: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin_code);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "set pin: %s", esp_err_to_name(err));
        return err;
    }

    spp_cfg.mode = ESP_SPP_MODE_CB;
    spp_cfg.enable_l2cap_ertm = true;
    spp_cfg.tx_buffer_size = 0U;
    err = esp_spp_enhanced_init(&spp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "spp init: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static EventBits_t spp_server_stage_bit(spp_server_stage_t stage)
{
    switch (stage) {
    case SPP_SERVER_STAGE_INIT:
        return SPP_SERVER_BIT_INIT_DONE;
    case SPP_SERVER_STAGE_START:
        return SPP_SERVER_BIT_START_DONE;
    case SPP_SERVER_STAGE_UNINIT:
    default:
        return SPP_SERVER_BIT_UNINIT_DONE;
    }
}

/* 等一个异步阶段完成：完成位未到 → ESP_ERR_TIMEOUT；否则在同一把锁下读取该阶段的实际状态
 * （回调先写状态、再置位），因此事件早于/晚于等待返回都能得到真实结果，不能用调用前的快照。 */
static esp_err_t spp_server_wait_stage(spp_server_stage_t stage, uint32_t timeout_ms)
{
    const EventBits_t bit = spp_server_stage_bit(stage);
    EventBits_t bits = 0U;
    esp_spp_status_t status = ESP_SPP_FAILURE;

    if (s_spp.rtos.evt_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bits = xEventGroupWaitBits(s_spp.rtos.evt_group, bit, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & bit) == 0U) {
        ESP_LOGE(SPP_SERVER_TAG, "stage 0x%" PRIx32 " timeout", (uint32_t)bit);
        return ESP_ERR_TIMEOUT;
    }
    if (stage == SPP_SERVER_STAGE_UNINIT) {
        return ESP_OK; /* UNINIT 回调没有业务状态 */
    }

    taskENTER_CRITICAL(&s_spp_mux);
    status = (stage == SPP_SERVER_STAGE_INIT) ? s_spp.init_status : s_spp.start_status;
    taskEXIT_CRITICAL(&s_spp_mux);
    return (status == ESP_SPP_SUCCESS) ? ESP_OK : ESP_FAIL;
}

/* 任务上下文完成启动阶段：等 INIT、启动服务端、等 START、设置设备名与可发现模式。
 * 这些 API 都可能阻塞在 btc_transfer_context 上，因此绝不能放到回调里（QA-E-REVIEW 第 2 项）。 */
static esp_err_t spp_server_bringup(void)
{
    esp_err_t err = ESP_OK;

    err = spp_server_wait_stage(SPP_SERVER_STAGE_INIT, SPP_SERVER_INIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    /* START 状态必须在发起前复位并清掉上次尝试的完成位：回调可能在 esp_spp_start_srv
     * 返回前（同步/极快）到达，不能在发起后再清。 */
    taskENTER_CRITICAL(&s_spp_mux);
    s_spp.start_status = ESP_SPP_FAILURE;
    taskEXIT_CRITICAL(&s_spp_mux);
    (void)xEventGroupClearBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_START_DONE);

    err = esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, SPP_SERVER_SERVER_NAME);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "start srv: %s", esp_err_to_name(err));
        return err;
    }

    err = spp_server_wait_stage(SPP_SERVER_STAGE_START, SPP_SERVER_INIT_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_bt_gap_set_device_name(s_spp.device_name);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "set name: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "set scan mode: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/* SPP 反初始化同样异步：发起后等 UNINIT_EVT，超时只告警（没有回滚手段）。 */
static void spp_server_spp_deinit(void)
{
    esp_err_t err = ESP_OK;

    /* 清掉上一次尝试可能残留的完成位，避免误以为本次已确认。 */
    if (s_spp.rtos.evt_group != NULL) {
        (void)xEventGroupClearBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_UNINIT_DONE);
    }
    err = esp_spp_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(SPP_SERVER_TAG, "spp deinit: %s", esp_err_to_name(err));
        return;
    }
    if (spp_server_wait_stage(SPP_SERVER_STAGE_UNINIT, SPP_SERVER_UNINIT_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(SPP_SERVER_TAG, "spp deinit ack missing");
    }
}

esp_err_t spp_server_init(const char *name)
{
    esp_err_t err = ESP_OK;
    spp_server_hw_state_t hw_state = {false, false, false, false};
    size_t name_len = 0U;

    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    name_len = strlen(name);
    if ((name_len == 0U) || (name_len > SPP_SERVER_NAME_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t idx = 0U; idx < name_len; idx++) {
        if ((name[idx] == '\n') || (name[idx] == '\r')) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (s_spp.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    err = spp_server_objects_create();
    if (err != ESP_OK) {
        ESP_LOGE(SPP_SERVER_TAG, "objects create: %s", esp_err_to_name(err));
        spp_server_objects_delete();
        return err;
    }

    for (size_t idx = 0U; idx < name_len; idx++) {
        s_spp.device_name[idx] = name[idx];
    }
    s_spp.device_name[name_len] = '\0';
    /* 阶段结果初值：回调未到按失败处理；同时清掉上次尝试可能残留的完成位，
     * 避免 INIT 等待被旧事件立即满足。清位在 profile_init 之前，同步回调也不会丢。 */
    taskENTER_CRITICAL(&s_spp_mux);
    s_spp.init_status = ESP_SPP_FAILURE;
    s_spp.start_status = ESP_SPP_FAILURE;
    taskEXIT_CRITICAL(&s_spp_mux);
    if (s_spp.rtos.evt_group != NULL) {
        (void)xEventGroupClearBits(s_spp.rtos.evt_group, SPP_SERVER_BIT_INIT_DONE | SPP_SERVER_BIT_START_DONE);
    }

    err = spp_server_hw_init(&hw_state);
    if (err != ESP_OK) {
        spp_server_objects_delete();
        return err;
    }

    err = spp_server_profile_init();
    if (err != ESP_OK) {
        spp_server_unwind(hw_state.bluedroid_enabled, hw_state.bluedroid_inited, hw_state.controller_enabled,
                          hw_state.controller_inited);
        spp_server_objects_delete();
        return err;
    }

    err = spp_server_bringup();
    if (err != ESP_OK) {
        /* 逆序清理：SPP → Bluedroid → 控制器 → RTOS 对象。 */
        spp_server_spp_deinit();
        spp_server_unwind(hw_state.bluedroid_enabled, hw_state.bluedroid_inited, hw_state.controller_enabled,
                          hw_state.controller_inited);
        spp_server_objects_delete();
        return err;
    }

    s_spp.initialized = true;
    return ESP_OK;
}

esp_err_t spp_server_get_local_addr(uint8_t addr[SPP_SERVER_ADDR_LEN])
{
    const uint8_t *local = NULL;

    if (addr == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    local = esp_bt_dev_get_address();
    if (local == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint32_t idx = 0U; idx < SPP_SERVER_ADDR_LEN; idx++) {
        addr[idx] = local[idx];
    }
    return ESP_OK;
}
