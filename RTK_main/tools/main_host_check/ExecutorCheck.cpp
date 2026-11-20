// 确定性执行器回归：只替换 RTOS 边界，运行真实 Executor；每轮末才退出模拟任务。
// 覆盖生命周期、同步 call、异步 post/poll/cancel/complete、表满/队满、延迟完成与 seq 回绕。
#include "rtos/Executor.hpp"
#include "rtos/EspStatus.hpp"
#include "esp_task_wdt.h"
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_log.h"

namespace rtos {
struct ExecutorTestAccess {
    static void setNextSeq(Executor &exec, uint32_t seq) noexcept
    {
        exec.next_seq_ = seq;
    }
};
} // namespace rtos

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
uint32_t s_notify_gives = 0;
std::jmp_buf s_return{};
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value);                               \
            ++s_failures;                                                                          \
        }                                                                                          \
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
    bool failInit = false;
    bool failStart = false;
    bool backlog = false;
    bool slow = false;
    bool deferCommand = false;
    bool autoCompleteInPoll = false;
    bool selfPostOnce = false;
    rtos::Ticket selfTicket{};
    fw::Result selfPostStatus = fw::Result::kInvalidState;
    std::array<uint32_t, 8> waits{};
    size_t waitCount = 0;
    std::array<uint8_t, 32> order{};
    size_t orderCount = 0;
    std::array<uint32_t, 16> deferredSeqs{};
    std::array<int32_t, 16> deferredDetails{};
    size_t deferredCount = 0;
    uint32_t completeUnknownSeq = 0;
};
fw::Result init(void *self) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 表只绑定 Probe 指针。
    ++probe.init;
    return probe.failInit ? fw::Result::kIoError : fw::Result::kOk;
}
fw::Result start(void *self) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 同上。
    ++probe.start;
    return probe.failStart ? fw::Result::kIoError : fw::Result::kOk;
}
bool poll(void *self, uint32_t now) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 同上。
    CHECK(now == s_clock);
    ++probe.poll;
    if (probe.slow) {
        s_clock += 21U;
    }
    if (probe.completeUnknownSeq != 0U) {
        const uint32_t seq = probe.completeUnknownSeq;
        probe.completeUnknownSeq = 0U;
        probe.executor->complete(seq, fw::CommandResult{seq, fw::Result::kOk, 0});
    }
    if (probe.autoCompleteInPoll && probe.deferredCount > 0U) {
        const size_t count = probe.deferredCount;
        probe.deferredCount = 0U;
        for (size_t i = 0; i < count; ++i) {
            fw::CommandResult done{0xBADU, fw::Result::kOk, probe.deferredDetails[i]};
            probe.executor->complete(probe.deferredSeqs[i], done);
        }
    }
    if (probe.selfPostOnce) {
        probe.selfPostOnce = false;
        fw::Command cmd{.target = 1, .verb = 0};
        cmd.args[0] = 0x5AU;
        probe.selfPostStatus = probe.executor->post(cmd, rtos::Caller::kInternal, probe.selfTicket);
    }
    return probe.backlog && probe.poll == 1U;
}
void command(void *self, const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 同上。
    ++probe.command;
    if (probe.orderCount < probe.order.size()) {
        probe.order[probe.orderCount++] = cmd.args[0];
    }
    fw::CommandResult nested{};
    CHECK(probe.executor->call(cmd, nested, 1) == fw::Result::kInvalidState);
    if (probe.deferCommand || cmd.verb == 1U) {
        if (probe.deferredCount < probe.deferredSeqs.size()) {
            probe.deferredSeqs[probe.deferredCount] = cmd.seq;
            probe.deferredDetails[probe.deferredCount] = cmd.args[0];
            ++probe.deferredCount;
        }
        result.detail = fw::kDetailDeferred;
        return;
    }
    result.detail = cmd.args[0];
    result.seq = 0xBAD; // 故意覆盖，执行器必须恢复正确的请求序号。
}
void wait(void *self, uint32_t maxMs) noexcept
{
    auto &probe = *static_cast<Probe *>(self); // 同上。
    CHECK(probe.waitCount < probe.waits.size());
    probe.waits[probe.waitCount++] = maxMs;
    s_clock += maxMs;
}
constexpr fw::ModuleOps kOps{init, start, poll, command};
constexpr fw::ModuleOps kNoStartOps{init, nullptr, poll, command};
constexpr fw::ModuleOps kUnsupportedOps{nullptr, nullptr, nullptr, nullptr};

