/* 蓝牙栈启停：控制器/Bluedroid 的初始化与逆序释放、GAP/SPP 协议注册。
 * 线程：只在 spp_server_init() 的启动/停止路径执行，失败路径在此内部回滚；绝不能在回调里调用。
 * 堆与阻塞：不分配堆（缓冲由组合根外供）；经 btc_transfer_context 阻塞等待控制器/Bluedroid。 */
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "fw/LogC.h"

#include "SppServerPriv.h"

/* 逆序释放阶梯：每个阶段的成败单独判定且只记日志，拆开反而难读（ADR-0013 §2.4）。 */
// NOLINTBEGIN(readability-function-size): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
void spp_server_unwind(bool bluedroid_enabled, bool bluedroid_inited, bool controller_enabled,
                       bool controller_inited)
{
    esp_err_t err = ESP_OK;

    if (bluedroid_enabled) {
        err = esp_bluedroid_disable();
        if (err != ESP_OK) {
            FW_CLOGW(FW_LOG_CAT_SPP, "bluedroid disable: %s", esp_err_to_name(err));
        }
    }
    if (bluedroid_inited) {
        err = esp_bluedroid_deinit();
        if (err != ESP_OK) {
            FW_CLOGW(FW_LOG_CAT_SPP, "bluedroid deinit: %s", esp_err_to_name(err));
        }
    }
    if (controller_enabled) {
        err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            FW_CLOGW(FW_LOG_CAT_SPP, "controller disable: %s", esp_err_to_name(err));
        }
    }
    if (controller_inited) {
        err = esp_bt_controller_deinit();
        if (err != ESP_OK) {
            FW_CLOGW(FW_LOG_CAT_SPP, "controller deinit: %s", esp_err_to_name(err));
        }
    }
}
// NOLINTEND(readability-function-size)

void spp_server_hw_state_clear(spp_server_hw_state_t *state)
{
    state->controller_inited = false;
    state->controller_enabled = false;
    state->bluedroid_inited = false;
    state->bluedroid_enabled = false;
}

/* 线性初始化序列：控制器/蓝牙栈逐级启停，失败即回滚，拆开反而难读（ADR-0013 §2.4）。 */
// NOLINTBEGIN(readability-function-size): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
esp_err_t spp_server_hw_init(spp_server_hw_state_t *state)
{
    esp_err_t err = ESP_OK;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();

    err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "mem release: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "controller init: %s", esp_err_to_name(err));
        return err;
    }
    state->controller_inited = true;

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "controller enable: %s", esp_err_to_name(err));
        spp_server_unwind(state->bluedroid_enabled, state->bluedroid_inited,
                          state->controller_enabled, state->controller_inited);
        spp_server_hw_state_clear(state);
        return err;
    }
    state->controller_enabled = true;

    err = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "bluedroid init: %s", esp_err_to_name(err));
        spp_server_unwind(state->bluedroid_enabled, state->bluedroid_inited,
                          state->controller_enabled, state->controller_inited);
        spp_server_hw_state_clear(state);
        return err;
    }
    state->bluedroid_inited = true;

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "bluedroid enable: %s", esp_err_to_name(err));
        spp_server_unwind(state->bluedroid_enabled, state->bluedroid_inited,
                          state->controller_enabled, state->controller_inited);
        spp_server_hw_state_clear(state);
        return err;
    }
    state->bluedroid_enabled = true;

    return ESP_OK;
}
// NOLINTEND(readability-function-size)

/* 线性注册序列：GAP/SPP 回调与 profile 参数一次配好，拆开反而难读（ADR-0013 §2.4）。 */
// NOLINTBEGIN(readability-function-size): ADR-0013 §2.4，保留线性流程或平铺分派，理由见契约。
esp_err_t spp_server_profile_init(void)
{
    esp_err_t err = ESP_OK;
    /* DisplayYesNo：手机走数字比较，CFM 请求回调到应用，配对窗口与 confirm 模式才能裁决；
     * NoIO 对端（报警器）仍是 Just Works，由 AUTH_CMPL 兜底（SppServerSession.c）。 */
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_pin_code_t pin_code = {0};
    esp_spp_cfg_t spp_cfg = {0};

    err = esp_bt_gap_register_callback(spp_server_gap_cb);
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "gap callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_spp_register_callback(spp_server_cb);
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "spp callback: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "set iocap: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin_code);
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "set pin: %s", esp_err_to_name(err));
        return err;
    }

    spp_cfg.mode = ESP_SPP_MODE_CB;
    spp_cfg.enable_l2cap_ertm = true;
    spp_cfg.tx_buffer_size = 0U;
    err = esp_spp_enhanced_init(&spp_cfg);
    if (err != ESP_OK) {
        FW_CLOGE(FW_LOG_CAT_SPP, "spp init: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
// NOLINTEND(readability-function-size)
