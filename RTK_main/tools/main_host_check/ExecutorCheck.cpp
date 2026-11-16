// 确定性执行器回归：只替换 RTOS 边界，运行真实 Executor；每轮末才退出模拟任务。
#include "rtos/Executor.hpp"
#include "rtos/EspStatus.hpp"
#include "esp_task_wdt.h"
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
uint32_t s_clock = 100;
int s_failures = 0;
int s_critical = 0;
TaskHandle_t s_current = nullptr;
TaskHandle_t s_created = nullptr;
uint32_t s_created_bytes = 0;
uint32_t s_round_limit = 1;
uint32_t s_rounds = 0;
bool s_pump_receive = false;
std::jmp_buf s_return{};
#define CHECK(value)                                                                                                   \
    do {                                                                                                               \
        if (!(value)) {                                                                                                \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value);                                                   \
            ++s_failures;                                                                                              \
        }                                                                                                              \
    } while (false)

void pump()
{
    const TaskHandle_t previous = s_current;
    s_current = s_created;
    s_rounds = 0;
    if (setjmp(s_return) == 0) {
        s_created->fn(s_created->arg);
    }
    s_current = previous;
}
struct Probe {
    rtos::Executor *executor = nullptr;
    uint32_t init = 0;
    uint32_t start = 0;
    uint32_t poll = 0;
    uint32_t command = 0;
    bool fail_init = false;
    bool fail_start = false;
    bool backlog = false;
    bool slow = false;
    std::array<uint32_t, 8> waits{};
    size_t wait_count = 0;
};
fw::Result init(void *self) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 表只绑定 Probe 指针。
    ++probe.init;
    return probe.fail_init ? fw::Result::kIoError : fw::Result::kOk;
}
fw::Result start(void *self) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 同上。
    ++probe.start;
    return probe.fail_start ? fw::Result::kIoError : fw::Result::kOk;
}
bool poll(void *self, uint32_t now) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 同上。
    CHECK(now == s_clock);
    ++probe.poll;
    if (probe.slow) {
        s_clock += 21U;
    }
    return probe.backlog && probe.poll == 1U;
}
void command(void *self, const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 同上。
    ++probe.command;
    fw::CommandResult nested{};
    CHECK(probe.executor->call(cmd, nested, 1) == fw::Result::kInvalidState);
    result.detail = cmd.args[0];
    result.seq = 0xBAD; // 故意覆盖，执行器必须恢复正确的请求序号。
}
void wait(void *self, uint32_t max_ms) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 同上。
    CHECK(probe.wait_count < probe.waits.size());
    probe.waits[probe.wait_count++] = max_ms;
    s_clock += max_ms;
}
constexpr fw::ModuleOps kOps{init, start, poll, command};
constexpr fw::ModuleOps kNoStartOps{init, nullptr, nullptr, command};
constexpr fw::ModuleOps kUnsupportedOps{nullptr, nullptr, nullptr, nullptr};