void lifecycle()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 4> storage{};
    static constinit Probe healthy{.executor = &executor, .backlog = true, .slow = true};
    static constinit Probe badInit{.executor = &executor, .failInit = true};
    static constinit Probe badStart{.executor = &executor, .failStart = true};
    static constinit Probe periodic{.executor = &executor};
    static constinit std::array<fw::Module, 6> modules{{
        {"healthy", 1, 0, 0, &healthy, &kOps},
        {"init fail", 2, 0, 0, &badInit, &kOps},
        {"start fail", 3, 0, 0, &badStart, &kOps},
        {"foreign", 1, 1, 0, nullptr, nullptr},
        {"periodic", 4, 0, 50, &periodic, &kOps},
        {"unsupported", 5, 0, 0, nullptr, &kUnsupportedOps},
    }};
    rtos::ExecutorConfig cfg{
        .name = "test", .priority = 10, .core = 1, .wait = wait, .waitCtx = &healthy};
    CHECK(executor.start() == fw::Status::kInvalidState);
    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kOk);
    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kBusy);
    CHECK(healthy.init == 1 && badInit.init == 1 && badStart.init == 1);
    CHECK(!executor.moduleOk(2) && executor.moduleOk(3));
    CHECK(executor.start() == fw::Status::kOk && s_created_bytes == 6144);
    CHECK(executor.start() == fw::Status::kBusy);
    fw::Command a{.target = 2};
    fw::Command b{.target = 5};
    rtos::Ticket ta{};
    rtos::Ticket tb{};
    CHECK(executor.post(a, rtos::Caller::kConsole, ta) == fw::Result::kOk);
    CHECK(executor.post(b, rtos::Caller::kWeb, tb) == fw::Result::kOk);
    healthy.completeUnknownSeq = 999U;
    s_round_limit = 3;
    pump();
    CHECK(healthy.start == 1 && badInit.start == 0 && badStart.start == 1);
    CHECK(healthy.poll == 3 && badInit.poll == 0 && badStart.poll == 0);
    CHECK(periodic.poll == 2 && !executor.moduleOk(3));
    CHECK(healthy.waits[0] == 20 && healthy.waits[1] == 0 && healthy.waits[2] == 20);
    const auto stats = executor.stats();
    CHECK(stats.rounds == 3 && stats.overruns == 3 && stats.maxRoundMs == 21);
    CHECK(stats.commands == 2 && stats.resultDrops == 1 && stats.stackFree == 2048);
    fw::CommandResult out{};
    CHECK(executor.poll(ta, out) == rtos::PollState::kDone);
    CHECK(out.seq == ta.seq && out.status == fw::Result::kInvalidState);
    CHECK(executor.poll(tb, out) == rtos::PollState::kDone);
    CHECK(out.seq == tb.seq && out.status == fw::Result::kNotSupported);
    CHECK(executor.poll(ta, out) == rtos::PollState::kInvalid);
}

