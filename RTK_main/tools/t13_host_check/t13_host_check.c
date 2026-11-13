/* T13 驱动级回归：直接链接真实的 components/spp_server/spp_server_*.c，用真实阻塞语义的
 * FreeRTOS 替身和可编排的假 IDF（BTC 线程投递回调）覆盖 QA-E-REVIEW 第 1–4 项与复审反馈。
 * 每个场景独立进程运行（驱动只有 init、没有 deinit 公共接口），用法：t13_host_check <场景名>。
 *
 * 调度一律用门闩/条件变量（fake_gate_*、fake_read_pause_*、fake_wait_*），不用 sleep 猜进度；
 * 只有验证“预算到期”的用例需要真实时间，等待时长按契约上界断言。
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h" /* 确定性读暂停钩子（fake_read_pause_*） */
#include "fake_idf.h"
#include "spp_server.h"

static int s_fail;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
            s_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

static const uint8_t s_bda[SPP_SERVER_ADDR_LEN] = {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU};

static uint32_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL));
}

static void bring_up(void)
{
    fake_reset();
    CHECK(spp_server_init("RTK_T13") == ESP_OK);
}

/* 轮询事件队列，跳过其它类型，直到拿到指定事件或超时。 */
static bool wait_event(spp_server_evt_type_t type, uint32_t handle, uint32_t timeout_ms)
{
    const uint32_t deadline = now_ms() + timeout_ms;

    for (;;) {
        spp_server_evt_t evt;
        const uint32_t now = now_ms();
        const uint32_t left = (now < deadline) ? (deadline - now) : 0U;

        if (!spp_server_poll_event(&evt, left)) {
            return false;
        }
        if ((evt.type == type) && (evt.handle == handle)) {
            return true;
        }
    }
}

static void open_session(uint32_t handle)
{
    fake_emit_srv_open(handle, s_bda);
    CHECK(wait_event(SPP_SERVER_EVT_OPEN, handle, 1000U));
}

/* ---------------- 线程参数 ---------------- */

typedef struct {
    const char *name;
    esp_err_t err;
    atomic_bool done;
} init_arg_t;

static void *init_main(void *arg)
{
    init_arg_t *ia = (init_arg_t *)arg;

    ia->err = spp_server_init(ia->name);
    atomic_store_explicit(&ia->done, true, memory_order_release);
    return NULL;
}

typedef struct {
    uint32_t handle;
    const uint8_t *data;
    size_t len;
    uint32_t timeout_ms;
    esp_err_t err;
    uint32_t elapsed_ms;
    atomic_bool done;
} writer_arg_t;

static void *writer_main(void *arg)
{
    writer_arg_t *wa = (writer_arg_t *)arg;
    const uint32_t start = now_ms();

    wa->err = spp_server_write(wa->handle, wa->data, wa->len, wa->timeout_ms);
    wa->elapsed_ms = now_ms() - start;
    atomic_store_explicit(&wa->done, true, memory_order_release);
    return NULL;
}

typedef struct {
    uint32_t handle;
    uint8_t buf[8];
    size_t got;
    atomic_bool done;
} reader_arg_t;

static void *reader_main(void *arg)
{
    reader_arg_t *ra = (reader_arg_t *)arg;

    ra->got = spp_server_read(ra->handle, ra->buf, sizeof(ra->buf));
    atomic_store_explicit(&ra->done, true, memory_order_release);
    return NULL;
}

/* ---------------- 初始化阶段 ---------------- */

/* #2：INIT/START 是异步的。用门闩让两个事件都停在回调之前，确认 init 线程在事件未到时
 * 不返回；放行后才成功返回，且启动/设名/可发现都在任务上下文里。 */
static void sc_init_waits_stages(void)
{
    pthread_t thread;
    init_arg_t ia;
    int idx_spp_init = -1;
    int idx_start_srv = -1;
    int idx_name = -1;
    int idx_scan = -1;

    fake_reset();
    memset(&ia, 0, sizeof(ia));
    atomic_init(&ia.done, false);
    ia.name = "RTK_T13";
    ia.err = ESP_FAIL;
    fake_gate_arm(FAKE_GATE_INIT);
    fake_gate_arm(FAKE_GATE_START);

    CHECK(pthread_create(&thread, NULL, init_main, &ia) == 0);
    /* INIT 回调被门闩挡住：init 不得返回。 */
    CHECK(fake_gate_wait_entered(FAKE_GATE_INIT, 2000U));
    CHECK(!atomic_load_explicit(&ia.done, memory_order_acquire));
    fake_gate_release(FAKE_GATE_INIT);

    /* START 回调被门闩挡住：init 仍不得返回。 */
    CHECK(fake_gate_wait_entered(FAKE_GATE_START, 2000U));
    CHECK(!atomic_load_explicit(&ia.done, memory_order_acquire));
    fake_gate_release(FAKE_GATE_START);

    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(atomic_load_explicit(&ia.done, memory_order_acquire));
    CHECK(ia.err == ESP_OK);
    /* 启动服务端、设名、可发现模式都不能出现在 BTC 回调线程里。 */
    CHECK(!fake_blocking_api_from_btc());

    for (int i = 0; i < fake_trace_count(); i++) {
        const char *t = fake_trace_at(i);

        if (strcmp(t, "spp_init") == 0) {
            idx_spp_init = i;
        } else if (strcmp(t, "start_srv") == 0) {
            idx_start_srv = i;
        } else if (strcmp(t, "set_device_name") == 0) {
            idx_name = i;
        } else if (strcmp(t, "set_scan_mode") == 0) {
            idx_scan = i;
        }
    }
    CHECK(idx_spp_init >= 0);
    CHECK(idx_spp_init < idx_start_srv);
    CHECK(idx_start_srv < idx_name);
    CHECK(idx_name < idx_scan);
    /* 重复 init 返回 INVALID_STATE。 */
    CHECK(spp_server_init("RTK_T13") == ESP_ERR_INVALID_STATE);
}

