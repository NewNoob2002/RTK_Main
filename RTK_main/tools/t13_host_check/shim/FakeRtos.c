/* 主机回归用的 FreeRTOS 原语实现（真实阻塞语义，见 shim/inc/freertos/FreeRTOS.h 的说明）。 */
#include "freertos/FreeRTOS.h"

#include <stdatomic.h>
#include <string.h>
#include <time.h>

/* 目标板上任务用 pcTaskGetName(NULL) 找自己；host 上用线程局部变量记录当前任务句柄，
 * 让 ulTaskNotifyTake() 作用于调用线程所属的任务（与固件语义一致）。 */
__thread StaticTask_t *fake_current_task = NULL;

/* 临界区：目标板上是关中断 + 自旋锁；host 上等价物是“不可重入的全局互斥量”，
 * 因此若被测代码在临界区里调用阻塞 API（违反项目规则）会立刻死锁，回归能发现。 */
static pthread_mutex_t s_crit = PTHREAD_MUTEX_INITIALIZER;

void fake_crit_enter(void)
{
    pthread_mutex_lock(&s_crit);
}

/* 临界区退出钩子：FakeIdf 用它把 esp_spp_disconnect “返回”的可见时刻推迟到调用线程
 * 完成随后第一段结算临界区之后（MT1，原因见 FakeIdf.c 的 esp_spp_disconnect）。 */
static _Atomic(void (*)(void)) s_crit_exit_hook = NULL;

void fake_crit_exit_hook_install(void (*hook)(void))
{
    atomic_store_explicit(&s_crit_exit_hook, hook, memory_order_release);
}

void fake_crit_exit(void)
{
    pthread_mutex_unlock(&s_crit);
    void (*hook)(void) = atomic_load_explicit(&s_crit_exit_hook, memory_order_acquire);
    if (hook != NULL) {
        hook();
    }
}

static void fake_cond_init(pthread_cond_t *cond)
{
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

static struct timespec fake_deadline(TickType_t ticks)
{
    struct timespec ts;
    uint64_t ns = 0U;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ns = (uint64_t)ts.tv_nsec + ((uint64_t)ticks * 1000000ULL);
    ts.tv_sec += (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    return ts;
}

/* 返回 true 表示被唤醒（可能是虚假唤醒），false 表示超时。 */
static bool fake_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *lock, TickType_t ticks)
{
    struct timespec deadline;

    if (ticks == portMAX_DELAY) {
        pthread_cond_wait(cond, lock);
        return true;
    }
    deadline = fake_deadline(ticks);
    return pthread_cond_timedwait(cond, lock, &deadline) == 0;
}

static _Atomic uint32_t s_tick_offset;

void fake_advance_ticks(uint32_t ticks)
{
    atomic_fetch_add_explicit(&s_tick_offset, ticks, memory_order_relaxed);
}

TickType_t xTaskGetTickCount(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (TickType_t)(((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL)) +
           atomic_load_explicit(&s_tick_offset, memory_order_relaxed);
}

/* ---------------- 任务与任务通知 ---------------- */

static void *fake_task_main(void *arg)
{
    StaticTask_t *task = (StaticTask_t *)arg;

    fake_current_task = task;
    task->fn(task->arg);
    return NULL;
}

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name,
                                           uint32_t stack_depth, void *arg, UBaseType_t priority,
                                           StackType_t *stack_buf, StaticTask_t *tcb,
                                           BaseType_t core)
{
    (void)name;
    (void)stack_depth;
    (void)priority;
    (void)stack_buf;
    (void)core;

    if ((fn == NULL) || (tcb == NULL) || tcb->started) {
        return NULL;
    }
    pthread_mutex_init(&tcb->lock, NULL);
    fake_cond_init(&tcb->cond);
    tcb->notif = 0U;
    tcb->fn = fn;
    tcb->arg = arg;
    if (pthread_create(&tcb->thread, NULL, fake_task_main, tcb) != 0) {
        return NULL;
    }
    tcb->started = true;
    return tcb;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    if ((task == NULL) || (!task->started)) {
        return;
    }
    pthread_mutex_lock(&task->lock);
    task->notif++;
    pthread_cond_signal(&task->cond);
    pthread_mutex_unlock(&task->lock);
}

uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait)
{
    TaskHandle_t task = NULL;
    uint32_t taken = 0U;

    task = fake_current_task;
    if (task == NULL) {
        return 0U;
    }
    pthread_mutex_lock(&task->lock);
    while (task->notif == 0U) {
        if (!fake_cond_timedwait(&task->cond, &task->lock, ticks_to_wait)) {
            pthread_mutex_unlock(&task->lock);
            return 0U;
        }
    }
    if (clear_count_on_exit != pdFALSE) {
        taken = task->notif;
        task->notif = 0U;
    } else {
        taken = 1U;
        task->notif--;
    }
    pthread_mutex_unlock(&task->lock);
    return taken;
}