void calls()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 2> storage{};
    static constinit Probe probe{.executor = &executor};
    static constinit std::array<fw::Module, 1> modules{
        {{"command", 1, 0, 0, &probe, &kNoStartOps}}};
    const rtos::ExecutorConfig cfg{.name = "call", .core = 1};
    fw::Command cmd{.target = 1};
    fw::CommandResult result{};
    CHECK(executor.call(cmd, result, 10) == fw::Result::kInvalidState);
    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kOk);
    CHECK(executor.start() == fw::Status::kOk);
    cmd.target = 9;
    CHECK(executor.call(cmd, result, 10) == fw::Result::kNotFound);
    cmd.target = 1;
    CHECK(xQueueSend(&storage.cmdQueue, &cmd, 0) == pdTRUE);
    CHECK(xQueueSend(&storage.cmdQueue, &cmd, 0) == pdTRUE);
    // 命令队列满返回 kBusy 且必须回滚完成表槽位。
    CHECK(executor.call(cmd, result, 10) == fw::Result::kBusy);
    CHECK(xQueueReceive(&storage.cmdQueue, &cmd, 0) == pdTRUE);
    CHECK(xQueueReceive(&storage.cmdQueue, &cmd, 0) == pdTRUE);
    // 超时后自动 cancel，未出队的命令被执行器跳过不执行。
    cmd.args[0] = 11;
    CHECK(executor.call(cmd, result, 10) == fw::Result::kTimeout);
    cmd.args[0] = 22;
    s_pump_receive = true;
    CHECK(executor.call(cmd, result, 100) == fw::Result::kOk);
    CHECK(result.detail == 22 && result.seq != 0xBAD && probe.command == 1);
    CHECK(executor.stats().commands == 1);
    // 0 ms 超时同样取消未出队命令，pump 后跳过执行。
    s_pump_receive = false;
    cmd.args[0] = 33;
    CHECK(executor.call(cmd, result, 0) == fw::Result::kTimeout);
    s_round_limit = 1;
    pump();
    CHECK(probe.command == 1);
    s_pump_receive = true;
    cmd.args[0] = 44;
    CHECK(executor.call(cmd, result, 100) == fw::Result::kOk && result.detail == 44);
    CHECK(probe.command == 2);
    // 延迟完成的 call：onCommand 返回 kDetailDeferred，poll 回调中 complete 唤醒并返回 kOk。
    probe.autoCompleteInPoll = true;
    cmd.verb = 1U;
    cmd.args[0] = 55;
    CHECK(executor.call(cmd, result, 100) == fw::Result::kOk && result.detail == 55);
    CHECK(probe.command == 3);
    // 延迟命令 call 超时：取消后即使稍后 complete() 到达也丢弃并释放槽位。
    probe.autoCompleteInPoll = false;
    s_pump_receive = false;
    cmd.verb = 1U;
    cmd.args[0] = 66;
    // 先由 pump 跑一轮让命令进入延迟状态，但这里测试 call 超时后在下一轮才 complete。
    CHECK(executor.call(cmd, result, 10) == fw::Result::kTimeout);
    s_round_limit = 1;
    pump(); // 已取消且未出队 -> 跳过。
    CHECK(probe.command == 3);
}

