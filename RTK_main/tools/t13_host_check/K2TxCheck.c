// K2 非阻塞发送回归：真实驱动与有门闩的 SDK/RTOS 替身。
// 每个场景独立进程；pthread 只用于 host，禁止连接硬件。
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FakeIdf.h"
#include "SppServerPriv.h"
#include "SppServerStorage.h"

static int s_failures;
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr);                                                         \
            ++s_failures;                                                                                              \
        }                                                                                                              \
    } while (false)

/* --wrap 仅拦截测试指定的外部源，确定性停在 stream 大复制中，不给产品增加测试钩子。 */
static pthread_mutex_t s_copy_mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_copy_cond = PTHREAD_COND_INITIALIZER;
static const void *s_copy_source;
static bool s_copy_entered;
static bool s_copy_release;
/* GNU ld 要求固定的链接符号，C 标识符通过 asm 标签保持组件前缀。 */
void *k2_real_memcpy(void *dest, const void *source, size_t len) __asm__("__real_memcpy");
void *k2_wrap_memcpy(void *dest, const void *source, size_t len) __asm__("__wrap_memcpy");
void *k2_wrap_memcpy(void *dest, const void *source, size_t len)
{
    pthread_mutex_lock(&s_copy_mux);
    if ((source == s_copy_source) && (len != 0U)) {
        s_copy_entered = true;
        pthread_cond_broadcast(&s_copy_cond);
        while (!s_copy_release) {
            pthread_cond_wait(&s_copy_cond, &s_copy_mux);
        }
    }
    pthread_mutex_unlock(&s_copy_mux);
    return k2_real_memcpy(dest, source, len);
}

static bool event(spp_server_evt_type_t type, uint32_t handle)
{
    spp_server_evt_t evt = {0};
    for (int count = 0; count < 64; ++count) {
        if (!spp_server_poll_event(&evt, 1000U)) {
            return false;
        }
        if ((evt.type == type) && (evt.handle == handle)) {
            return true;
        }
    }
    return false;
}

static void open_session(uint32_t handle)
{
    const uint8_t addr[6] = {1U, 2U, 3U, 4U, 5U, 6U};
    fake_emit_srv_open(handle, addr);
    CHECK(event(SPP_SERVER_EVT_OPEN, handle));
}

static uint8_t s_rx_buf[SPP_SERVER_MAX_SESSIONS][SPP_SERVER_RX_BUF_SIZE];
static uint8_t s_reply_buf[SPP_SERVER_MAX_SESSIONS][SPP_SERVER_REPLY_CAPACITY];
static uint8_t s_stream_buf[SPP_SERVER_STREAM_CAPACITY];
static uint8_t s_merge_buf[SPP_SERVER_MERGE_CAPACITY];
static _Alignas(uint32_t) StackType_t s_worker_stack[SPP_SERVER_WORKER_STACK_BYTES / sizeof(StackType_t)];

static void init(void)
{
    spp_server_storage_t st;
    memset(&st, 0, sizeof(st));
    for (uint32_t i = 0U; i < SPP_SERVER_MAX_SESSIONS; i++) {
        st.rx_buf[i] = s_rx_buf[i];
        st.rx_buf_bytes[i] = sizeof(s_rx_buf[i]);
        st.reply_buf[i] = s_reply_buf[i];
        st.reply_buf_bytes[i] = sizeof(s_reply_buf[i]);
    }
    st.stream_buf = s_stream_buf;
    st.stream_buf_bytes = sizeof(s_stream_buf);
    st.merge_buf = s_merge_buf;
    st.merge_buf_bytes = sizeof(s_merge_buf);
    st.worker_stack = s_worker_stack;
    st.worker_stack_bytes = sizeof(s_worker_stack);

    fake_reset();
    CHECK(spp_server_bind_storage(&st) == ESP_OK);
    CHECK(spp_server_init("K2") == ESP_OK);
    open_session(0x11U);
    open_session(0x22U);
    fake_set_write_mode(FAKE_WRITE_HOLD);
}

static spp_server_tx_stats_t stats(void)
{
    spp_server_tx_stats_t result = {0};
    spp_server_tx_stats(&result);
    return result;
}

static bool settled(uint32_t inflight)
{
    for (int retry = 0; retry < 1000; ++retry) {
        if (stats().inflight == inflight) {
            return true;
        }
        fake_sleep_ms(1U);
    }
    return false;
}

