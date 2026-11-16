/* IDF（BTC/Bluedroid/GAP/SPP）的 host 替身实现。
 * 关键点：所有 SPP 回调都由独立的假 BTC 线程投递（与固件一致：回调不在调用者线程里跑），
 * 用例可以据此验证驱动的跨任务语义。 */
#include "fake_idf.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/task.h"

#define FAKE_CALLS_MAX  32
#define FAKE_CALL_BYTES 1024 /* K2 merge 上限，必须保存整块才能逐字节检查 stream。 */
#define FAKE_ALL_BYTES  8192
#define FAKE_JOBS_MAX   64
#define FAKE_TRACE_MAX  32
#define FAKE_OUT_MAX    8

typedef struct {
    bool is_gap;
    esp_spp_cb_event_t ev;
    esp_spp_cb_param_t param;
    esp_bt_gap_cb_event_t gap_ev;
    esp_bt_gap_cb_param_t gap_param;
} fake_job_t;

/* INIT/START 阶段门闩：BTC 线程在调用驱动回调之前等待，用例据此验证“事件未到，init 不返回”。 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int state; /* 0=off 1=armed 2=entered 3=released */
} fake_gate_state_t;

static fake_gate_state_t s_gates[FAKE_GATE_COUNT] = {
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0},
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0},
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0},
};

typedef struct {
    uint32_t handle;
    int len;
    bool in_flight; /* 已提交、尚未收到完成事件 */
    uint8_t data[FAKE_CALL_BYTES];
} fake_call_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond;
static pthread_once_t s_cond_once = PTHREAD_ONCE_INIT;

/* esp_spp_write 入口门闩（见 fake_idf.h）：0=off 1=armed 2=entered 3=released。 */
static pthread_mutex_t s_write_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_write_gate_cond = PTHREAD_COND_INITIALIZER;
static int s_write_gate_state;

static esp_spp_cb_t s_spp_cb; /* 驱动注册的回调 */
static esp_bt_gap_cb_t s_gap_cb;

static fake_stage_t s_init_mode = FAKE_STAGE_OK;
static fake_stage_t s_start_mode = FAKE_STAGE_OK;
static bool s_stage_sync;
static fake_write_mode_t s_write_mode = FAKE_WRITE_OK;
static int s_pin_reply_count;
static int s_ssp_confirm_count;
static uint8_t s_last_gap_bda[6];
static uint32_t s_write_block_ms = 0U;
static uint32_t s_disconnect_block_ms = 0U;
static esp_err_t s_deinit_return = ESP_OK;
/* 记录“从 BTC 回调线程里调用了可能阻塞投递的 API”（QA-E-REVIEW 第 2 项要禁止的行为）。 */
static bool s_blocking_from_btc;

static void fake_trace_add(const char *name);
static void fake_note_blocking_api(void);

static fake_call_t s_calls[FAKE_CALLS_MAX];
static int s_call_count;
static int s_write_return_count;
static uint8_t s_all_data[FAKE_ALL_BYTES];
static size_t s_all_len;
static bool s_overlap;
static uint32_t s_disconnect[FAKE_CALLS_MAX];
static int s_disconnect_count;
static int s_disconnect_return_count;
static esp_err_t s_disconnect_result;
static const char *s_trace[FAKE_TRACE_MAX];
static int s_trace_count;

static fake_job_t s_jobs[FAKE_JOBS_MAX];
static int s_job_head;
static int s_job_count;
static uint32_t s_posted; /* 已投递的回调数 */
static uint32_t s_done;   /* 已执行完的回调数（fake_emit_*_sync 据此等待） */
static pthread_t s_btc;
static bool s_btc_started;

/* 在可能阻塞投递的 API 入口调用：若当前线程就是假 BTC 线程，说明驱动把坏调用放回了回调里。 */
static void fake_note_blocking_api(void)
{
    if (s_btc_started && pthread_equal(pthread_self(), s_btc)) {
        s_blocking_from_btc = true;
    }
}

static void cond_init_once(void)
{
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&s_cond, &attr);
    pthread_condattr_destroy(&attr);
}

static void ensure_cond(void)
{
    pthread_once(&s_cond_once, cond_init_once);
}

