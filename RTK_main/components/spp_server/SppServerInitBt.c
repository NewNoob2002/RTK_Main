/* 蓝牙栈启停：控制器/Bluedroid 的初始化与逆序释放、GAP/SPP 协议注册。
 * 只在 spp_server_init() 的启动任务上下文执行（失败路径在此内部回滚）；不分配内存，非 ISR。 */
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "fw/LogC.h"

#include "SppServerPriv.h"

/**
 * @brief  逆序释放蓝牙栈：Bluedroid 先于控制器
 * @param  bluedroid_enabled: Bluedroid 已使能
 * @param  bluedroid_inited: Bluedroid 已初始化
 * @param  controller_enabled: 控制器已使能
 * @param  controller_inited: 控制器已初始化
 * @note   不拆（S4 §2.1）：逆序释放阶梯，每个阶段独立判错且只记日志。
 */
void spp_server_unwind(bool bluedroid_enabled, // NOLINT(readability-function-*)
                       bool bluedroid_inited, bool controller_enabled, bool controller_inited)
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

/**
 * @brief  清空蓝牙栈各阶段完成标志
 * @param[out] state: 阶段标志
 */
void spp_server_hw_state_clear(spp_server_hw_state_t *state)
{
    state->controller_inited = false;
    state->controller_enabled = false;
    state->bluedroid_inited = false;
    state->bluedroid_enabled = false;
}

/**
 * @brief  初始化并使能控制器与 Bluedroid；失败时在内部逆序释放
 * @param[out] state: 各阶段完成标志
 * @retval ESP_OK 成功；其他为 IDF 错误码
 * @note   不拆（S4 §2.1）：线性初始化序列（控制器→使能→Bluedroid→使能），失败即回滚。
 */
esp_err_t spp_server_hw_init(spp_server_hw_state_t *state) // NOLINT(readability-function-*)
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

/**
 * @brief  注册 GAP/SPP 回调、设置 SSP 参数并发起 SPP 初始化
 * @retval ESP_OK 成功；其他为 IDF 错误码（由调用方统一 unwind）
 * @note   不拆（S4 §2.1）：线性注册序列（GAP/SPP 回调→SSP 参数→SPP init），失败即由调用方回滚。
 */
esp_err_t spp_server_profile_init(void) // NOLINT(readability-function-*)
{
    esp_err_t err = ESP_OK;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
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