static bool unused(void)
{
    for (int retry = 0; retry < 1000; ++retry) {
        bool idle = false;
        taskENTER_CRITICAL(&s_spp_mux);
        idle = (s_spp.sessions[0].users == 0U);
        taskEXIT_CRITICAL(&s_spp_mux);
        if (idle) {
            return true;
        }
        fake_sleep_ms(1U);
    }
    return false;
}

static void reply(uint32_t handle, uint8_t byte, size_t size)
{
    uint8_t data[SPP_SERVER_REPLY_RESERVE];
    spp_server_tx_token_t token = {0};
    memset(data, byte, sizeof(data));
    CHECK(spp_server_reply_reserve(handle, &token) == ESP_OK);
    CHECK(spp_server_tx_commit(&token, data, size) == ESP_OK);
}

static void capacity(void)
{
    uint8_t stream[SPP_SERVER_STREAM_CAPACITY];
    spp_server_tx_token_t token = {0};
    spp_server_tx_token_t old = {0};
    init();
    fake_emit_cong(0x11U, true);
    fake_emit_cong(0x22U, true);
    CHECK(spp_server_stream_reserve(0x22U, 1U, &token) == ESP_ERR_INVALID_STATE);
    CHECK(spp_server_stream_bind(0x11U, true) == ESP_OK);
    CHECK(spp_server_stream_bind(0x22U, true) == ESP_ERR_TIMEOUT);
    CHECK(spp_server_reply_reserve(0x11U, &token) == ESP_OK);
    old = token;
    spp_server_tx_cancel(&token);
    CHECK(spp_server_reply_ready(0x11U));
    CHECK(spp_server_reply_reserve(0x11U, &token) == ESP_OK);
    CHECK(spp_server_tx_commit(&old, stream, 1U) == ESP_ERR_INVALID_STATE);
    spp_server_tx_cancel(&token);
    for (int count = 0; count < 7; ++count) {
        reply(0x11U, 0xA1U, SPP_SERVER_REPLY_RESERVE);
    }
    CHECK(!spp_server_reply_ready(0x11U));
    CHECK(spp_server_reply_reserve(0x11U, &token) == ESP_ERR_TIMEOUT);
    reply(0x22U, 0xB2U, SPP_SERVER_REPLY_RESERVE);
    memset(stream, 0xC3, sizeof(stream));
    CHECK(spp_server_stream_reserve(0x11U, sizeof(stream), &token) == ESP_OK);
    CHECK(spp_server_tx_commit(&token, stream, sizeof(stream)) == ESP_OK);
    CHECK(spp_server_stream_reserve(0x11U, 1U, &token) == ESP_ERR_TIMEOUT);
    for (int count = 0; count < 10; ++count) {
        CHECK(spp_server_push_latest(0x11U, stream, 40U) == ESP_OK);
    }
    CHECK(stats().cancelled == 9U * 40U && stats().high_water >= 1960U);
    CHECK(fake_write_call_count() == 0);
    fake_emit_cong(0x11U, false);
    fake_emit_cong(0x22U, false);
    CHECK(fake_wait_write_calls(4, 1000U));
    CHECK(stats().inflight == SPP_SERVER_INFLIGHT_MAX);
    /* 两个 CONG 依次解除；轮转公平性在同时 ready 的 stream_order 场景验证。 */
    fake_sleep_ms(20U);
    CHECK(fake_write_call_count() == 4); /* 满信用不重试、不继续投递 SDK。 */
    fake_emit_write(fake_write_call_handle(0), fake_write_call_len(0), ESP_SPP_SUCCESS, false);
    CHECK(fake_wait_write_calls(5, 1000U));
    CHECK(stats().inflight <= SPP_SERVER_INFLIGHT_MAX);
}