/* #2：同步完成顺序——回调在发起 API 内就执行完，等待方看到的位已置位。 */
static void sc_init_sync_stage_order(void)
{
    fake_reset();
    fake_set_stage_sync(true);
    CHECK(spp_server_init("RTK_T13") == ESP_OK);
    CHECK(!fake_blocking_api_from_btc());
}

/* #2：INIT 阶段异步失败 → 返回错误且按逆序释放，未初始化时写返回 INVALID_STATE。 */
static void sc_init_failure_rollback(void)
{
    static const char *const expected[] = {
        "mem_release",      "controller_init",    "controller_enable",
        "bluedroid_init",   "bluedroid_enable",   "gap_cb",
        "spp_cb",           "set_security_param", "set_pin",
        "spp_init",         "spp_deinit",         "bluedroid_disable",
        "bluedroid_deinit", "controller_disable", "controller_deinit",
    };
    const uint8_t payload[4] = {1U, 2U, 3U, 4U};

    fake_reset();
    fake_set_init_mode(FAKE_STAGE_FAIL);
    CHECK(spp_server_init("RTK_T13") == ESP_FAIL);
    CHECK(fake_trace_count() == (int)(sizeof(expected) / sizeof(expected[0])));
    for (int i = 0; i < (int)(sizeof(expected) / sizeof(expected[0])); i++) {
        CHECK(strcmp(fake_trace_at(i), expected[i]) == 0);
    }
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 100U) == ESP_ERR_INVALID_STATE);
}

/* #2：INIT 成功但 START 阶段异步失败 → init 必须返回 ESP_FAIL 并回滚。 */
static void sc_init_start_status_fail(void)
{
    bool saw_deinit = false;

    fake_reset();
    fake_set_start_mode(FAKE_STAGE_FAIL);
    CHECK(spp_server_init("RTK_T13") == ESP_FAIL);
    for (int i = 0; i < fake_trace_count(); i++) {
        if (strcmp(fake_trace_at(i), "spp_deinit") == 0) {
            saw_deinit = true;
        }
    }
    CHECK(saw_deinit);
    CHECK(fake_trace_count() > 0);
    CHECK(strcmp(fake_trace_at(fake_trace_count() - 1), "controller_deinit") == 0);
}

/* #2：START 事件缺失 → init 按预算超时失败，不能假装成功。 */
static void sc_init_start_timeout(void)
{
    uint32_t start = 0U;

    fake_reset();
    fake_set_start_mode(FAKE_STAGE_SILENT);
    start = now_ms();
    CHECK(spp_server_init("RTK_T13") == ESP_ERR_TIMEOUT);
    CHECK((now_ms() - start) >= 4900U);
    CHECK(fake_trace_count() > 0);
    CHECK(strcmp(fake_trace_at(fake_trace_count() - 1), "controller_deinit") == 0);
}

/* ---------------- 会话槽 ---------------- */

/* #1/#D：两个槽都被占用时，新连接不在回调里断开，也不延后接纳：由发送任务拒绝。 */
static void sc_open_no_slot_rejects_from_task(void)
{
    const uint8_t payload[4] = {1U, 2U, 3U, 4U};

    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    fake_emit_srv_open(0x33U, s_bda);
    /* 拒绝命令由发送任务执行（回调线程不能调用 esp_spp_disconnect）。 */
    CHECK(fake_wait_disconnect_calls(1, 2000U));
    CHECK(fake_disconnect_count() == 1);
    CHECK(fake_disconnect_handle(0) == 0x33U);
    CHECK(!fake_blocking_api_from_btc());
    CHECK(!wait_event(SPP_SERVER_EVT_OPEN, 0x33U, 0U));
    /* 被拒绝的连接不会在旧槽空出后被重新接纳：0x33 没有会话。 */
    CHECK(spp_server_write(0x33U, payload, sizeof(payload), 100U) == ESP_ERR_NOT_FOUND);
    CHECK(fake_write_call_count() == 0);
}

/* #1：旧读者未退出前槽不得 reset/复用。用真实 spp_server_read 停在 shim 的确定性暂停点，
 * CLOSE 释放 in_use 但 users>0，新连接必须被拒绝；读者继续读到原数据（证明没有 reset），
 * 且拒绝之后不会出现延后接纳。 */
static void sc_slot_held_rejects_new_connect(void)
{
    pthread_t thread;
    reader_arg_t ra;
    static const uint8_t rx_data[4] = {0x10U, 0x20U, 0x30U, 0x40U};
    const uint8_t payload[4] = {1U, 2U, 3U, 4U};

    bring_up();
    open_session(0x11U);
    open_session(0x22U); /* 先占满两个槽，0x33 只能被拒绝 */
    fake_emit_data(0x11U, rx_data, sizeof(rx_data));

    memset(&ra, 0, sizeof(ra));
    atomic_init(&ra.done, false);
    ra.handle = 0x11U;
    fake_read_pause_arm();
    CHECK(pthread_create(&thread, NULL, reader_main, &ra) == 0);
    CHECK(fake_read_pause_wait_entered(2000U));

    /* 读者已登记 users=1、停在取数之前：CLOSE 只释放 in_use，槽仍不可复用/不可 reset。 */
    fake_emit_close(0x11U);
    fake_emit_srv_open(0x33U, s_bda);
    CHECK(fake_wait_disconnect_calls(1, 2000U));
    CHECK(fake_disconnect_handle(0) == 0x33U);
    CHECK(!fake_blocking_api_from_btc());
    CHECK(!wait_event(SPP_SERVER_EVT_OPEN, 0x33U, 0U));

    fake_read_pause_release();
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(atomic_load_explicit(&ra.done, memory_order_acquire));
    CHECK(ra.got == sizeof(rx_data));
    CHECK(memcmp(ra.buf, rx_data, sizeof(rx_data)) == 0); /* 缓冲未被 reset */

    /* 旧读者退出后也不得把已拒绝的 0x33 重新发布为会话。 */
    CHECK(!wait_event(SPP_SERVER_EVT_OPEN, 0x33U, 200U));
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 100U) == ESP_ERR_NOT_FOUND);
    CHECK(fake_write_call_count() == 0);
}