static void abs_deadline(struct timespec *ts, uint32_t ms)
{
    uint64_t ns = 0U;

    clock_gettime(CLOCK_MONOTONIC, ts);
    ns = (uint64_t)ts->tv_nsec + ((uint64_t)ms * 1000000ULL);
    ts->tv_sec += (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
}

void fake_sleep_ms(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)((ms % 1000U) * 1000000U);
    (void)nanosleep(&ts, NULL);
}

/* 本替身固定 1000 Hz；仅 T13 的假 IDF 库提供真实让出 CPU 的延时，main 检查仍使用自己的时钟桩。 */
void vTaskDelay(TickType_t ticks)
{
    fake_sleep_ms(ticks);
}

/* ---------------- 假 BTC 线程：所有回调都在这里执行 ---------------- */

/* 完成事件到达后，该句柄的在途提交结束（用于检测并发提交）。 */
static void fake_clear_in_flight(uint32_t handle)
{
    for (int i = 0; i < s_call_count; i++) {
        if (s_calls[i].in_flight && (s_calls[i].handle == handle)) {
            s_calls[i].in_flight = false;
            return;
        }
    }
}

static void fake_gate_enter(fake_gate_t which)
{
    fake_gate_state_t *gate = &s_gates[which];

    pthread_mutex_lock(&gate->lock);
    if (gate->state == 1) {
        gate->state = 2;
        pthread_cond_broadcast(&gate->cond);
        while (gate->state == 2) {
            pthread_cond_wait(&gate->cond, &gate->lock);
        }
    }
    pthread_mutex_unlock(&gate->lock);
}