void lifecycle()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 2> storage{};
    static constinit Probe healthy{.executor = &executor, .backlog = true, .slow = true};
    static constinit Probe bad_init{.executor = &executor, .fail_init = true};
    static constinit Probe bad_start{.executor = &executor, .fail_start = true};
    static constinit Probe periodic{.executor = &executor};
    static constinit std::array<fw::Module, 6> modules{{
        {"healthy", 1, 0, 0, &healthy, &kOps},
        {"init fail", 2, 0, 0, &bad_init, &kOps},
        {"start fail", 3, 0, 0, &bad_start, &kOps},
        {"foreign", 1, 1, 0, nullptr, nullptr},
        {"periodic", 4, 0, 50, &periodic, &kOps},
        {"unsupported", 5, 0, 0, nullptr, &kUnsupportedOps},
    }};
    rtos::ExecutorConfig cfg{.name = "test", .priority = 10, .core = 1, .wait = wait, .wait_ctx = &healthy};
    CHECK(executor.start() == fw::Status::kInvalidState);
    CHECK(executor.init(cfg, modules, rtos::view_of(storage)) == fw::Status::kOk);
    CHECK(executor.init(cfg, modules, rtos::view_of(storage)) == fw::Status::kBusy);
    CHECK(healthy.init == 1 && bad_init.init == 1 && bad_start.init == 1);
    CHECK(!executor.module_ok(2) && executor.module_ok(3));
    CHECK(executor.start() == fw::Status::kOk && s_created_bytes == 6144);
    CHECK(executor.start() == fw::Status::kBusy);
    fw::Command a{.target = 2, .seq = 7};
    fw::Command b{.target = 5, .seq = 8};
    CHECK(xQueueSend(&storage.cmd_queue, &a, 0) == pdTRUE);
    CHECK(xQueueSend(&storage.cmd_queue, &b, 0) == pdTRUE);
    fw::CommandResult stale{};
    CHECK(xQueueSend(&storage.result_queue, &stale, 0) == pdTRUE);
    CHECK(xQueueSend(&storage.result_queue, &stale, 0) == pdTRUE);
    s_round_limit = 3;
    pump();
    CHECK(healthy.start == 1 && bad_init.start == 0 && bad_start.start == 1);
    CHECK(healthy.poll == 3 && bad_init.poll == 0 && bad_start.poll == 0);
    CHECK(periodic.poll == 2 && !executor.module_ok(3));
    CHECK(healthy.waits[0] == 20 && healthy.waits[1] == 0 && healthy.waits[2] == 20);
    const auto stats = executor.stats();
    CHECK(stats.rounds == 3 && stats.overruns == 3 && stats.max_round_ms == 21);
    CHECK(stats.commands == 2 && stats.result_drops == 2 && stats.stack_free == 2048);
    CHECK(xQueueReceive(&storage.result_queue, &stale, 0) == pdTRUE);
    CHECK(stale.seq == 7 && stale.status == fw::Result::kInvalidState);
    CHECK(xQueueReceive(&storage.result_queue, &stale, 0) == pdTRUE);
    CHECK(stale.seq == 8 && stale.status == fw::Result::kNotSupported);
}
void calls()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 2> storage{};
    static constinit Probe probe{.executor = &executor};
    static constinit std::array<fw::Module, 1> modules{{{"command", 1, 0, 0, &probe, &kNoStartOps}}};
    const rtos::ExecutorConfig cfg{.name = "call", .core = 1};
    fw::Command cmd{.target = 1};
    fw::CommandResult result{};
    CHECK(executor.call(cmd, result, 10) == fw::Result::kInvalidState);
    CHECK(executor.init(cfg, modules, rtos::view_of(storage)) == fw::Status::kOk);
    CHECK(executor.start() == fw::Status::kOk);
    cmd.target = 9;
    CHECK(executor.call(cmd, result, 10) == fw::Result::kNotFound);
    cmd.target = 1;
    CHECK(xQueueSend(&storage.cmd_queue, &cmd, 0) == pdTRUE);
    CHECK(xQueueSend(&storage.cmd_queue, &cmd, 0) == pdTRUE);
    CHECK(executor.call(cmd, result, 10) == fw::Result::kBusy);
    CHECK(xQueueReceive(&storage.cmd_queue, &cmd, 0) == pdTRUE);
    CHECK(xQueueReceive(&storage.cmd_queue, &cmd, 0) == pdTRUE);
    cmd.args[0] = 11;
    CHECK(executor.call(cmd, result, 10) == fw::Result::kTimeout);
    cmd.args[0] = 22;
    s_pump_receive = true;
    CHECK(executor.call(cmd, result, 100) == fw::Result::kOk);
    CHECK(result.detail == 22 && result.seq != 0xBAD && probe.command == 2);
    CHECK(executor.stats().commands == 2);
    s_pump_receive = false;
    cmd.args[0] = 33;
    CHECK(executor.call(cmd, result, 0) == fw::Result::kTimeout);
    pump(); // 超时命令仍会执行；它的结果留在队列中。
    CHECK(probe.command == 3);
    s_pump_receive = true;
    cmd.args[0] = 44;
    CHECK(executor.call(cmd, result, 100) == fw::Result::kOk && result.detail == 44);
}
void invalid()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 2> storage{};
    rtos::ExecutorConfig cfg{};
    CHECK(executor.init(cfg, {}, rtos::view_of(storage)) == fw::Status::kInvalidArg);
    cfg.name = "invalid";
    auto view = rtos::view_of(storage);
    view.cmd_area = {};
    CHECK(executor.init(cfg, {}, view) == fw::Status::kInvalidArg);
    std::array<fw::Module, 2> modules{
        {{"a", 1, 0, 0, nullptr, &kUnsupportedOps}, {"b", 1, 0, 0, nullptr, &kUnsupportedOps}}};
    CHECK(executor.init(cfg, modules, rtos::view_of(storage)) == fw::Status::kInvalidArg);
    modules[1].id = 2;
    modules[1].ops = nullptr;
    CHECK(executor.init(cfg, modules, rtos::view_of(storage)) == fw::Status::kInvalidArg);
    cfg.core = configNUMBER_OF_CORES;
    CHECK(executor.init(cfg, {}, rtos::view_of(storage)) == fw::Status::kInvalidArg);
}
} // namespace