/* ---------------- 发送 ---------------- */

/* #3：write 必须等真实 WRITE_EVT，而不是入队就返回。 */
static void sc_write_waits_completion(void)
{
    pthread_t thread;
    writer_arg_t wa;
    uint8_t payload[32];

    bring_up();
    open_session(0x11U);
    memset(payload, 0x5A, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_HOLD);
    memset(&wa, 0, sizeof(wa));
    atomic_init(&wa.done, false);
    wa.handle = 0x11U;
    wa.data = payload;
    wa.len = sizeof(payload);
    wa.timeout_ms = 1500U;
    CHECK(pthread_create(&thread, NULL, writer_main, &wa) == 0);
    CHECK(fake_wait_write_calls(1, 1000U));
    CHECK(!atomic_load_explicit(&wa.done, memory_order_acquire)); /* 完成事件未到 → 仍在等 */
    fake_emit_write(0x11U, (int)sizeof(payload), ESP_SPP_SUCCESS, false);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(wa.err == ESP_OK);
    CHECK(wa.elapsed_ms < 1500U);
}

/* #3：公共契约不限制 len；大于提交缓冲的负载必须分块，且不并发提交。 */
static void sc_write_chunks_large_payload(void)
{
    uint8_t payload[1200];

    bring_up();
    open_session(0x11U);
    for (size_t i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7U);
    }
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 3000U) == ESP_OK);
    CHECK(fake_write_call_count() == 3);
    CHECK(fake_write_call_len(0) == 512);
    CHECK(fake_write_call_len(1) == 512);
    CHECK(fake_write_call_len(2) == 176);
    CHECK(fake_write_total_bytes() == sizeof(payload));
    CHECK(memcmp(fake_write_all_data(), payload, sizeof(payload)) == 0);
    CHECK(!fake_write_overlapping_submit());
}

/* #4：提交卡在 esp_spp_write 里时，调用方按剩余预算超时返回，不被拖住。 */
static void sc_write_timeout_budget(void)
{
    uint8_t payload[100];
    uint32_t start = 0U;

    bring_up();
    open_session(0x11U);
    memset(payload, 0xA5, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_BLOCK);
    fake_set_write_block_ms(400U);
    start = now_ms();
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 100U) == ESP_ERR_TIMEOUT);
    CHECK((now_ms() - start) >= 90U);
    CHECK((now_ms() - start) <= 350U);
    /* 提交发生在发送任务里：等它真正进入 esp_spp_write，不能靠调度时序猜。 */
    CHECK(fake_wait_write_calls(1, 1000U));
    /* 等提交线程退出阻塞并投递迟到的完成；之后同一会话还能继续用。 */
    fake_sleep_ms(500U);
    fake_set_write_mode(FAKE_WRITE_OK);
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 1000U) == ESP_OK);
}

/* #3/K2：WRITE_EVT 的 status 与回报长度错误必须返回失败，并请求断开受影响会话而不影响对端。 */
static void sc_write_status_propagation(void)
{
    static const fake_write_mode_t modes[] = {FAKE_WRITE_FAIL, FAKE_WRITE_NO_RESOURCE, FAKE_WRITE_SHORT};
    uint8_t payload[8];

    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    memset(payload, 0x11, sizeof(payload));
    for (size_t idx = 0U; idx < sizeof(modes) / sizeof(modes[0]); ++idx) {
        fake_set_write_mode(modes[idx]);
        CHECK(spp_server_write(0x11U, payload, sizeof(payload), 1000U) == ESP_FAIL);
        CHECK(fake_wait_disconnect_calls((int)idx + 1, 1000U));
        CHECK(fake_disconnect_handle((int)idx) == 0x11U);
        fake_emit_close(0x11U);
        CHECK(wait_event(SPP_SERVER_EVT_CLOSE, 0x11U, 1000U));
        fake_set_write_mode(FAKE_WRITE_OK);
        CHECK(spp_server_write(0x22U, payload, sizeof(payload), 1000U) == ESP_OK);
        open_session(0x11U);
        CHECK(spp_server_write(0x11U, payload, sizeof(payload), 1000U) == ESP_OK);
    }
}

/* #3/K2：提交阶段直接失败时把错误返回给调用方、不等待完成事件，并断开受影响会话。 */
static void sc_write_submit_error(void)
{
    uint8_t payload[8];

    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    memset(payload, 0x22, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_SUBMIT_ERROR);
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 1000U) == ESP_ERR_INVALID_STATE);
    CHECK(fake_wait_disconnect_calls(1, 1000U));
    CHECK(fake_disconnect_handle(0) == 0x11U);
    fake_emit_close(0x11U);
    CHECK(wait_event(SPP_SERVER_EVT_CLOSE, 0x11U, 1000U));
    fake_set_write_mode(FAKE_WRITE_OK);
    CHECK(spp_server_write(0x22U, payload, sizeof(payload), 1000U) == ESP_OK);
    open_session(0x11U);
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 1000U) == ESP_OK);
}

/* #3：拥塞时按预算等待解除，不在拥塞期间提交。 */
static void sc_write_congestion(void)
{
    uint8_t payload[8];

    bring_up();
    open_session(0x11U);
    memset(payload, 0x33, sizeof(payload));
    fake_emit_cong(0x11U, true);
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 100U) == ESP_ERR_TIMEOUT);
    CHECK(fake_write_call_count() == 0);
    fake_emit_cong(0x11U, false);
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 1000U) == ESP_OK);
    CHECK(fake_write_call_count() == 1);
}