static void *fake_btc_main(void *arg)
{
    (void)arg;

    for (;;) {
        fake_job_t job;

        pthread_mutex_lock(&s_lock);
        while (s_job_count == 0) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
        job = s_jobs[s_job_head];
        s_job_head = (s_job_head + 1) % FAKE_JOBS_MAX;
        s_job_count--;
        if (!job.is_gap && (job.ev == ESP_SPP_WRITE_EVT)) {
            fake_clear_in_flight(job.param.write.handle);
        }
        pthread_mutex_unlock(&s_lock);

        if ((!job.is_gap) && (job.ev == ESP_SPP_INIT_EVT)) {
            fake_gate_enter(FAKE_GATE_INIT);
        } else if ((!job.is_gap) && (job.ev == ESP_SPP_START_EVT)) {
            fake_gate_enter(FAKE_GATE_START);
        }

        if (job.is_gap) {
            if (s_gap_cb != NULL) {
                s_gap_cb(job.gap_ev, &job.gap_param);
            }
        } else if (s_spp_cb != NULL) {
            s_spp_cb(job.ev, &job.param);
        }

        pthread_mutex_lock(&s_lock);
        s_done++;
        pthread_cond_broadcast(&s_cond);
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

/* 把 job 放进 BTC 队列；sync=true 时等它执行完毕。 */
static void fake_enqueue(const fake_job_t *job, bool sync)
{
    uint32_t target = 0U;

    ensure_cond();
    pthread_mutex_lock(&s_lock);
    if (!s_btc_started) {
        s_btc_started = (pthread_create(&s_btc, NULL, fake_btc_main, NULL) == 0);
    }
    if (s_btc_started) {
        while (s_job_count == FAKE_JOBS_MAX) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
        s_jobs[(s_job_head + s_job_count) % FAKE_JOBS_MAX] = *job;
        s_job_count++;
        s_posted++;
        target = s_posted;
        pthread_cond_broadcast(&s_cond);
        while (sync && (s_done < target)) {
            pthread_cond_wait(&s_cond, &s_lock);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

/* sync=true 时等回调执行完毕再返回（用例按需选择）。 */
static void fake_dispatch(esp_spp_cb_event_t ev, const esp_spp_cb_param_t *param, bool sync)
{
    fake_job_t job;

    memset(&job, 0, sizeof(job));
    job.ev = ev;
    job.param = *param;
    fake_enqueue(&job, sync);
}

static void fake_dispatch_gap(esp_bt_gap_cb_event_t ev, const esp_bt_gap_cb_param_t *param, bool sync)
{
    fake_job_t job;

    memset(&job, 0, sizeof(job));
    job.is_gap = true;
    job.gap_ev = ev;
    job.gap_param = *param;
    fake_enqueue(&job, sync);
}

static void fake_trace_add(const char *name)
{
    pthread_mutex_lock(&s_lock);
    if (s_trace_count < FAKE_TRACE_MAX) {
        s_trace[s_trace_count] = name;
        s_trace_count++;
    }
    pthread_mutex_unlock(&s_lock);
}

/* ---------------- 用例控制接口 ---------------- */

void fake_reset(void)
{
    ensure_cond();
    pthread_mutex_lock(&s_lock);
    s_init_mode = FAKE_STAGE_OK;
    s_start_mode = FAKE_STAGE_OK;
    s_write_mode = FAKE_WRITE_OK;
    s_write_block_ms = 0U;
    s_disconnect_block_ms = 0U;
    s_blocking_from_btc = false;
    s_deinit_return = ESP_OK;
    s_call_count = 0;
    s_all_len = 0U;
    s_overlap = false;
    s_disconnect_count = 0;
    s_disconnect_return_count = 0;
    s_disconnect_result = ESP_OK;
    s_write_return_count = 0;
    s_trace_count = 0;
    s_stage_sync = false;
    s_pin_reply_count = 0;
    s_ssp_confirm_count = 0;
    memset(s_last_gap_bda, 0, sizeof(s_last_gap_bda));
    memset(s_calls, 0, sizeof(s_calls));
    memset(s_all_data, 0, sizeof(s_all_data));
    pthread_mutex_unlock(&s_lock);

    for (int i = 0; i < (int)FAKE_GATE_COUNT; i++) {
        pthread_mutex_lock(&s_gates[i].lock);
        s_gates[i].state = 0;
        pthread_mutex_unlock(&s_gates[i].lock);
    }
    pthread_mutex_lock(&s_write_gate_lock);
    s_write_gate_state = 0;
    pthread_mutex_unlock(&s_write_gate_lock);
}

void fake_set_stage_sync(bool sync)
{
    pthread_mutex_lock(&s_lock);
    s_stage_sync = sync;
    pthread_mutex_unlock(&s_lock);
}

/* ---------------- 阶段门闩 ---------------- */

void fake_gate_arm(fake_gate_t which)
{
    if ((int)which >= (int)FAKE_GATE_COUNT) {
        return;
    }
    pthread_mutex_lock(&s_gates[which].lock);
    s_gates[which].state = 1;
    pthread_mutex_unlock(&s_gates[which].lock);
}

bool fake_gate_wait_entered(fake_gate_t which, uint32_t timeout_ms)
{
    struct timespec deadline;
    bool entered = false;

    if ((int)which >= (int)FAKE_GATE_COUNT) {
        return false;
    }
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)((timeout_ms % 1000U) * 1000000U);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&s_gates[which].lock);
    while (s_gates[which].state != 2) {
        if (pthread_cond_timedwait(&s_gates[which].cond, &s_gates[which].lock, &deadline) != 0) {
            break;
        }
    }
    entered = (s_gates[which].state == 2);
    pthread_mutex_unlock(&s_gates[which].lock);
    return entered;
}

void fake_gate_release(fake_gate_t which)
{
    if ((int)which >= (int)FAKE_GATE_COUNT) {
        return;
    }
    pthread_mutex_lock(&s_gates[which].lock);
    s_gates[which].state = 3;
    pthread_cond_broadcast(&s_gates[which].cond);
    pthread_mutex_unlock(&s_gates[which].lock);
}

void fake_write_gate_arm(void)
{
    pthread_mutex_lock(&s_write_gate_lock);
    s_write_gate_state = 1;
    pthread_mutex_unlock(&s_write_gate_lock);
}

bool fake_write_gate_wait_entered(uint32_t timeout_ms)
{
    struct timespec deadline;
    bool entered = false;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)((timeout_ms % 1000U) * 1000000U);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&s_write_gate_lock);
    while (s_write_gate_state != 2) {
        if (pthread_cond_timedwait(&s_write_gate_cond, &s_write_gate_lock, &deadline) != 0) {
            break;
        }
    }
    entered = (s_write_gate_state == 2);
    pthread_mutex_unlock(&s_write_gate_lock);
    return entered;
}

void fake_write_gate_release(void)
{
    pthread_mutex_lock(&s_write_gate_lock);
    if (s_write_gate_state == 2) {
        s_write_gate_state = 3;
        pthread_cond_broadcast(&s_write_gate_cond);
    }
    pthread_mutex_unlock(&s_write_gate_lock);
}

static void fake_write_gate_wait(void)
{
    pthread_mutex_lock(&s_write_gate_lock);
    if (s_write_gate_state == 1) {
        s_write_gate_state = 2;
        pthread_cond_broadcast(&s_write_gate_cond);
        while (s_write_gate_state == 2) {
            pthread_cond_wait(&s_write_gate_cond, &s_write_gate_lock);
        }
    }
    pthread_mutex_unlock(&s_write_gate_lock);
}

void fake_set_init_mode(fake_stage_t mode)
{
    pthread_mutex_lock(&s_lock);
    s_init_mode = mode;
    pthread_mutex_unlock(&s_lock);
}

void fake_set_start_mode(fake_stage_t mode)
{
    pthread_mutex_lock(&s_lock);
    s_start_mode = mode;
    pthread_mutex_unlock(&s_lock);
}

void fake_set_write_mode(fake_write_mode_t mode)
{
    pthread_mutex_lock(&s_lock);
    s_write_mode = mode;
    pthread_mutex_unlock(&s_lock);
}

void fake_set_write_block_ms(uint32_t ms)
{
    pthread_mutex_lock(&s_lock);
    s_write_block_ms = ms;
    pthread_mutex_unlock(&s_lock);
}

void fake_set_deinit_return(esp_err_t err)
{
    pthread_mutex_lock(&s_lock);
    s_deinit_return = err;
    pthread_mutex_unlock(&s_lock);
}

int fake_write_call_count(void)
{
    int count = 0;

    pthread_mutex_lock(&s_lock);
    count = s_call_count;
    pthread_mutex_unlock(&s_lock);
    return count;
}

int fake_write_return_count(void)
{
    int count = 0;

    pthread_mutex_lock(&s_lock);
    count = s_write_return_count;
    pthread_mutex_unlock(&s_lock);
    return count;
}

bool fake_wait_write_returns(int count, uint32_t timeout_ms)
{
    struct timespec deadline;
    bool ok = false;

    ensure_cond();
    abs_deadline(&deadline, timeout_ms);
    pthread_mutex_lock(&s_lock);
    while (s_write_return_count < count) {
        if (pthread_cond_timedwait(&s_cond, &s_lock, &deadline) != 0) {
            break;
        }
    }
    ok = (s_write_return_count >= count);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

static void fake_note_write_return(void)
{
    pthread_mutex_lock(&s_lock);
    s_write_return_count++;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
}

uint32_t fake_write_call_handle(int index)
{
    uint32_t handle = 0U;

    pthread_mutex_lock(&s_lock);
    if ((index >= 0) && (index < s_call_count)) {
        handle = s_calls[index].handle;
    }
    pthread_mutex_unlock(&s_lock);
    return handle;
}

int fake_write_call_len(int index)
{
    int len = -1;

    pthread_mutex_lock(&s_lock);
    if ((index >= 0) && (index < s_call_count)) {
        len = s_calls[index].len;
    }
    pthread_mutex_unlock(&s_lock);
    return len;
}

const uint8_t *fake_write_call_data(int index)
{
    const uint8_t *data = NULL;

    pthread_mutex_lock(&s_lock);
    if ((index >= 0) && (index < s_call_count)) {
        data = s_calls[index].data;
    }
    pthread_mutex_unlock(&s_lock);
    return data;
}

size_t fake_write_total_bytes(void)
{
    size_t len = 0U;

    pthread_mutex_lock(&s_lock);
    len = s_all_len;
    pthread_mutex_unlock(&s_lock);
    return len;
}

const uint8_t *fake_write_all_data(void)
{
    return s_all_data;
}

bool fake_write_overlapping_submit(void)
{
    bool overlap = false;

    pthread_mutex_lock(&s_lock);
    overlap = s_overlap;
    pthread_mutex_unlock(&s_lock);
    return overlap;
}

int fake_disconnect_count(void)
{
    int count = 0;

    pthread_mutex_lock(&s_lock);
    count = s_disconnect_count;
    pthread_mutex_unlock(&s_lock);
    return count;
}

uint32_t fake_disconnect_handle(int index)
{
    uint32_t handle = 0U;

    pthread_mutex_lock(&s_lock);
    if ((index >= 0) && (index < s_disconnect_count)) {
        handle = s_disconnect[index];
    }
    pthread_mutex_unlock(&s_lock);
    return handle;
}

void fake_trace_reset(void)
{
    pthread_mutex_lock(&s_lock);
    s_trace_count = 0;
    pthread_mutex_unlock(&s_lock);
}

int fake_trace_count(void)
{
    int count = 0;

    pthread_mutex_lock(&s_lock);
    count = s_trace_count;
    pthread_mutex_unlock(&s_lock);
    return count;
}

const char *fake_trace_at(int index)
{
    const char *name = "";

    pthread_mutex_lock(&s_lock);
    if ((index >= 0) && (index < s_trace_count)) {
        name = s_trace[index];
    }
    pthread_mutex_unlock(&s_lock);
    return name;
}

bool fake_wait_write_calls(int count, uint32_t timeout_ms)
{
    struct timespec deadline;
    bool ok = false;

    ensure_cond();
    abs_deadline(&deadline, timeout_ms);
    pthread_mutex_lock(&s_lock);
    while (s_call_count < count) {
        if (pthread_cond_timedwait(&s_cond, &s_lock, &deadline) != 0) {
            break;
        }
    }
    ok = (s_call_count >= count);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool fake_wait_disconnect_calls(int count, uint32_t timeout_ms)
{
    struct timespec deadline;
    bool ok = false;

    ensure_cond();
    abs_deadline(&deadline, timeout_ms);
    pthread_mutex_lock(&s_lock);
    while (s_disconnect_count < count) {
        if (pthread_cond_timedwait(&s_cond, &s_lock, &deadline) != 0) {
            break;
        }
    }
    ok = (s_disconnect_count >= count);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

static bool fake_wait_counter(const int *counter, int count, uint32_t timeout_ms)
{
    struct timespec deadline;
    bool ok = false;

    ensure_cond();
    abs_deadline(&deadline, timeout_ms);
    pthread_mutex_lock(&s_lock);
    while (*counter < count) {
        if (pthread_cond_timedwait(&s_cond, &s_lock, &deadline) != 0) {
            break;
        }
    }
    ok = (*counter >= count);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool fake_wait_pin_reply(int count, uint32_t timeout_ms)
{
    return fake_wait_counter(&s_pin_reply_count, count, timeout_ms);
}

bool fake_wait_ssp_confirm(int count, uint32_t timeout_ms)
{
    return fake_wait_counter(&s_ssp_confirm_count, count, timeout_ms);
}

int fake_pin_reply_count(void)
{
    int count = 0;

    pthread_mutex_lock(&s_lock);
    count = s_pin_reply_count;
    pthread_mutex_unlock(&s_lock);
    return count;
}

int fake_ssp_confirm_count(void)
{
    int count = 0;

    pthread_mutex_lock(&s_lock);
    count = s_ssp_confirm_count;
    pthread_mutex_unlock(&s_lock);
    return count;
}

const uint8_t *fake_last_gap_reply_bda(void)
{
    return s_last_gap_bda;
}

/* ---------------- 事件注入 ---------------- */

void fake_emit_write(uint32_t handle, int len, esp_spp_status_t status, bool cong)
{
    esp_spp_cb_param_t param;

    memset(&param, 0, sizeof(param));
    param.write.status = status;
    param.write.handle = handle;
    param.write.len = len;
    param.write.cong = cong;
    fake_dispatch(ESP_SPP_WRITE_EVT, &param, true);
}

void fake_emit_cong(uint32_t handle, bool cong)
{
    esp_spp_cb_param_t param;

    memset(&param, 0, sizeof(param));
    param.cong.status = ESP_SPP_SUCCESS;
    param.cong.handle = handle;
    param.cong.cong = cong;
    fake_dispatch(ESP_SPP_CONG_EVT, &param, true);
}

void fake_emit_srv_open(uint32_t handle, const uint8_t bda[6])
{
    esp_spp_cb_param_t param;

    memset(&param, 0, sizeof(param));
    param.srv_open.status = ESP_SPP_SUCCESS;
    param.srv_open.handle = handle;
    param.srv_open.fd = -1;
    memcpy(param.srv_open.rem_bda, bda, sizeof(esp_bd_addr_t));
    fake_dispatch(ESP_SPP_SRV_OPEN_EVT, &param, true);
}

void fake_emit_close(uint32_t handle)
{
    esp_spp_cb_param_t param;

    memset(&param, 0, sizeof(param));
    param.close.status = ESP_SPP_SUCCESS;
    param.close.handle = handle;
    param.close.async = false;
    fake_dispatch(ESP_SPP_CLOSE_EVT, &param, true);
}

void fake_emit_data(uint32_t handle, const uint8_t *data, uint16_t len)
{
    esp_spp_cb_param_t param;

    memset(&param, 0, sizeof(param));
    param.data_ind.status = ESP_SPP_SUCCESS;
    param.data_ind.handle = handle;
    param.data_ind.len = len;
    param.data_ind.data = (uint8_t *)data;
    /* 同步投递：返回时驱动已完成 xStreamBufferSend 拷贝，data 可以立刻失效。 */
    fake_dispatch(ESP_SPP_DATA_IND_EVT, &param, true);
}

void fake_emit_gap_pin_req(const uint8_t bda[6])
{
    esp_bt_gap_cb_param_t param;

    memset(&param, 0, sizeof(param));
    memcpy(param.pin_req.bda, bda, sizeof(esp_bd_addr_t));
    param.pin_req.pin_code_len = 0;
    fake_dispatch_gap(ESP_BT_GAP_PIN_REQ_EVT, &param, false);
}

void fake_emit_gap_cfm_req(const uint8_t bda[6])
{
    esp_bt_gap_cb_param_t param;

    memset(&param, 0, sizeof(param));
    memcpy(param.cfm_req.bda, bda, sizeof(esp_bd_addr_t));
    fake_dispatch_gap(ESP_BT_GAP_CFM_REQ_EVT, &param, false);
}

/* ---------------- 日志 / 错误名 ---------------- */

void fake_log(const char *level, const char *tag, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s (%s) ", level, tag);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    default:
        return "ESP_ERR_UNKNOWN";
    }
}

/* ---------------- BT 控制器 / Bluedroid / GAP ---------------- */

esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *cfg)
{
    (void)cfg;
    fake_trace_add("controller_init");
    return ESP_OK;
}

esp_err_t esp_bt_controller_enable(esp_bt_mode_t mode)
{
    (void)mode;
    fake_trace_add("controller_enable");
    return ESP_OK;
}

esp_err_t esp_bt_controller_disable(void)
{
    fake_trace_add("controller_disable");
    return ESP_OK;
}

esp_err_t esp_bt_controller_deinit(void)
{
    fake_trace_add("controller_deinit");
    return ESP_OK;
}

esp_err_t esp_bt_controller_mem_release(esp_bt_mode_t mode)
{
    (void)mode;
    fake_trace_add("mem_release");
    return ESP_OK;
}

esp_bt_controller_status_t esp_bt_controller_get_status(void)
{
    return ESP_BT_CONTROLLER_STATUS_ENABLED;
}

esp_err_t esp_bluedroid_init_with_cfg(esp_bluedroid_config_t *cfg)
{
    (void)cfg;
    fake_trace_add("bluedroid_init");
    return ESP_OK;
}

esp_err_t esp_bluedroid_enable(void)
{
    fake_trace_add("bluedroid_enable");
    return ESP_OK;
}

esp_err_t esp_bluedroid_disable(void)
{
    fake_trace_add("bluedroid_disable");
    return ESP_OK;
}

esp_err_t esp_bluedroid_deinit(void)
{
    fake_trace_add("bluedroid_deinit");
    return ESP_OK;
}

esp_bluedroid_status_t esp_bluedroid_get_status(void)
{
    return ESP_BLUEDROID_STATUS_ENABLED;
}

static const uint8_t s_local_addr[6] = {0x24U, 0x0AU, 0xC4U, 0x01U, 0x02U, 0x03U};

const uint8_t *esp_bt_dev_get_address(void)
{
    return s_local_addr;
}

esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t callback)
{
    s_gap_cb = callback;
    fake_trace_add("gap_cb");
    return ESP_OK;
}

esp_err_t esp_bt_gap_set_device_name(const char *name)
{
    (void)name;
    fake_note_blocking_api();
    fake_trace_add("set_device_name");
    return ESP_OK;
}

esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c_mode, esp_bt_discovery_mode_t d_mode)
{
    (void)c_mode;
    (void)d_mode;
    fake_note_blocking_api();
    fake_trace_add("set_scan_mode");
    return ESP_OK;
}

esp_err_t esp_bt_gap_set_pin(esp_bt_pin_type_t pin_type, uint8_t pin_code_len, esp_bt_pin_code_t pin_code)
{
    (void)pin_type;
    (void)pin_code_len;
    (void)pin_code;
    fake_trace_add("set_pin");
    return ESP_OK;
}

esp_err_t esp_bt_gap_set_security_param(esp_bt_sp_param_t param_type, void *value, uint8_t len)
{
    (void)param_type;
    (void)value;
    (void)len;
    fake_trace_add("set_security_param");
    return ESP_OK;
}

esp_err_t esp_bt_gap_pin_reply(esp_bd_addr_t bd_addr, bool accept, uint8_t pin_code_len, esp_bt_pin_code_t pin_code)
{
    (void)accept;
    (void)pin_code_len;
    (void)pin_code;
    fake_note_blocking_api();
    pthread_mutex_lock(&s_lock);
    s_pin_reply_count++;
    memcpy(s_last_gap_bda, bd_addr, sizeof(s_last_gap_bda));
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    fake_trace_add("pin_reply");
    return ESP_OK;
}

esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t bd_addr, bool accept)
{
    (void)accept;
    fake_note_blocking_api();
    pthread_mutex_lock(&s_lock);
    s_ssp_confirm_count++;
    memcpy(s_last_gap_bda, bd_addr, sizeof(s_last_gap_bda));
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    fake_trace_add("ssp_confirm_reply");
    return ESP_OK;
}