static void stream_order(void)
{
    uint8_t stream[SPP_SERVER_STREAM_CAPACITY];
    spp_server_tx_token_t token = {0};
    init();
    CHECK(spp_server_stream_bind(0x11U, true) == ESP_OK);
    for (size_t index = 0; index < sizeof(stream); ++index) {
        stream[index] = (uint8_t)(index % 251U); /* 余数<=250。 */
    }
    CHECK(spp_server_stream_reserve(0x11U, sizeof(stream), &token) == ESP_OK);
    fake_write_gate_arm();
    CHECK(spp_server_tx_commit(&token, stream, sizeof(stream)) == ESP_OK);
    CHECK(fake_write_gate_wait_entered(1000U));
    reply(0x11U, 0xFFU, 26U);
    reply(0x22U, 0xBBU, 26U);
    fake_write_gate_release();
    size_t offset = 0U;
    bool alarm = false;
    for (int call = 0; call < 6; ++call) {
        CHECK(fake_wait_write_calls(call + 1, 1000U));
        const int len = fake_write_call_len(call);
        const uint32_t handle = fake_write_call_handle(call);
        if (handle == 0x11U) {
            if (offset < sizeof(stream)) {
                CHECK(len == 1024);
                CHECK(memcmp(fake_write_call_data(call), stream + offset, (size_t)len) == 0);
                offset += (size_t)len;
            } else {
                CHECK(len == 26 && fake_write_call_data(call)[0] == 0xFFU);
            }
        } else {
            CHECK(handle == 0x22U && call == 1);
            alarm = true;
        }
        fake_emit_write(handle, len, ESP_SPP_SUCCESS, false);
    }
    CHECK(settled(0U) && alarm && offset == sizeof(stream));
    CHECK(stats().completed == sizeof(stream) + 52U);
    /* 环头回绕：完整4096B记录仍能接纳，容量不受记录计数上限限制。 */
    CHECK(spp_server_stream_reserve(0x11U, 37U, &token) == ESP_OK);
    CHECK(spp_server_tx_commit(&token, stream, 37U) == ESP_OK);
    CHECK(fake_wait_write_calls(7, 1000U));
    fake_emit_write(0x11U, 37, ESP_SPP_SUCCESS, false);
    CHECK(settled(0U));
    CHECK(spp_server_stream_reserve(0x11U, sizeof(stream), &token) == ESP_OK);
    CHECK(spp_server_tx_commit(&token, stream, sizeof(stream)) == ESP_OK);
    for (int call = 7; call < 11; ++call) {
        CHECK(fake_wait_write_calls(call + 1, 1000U));
        CHECK(memcmp(fake_write_call_data(call), stream + (size_t)(call - 7) * 1024U, 1024U) == 0);
        fake_emit_write(0x11U, 1024, ESP_SPP_SUCCESS, false);
    }
    CHECK(settled(0U));
    /* 两段提交：源环跨尾的记录按序拼成一条，目标环同样跨尾（head=37）。 */
    CHECK(spp_server_stream_reserve(0x11U, sizeof(stream), &token) == ESP_OK);
    CHECK(spp_server_tx_commit2(&token, stream, 1000U, stream + 1000U, 0U) == ESP_ERR_INVALID_ARG);
    CHECK(spp_server_tx_commit2(&token, stream, 1000U, stream + 1000U, sizeof(stream)) == ESP_ERR_INVALID_ARG);
    CHECK(spp_server_tx_commit2(&token, stream, 1000U, stream + 1000U, sizeof(stream) - 1000U) == ESP_OK);
    for (int call = 11; call < 15; ++call) {
        CHECK(fake_wait_write_calls(call + 1, 1000U));
        CHECK(memcmp(fake_write_call_data(call), stream + (size_t)(call - 11) * 1024U, 1024U) == 0);
        fake_emit_write(0x11U, 1024, ESP_SPP_SUCCESS, false);
    }
    CHECK(settled(0U));
}

static void early_close(void)
{
    init();
    fake_write_gate_arm();
    reply(0x11U, 0xAAU, 26U);
    CHECK(fake_write_gate_wait_entered(1000U));
    fake_emit_write(0x11U, 26, ESP_SPP_SUCCESS, false);
    CHECK(stats().inflight == 1U && stats().completed == 0U); /* submitting 保留，不能提前复用 merge。 */
    fake_emit_write(0U, -1, ESP_SPP_NO_RESOURCE, false);
    CHECK(stats().anon_write_fail == 1U && !spp_server_reply_ready(0x11U) && spp_server_reply_ready(0x22U));
    fake_emit_close(0x11U);
    CHECK(event(SPP_SERVER_EVT_CLOSE, 0x11U));
    CHECK(stats().inflight == 1U);
    const uint8_t addr[6] = {0};
    fake_emit_srv_open(0x11U, addr);
    CHECK(!spp_server_reply_ready(0x11U));
    reply(0x22U, 0xBBU, 26U);
    CHECK(fake_write_call_count() == 1);
    fake_write_gate_release();
    CHECK(fake_wait_write_calls(2, 1000U));
    CHECK(fake_write_call_data(0)[0] == 0xAAU && fake_write_call_data(1)[0] == 0xBBU);
    CHECK(unused());
    fake_emit_write(0x22U, 26, ESP_SPP_SUCCESS, false);
    CHECK(settled(0U));
    fake_emit_write(0x11U, 26, ESP_SPP_SUCCESS, false);
    CHECK(stats().stray_write == 1U && stats().completed == 26U);
}