void asyncApi()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 16> storage{};
    static constinit Probe probe{.executor = &executor};
    static constinit std::array<fw::Module, 1> modules{{{"worker", 1, 0, 0, &probe, &kNoStartOps}}};
    const rtos::ExecutorConfig cfg{.name = "async", .core = 1};
    fw::Command cmd{.target = 1};
    rtos::Ticket ticket{.seq = 123U, .slot = 2U};
    // 未启动时 post 返回 kInvalidState 并清空输出凭据。
    CHECK(executor.post(cmd, rtos::Caller::kWeb, ticket) == fw::Result::kInvalidState);
    CHECK(ticket.seq == 0U && ticket.slot == rtos::kInvalidSlot);

    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kOk);
    CHECK(executor.start() == fw::Status::kOk);
    // 目标不存在返回 kNotFound。
    cmd.target = 9;
    CHECK(executor.post(cmd, rtos::Caller::kPhone, ticket) == fw::Result::kNotFound);
    CHECK(ticket.seq == 0U && ticket.slot == rtos::kInvalidSlot);
    cmd.target = 1;

    // 1. 表满 kBusy 与 FIFO 顺序（覆盖全部 5 种 Caller）。
    constexpr std::array<rtos::Caller, rtos::kMaxPending> kCallers{
        rtos::Caller::kConsole, rtos::Caller::kWeb, rtos::Caller::kPhone, rtos::Caller::kInternal,
        rtos::Caller::kMqtt,    rtos::Caller::kWeb, rtos::Caller::kPhone, rtos::Caller::kConsole,
    };
    std::array<rtos::Ticket, rtos::kMaxPending> tickets{};
    for (size_t i = 0; i < rtos::kMaxPending; ++i) {
        cmd.verb = 0U;
        cmd.args[0] = static_cast<uint8_t>(10U + i);
        CHECK(executor.post(cmd, kCallers[i], tickets[i]) == fw::Result::kOk);
        CHECK(tickets[i].seq != 0U && tickets[i].slot < rtos::kMaxPending);
    }
    rtos::Ticket overflow{};
    CHECK(executor.post(cmd, rtos::Caller::kWeb, overflow) == fw::Result::kBusy);
    CHECK(overflow.seq == 0U && overflow.slot == rtos::kInvalidSlot);

    s_round_limit = 1;
    pump();
    CHECK(probe.orderCount == rtos::kMaxPending);
    for (size_t i = 0; i < rtos::kMaxPending; ++i) {
        CHECK(probe.order[i] == static_cast<uint8_t>(10U + i));
    }
    // 仍满（尚未 poll 取走）→ 再 post 仍 kBusy。
    CHECK(executor.post(cmd, rtos::Caller::kWeb, overflow) == fw::Result::kBusy);
    // 取走全部 8 项并核对结果与释放。
    for (size_t i = 0; i < rtos::kMaxPending; ++i) {
        fw::CommandResult out{};
        CHECK(executor.poll(tickets[i], out) == rtos::PollState::kDone);
        CHECK(out.seq == tickets[i].seq && out.status == fw::Result::kOk &&
              out.detail == static_cast<int32_t>(10U + i));
        CHECK(executor.poll(tickets[i], out) == rtos::PollState::kInvalid);
    }

    // 2. 取消未执行命令：poll 返回 kCancelled，执行器取出时跳过并释放槽位。
    const uint32_t cmdsBeforeCancel = probe.command;
    cmd.verb = 0U;
    cmd.args[0] = 77U;
    rtos::Ticket tQueued{};
    CHECK(executor.post(cmd, rtos::Caller::kWeb, tQueued) == fw::Result::kOk);
    fw::CommandResult out{};
    CHECK(executor.poll(tQueued, out) == rtos::PollState::kPending);
    executor.cancel(tQueued);
    CHECK(executor.poll(tQueued, out) == rtos::PollState::kCancelled);
    s_round_limit = 1;
    pump();
    CHECK(probe.command == cmdsBeforeCancel);
    CHECK(executor.poll(tQueued, out) == rtos::PollState::kInvalid);

    // 3. 延迟完成与取消延迟中命令。
    probe.autoCompleteInPoll = false;
    probe.deferredCount = 0;
    cmd.verb = 1U;
    cmd.args[0] = 81U;
    rtos::Ticket tDefOk{};
    CHECK(executor.post(cmd, rtos::Caller::kPhone, tDefOk) == fw::Result::kOk);
    cmd.args[0] = 82U;
    rtos::Ticket tDefCancel{};
    CHECK(executor.post(cmd, rtos::Caller::kWeb, tDefCancel) == fw::Result::kOk);
    s_round_limit = 1;
    pump();
    CHECK(probe.deferredCount == 2U);
    CHECK(executor.poll(tDefOk, out) == rtos::PollState::kPending);
    CHECK(executor.poll(tDefCancel, out) == rtos::PollState::kPending);
    // 非本执行器任务调用 complete 必须被忽略。
    executor.complete(tDefOk.seq, fw::CommandResult{tDefOk.seq, fw::Result::kOk, 999});
    CHECK(executor.poll(tDefOk, out) == rtos::PollState::kPending);
    // 取消其中一条延迟命令。
    executor.cancel(tDefCancel);
    CHECK(executor.poll(tDefCancel, out) == rtos::PollState::kCancelled);
    const uint32_t dropsBefore = executor.stats().resultDrops;
    // 下一轮在执行器任务内完成两条：tDefOk 变 kDone，tDefCancel 丢弃并释放（不计 resultDrops）。
    probe.autoCompleteInPoll = true;
    s_round_limit = 1;
    pump();
    CHECK(executor.stats().resultDrops == dropsBefore);
    CHECK(executor.poll(tDefOk, out) == rtos::PollState::kDone);
    CHECK(out.seq == tDefOk.seq && out.detail == 81);
    CHECK(executor.poll(tDefCancel, out) == rtos::PollState::kInvalid);

    // 4. cancel 已完成命令立即释放槽位；本执行器任务内可自 post。
    cmd.verb = 0U;
    cmd.args[0] = 88U;
    rtos::Ticket tDoneCancel{};
    CHECK(executor.post(cmd, rtos::Caller::kWeb, tDoneCancel) == fw::Result::kOk);
    probe.selfPostOnce = true;
    s_round_limit = 1;
    pump();
    executor.cancel(tDoneCancel);
    CHECK(executor.poll(tDoneCancel, out) == rtos::PollState::kInvalid);
    CHECK(probe.selfPostStatus == fw::Result::kOk);
    s_round_limit = 1;
    pump();
    CHECK(executor.poll(probe.selfTicket, out) == rtos::PollState::kDone && out.detail == 0x5A);

    // 5. seq 回绕：跳过 0，且跳过当前仍在完成表中的冲突序号 1。
    rtos::ExecutorTestAccess::setNextSeq(executor, 0xFFFFFFFEU);
    rtos::Ticket w0{};
    rtos::Ticket w1{};
    rtos::Ticket w2{};
    rtos::Ticket w3{};
    cmd.verb = 0U;
    cmd.args[0] = 1U;
    CHECK(executor.post(cmd, rtos::Caller::kConsole, w0) == fw::Result::kOk);
    CHECK(executor.post(cmd, rtos::Caller::kConsole, w1) == fw::Result::kOk);
    cmd.verb = 1U; // 让 seq=1 保持延迟在途，验证再次回绕时跳过 1。
    probe.autoCompleteInPoll = false;
    CHECK(executor.post(cmd, rtos::Caller::kConsole, w2) == fw::Result::kOk);
    cmd.verb = 0U;
    CHECK(executor.post(cmd, rtos::Caller::kConsole, w3) == fw::Result::kOk);
    CHECK(w0.seq == 0xFFFFFFFEU && w1.seq == 0xFFFFFFFFU && w2.seq == 1U && w3.seq == 2U);
    s_round_limit = 1;
    pump();
    CHECK(executor.poll(w0, out) == rtos::PollState::kDone && out.seq == 0xFFFFFFFEU);
    CHECK(executor.poll(w1, out) == rtos::PollState::kDone && out.seq == 0xFFFFFFFFU);
    CHECK(executor.poll(w3, out) == rtos::PollState::kDone && out.seq == 2U);
    CHECK(executor.poll(w2, out) == rtos::PollState::kPending);

    rtos::ExecutorTestAccess::setNextSeq(executor, 0xFFFFFFFFU);
    rtos::Ticket c0{};
    rtos::Ticket c1{};
    CHECK(executor.post(cmd, rtos::Caller::kWeb, c0) == fw::Result::kOk);
    CHECK(executor.post(cmd, rtos::Caller::kWeb, c1) == fw::Result::kOk);
    CHECK(c0.seq == 0xFFFFFFFFU && c1.seq == 2U); // 跳过了 0 与仍在途的 1！
    probe.autoCompleteInPoll = true;
    s_round_limit = 1;
    pump();
    CHECK(executor.poll(w2, out) == rtos::PollState::kDone && out.seq == 1U);
    CHECK(executor.poll(c0, out) == rtos::PollState::kDone && out.seq == 0xFFFFFFFFU);
    CHECK(executor.poll(c1, out) == rtos::PollState::kDone && out.seq == 2U);

    // 6. 无效/过期凭据与确认全部 8 个槽位均已释放无泄漏。
    CHECK(executor.poll(rtos::Ticket{}, out) == rtos::PollState::kInvalid);
    CHECK(executor.poll(rtos::Ticket{1U, rtos::kMaxPending}, out) == rtos::PollState::kInvalid);
    executor.cancel(rtos::Ticket{});
    executor.cancel(rtos::Ticket{1U, rtos::kMaxPending});
    for (size_t i = 0; i < rtos::kMaxPending; ++i) {
        CHECK(executor.post(cmd, rtos::Caller::kWeb, tickets[i]) == fw::Result::kOk);
    }
    for (const auto &t : tickets) {
        executor.cancel(t);
    }
    s_round_limit = 1;
    pump();
}