/* ---------------- SPP ---------------- */

esp_err_t esp_spp_register_callback(esp_spp_cb_t callback)
{
    s_spp_cb = callback;
    fake_trace_add("spp_cb");
    return ESP_OK;
}

esp_err_t esp_spp_enhanced_init(const esp_spp_cfg_t *cfg)
{
    esp_spp_cb_param_t param;
    fake_stage_t mode;
    bool sync = false;

    (void)cfg;
    fake_trace_add("spp_init");
    pthread_mutex_lock(&s_lock);
    mode = s_init_mode;
    sync = s_stage_sync;
    pthread_mutex_unlock(&s_lock);

    memset(&param, 0, sizeof(param));
    param.init.status = (mode == FAKE_STAGE_FAIL) ? ESP_SPP_FAILURE : ESP_SPP_SUCCESS;
    /* 默认异步：事件由假 BTC 线程投递，init 必须等事件而不是等函数返回。
     * fake_set_stage_sync(true) 时同步执行完，覆盖“事件早于等待”的顺序。 */
    if (mode != FAKE_STAGE_SILENT) {
        fake_dispatch(ESP_SPP_INIT_EVT, &param, sync);
    }
    return ESP_OK;
}

esp_err_t esp_spp_deinit(void)
{
    esp_spp_cb_param_t param;
    esp_err_t ret = ESP_OK;

    fake_trace_add("spp_deinit");
    pthread_mutex_lock(&s_lock);
    ret = s_deinit_return;
    pthread_mutex_unlock(&s_lock);
    if (ret != ESP_OK) {
        return ret;
    }
    memset(&param, 0, sizeof(param));
    param.uninit.status = ESP_SPP_SUCCESS;
    fake_dispatch(ESP_SPP_UNINIT_EVT, &param, false);
    return ESP_OK;
}