namespace rtos {
uint32_t now_ms() noexcept
{
    CHECK(s_critical == 0);
    return s_clock;
}
} // namespace rtos
extern "C" {
void fake_crit_enter(void)
{
    CHECK(s_critical == 0);
    ++s_critical;
}
void fake_crit_exit(void)
{
    --s_critical;
    CHECK(s_critical == 0);
}
void fake_log(const char *, const char *, const char *, ...)
{
    CHECK(s_critical == 0);
}
const char *esp_err_to_name(esp_err_t)
{
    return "test";
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    CHECK(s_critical == 0);
    return s_current;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *, uint32_t bytes, void *arg, UBaseType_t,
                                           StackType_t *, StaticTask_t *tcb, BaseType_t)
{
    CHECK(s_critical == 0);
    tcb->fn = fn;
    tcb->arg = arg;
    s_created = tcb;
    s_created_bytes = bytes;
    return tcb;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item, uint8_t *area, StaticQueue_t *queue)
{
    CHECK(s_critical == 0);
    queue->buf = area;
    queue->capacity = length;
    queue->item_size = item;
    queue->head = queue->count = 0;
    return queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t)
{
    CHECK(s_critical == 0);
    if (queue->count == queue->capacity) {
        return pdFALSE;
    }
    const size_t tail = (queue->head + queue->count) % queue->capacity;
    std::memcpy(queue->buf + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    CHECK(s_critical == 0);
    if (queue->count == 0 && s_pump_receive && s_current == nullptr && ticks != 0) {
        pump();
    }
    if (queue->count == 0) {
        s_clock += ticks;
        return pdFALSE;
    }
    std::memcpy(item, queue->buf + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t)
{
    CHECK(s_critical == 0);
}
void vTaskDelay(TickType_t ticks)
{
    CHECK(s_critical == 0);
    s_clock += ticks;
    if (++s_rounds == s_round_limit) {
        std::longjmp(s_return, 1);
    }
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t)
{
    CHECK(s_critical == 0);
    return 2048;
}
esp_err_t esp_task_wdt_add(TaskHandle_t)
{
    CHECK(s_critical == 0);
    return ESP_OK;
}
esp_err_t esp_task_wdt_reset(void)
{
    CHECK(s_critical == 0);
    return ESP_OK;
}
}
int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (std::strcmp(argv[1], "lifecycle") == 0) {
        lifecycle();
    } else if (std::strcmp(argv[1], "calls") == 0) {
        calls();
    } else if (std::strcmp(argv[1], "invalid") == 0) {
        invalid();
    } else {
        return 2;
    }
    std::printf("%s %s\n", s_failures == 0 ? "PASS" : "FAIL", argv[1]);
    return s_failures == 0 ? 0 : 1;
}