void invalid()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 2> storage{};
    rtos::ExecutorConfig cfg{};
    CHECK(executor.init(cfg, {}, rtos::viewOf(storage)) == fw::Status::kInvalidArg);
    cfg.name = "invalid";
    auto view = rtos::viewOf(storage);
    view.cmdArea = {};
    CHECK(executor.init(cfg, {}, view) == fw::Status::kInvalidArg);
    view = rtos::viewOf(storage);
    view.pending = {};
    CHECK(executor.init(cfg, {}, view) == fw::Status::kInvalidArg);
    view = rtos::viewOf(storage);
    view.pending = view.pending.subspan(0, rtos::kMaxPending - 1U);
    CHECK(executor.init(cfg, {}, view) == fw::Status::kInvalidArg);
    std::array<fw::Module, 2> modules{
        {{"a", 1, 0, 0, nullptr, &kUnsupportedOps}, {"b", 1, 0, 0, nullptr, &kUnsupportedOps}}};
    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kInvalidArg);
    modules[1].id = 2;
    modules[1].ops = nullptr;
    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kInvalidArg);
    cfg.core = configNUMBER_OF_CORES;
    CHECK(executor.init(cfg, {}, rtos::viewOf(storage)) == fw::Status::kInvalidArg);
}
} // namespace

namespace rtos {
uint32_t nowMs() noexcept
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
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *, uint32_t bytes,
                                           void *arg, UBaseType_t, StackType_t *, StaticTask_t *tcb,
                                           BaseType_t)
{
    CHECK(s_critical == 0);
    tcb->fn = fn;
    tcb->arg = arg;
    s_created = tcb;
    s_created_bytes = bytes;
    return tcb;
}
void xTaskNotifyGive(TaskHandle_t)
{
    CHECK(s_critical == 0);
    ++s_notify_gives;
}
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t ticks)
{
    CHECK(s_critical == 0);
    if (s_pump_receive && s_current == nullptr && ticks != 0U) {
        s_round_limit = 1;
        pump();
        return 1U;
    }
    s_clock += ticks;
    return 0U;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item, uint8_t *area,
                                 StaticQueue_t *queue)
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
    } else if (std::strcmp(argv[1], "async") == 0) {
        asyncApi();
    } else if (std::strcmp(argv[1], "invalid") == 0) {
        invalid();
    } else {
        return 2;
    }
    std::printf("%s %s\n", s_failures == 0 ? "PASS" : "FAIL", argv[1]);
    return s_failures == 0 ? 0 : 1;
}