/* #1/#3：写进行中会话被关闭 → 立即以 INVALID_STATE 结束，不等到超时。 */
static void sc_write_close_midflight(void)
{
    pthread_t thread;
    writer_arg_t wa;
    uint8_t payload[16];

    bring_up();
    open_session(0x11U);
    memset(payload, 0x44, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_HOLD);
    memset(&wa, 0, sizeof(wa));
    atomic_init(&wa.done, false);
    wa.handle = 0x11U;
    wa.data = payload;
    wa.len = sizeof(payload);
    wa.timeout_ms = 2000U;
    CHECK(pthread_create(&thread, NULL, writer_main, &wa) == 0);
    CHECK(fake_wait_write_calls(1, 1000U));
    CHECK(!atomic_load_explicit(&wa.done, memory_order_acquire));
    fake_emit_close(0x11U);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(wa.err == ESP_ERR_INVALID_STATE);
    CHECK(wa.elapsed_ms < 800U);
}

/* #1：CLOSE 即取消完成。CB 模式 CLOSE 会清空未完成 TX 且不合成 WRITE，因此关闭后
 * 不投递任何旧 WRITE，另一会话的写也必须成功；旧槽也必须可复用。 */
static void sc_write_cancel_by_close(void)
{
    pthread_t thread;
    writer_arg_t wa;
    uint8_t payload[8];

    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    memset(payload, 0x91, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_HOLD);
    memset(&wa, 0, sizeof(wa));
    atomic_init(&wa.done, false);
    wa.handle = 0x11U;
    wa.data = payload;
    wa.len = sizeof(payload);
    wa.timeout_ms = 1500U;
    CHECK(pthread_create(&thread, NULL, writer_main, &wa) == 0);
    CHECK(fake_wait_write_calls(1, 1000U));
    /* 等 API 返回：请求已进入 IDF（HOLD 不投递完成）。驱动随后的收尾很快完成；
     * 即使 CLOSE 恰好落在收尾前，也会走“取消后由 worker 释放”的路径。 */
    CHECK(fake_wait_write_returns(1, 1000U));

    /* CLOSE 取消在途请求；不投递任何 0x11 WRITE。 */
    fake_emit_close(0x11U);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(wa.err == ESP_ERR_INVALID_STATE);
    CHECK(wa.elapsed_ms < 800U);

    fake_set_write_mode(FAKE_WRITE_OK);
    CHECK(spp_server_write(0x22U, payload, sizeof(payload), 1000U) == ESP_OK);
    CHECK(wait_event(SPP_SERVER_EVT_CLOSE, 0x11U, 100U));
    /* CLOSE 已交付，旧槽可被新连接复用。 */
    open_session(0x33U);
    CHECK(spp_server_write(0x33U, payload, sizeof(payload), 1000U) == ESP_OK);
    /* 迟到的旧句柄事件不得影响其他会话。 */
    fake_emit_write(0x11U, (int)sizeof(payload), ESP_SPP_SUCCESS, false);
    CHECK(spp_server_write(0x22U, payload, sizeof(payload), 1000U) == ESP_OK);
    CHECK(fake_write_call_count() == 4);
}

/* #1：CLOSE 发生在 worker 仍在 esp_spp_write 内时，静态 tx_buf 不可重用；owner 要等
 * API 返回后才释放。返回后旧请求的匿名 NO_CONNECTION 必须被忽略，不能结算新请求。 */
static void sc_write_cancel_during_submit(void)
{
    pthread_t thread_a;
    pthread_t thread_b;
    writer_arg_t wa;
    writer_arg_t wb;
    uint8_t payload[8];

    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    memset(payload, 0xA1, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_HOLD);
    fake_write_gate_arm();
    memset(&wa, 0, sizeof(wa));
    atomic_init(&wa.done, false);
    wa.handle = 0x11U;
    wa.data = payload;
    wa.len = sizeof(payload);
    wa.timeout_ms = 2000U;
    CHECK(pthread_create(&thread_a, NULL, writer_main, &wa) == 0);
    CHECK(fake_wait_write_calls(1, 1000U));
    CHECK(fake_write_gate_wait_entered(1000U)); /* worker 停在 esp_spp_write 内部 */

    fake_emit_close(0x11U); /* tx_submitting=true：只标记取消，不释放 owner */

    memset(&wb, 0, sizeof(wb));
    atomic_init(&wb.done, false);
    wb.handle = 0x22U;
    wb.data = payload;
    wb.len = sizeof(payload);
    wb.timeout_ms = 2000U;
    CHECK(pthread_create(&thread_b, NULL, writer_main, &wb) == 0);
    /* B 已排队，但旧调用仍占用静态缓冲/owner：不得提交。 */
    fake_sleep_ms(100U);
    CHECK(fake_write_call_count() == 1);

    fake_write_gate_release();
    /* API 返回后释放取消的 owner，B 才进入 IDF。 */
    CHECK(fake_wait_write_calls(2, 2000U));
    CHECK(fake_write_call_handle(1) == 0x22U);

    /* 有明确旧句柄的迟到失败只计 stray，不得结算 B；匿名失败另测候选断开。 */
    fake_emit_write(0x11U, (int)sizeof(payload), ESP_SPP_NO_CONNECTION, false);
    fake_emit_write(0x22U, (int)sizeof(payload), ESP_SPP_SUCCESS, false);

    CHECK(pthread_join(thread_a, NULL) == 0);
    CHECK(pthread_join(thread_b, NULL) == 0);
    CHECK(wa.err == ESP_ERR_INVALID_STATE);
    CHECK(wb.err == ESP_OK);
    CHECK(fake_write_call_count() == 2);
}

/* #1：会话关闭并复用后，旧句柄的迟到完成事件不得污染新会话；成功/失败都不需要。 */
static void sc_stale_completion_after_reopen(void)
{
    uint8_t payload[8];

    bring_up();
    open_session(0x11U);
    memset(payload, 0x55, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_HOLD);
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 100U) == ESP_ERR_TIMEOUT);
    CHECK(fake_wait_write_returns(1, 1000U));
    fake_emit_close(0x11U);
    CHECK(wait_event(SPP_SERVER_EVT_CLOSE, 0x11U, 1000U));
    open_session(0x22U); /* API 已返回且 CLOSE 已消费，允许复用。 */
    fake_set_write_mode(FAKE_WRITE_OK);
    CHECK(spp_server_write(0x22U, payload, sizeof(payload), 1000U) == ESP_OK);
    /* 旧句柄的迟到成功/失败都应被忽略。 */
    fake_emit_write(0x11U, (int)sizeof(payload), ESP_SPP_SUCCESS, false);
    fake_emit_write(0x11U, (int)sizeof(payload), ESP_SPP_FAILURE, false);
    CHECK(spp_server_write(0x22U, payload, sizeof(payload), 1000U) == ESP_OK);
    CHECK(fake_write_call_count() == 3);
}

