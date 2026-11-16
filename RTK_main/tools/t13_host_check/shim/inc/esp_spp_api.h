#pragma once

/* SPP API 的 host 替身：枚举/结构体与 IDF esp_spp_api.h 同名同形，
 * 函数实现放在 FakeIdf.c，行为可由回归用例控制。 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_bt.h"
#include "esp_err.h"

typedef enum {
    ESP_SPP_SUCCESS = 0,
    ESP_SPP_FAILURE,
    ESP_SPP_BUSY,
    ESP_SPP_NO_DATA,
    ESP_SPP_NO_RESOURCE,
    ESP_SPP_NEED_INIT,
    ESP_SPP_NEED_DEINIT,
    ESP_SPP_NO_CONNECTION,
    ESP_SPP_NO_SERVER,
} esp_spp_status_t;

typedef enum {
    ESP_SPP_INIT_EVT = 0,
    ESP_SPP_UNINIT_EVT = 1,
    ESP_SPP_DISCOVERY_COMP_EVT = 8,
    ESP_SPP_OPEN_EVT = 26,
    ESP_SPP_CLOSE_EVT = 27,
    ESP_SPP_START_EVT = 28,
    ESP_SPP_CL_INIT_EVT = 29,
    ESP_SPP_DATA_IND_EVT = 30,
    ESP_SPP_CONG_EVT = 31,
    ESP_SPP_WRITE_EVT = 33,
    ESP_SPP_SRV_OPEN_EVT = 34,
    ESP_SPP_SRV_STOP_EVT = 35,
} esp_spp_cb_event_t;

typedef enum {
    ESP_SPP_SEC_NONE = 0,
    ESP_SPP_SEC_AUTHENTICATE = 1,
    ESP_SPP_SEC_ENCRYPT = 2,
} esp_spp_sec_t;

typedef enum {
    ESP_SPP_ROLE_MASTER = 0,
    ESP_SPP_ROLE_SLAVE = 1,
} esp_spp_role_t;

typedef enum {
    ESP_SPP_MODE_CB = 0,
    ESP_SPP_MODE_VFS = 1,
} esp_spp_mode_t;

typedef struct {
    esp_spp_mode_t mode;
    bool enable_l2cap_ertm;
    uint16_t tx_buffer_size;
} esp_spp_cfg_t;

typedef union {
    struct {
        esp_spp_status_t status;
    } init;
    struct {
        esp_spp_status_t status;
    } uninit;
    struct {
        esp_spp_status_t status;
        uint32_t handle;
        uint32_t new_listen_handle;
        int fd;
        esp_bd_addr_t rem_bda;
    } srv_open;
    struct {
        esp_spp_status_t status;
        int port_status;
        uint32_t handle;
        bool async;
    } close;
    struct {
        esp_spp_status_t status;
        uint32_t handle;
        uint32_t sec_id;
        int scn;
        bool use_co;
    } start;
    struct {
        esp_spp_status_t status;
        uint32_t handle;
        int len;
        bool cong;
    } write;
    struct {
        esp_spp_status_t status;
        uint32_t handle;
        uint16_t len;
        uint8_t *data;
    } data_ind;
    struct {
        esp_spp_status_t status;
        uint32_t handle;
        bool cong;
    } cong;
} esp_spp_cb_param_t;

typedef void (*esp_spp_cb_t)(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);

esp_err_t esp_spp_register_callback(esp_spp_cb_t callback);
esp_err_t esp_spp_enhanced_init(const esp_spp_cfg_t *cfg);
esp_err_t esp_spp_deinit(void);
esp_err_t esp_spp_start_srv(esp_spp_sec_t sec_mask, esp_spp_role_t role, uint8_t local_scn, const char *name);
esp_err_t esp_spp_write(uint32_t handle, int len, uint8_t *p_data);
esp_err_t esp_spp_disconnect(uint32_t handle);