/* ---------------- 队列 ---------------- */

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t *storage,
                                 StaticQueue_t *queue)
{
    if ((length == 0U) || (item_size == 0U) || (storage == NULL) || (queue == NULL)) {
        return NULL;
    }
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->lock, NULL);
    fake_cond_init(&queue->cond);
    queue->buf = storage;
    queue->item_size = item_size;
    queue->capacity = length;
    queue->created = true;
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait)
{
    if ((queue == NULL) || (!queue->created)) {
        return pdFALSE;
    }
    pthread_mutex_lock(&queue->lock);
    while (queue->count == queue->capacity) {
        if (!fake_cond_timedwait(&queue->cond, &queue->lock, ticks_to_wait)) {
            pthread_mutex_unlock(&queue->lock);
            return pdFALSE;
        }
    }
    memcpy(queue->buf + (((queue->head + queue->count) % queue->capacity) * queue->item_size), item,
           queue->item_size);
    queue->count++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks_to_wait)
{
    if ((queue == NULL) || (!queue->created)) {
        return pdFALSE;
    }
    pthread_mutex_lock(&queue->lock);
    while (queue->count == 0U) {
        if (!fake_cond_timedwait(&queue->cond, &queue->lock, ticks_to_wait)) {
            pthread_mutex_unlock(&queue->lock);
            return pdFALSE;
        }
    }
    memcpy(item, queue->buf + (queue->head * queue->item_size), queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    queue->count--;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    return pdPASS;
}

void vQueueDelete(QueueHandle_t queue)
{
    if ((queue == NULL) || (!queue->created)) {
        return;
    }
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->cond);
    memset(queue, 0, sizeof(*queue));
}

/* ---------------- 事件组 ---------------- */

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *group)
{
    if (group == NULL) {
        return NULL;
    }
    memset(group, 0, sizeof(*group));
    pthread_mutex_init(&group->lock, NULL);
    fake_cond_init(&group->cond);
    group->created = true;
    return group;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_for_all,
                                TickType_t ticks_to_wait)
{
    EventBits_t got = 0U;

    if ((group == NULL) || (!group->created) || (bits == 0U)) {
        return 0U;
    }
    pthread_mutex_lock(&group->lock);
    for (;;) {
        got = group->bits & bits;
        if ((wait_for_all != pdFALSE) ? (got == bits) : (got != 0U)) {
            if (clear_on_exit != pdFALSE) {
                group->bits &= ~bits;
            }
            break;
        }
        if (ticks_to_wait == 0U) {
            break;
        }
        if (!fake_cond_timedwait(&group->cond, &group->lock, ticks_to_wait)) {
            got = group->bits & bits;
            break;
        }
    }
    pthread_mutex_unlock(&group->lock);
    return got;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    EventBits_t before = 0U;

    if ((group == NULL) || (!group->created)) {
        return 0U;
    }
    pthread_mutex_lock(&group->lock);
    before = group->bits;
    group->bits |= bits;
    pthread_cond_broadcast(&group->cond);
    pthread_mutex_unlock(&group->lock);
    return before;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    EventBits_t before = 0U;

    if ((group == NULL) || (!group->created)) {
        return 0U;
    }
    pthread_mutex_lock(&group->lock);
    before = group->bits;
    group->bits &= ~bits;
    pthread_mutex_unlock(&group->lock);
    return before;
}

void vEventGroupDelete(EventGroupHandle_t group)
{
    if ((group == NULL) || (!group->created)) {
        return;
    }
    pthread_mutex_destroy(&group->lock);
    pthread_cond_destroy(&group->cond);
    memset(group, 0, sizeof(*group));
}

/* ---------------- 流缓冲（单写者/单读者，字节流） ---------------- */

StreamBufferHandle_t xStreamBufferCreateStatic(size_t buffer_size, size_t trigger_level,
                                               uint8_t *storage,
                                               StaticStreamBuffer_t *stream_buffer)
{
    (void)trigger_level;

    if ((buffer_size == 0U) || (storage == NULL) || (stream_buffer == NULL)) {
        return NULL;
    }
    memset(stream_buffer, 0, sizeof(*stream_buffer));
    pthread_mutex_init(&stream_buffer->lock, NULL);
    fake_cond_init(&stream_buffer->cond);
    stream_buffer->buf = storage;
    stream_buffer->capacity = buffer_size;
    stream_buffer->created = true;
    return stream_buffer;
}

size_t xStreamBufferSend(StreamBufferHandle_t stream_buffer, const void *data, size_t len,
                         TickType_t ticks_to_wait)
{
    size_t written = 0U;

    if ((stream_buffer == NULL) || (!stream_buffer->created) || (data == NULL)) {
        return 0U;
    }
    pthread_mutex_lock(&stream_buffer->lock);
    while ((written == 0U) && (stream_buffer->count == stream_buffer->capacity)) {
        if (!fake_cond_timedwait(&stream_buffer->cond, &stream_buffer->lock, ticks_to_wait)) {
            pthread_mutex_unlock(&stream_buffer->lock);
            return 0U;
        }
    }
    while ((written < len) && (stream_buffer->count < stream_buffer->capacity)) {
        const size_t tail = (stream_buffer->head + stream_buffer->count) % stream_buffer->capacity;

        stream_buffer->buf[tail] = ((const uint8_t *)data)[written];
        stream_buffer->count++;
        written++;
    }
    pthread_cond_broadcast(&stream_buffer->cond);
    pthread_mutex_unlock(&stream_buffer->lock);
    return written;
}