/* 匿名 NO_RESOURCE 不能归因到任意一笔：保留在途信用，断开所有在途候选，CLOSE 清理。 */
static void sc_write_unmatched_no_resource(void)
{
    pthread_t thread_a;
    pthread_t thread_b;
    writer_arg_t wa = {0};
    writer_arg_t wb = {0};
    uint8_t payload[8];
    spp_server_tx_stats_t stats = {0};

    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    memset(payload, 0x66, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_HOLD);
    atomic_init(&wa.done, false);
    atomic_init(&wb.done, false);
    wa.handle = 0x11U;
    wb.handle = 0x22U;
    wa.data = payload;
    wb.data = payload;
    wa.len = sizeof(payload);
    wb.len = sizeof(payload);
    wa.timeout_ms = 1500U;
    wb.timeout_ms = 1500U;
    CHECK(pthread_create(&thread_a, NULL, writer_main, &wa) == 0);
    CHECK(pthread_create(&thread_b, NULL, writer_main, &wb) == 0);
    CHECK(fake_wait_write_calls(2, 1000U));
    CHECK(fake_wait_write_returns(2, 1000U));
    fake_emit_write(0U, -1, ESP_SPP_NO_RESOURCE, false);
    CHECK(fake_wait_disconnect_calls(2, 1000U));
    spp_server_tx_stats(&stats);
    CHECK(stats.anon_write_fail == 1U && stats.inflight == 2U);
    CHECK(!atomic_load(&wa.done) && !atomic_load(&wb.done));
    CHECK((fake_disconnect_handle(0) == 0x11U && fake_disconnect_handle(1) == 0x22U) ||
          (fake_disconnect_handle(0) == 0x22U && fake_disconnect_handle(1) == 0x11U));
    fake_emit_close(0x11U);
    fake_emit_close(0x22U);
    CHECK(pthread_join(thread_a, NULL) == 0);
    CHECK(pthread_join(thread_b, NULL) == 0);
    CHECK(wa.err == ESP_ERR_INVALID_STATE && wb.err == ESP_ERR_INVALID_STATE);
    spp_server_tx_stats(&stats);
    CHECK(stats.inflight == 0U && stats.completed == 0U);
}

/* #3/D：旧句柄关闭后，它的迟到失败事件不得结算新句柄的在途请求。
 * 新请求必须一直等到自己的成功 WRITE_EVT 才返回 ESP_OK。 */
static void sc_stale_failure_does_not_settle_new(void)
{
    pthread_t thread;
    writer_arg_t wa;
    uint8_t payload[8];

    bring_up();
    open_session(0x11U);
    fake_emit_close(0x11U);
    open_session(0x22U);
    memset(payload, 0x88, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_HOLD);
    memset(&wa, 0, sizeof(wa));
    atomic_init(&wa.done, false);
    wa.handle = 0x22U;
    wa.data = payload;
    wa.len = sizeof(payload);
    wa.timeout_ms = 1500U;
    CHECK(pthread_create(&thread, NULL, writer_main, &wa) == 0);
    CHECK(fake_wait_write_calls(1, 1000U));

    /* 旧句柄的迟到失败：忽略。 */
    fake_emit_write(0x11U, (int)sizeof(payload), ESP_SPP_FAILURE, false);
    /* 只有新句柄自己的正确完成才能让它成功返回。 */
    fake_emit_write(0x22U, (int)sizeof(payload), ESP_SPP_SUCCESS, false);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(wa.err == ESP_OK);
    CHECK(fake_write_call_count() == 1);
}

/* #C：整次 write 的预算覆盖分块认领/复制/排队。第一块在预算边界完成（或超时）后，
 * 不得再提交第二块；旧实现只在等待前检查预算，快速完成时会越过预算继续提交。 */
static void sc_write_chunk_budget_shared(void)
{
    pthread_t thread;
    writer_arg_t wa;
    uint8_t payload[1024];

    bring_up();
    open_session(0x11U);
    memset(payload, 0x77, sizeof(payload));
    fake_set_write_mode(FAKE_WRITE_BLOCK);
    fake_set_write_block_ms(400U); /* 与预算同值：第一块完成不可能早于预算 */
    memset(&wa, 0, sizeof(wa));
    atomic_init(&wa.done, false);
    wa.handle = 0x11U;
    wa.data = payload;
    wa.len = sizeof(payload);
    wa.timeout_ms = 400U;
    CHECK(pthread_create(&thread, NULL, writer_main, &wa) == 0);
    CHECK(fake_wait_write_calls(1, 2000U)); /* 第一块确实已进入 IDF，不是没提交 */
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(wa.done);
    CHECK(wa.err == ESP_ERR_TIMEOUT);
    /* 512 字节之后不再提交第二块：块间预算已耗尽。 */
    CHECK(fake_write_call_count() == 1);
    CHECK(fake_write_total_bytes() == 512U);
    CHECK(wa.elapsed_ms >= 350U);
    CHECK(wa.elapsed_ms < 1500U);
}