static void anonymous(void)
{
    init();
    fake_emit_cong(0x22U, true);
    reply(0x11U, 0xAAU, 26U);
    reply(0x22U, 0xBBU, 26U);
    CHECK(fake_wait_write_calls(1, 1000U));
    CHECK(fake_wait_write_returns(1, 1000U));
    fake_emit_write(0U, -1, ESP_SPP_NO_CONNECTION, false);
    CHECK(fake_wait_disconnect_calls(1, 1000U));
    CHECK(fake_disconnect_handle(0) == 0x11U && stats().inflight == 1U);
    CHECK(!spp_server_reply_ready(0x11U) && spp_server_reply_ready(0x22U));
    fake_emit_close(0x11U);
    CHECK(event(SPP_SERVER_EVT_CLOSE, 0x11U));
    CHECK(settled(0U) && stats().completed == 0U);
    fake_emit_write(0U, -1, ESP_SPP_NO_RESOURCE, false); /* 无候选，只累计匿名计数。 */
    CHECK(stats().anon_write_fail == 2U && fake_disconnect_count() == 1);
    fake_emit_cong(0x22U, false);
    CHECK(fake_wait_write_calls(2, 1000U));
    fake_emit_write(0x22U, 26, ESP_SPP_SUCCESS, false);
    CHECK(settled(0U) && stats().completed == 26U);
}

static void errors(fake_write_mode_t mode)
{
    spp_server_tx_token_t token = {0};
    const uint8_t push_byte = 0x5AU;
    init();
    if (mode == FAKE_WRITE_SUBMIT_ERROR) {
        fake_emit_cong(0x11U, true);
        /* 1400B > 1024B 拼包上限：同步提交失败后不得继续提交剩余 376B 尾部。 */
        for (int count = 0; count < 5; ++count) {
            reply(0x11U, 0xAAU, SPP_SERVER_REPLY_RESERVE);
        }
        fake_set_write_mode(mode);
        fake_emit_cong(0x11U, false);
    } else if (mode == FAKE_WRITE_SHORT) {
        fake_emit_cong(0x11U, true);
        for (int count = 0; count < 5; ++count) {
            reply(0x11U, 0xAAU, SPP_SERVER_REPLY_RESERVE);
        }
        fake_write_gate_arm();
        fake_emit_cong(0x11U, false);
        CHECK(fake_wait_write_calls(1, 1000U));
        CHECK(fake_write_gate_wait_entered(1000U));
        fake_emit_write(0x11U, SPP_SERVER_MERGE_CAPACITY - 1, ESP_SPP_SUCCESS, false);
        fake_write_gate_release();
    } else {
        fake_set_write_mode(mode);
        reply(0x11U, 0xAAU, 26U);
    }
    CHECK(fake_wait_write_returns(1, 1000U));
    for (int retry = 0; retry < 1000 && stats().errors == 0U; ++retry) {
        fake_sleep_ms(1U);
    }
    CHECK(fake_wait_disconnect_calls(1, 1000U));
    CHECK(fake_disconnect_handle(0) == 0x11U);
    CHECK(stats().errors == 1U && stats().completed == 0U && settled(0U));
    CHECK(fake_write_call_count() == 1 && !spp_server_reply_ready(0x11U) && spp_server_reply_ready(0x22U));
    CHECK(spp_server_reply_reserve(0x11U, &token) == ESP_ERR_TIMEOUT);
    CHECK(spp_server_push_latest(0x11U, &push_byte, 1U) == ESP_ERR_NOT_FOUND);
    fake_set_write_mode(FAKE_WRITE_OK);
    reply(0x22U, 0xBBU, 26U);
    CHECK(fake_wait_write_returns(2, 1000U));
    CHECK(settled(0U) && stats().completed == 26U && fake_write_call_count() == 2);
    fake_emit_close(0x11U);
    CHECK(event(SPP_SERVER_EVT_CLOSE, 0x11U));
    if ((mode == FAKE_WRITE_SUBMIT_ERROR) || (mode == FAKE_WRITE_SHORT)) {
        CHECK(stats().cancelled == (5U * SPP_SERVER_REPLY_RESERVE) - SPP_SERVER_MERGE_CAPACITY);
    }
    open_session(0x11U);
    reply(0x11U, 0xCCU, 26U);
    CHECK(fake_wait_write_returns(3, 1000U));
    CHECK(settled(0U) && stats().completed == 52U);
}

