/* 主机回归用的 FreeRTOS 最小替身：只覆盖 components/spp_server/spp_server.c 用到的 API。
 * 语义按目标板配置（CONFIG_FREERTOS_HZ=1000、32 位 tick/EventBits_t、静态对象）实现：
 * 事件组/队列/流缓冲/任务通知都是真的阻塞原语（pthread 条件变量），临界区是全局互斥量，
 * 因此被测驱动里的跨任务竞态是真实发生的，而不是单线程模拟。
 * 该目录只在 host 上编译，不参与 idf.py 构建。 */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define configUSE_16_BIT_TICKS 0
#define configTICK_RATE_HZ     1000
#define configSTACK_DEPTH_TYPE uint32_t

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY     ((TickType_t)0xFFFFFFFFUL)

typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;
typedef uint32_t UBaseType_t;
typedef int32_t BaseType_t;
typedef uint8_t StackType_t;
typedef void (*TaskFunction_t)(void *arg);

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint32_t notif;
    TaskFunction_t fn;
    void *arg;
    pthread_t thread;
    bool started;
} StaticTask_t;
typedef StaticTask_t *TaskHandle_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t *buf;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
    bool created;
} StaticQueue_t;
typedef StaticQueue_t *QueueHandle_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    EventBits_t bits;
    bool created;
} StaticEventGroup_t;
typedef StaticEventGroup_t *EventGroupHandle_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t *buf;
    size_t capacity;
    size_t head;
    size_t count;
    bool created;
} StaticStreamBuffer_t;
typedef StaticStreamBuffer_t *StreamBufferHandle_t;

/* 目标板上是 portMUX_TYPE；host 上用全局互斥量实现同样的“不可重入、不阻塞”区间。 */
typedef struct {
    int unused;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}

void fake_crit_enter(void);
void fake_crit_exit(void);

#define taskENTER_CRITICAL(mux) ((void)(mux), fake_crit_enter())
#define taskEXIT_CRITICAL(mux)  ((void)(mux), fake_crit_exit())

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_depth, void *arg,
                                           UBaseType_t priority, StackType_t *stack_buf, StaticTask_t *tcb,
                                           BaseType_t core);
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear_count_on_exit, TickType_t ticks_to_wait);
TickType_t xTaskGetTickCount(void);

QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size, uint8_t *storage, StaticQueue_t *queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks_to_wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks_to_wait);
void vQueueDelete(QueueHandle_t queue);

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *group);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, BaseType_t clear_on_exit,
                                BaseType_t wait_for_all, TickType_t ticks_to_wait);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits);
void vEventGroupDelete(EventGroupHandle_t group);

StreamBufferHandle_t xStreamBufferCreateStatic(size_t buffer_size, size_t trigger_level, uint8_t *storage,
                                               StaticStreamBuffer_t *stream_buffer);
size_t xStreamBufferSend(StreamBufferHandle_t stream_buffer, const void *data, size_t len, TickType_t ticks_to_wait);
size_t xStreamBufferReceive(StreamBufferHandle_t stream_buffer, void *data, size_t len, TickType_t ticks_to_wait);
BaseType_t xStreamBufferReset(StreamBufferHandle_t stream_buffer);
void vStreamBufferDelete(StreamBufferHandle_t stream_buffer);

/* 确定性读暂停（仅 host 回归）：arm 后，下一次 xStreamBufferReceive 在取缓冲锁之前暂停，
 * 让用例在“读者已登记、尚未完成接收”的窗口里注入 CLOSE/OPEN。暂停点不持有缓冲锁，
 * 因此不会比 FreeRTOS（零等待接收不阻止 reset）提供更强的同步而掩盖竞态。 */
void fake_read_pause_arm(void);
bool fake_read_pause_wait_entered(uint32_t timeout_ms);
void fake_read_pause_release(void);