/* B：合法参数后先确认会话。未知句柄即使预算为 0 也必须 NOT_FOUND；已知会话 0 预算超时且不提交。 */
static void sc_unknown_handle_errors(void)
{
    uint8_t payload[4] = {0U, 1U, 2U, 3U};
    uint8_t local[SPP_SERVER_ADDR_LEN];
    static const uint8_t expected[SPP_SERVER_ADDR_LEN] = {0x24U, 0x0AU, 0xC4U, 0x01U, 0x02U, 0x03U};

    bring_up();
    CHECK(spp_server_write(0x99U, payload, sizeof(payload), 100U) == ESP_ERR_NOT_FOUND);
    CHECK(spp_server_write(0x99U, NULL, 0U, 100U) == ESP_ERR_INVALID_ARG);
    CHECK(spp_server_write(0x99U, payload, sizeof(payload), 0U) == ESP_ERR_NOT_FOUND);
    CHECK(spp_server_disconnect(0x99U) == ESP_ERR_NOT_FOUND);
    CHECK(spp_server_get_local_addr(local) == ESP_OK);
    CHECK(memcmp(local, expected, sizeof(expected)) == 0);
    CHECK(spp_server_dropped_events() == 0U);

    open_session(0x11U);
    CHECK(spp_server_write(0x11U, payload, sizeof(payload), 0U) == ESP_ERR_TIMEOUT);
    CHECK(fake_write_call_count() == 0);
}

/* D：GAP PIN/CFM 应答不得在 BTC 回调线程里执行；地址必须原样带到任务上下文。 */
static void sc_gap_replies_from_task(void)
{
    bring_up();
    fake_emit_gap_pin_req(s_bda);
    CHECK(fake_wait_pin_reply(1, 2000U));
    CHECK(fake_pin_reply_count() == 1);
    CHECK(memcmp(fake_last_gap_reply_bda(), s_bda, SPP_SERVER_ADDR_LEN) == 0);
    CHECK(!fake_blocking_api_from_btc());

    fake_emit_gap_cfm_req(s_bda);
    CHECK(fake_wait_ssp_confirm(1, 2000U));
    CHECK(fake_ssp_confirm_count() == 1);
    CHECK(!fake_blocking_api_from_btc());
}

/* 数据突发与未 poll 的重连不能让生命周期积压无界；跨槽严格按回调顺序交付。 */
static void sc_lifecycle_burst_churn(void)
{
    uint8_t bytes[1024] = {0};
    spp_server_evt_t evt;
    const spp_server_evt_type_t types[] = {SPP_SERVER_EVT_OPEN,    SPP_SERVER_EVT_OPEN, SPP_SERVER_EVT_DATA,
                                           SPP_SERVER_EVT_RX_DROP, SPP_SERVER_EVT_DATA, SPP_SERVER_EVT_RX_DROP,
                                           SPP_SERVER_EVT_CLOSE,   SPP_SERVER_EVT_CLOSE};
    const uint32_t handles[] = {0x11U, 0x22U, 0x11U, 0x11U, 0x22U, 0x22U, 0x22U, 0x11U};
    bring_up();
    fake_emit_srv_open(0x11U, s_bda);
    fake_emit_srv_open(0x22U, s_bda);
    for (int idx = 0; idx < 40; ++idx) {
        fake_emit_data(0x11U, bytes, sizeof(bytes));
    }
    for (int idx = 0; idx < 40; ++idx) {
        fake_emit_data(0x22U, bytes, sizeof(bytes));
    }
    fake_emit_close(0x22U);
    fake_emit_close(0x11U);
    /* 生命周期未交付：槽不能被任意多次断连重连占用，拒绝不产生应用 CLOSE。 */
    for (int idx = 0; idx < 40; ++idx) {
        fake_emit_srv_open(0x33U, s_bda);
        fake_emit_close(0x33U);
    }
    for (size_t idx = 0U; idx < sizeof(types) / sizeof(types[0]); ++idx) {
        CHECK(spp_server_poll_event(&evt, 0U));
        CHECK(evt.type == types[idx]);
        CHECK(evt.handle == handles[idx]);
    }
    CHECK(!spp_server_poll_event(&evt, 0U));
    CHECK(spp_server_dropped_events() == 0U);
    /* CLOSE 已交付后同数字句柄可以重新接纳，旧 CLOSE 不会在它之后出现。 */
    open_session(0x11U);
    fake_emit_data(0x11U, bytes, 1U);
    CHECK(wait_event(SPP_SERVER_EVT_DATA, 0x11U, 100U));
    CHECK(spp_server_read(0x11U, bytes, sizeof(bytes)) == 1U);
    CHECK(!spp_server_poll_event(&evt, 0U));
}

static void sc_drop_counts_by_handle(void)
{
    uint8_t bytes[SPP_SERVER_RX_BUF_SIZE] = {0};
    spp_server_evt_t evt;
    uint32_t drops_a = 0U;
    uint32_t drops_b = 0U;
    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    fake_emit_data(0x11U, bytes, sizeof(bytes));
    fake_emit_data(0x22U, bytes, sizeof(bytes));
    const size_t accepted_a = spp_server_read(0x11U, bytes, sizeof(bytes));
    const size_t accepted_b = spp_server_read(0x22U, bytes, sizeof(bytes));
    fake_emit_data(0x11U, bytes, sizeof(bytes));
    fake_emit_data(0x22U, bytes, sizeof(bytes));
    fake_emit_data(0x11U, bytes, 17U);
    fake_emit_data(0x22U, bytes, 31U);
    fake_emit_data(0x11U, bytes, 19U);
    fake_emit_close(0x11U);
    fake_emit_close(0x22U);
    while (spp_server_poll_event(&evt, 0U)) {
        if (evt.type == SPP_SERVER_EVT_RX_DROP) {
            if (evt.handle == 0x11U) {
                CHECK(drops_a == 0U);
                drops_a = evt.dropped;
            } else {
                CHECK(evt.handle == 0x22U);
                CHECK(drops_b == 0U);
                drops_b = evt.dropped;
            }
        }
    }
    CHECK(drops_a == (2U * sizeof(bytes) - 2U * accepted_a + 36U));
    CHECK(drops_b == (2U * sizeof(bytes) - 2U * accepted_b + 31U));
    CHECK(spp_server_dropped_events() == 0U);
}