size_t xStreamBufferBytesAvailable(StreamBufferHandle_t stream_buffer)
{
    size_t available = 0U;

    if ((stream_buffer == NULL) || (!stream_buffer->created)) {
        return 0U;
    }
    pthread_mutex_lock(&stream_buffer->lock);
    available = stream_buffer->count;
    pthread_mutex_unlock(&stream_buffer->lock);
    return available;
}

size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t stream_buffer)
{
    size_t spaces = 0U;

    if ((stream_buffer == NULL) || (!stream_buffer->created)) {
        return 0U;
    }
    pthread_mutex_lock(&stream_buffer->lock);
    spaces = stream_buffer->capacity - stream_buffer->count;
    pthread_mutex_unlock(&stream_buffer->lock);
    return spaces;
}

/* ---------------- 确定性读暂停 ---------------- */

static pthread_mutex_t s_read_pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_read_pause_cond = PTHREAD_COND_INITIALIZER;
/* 0=off 1=armed 2=entered 3=released */
static int s_read_pause_state;

void fake_read_pause_arm(void)
{
    pthread_mutex_lock(&s_read_pause_lock);
    s_read_pause_state = 1;
    pthread_mutex_unlock(&s_read_pause_lock);
}

bool fake_read_pause_wait_entered(uint32_t timeout_ms)
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
    pthread_mutex_lock(&s_read_pause_lock);
    while (s_read_pause_state != 2) {
        if (pthread_cond_timedwait(&s_read_pause_cond, &s_read_pause_lock, &deadline) != 0) {
            break;
        }
    }
    entered = (s_read_pause_state == 2);
    pthread_mutex_unlock(&s_read_pause_lock);
    return entered;
}

void fake_read_pause_release(void)
{
    pthread_mutex_lock(&s_read_pause_lock);
    if (s_read_pause_state == 2) {
        s_read_pause_state = 3;
        pthread_cond_broadcast(&s_read_pause_cond);
    }
    pthread_mutex_unlock(&s_read_pause_lock);
}

/* 在取缓冲锁之前调用：armed 时通知用例并等待 release。 */
static void fake_read_pause_gate(void)
{
    pthread_mutex_lock(&s_read_pause_lock);
    if (s_read_pause_state == 1) {
        s_read_pause_state = 2;
        pthread_cond_broadcast(&s_read_pause_cond);
        while (s_read_pause_state == 2) {
            pthread_cond_wait(&s_read_pause_cond, &s_read_pause_lock);
        }
    }
    pthread_mutex_unlock(&s_read_pause_lock);
}

size_t xStreamBufferReceive(StreamBufferHandle_t stream_buffer, void *data, size_t len,
                            TickType_t ticks_to_wait)
{
    size_t got = 0U;

    if ((stream_buffer == NULL) || (!stream_buffer->created) || (data == NULL) || (len == 0U)) {
        return 0U;
    }
    fake_read_pause_gate();
    pthread_mutex_lock(&stream_buffer->lock);
    while (stream_buffer->count == 0U) {
        if (!fake_cond_timedwait(&stream_buffer->cond, &stream_buffer->lock, ticks_to_wait)) {
            pthread_mutex_unlock(&stream_buffer->lock);
            return 0U;
        }
    }
    while ((got < len) && (stream_buffer->count > 0U)) {
        ((uint8_t *)data)[got] = stream_buffer->buf[stream_buffer->head];
        stream_buffer->head = (stream_buffer->head + 1U) % stream_buffer->capacity;
        stream_buffer->count--;
        got++;
    }
    pthread_cond_broadcast(&stream_buffer->cond);
    pthread_mutex_unlock(&stream_buffer->lock);
    return got;
}

BaseType_t xStreamBufferReset(StreamBufferHandle_t stream_buffer)
{
    if ((stream_buffer == NULL) || (!stream_buffer->created)) {
        return pdFALSE;
    }
    pthread_mutex_lock(&stream_buffer->lock);
    stream_buffer->head = 0U;
    stream_buffer->count = 0U;
    pthread_cond_broadcast(&stream_buffer->cond);
    pthread_mutex_unlock(&stream_buffer->lock);
    return pdPASS;
}

void vStreamBufferDelete(StreamBufferHandle_t stream_buffer)
{
    if ((stream_buffer == NULL) || (!stream_buffer->created)) {
        return;
    }
    pthread_mutex_destroy(&stream_buffer->lock);
    pthread_cond_destroy(&stream_buffer->cond);
    memset(stream_buffer, 0, sizeof(*stream_buffer));
}