typedef struct {
    spp_server_tx_token_t token;
    const uint8_t *data;
    esp_err_t result;
} copy_arg_t;

static void *commit_main(void *opaque)
{
    copy_arg_t *arg = opaque;
    arg->result = spp_server_tx_commit(&arg->token, arg->data, SPP_SERVER_STREAM_CAPACITY);
    return NULL;
}

static void copy_cancel(bool close_session)
{
    uint8_t data[SPP_SERVER_STREAM_CAPACITY];
    copy_arg_t arg = {0};
    pthread_t thread;
    init();
    memset(data, 0xCC, sizeof(data));
    CHECK(spp_server_stream_bind(0x11U, true) == ESP_OK);
    CHECK(spp_server_stream_reserve(0x11U, sizeof(data), &arg.token) == ESP_OK);
    arg.data = data;
    s_copy_source = data;
    CHECK(pthread_create(&thread, NULL, commit_main, &arg) == 0);
    pthread_mutex_lock(&s_copy_mux);
    while (!s_copy_entered) {
        pthread_cond_wait(&s_copy_cond, &s_copy_mux);
    }
    pthread_mutex_unlock(&s_copy_mux);
    CHECK(fake_write_call_count() == 0); /* 发布以前 worker 看不到半条记录。 */
    if (close_session) {
        fake_emit_close(0x11U);
        CHECK(event(SPP_SERVER_EVT_CLOSE, 0x11U));
        const uint8_t addr[6] = {0};
        fake_emit_srv_open(0x11U, addr);
        CHECK(!spp_server_reply_ready(0x11U));
    } else {
        CHECK(spp_server_stream_bind(0x11U, false) == ESP_OK);
    }
    CHECK(spp_server_stream_bind(0x22U, true) == ESP_ERR_TIMEOUT);
    pthread_mutex_lock(&s_copy_mux);
    s_copy_release = true;
    pthread_cond_broadcast(&s_copy_cond);
    pthread_mutex_unlock(&s_copy_mux);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(arg.result == ESP_ERR_INVALID_STATE);
    CHECK(spp_server_stream_bind(0x22U, true) == ESP_OK);
    CHECK(fake_write_call_count() == 0);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "capacity") == 0) {
        capacity();
    } else if (strcmp(argv[1], "stream_order") == 0) {
        stream_order();
    } else if (strcmp(argv[1], "early_close") == 0) {
        early_close();
    } else if (strcmp(argv[1], "anonymous") == 0) {
        anonymous();
    } else if (strcmp(argv[1], "short") == 0) {
        errors(FAKE_WRITE_SHORT);
    } else if (strcmp(argv[1], "submit_error") == 0) {
        errors(FAKE_WRITE_SUBMIT_ERROR);
    } else if (strcmp(argv[1], "failure") == 0) {
        errors(FAKE_WRITE_FAIL);
    } else if (strcmp(argv[1], "copy_close") == 0) {
        copy_cancel(true);
    } else if (strcmp(argv[1], "copy_unbind") == 0) {
        copy_cancel(false);
    } else {
        return 2;
    }
    printf("%s %s; session=%zu state=%zu inflight=%zu payload=%u\n", s_failures ? "FAIL" : "PASS", argv[1],
           sizeof(spp_server_session_t), sizeof(spp_server_state_t), sizeof(spp_server_inflight_t),
           2U * SPP_SERVER_REPLY_CAPACITY + SPP_SERVER_STREAM_CAPACITY + SPP_SERVER_MERGE_CAPACITY);
    return s_failures ? 1 : 0;
}