/* SDK 可无限等投递，但调用者只登记；普通请求不会占用拒绝控制表。 */
static void sc_disconnect_async_and_reject(void)
{
    uint8_t byte = 42U;
    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    fake_gate_arm(FAKE_GATE_DISCONNECT);
    const uint32_t start = now_ms();
    CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    CHECK((now_ms() - start) < 100U);
    CHECK(fake_gate_wait_entered(FAKE_GATE_DISCONNECT, 1000U));
    for (int idx = 0; idx < 20; ++idx) {
        CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    }
    fake_emit_srv_open(0x33U, s_bda);
    fake_emit_data(0x22U, &byte, 1U);
    CHECK(wait_event(SPP_SERVER_EVT_DATA, 0x22U, 100U));
    CHECK(spp_server_read(0x22U, &byte, 1U) == 1U);
    CHECK(fake_disconnect_count() == 1);
    fake_gate_release(FAKE_GATE_DISCONNECT);
    CHECK(fake_wait_disconnect_returns(2, 1000U));
    CHECK(fake_disconnect_handle(1) == 0x33U);
    CHECK(!fake_wait_disconnect_calls(3, 150U));
    CHECK(!fake_blocking_api_from_btc());
    fake_emit_close(0x11U);
    CHECK(wait_event(SPP_SERVER_EVT_CLOSE, 0x11U, 100U));
    CHECK(spp_server_disconnect(0x11U) == ESP_ERR_NOT_FOUND);
}

static void sc_disconnect_retry(void)
{
    bring_up();
    open_session(0x11U);
    fake_set_disconnect_result(ESP_FAIL);
    const uint32_t start = now_ms();
    CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    CHECK(fake_wait_disconnect_returns(1, 1000U));
    CHECK(!wait_event(SPP_SERVER_EVT_CLOSE, 0x11U, 0U));
    fake_set_disconnect_result(ESP_OK);
    CHECK(fake_wait_disconnect_returns(2, 1000U));
    CHECK((now_ms() - start) >= 90U);
    CHECK((now_ms() - start) < 1000U);
    CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    CHECK(!fake_wait_disconnect_calls(3, 150U));
    fake_emit_close(0x11U);
    CHECK(wait_event(SPP_SERVER_EVT_CLOSE, 0x11U, 100U));
    open_session(0x11U);
    CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    CHECK(fake_wait_disconnect_returns(3, 1000U));
}

/* CLOSE 已被消费而 SDK 仍阻塞时，也不能在另一空槽接纳相同 handle。 */
static void sc_disconnect_reuse_across_slots(void)
{
    spp_server_evt_t evt;
    bring_up();
    open_session(0x11U); /* 第二槽刻意空闲 */
    fake_gate_arm(FAKE_GATE_DISCONNECT);
    CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    CHECK(fake_gate_wait_entered(FAKE_GATE_DISCONNECT, 1000U));
    fake_emit_close(0x11U);
    CHECK(wait_event(SPP_SERVER_EVT_CLOSE, 0x11U, 100U));
    fake_emit_srv_open(0x11U, s_bda);
    CHECK(!spp_server_poll_event(&evt, 0U));
    fake_gate_release(FAKE_GATE_DISCONNECT);
    CHECK(fake_wait_disconnect_returns(2, 1000U));
    fake_emit_close(0x11U); /* 拒绝连接不发布 CLOSE */
    CHECK(!spp_server_poll_event(&evt, 0U));
    open_session(0x11U);
    CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    CHECK(fake_wait_disconnect_returns(3, 1000U));
}

/* 前槽已空、后槽已有拒绝时必须先全表查重，否则重复 OPEN 会耗尽控制表。 */
static void sc_reject_dedup_hole(void)
{
    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    fake_gate_arm(FAKE_GATE_DISCONNECT);
    CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    CHECK(fake_gate_wait_entered(FAKE_GATE_DISCONNECT, 1000U));
    fake_emit_srv_open(0x33U, s_bda);
    fake_emit_srv_open(0x44U, s_bda);
    fake_emit_close(0x33U);
    for (int idx = 0; idx < 12; ++idx) {
        fake_emit_srv_open(0x44U, s_bda);
    }
    fake_emit_srv_open(0x55U, s_bda);
    fake_emit_srv_open(0x66U, s_bda);
    fake_gate_release(FAKE_GATE_DISCONNECT);
    CHECK(fake_wait_disconnect_returns(4, 1000U));
    int n44 = 0;
    int n55 = 0;
    int n66 = 0;
    for (int idx = 1; idx < 4; ++idx) {
        const uint32_t handle = fake_disconnect_handle(idx);
        n44 += (handle == 0x44U);
        n55 += (handle == 0x55U);
        n66 += (handle == 0x66U);
    }
    CHECK(n44 == 1 && n55 == 1 && n66 == 1);
    CHECK(!fake_wait_disconnect_calls(5, 150U));
}

/* 旧拒绝正在 SDK 中返回失败，CLOSE 后出现的新 OPEN 必须保留后继拒绝。 */
static void sc_reject_rearm_after_close(void)
{
    spp_server_evt_t evt;
    bring_up();
    open_session(0x11U);
    open_session(0x22U);
    fake_set_disconnect_result(ESP_FAIL);
    fake_gate_arm(FAKE_GATE_DISCONNECT);
    fake_emit_srv_open(0x33U, s_bda);
    CHECK(fake_gate_wait_entered(FAKE_GATE_DISCONNECT, 1000U));
    fake_emit_close(0x33U);
    fake_emit_srv_open(0x33U, s_bda);
    fake_emit_srv_open(0x33U, s_bda);
    CHECK(!spp_server_poll_event(&evt, 0U));
    fake_set_disconnect_result(ESP_OK);
    fake_gate_release(FAKE_GATE_DISCONNECT);
    CHECK(fake_wait_disconnect_returns(2, 1000U));
    CHECK(fake_disconnect_handle(0) == 0x33U && fake_disconnect_handle(1) == 0x33U);
    CHECK(!fake_wait_disconnect_calls(3, 150U));
    fake_emit_close(0x33U);
    CHECK(!spp_server_poll_event(&evt, 0U));
    CHECK(spp_server_disconnect(0x11U) == ESP_OK);
    CHECK(fake_wait_disconnect_returns(3, 1000U));
}

typedef struct {
    const char *name;
    void (*run)(void);
} scenario_t;

