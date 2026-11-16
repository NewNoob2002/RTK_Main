#pragma once

/* 回归用例控制假 IDF 行为的钩子：只用于 host 回归，不参与固件构建。 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_spp_api.h"

typedef enum {
    FAKE_STAGE_OK = 0, /* 异步投递成功事件 */
    FAKE_STAGE_FAIL,   /* 异步投递失败事件 */
    FAKE_STAGE_SILENT, /* 不投递事件（验证 init 会按预算超时） */
} fake_stage_t;

typedef enum {
    FAKE_WRITE_OK = 0,       /* 投递 status=SUCCESS 的 WRITE_EVT */
    FAKE_WRITE_FAIL,         /* 投递 status=ESP_SPP_FAILURE 的 WRITE_EVT */
    FAKE_WRITE_NO_RESOURCE,  /* 投递 status=ESP_SPP_NO_RESOURCE 的 WRITE_EVT */
    FAKE_WRITE_SHORT,        /* 投递 SUCCESS 但 len 比提交少 1（短写） */
    FAKE_WRITE_HOLD,         /* 不自动投递，由用例调用 fake_emit_write() 补 */
    FAKE_WRITE_BLOCK,        /* 在 esp_spp_write 内部阻塞后投递成功事件 */
    FAKE_WRITE_SUBMIT_ERROR, /* esp_spp_write 直接返回 ESP_ERR_INVALID_STATE */
} fake_write_mode_t;

/* INIT/START 完成门闩：armed 后该阶段的回调在调用驱动前暂停，直到用例 release。
 * 让“事件尚未到达时 init 不得返回”可确定性验证，不依赖 sleep 猜时序。 */
typedef enum {
    FAKE_GATE_INIT = 0,
    FAKE_GATE_START,
    FAKE_GATE_DISCONNECT,
    FAKE_GATE_COUNT,
} fake_gate_t;

void fake_reset(void);
void fake_set_init_mode(fake_stage_t mode);
void fake_set_start_mode(fake_stage_t mode);
/* true 时 INIT/START 回调在发起 API 内同步执行完（模拟事件早于等待返回的顺序）。 */
void fake_set_stage_sync(bool sync);
void fake_set_write_mode(fake_write_mode_t mode);
void fake_set_write_block_ms(uint32_t ms);
void fake_set_disconnect_block_ms(uint32_t ms);
void fake_set_deinit_return(esp_err_t err);
/* 是否出现过“从 BTC 回调线程调用可能阻塞投递的 API”（esp_spp_write/start_srv/disconnect/GAP）。 */
bool fake_blocking_api_from_btc(void);

void fake_gate_arm(fake_gate_t which);
bool fake_gate_wait_entered(fake_gate_t which, uint32_t timeout_ms);
void fake_gate_release(fake_gate_t which);

/* esp_spp_write 入口门闩：arm 后，下一次调用记录完调用信息即暂停，直到用例 release。
 * 用于确定“CLOSE 发生在 worker 仍位于 esp_spp_write 内”的窗口。 */
void fake_write_gate_arm(void);
bool fake_write_gate_wait_entered(uint32_t timeout_ms);
void fake_write_gate_release(void);

void fake_emit_write(uint32_t handle, int len, esp_spp_status_t status, bool cong);
void fake_emit_cong(uint32_t handle, bool cong);
void fake_emit_srv_open(uint32_t handle, const uint8_t bda[6]);
void fake_emit_close(uint32_t handle);
void fake_emit_data(uint32_t handle, const uint8_t *data, uint16_t len);
/* GAP 事件由独立假 BTC 线程投递；应答计数/bda 用于验证“不在回调线程里应答”。 */
void fake_emit_gap_pin_req(const uint8_t bda[6]);
void fake_emit_gap_cfm_req(const uint8_t bda[6]);
int fake_pin_reply_count(void);
int fake_ssp_confirm_count(void);
const uint8_t *fake_last_gap_reply_bda(void);
bool fake_wait_pin_reply(int count, uint32_t timeout_ms);
bool fake_wait_ssp_confirm(int count, uint32_t timeout_ms);
bool fake_wait_disconnect_calls(int count, uint32_t timeout_ms);

int fake_write_call_count(void);
/* esp_spp_write 已返回的次数（调用记录发生在入口，返回另有计数）。 */
int fake_write_return_count(void);
bool fake_wait_write_returns(int count, uint32_t timeout_ms);
uint32_t fake_write_call_handle(int index);
int fake_write_call_len(int index);
const uint8_t *fake_write_call_data(int index);
size_t fake_write_total_bytes(void);
const uint8_t *fake_write_all_data(void);
bool fake_write_overlapping_submit(void);
int fake_disconnect_count(void);
uint32_t fake_disconnect_handle(int index);

void fake_trace_reset(void);
int fake_trace_count(void);
const char *fake_trace_at(int index);

/* 等假 IDF 收到至少 count 次 esp_spp_write 调用；超时返回 false。 */
bool fake_wait_write_calls(int count, uint32_t timeout_ms);
void fake_sleep_ms(uint32_t ms);

void fake_set_disconnect_result(esp_err_t err);
bool fake_wait_disconnect_returns(int count, uint32_t timeout_ms);