esp_err_t esp_spp_start_srv(esp_spp_sec_t sec_mask, esp_spp_role_t role, uint8_t local_scn, const char *name)
{
    esp_spp_cb_param_t param;
    fake_stage_t mode;
    bool sync = false;

    (void)sec_mask;
    (void)role;
    (void)local_scn;
    (void)name;
    fake_note_blocking_api();
    fake_trace_add("start_srv");
    pthread_mutex_lock(&s_lock);
    mode = s_start_mode;
    sync = s_stage_sync;
    pthread_mutex_unlock(&s_lock);

    memset(&param, 0, sizeof(param));
    param.start.status = (mode == FAKE_STAGE_FAIL) ? ESP_SPP_FAILURE : ESP_SPP_SUCCESS;
    param.start.handle = 0xFFFFU;
    param.start.scn = 1;
    if (mode != FAKE_STAGE_SILENT) {
        fake_dispatch(ESP_SPP_START_EVT, &param, sync);
    }
    return ESP_OK;
}

esp_err_t esp_spp_write(uint32_t handle, int len, uint8_t *p_data)
{
    fake_write_mode_t mode;
    uint32_t block_ms = 0U;
    bool overlap = false;

    if ((p_data == NULL) || (len <= 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    fake_note_blocking_api();
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_call_count; i++) {
        /* 同一句柄已有在途（未完成的）提交 → 记录并发提交（驱动串行化后应永不发生）。 */
        if (s_calls[i].in_flight && (s_calls[i].handle == handle)) {
            overlap = true;
        }
    }
    if (s_call_count < FAKE_CALLS_MAX) {
        fake_call_t *call = &s_calls[s_call_count];

        call->handle = handle;
        call->len = len;
        call->in_flight = true;
        memcpy(call->data, p_data, ((size_t)len > FAKE_CALL_BYTES) ? FAKE_CALL_BYTES : (size_t)len);
        s_call_count++;
        if (s_all_len + (size_t)len <= FAKE_ALL_BYTES) {
            memcpy(s_all_data + s_all_len, p_data, (size_t)len);
            s_all_len += (size_t)len;
        }
    }
    if (overlap) {
        s_overlap = true;
    }
    mode = s_write_mode;
    block_ms = s_write_block_ms;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);

    /* 用例可在“调用已记录、尚未返回”的窗口里注入 CLOSE。 */
    fake_write_gate_wait();

    if (mode == FAKE_WRITE_SUBMIT_ERROR) {
        pthread_mutex_lock(&s_lock);
        fake_clear_in_flight(handle);
        pthread_mutex_unlock(&s_lock);
        fake_note_write_return();
        return ESP_ERR_INVALID_STATE;
    }

    if (mode == FAKE_WRITE_BLOCK) {
        fake_sleep_ms(block_ms);
    }
    if (mode != FAKE_WRITE_HOLD) {
        esp_spp_status_t status = ESP_SPP_SUCCESS;
        esp_spp_cb_param_t param;
        int reported_len = len;

        if (mode == FAKE_WRITE_FAIL) {
            status = ESP_SPP_FAILURE;
        } else if (mode == FAKE_WRITE_NO_RESOURCE) {
            status = ESP_SPP_NO_RESOURCE;
        } else if (mode == FAKE_WRITE_SHORT) {
            /* RFCOMM 只接受了部分字节：status 成功但 len 小于提交长度（短写）。 */
            reported_len = len - 1;
        }
        /* 完成事件异步投递（与固件的 BTC 线程一致），不阻塞在调用者线程里。 */
        memset(&param, 0, sizeof(param));
        param.write.status = status;
        param.write.handle = handle;
        param.write.len = reported_len;
        param.write.cong = false;
        fake_dispatch(ESP_SPP_WRITE_EVT, &param, false);
    }
    fake_note_write_return();
    return ESP_OK;
}