static void sc_rx_wake_cb(void *ctx)
{
    atomic_int *calls = (atomic_int *)ctx;
    /* 故意查询 rx_ready（内部会拿 s_spp_mux），验证回调确在 portMUX 外执行、不会自锁。 */
    (void)spp_server_any_rx_ready();
    atomic_fetch_add(calls, 1);
}

/* K3：验证 RX 唤醒钩子在 portMUX 外触发、level/stats 统计准确、且队列满时不丢数据与掉包状态。 */
static void sc_rx_wake_level_and_stats(void)
{
    atomic_int hook_calls = 0;
    spp_server_set_rx_wake_hook(sc_rx_wake_cb, &hook_calls);
    bring_up();
    open_session(0x11U);
    CHECK(atomic_load(&hook_calls) >= 1); /* OPEN 也立即唤醒 */

    const int before_data = atomic_load(&hook_calls);
    uint8_t payload[600];
    memset(payload, 0x5AU, sizeof(payload));
    fake_emit_data(0x11U, payload, sizeof(payload));
    CHECK(atomic_load(&hook_calls) == before_data + 1);
    CHECK(spp_server_rx_ready(0x11U));
    CHECK(spp_server_any_rx_ready());

    spp_server_rx_stats_t rxs;
    spp_server_rx_stats(&rxs);
    CHECK(rxs.sessions[0].used == sizeof(payload));
    CHECK(rxs.sessions[0].high_water == sizeof(payload));
    CHECK(rxs.sessions[0].dropped_bytes == 0U);
    CHECK(rxs.sessions[0].rx_events == 1U);
    CHECK(rxs.sessions[0].drop_events == 0U);

    /* 读走后 level 归零，但高水位保留 */
    uint8_t rx_buf[600];
    CHECK(spp_server_read(0x11U, rx_buf, sizeof(rx_buf)) == sizeof(payload));
    spp_server_evt_t evt;
    while (spp_server_poll_event(&evt, 0U)) {
    }
    CHECK(!spp_server_rx_ready(0x11U));
    CHECK(!spp_server_any_rx_ready());
    spp_server_rx_stats(&rxs);
    CHECK(rxs.sessions[0].used == 0U);
    CHECK(rxs.sessions[0].high_water == sizeof(payload));

    /* 灌满 rx StreamBuffer 触发 RX_DROP，验证累计丢字节与 drop_events。 */
    uint8_t big[SPP_SERVER_RX_BUF_SIZE];
    memset(big, 0xA5U, sizeof(big));
    fake_emit_data(0x11U, big, sizeof(big));
    fake_emit_data(0x11U, payload, 128U);
    CHECK(spp_server_rx_ready(0x11U));
    spp_server_rx_stats(&rxs);
    CHECK(rxs.sessions[0].dropped_bytes == 128U);
    CHECK(rxs.sessions[0].drop_events == 1U);
    CHECK(rxs.sessions[0].high_water == SPP_SERVER_RX_BUF_SIZE);
    CHECK(wait_event(SPP_SERVER_EVT_RX_DROP, 0x11U, 1000U));
    spp_server_set_rx_wake_hook(NULL, NULL);
}

static const scenario_t s_scenarios[] = {
    {"rx_wake_level_and_stats", sc_rx_wake_level_and_stats},
    {"reject_rearm_after_close", sc_reject_rearm_after_close},
    {"lifecycle_burst_churn", sc_lifecycle_burst_churn},
    {"drop_counts_by_handle", sc_drop_counts_by_handle},
    {"disconnect_async_and_reject", sc_disconnect_async_and_reject},
    {"disconnect_retry", sc_disconnect_retry},
    {"disconnect_reuse_across_slots", sc_disconnect_reuse_across_slots},
    {"reject_dedup_hole", sc_reject_dedup_hole},

    {"init_waits_stages", sc_init_waits_stages},
    {"init_sync_stage_order", sc_init_sync_stage_order},
    {"init_failure_rollback", sc_init_failure_rollback},
    {"init_start_status_fail", sc_init_start_status_fail},
    {"init_start_timeout", sc_init_start_timeout},
    {"open_no_slot_rejects_from_task", sc_open_no_slot_rejects_from_task},
    {"slot_held_rejects_new_connect", sc_slot_held_rejects_new_connect},
    {"write_waits_completion", sc_write_waits_completion},
    {"write_chunks_large_payload", sc_write_chunks_large_payload},
    {"write_timeout_budget", sc_write_timeout_budget},
    {"write_status_propagation", sc_write_status_propagation},
    {"write_submit_error", sc_write_submit_error},
    {"write_congestion", sc_write_congestion},
    {"write_close_midflight", sc_write_close_midflight},
    {"write_cancel_by_close", sc_write_cancel_by_close},
    {"write_cancel_during_submit", sc_write_cancel_during_submit},
    {"stale_completion_after_reopen", sc_stale_completion_after_reopen},
    {"write_unmatched_no_resource", sc_write_unmatched_no_resource},
    {"write_stale_failure_ignored", sc_stale_failure_does_not_settle_new},
    {"write_chunk_budget_shared", sc_write_chunk_budget_shared},
    {"unknown_handle_errors", sc_unknown_handle_errors},
    {"gap_replies_from_task", sc_gap_replies_from_task},
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("可用场景:\n");
        for (size_t i = 0U; i < (sizeof(s_scenarios) / sizeof(s_scenarios[0])); i++) {
            printf("  %s\n", s_scenarios[i].name);
        }
        return 2;
    }
    for (size_t i = 0U; i < (sizeof(s_scenarios) / sizeof(s_scenarios[0])); i++) {
        if (strcmp(argv[1], s_scenarios[i].name) == 0) {
            s_scenarios[i].run();
            if (s_fail == 0) {
                printf("PASS %s\n", argv[1]);
                return 0;
            }
            printf("FAIL %s (%d checks)\n", argv[1], s_fail);
            return 1;
        }
    }
    printf("未知场景: %s\n", argv[1]);
    return 2;
}