esp_err_t esp_spp_disconnect(uint32_t handle)
{
    uint32_t block_ms = 0U;
    esp_err_t result = ESP_OK;

    fake_note_blocking_api();
    pthread_mutex_lock(&s_lock);
    if (s_disconnect_count < FAKE_CALLS_MAX) {
        s_disconnect[s_disconnect_count] = handle;
        s_disconnect_count++;
    }
    block_ms = s_disconnect_block_ms;
    result = s_disconnect_result;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    fake_gate_enter(FAKE_GATE_DISCONNECT);
    if (block_ms != 0U) {
        /* 模拟 btc_transfer_context 阻塞在 BTC 队列上（OSI_THREAD_MAX_TIMEOUT）。 */
        fake_sleep_ms(block_ms);
    }
    pthread_mutex_lock(&s_lock);
    ++s_disconnect_return_count;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    return result;
}

void fake_set_disconnect_block_ms(uint32_t ms)
{
    pthread_mutex_lock(&s_lock);
    s_disconnect_block_ms = ms;
    pthread_mutex_unlock(&s_lock);
}

bool fake_blocking_api_from_btc(void)
{
    bool from_btc = false;

    pthread_mutex_lock(&s_lock);
    from_btc = s_blocking_from_btc;
    pthread_mutex_unlock(&s_lock);
    return from_btc;
}

void fake_set_disconnect_result(esp_err_t err)
{
    pthread_mutex_lock(&s_lock);
    s_disconnect_result = err;
    pthread_mutex_unlock(&s_lock);
}

bool fake_wait_disconnect_returns(int count, uint32_t timeout_ms)
{
    struct timespec deadline;
    bool ok = false;
    ensure_cond();
    abs_deadline(&deadline, timeout_ms);
    pthread_mutex_lock(&s_lock);
    while (s_disconnect_return_count < count) {
        if (pthread_cond_timedwait(&s_cond, &s_lock, &deadline) != 0) {
            break;
        }
    }
    ok = (s_disconnect_return_count >= count);
    pthread_mutex_unlock(&s_lock);
    return ok;
}
