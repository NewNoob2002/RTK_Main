// 多线程执行器异步总线主机回归：链接真实 pthread 版 FakeRtos，验证多生产者并发 post/call/cancel/延迟完成。
// 本文件仅用于 host 门禁，不参与固件构建。
#include "rtos/Executor.hpp"
#include "rtos/EspStatus.hpp"
#include "esp_task_wdt.h"
#include "esp_log.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

std::atomic<int> s_failures{0};

#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value);                               \
            s_failures.fetch_add(1, std::memory_order_relaxed);                                    \
        }                                                                                          \
    } while (false)

struct HostTaskGuard {
    StaticTask_t tcb{};

    HostTaskGuard() noexcept
    {
        pthread_mutex_init(&tcb.lock, nullptr);
        pthread_condattr_t attr{};
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&tcb.cond, &attr);
        pthread_condattr_destroy(&attr);
        tcb.notif = 0U;
        tcb.started = true;
        fake_current_task = &tcb;
    }

    ~HostTaskGuard() noexcept
    {
        fake_current_task = nullptr;
        pthread_cond_destroy(&tcb.cond);
        pthread_mutex_destroy(&tcb.lock);
    }
};

constexpr uint8_t kVerbEcho = 0U;
constexpr uint8_t kVerbDefer = 1U;
constexpr uint8_t kVerbSlowDefer = 2U;

struct DeferredItem {
    uint32_t seq = 0U;
    int32_t value = 0;
    uint32_t readyAtMs = 0U;
    bool active = false;
};

struct AsyncWorker {
    rtos::Executor *executor = nullptr;
    std::array<DeferredItem, 16> deferred{};
    std::atomic<uint32_t> dispatched{0U};
    std::atomic<uint32_t> completed{0U};
    std::array<int32_t, 8> fifoLog{};
    size_t fifoCount = 0U;
    bool recordFifo = false;
};

int32_t decodePayload(const fw::Command &cmd) noexcept
{
    const uint32_t raw =
        static_cast<uint32_t>(cmd.args[0]) | (static_cast<uint32_t>(cmd.args[1]) << 8U) |
        (static_cast<uint32_t>(cmd.args[2]) << 16U) | (static_cast<uint32_t>(cmd.args[3]) << 24U);
    return static_cast<int32_t>(raw);
}

void encodePayload(fw::Command &cmd, uint32_t value) noexcept
{
    cmd.argLen = 4U;
    cmd.args[0] = static_cast<uint8_t>(value & 0xFFU);
    cmd.args[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    cmd.args[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    cmd.args[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

bool workerPoll(void *self, uint32_t nowMs) noexcept
{
    auto &worker = *static_cast<AsyncWorker *>(self);
    bool anyLeft = false;
    for (auto &item : worker.deferred) {
        if (!item.active) {
            continue;
        }
        if (static_cast<int32_t>(nowMs - item.readyAtMs) >= 0) {
            item.active = false;
            worker.executor->complete(item.seq, fw::CommandResult{0U, fw::Result::kOk, item.value});
            worker.completed.fetch_add(1U, std::memory_order_relaxed);
        } else {
            anyLeft = true;
        }
    }
    return anyLeft;
}

void workerOnCommand(void *self, const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    auto &worker = *static_cast<AsyncWorker *>(self);
    worker.dispatched.fetch_add(1U, std::memory_order_relaxed);
    const int32_t val = decodePayload(cmd);
    if (worker.recordFifo && worker.fifoCount < worker.fifoLog.size()) {
        worker.fifoLog[worker.fifoCount++] = val;
    }
    if (cmd.verb == kVerbEcho) {
        result.status = fw::Result::kOk;
        result.detail = val;
        return;
    }
    const uint32_t delayMs = (cmd.verb == kVerbSlowDefer) ? 80U : 5U;
    for (auto &item : worker.deferred) {
        if (!item.active) {
            item.active = true;
            item.seq = cmd.seq;
            item.value = val;
            item.readyAtMs = rtos::nowMs() + delayMs;
            result.detail = fw::kDetailDeferred;
            return;
        }
    }
    result.status = fw::Result::kBusy;
}

constexpr fw::ModuleOps kWorkerOps{nullptr, nullptr, workerPoll, workerOnCommand};

void testMultiProducerPost()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 8> storage{};
    static AsyncWorker worker{};
    worker.executor = &executor;
    worker.recordFifo = true;
    static constinit std::array<fw::Module, 1> modules{{{"worker", 1, 0, 0, &worker, &kWorkerOps}}};
    const rtos::ExecutorConfig cfg{
        .name = "mp_post", .id = 0, .priority = 5, .core = 1, .idleWaitMs = 2, .yieldTicks = 1};
    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kOk);
    CHECK(executor.start() == fw::Status::kOk);

    // 1. 先批量投递 8 条，验证多线程真实队列下 FIFO 执行顺序。
    std::array<rtos::Ticket, rtos::kMaxPending> batch{};
    for (size_t i = 0; i < rtos::kMaxPending; ++i) {
        fw::Command cmd{.target = 1, .verb = kVerbEcho};
        encodePayload(cmd, static_cast<uint32_t>(100U + i));
        CHECK(executor.post(cmd, rtos::Caller::kWeb, batch[i]) == fw::Result::kOk);
    }
    for (size_t i = 0; i < rtos::kMaxPending; ++i) {
        fw::CommandResult out{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            const auto st = executor.poll(batch[i], out);
            if (st == rtos::PollState::kDone) {
                CHECK(out.seq == batch[i].seq);
                CHECK(out.detail == static_cast<int32_t>(100U + i));
                break;
            }
            CHECK(st == rtos::PollState::kPending);
            if (std::chrono::steady_clock::now() > deadline) {
                CHECK(false);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(worker.fifoCount == rtos::kMaxPending);
    for (size_t i = 0; i < rtos::kMaxPending; ++i) {
        CHECK(worker.fifoLog[i] == static_cast<int32_t>(100U + i));
    }

    // 2. 4 路并发生产者同时 post + poll（含即时与延迟完成）。
    constexpr size_t kProducers = 4U;
    constexpr size_t kPerThread = 50U;
    constexpr std::array<rtos::Caller, kProducers> kCallers{
        rtos::Caller::kConsole, rtos::Caller::kWeb, rtos::Caller::kPhone, rtos::Caller::kInternal};
    std::array<std::thread, kProducers> threads{};
    for (size_t p = 0; p < kProducers; ++p) {
        threads[p] = std::thread([&, p]() {
            for (size_t i = 0; i < kPerThread; ++i) {
                fw::Command cmd{.target = 1, .verb = (i % 5U == 0U) ? kVerbDefer : kVerbEcho};
                const uint32_t tag = static_cast<uint32_t>((p << 16U) | i);
                encodePayload(cmd, tag);
                rtos::Ticket ticket{};
                for (;;) {
                    const fw::Result r = executor.post(cmd, kCallers[p], ticket);
                    if (r == fw::Result::kOk) {
                        break;
                    }
                    CHECK(r == fw::Result::kBusy);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                fw::CommandResult out{};
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                for (;;) {
                    const rtos::PollState st = executor.poll(ticket, out);
                    if (st == rtos::PollState::kDone) {
                        CHECK(out.seq == ticket.seq);
                        CHECK(out.status == fw::Result::kOk);
                        CHECK(out.detail == static_cast<int32_t>(tag));
                        break;
                    }
                    CHECK(st == rtos::PollState::kPending);
                    if (std::chrono::steady_clock::now() > deadline) {
                        CHECK(false);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    CHECK(worker.dispatched.load() == rtos::kMaxPending + (kProducers * kPerThread));
}

void testConcurrentCallAndTimeout()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 8> storage{};
    static AsyncWorker worker{};
    worker.executor = &executor;
    static constinit std::array<fw::Module, 1> modules{{{"worker", 1, 0, 0, &worker, &kWorkerOps}}};
    const rtos::ExecutorConfig cfg{
        .name = "mp_call", .id = 0, .priority = 5, .core = 1, .idleWaitMs = 2, .yieldTicks = 1};
    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kOk);
    CHECK(executor.start() == fw::Status::kOk);

    // 3 个并发调用方同时阻塞 call()（含即时与延迟完成）。
    constexpr size_t kCallers = 3U;
    constexpr size_t kCallsEach = 20U;
    std::array<std::thread, kCallers> callers{};
    for (size_t c = 0; c < kCallers; ++c) {
        callers[c] = std::thread([&, c]() {
            const HostTaskGuard guard{};
            for (size_t i = 0; i < kCallsEach; ++i) {
                fw::Command cmd{.target = 1, .verb = ((i + c) % 2U == 0U) ? kVerbDefer : kVerbEcho};
                const uint32_t tag = static_cast<uint32_t>(0x1000U + (c * 100U) + i);
                encodePayload(cmd, tag);
                fw::CommandResult out{};
                const fw::Result r = executor.call(cmd, out, 2000U);
                CHECK(r == fw::Result::kOk);
                if (r == fw::Result::kOk) {
                    CHECK(out.status == fw::Result::kOk);
                    CHECK(out.detail == static_cast<int32_t>(tag));
                }
            }
        });
    }
    for (auto &t : callers) {
        t.join();
    }

    // 延迟命令 call 超时：80 ms 后才完成，给 15 ms 超时 → kTimeout；迟到 complete 释放槽位。
    {
        const HostTaskGuard guard{};
        fw::Command slow{.target = 1, .verb = kVerbSlowDefer};
        encodePayload(slow, 0xCAFEU);
        fw::CommandResult out{};
        CHECK(executor.call(slow, out, 15U) == fw::Result::kTimeout);
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        fw::Command fast{.target = 1, .verb = kVerbEcho};
        encodePayload(fast, 0xBEEFU);
        CHECK(executor.call(fast, out, 1000U) == fw::Result::kOk);
        CHECK(out.detail == static_cast<int32_t>(0xBEEFU));
    }
}

void testConcurrentCancelStress()
{
    static constinit rtos::Executor executor{};
    static constinit rtos::ExecutorStorage<6144, 8> storage{};
    static AsyncWorker worker{};
    worker.executor = &executor;
    static constinit std::array<fw::Module, 1> modules{{{"worker", 1, 0, 0, &worker, &kWorkerOps}}};
    const rtos::ExecutorConfig cfg{
        .name = "mp_cancel", .id = 0, .priority = 5, .core = 1, .idleWaitMs = 2, .yieldTicks = 1};
    CHECK(executor.init(cfg, modules, rtos::viewOf(storage)) == fw::Status::kOk);
    CHECK(executor.start() == fw::Status::kOk);

    constexpr size_t kWorkers = 3U;
    std::array<std::thread, kWorkers> threads{};
    for (size_t w = 0; w < kWorkers; ++w) {
        threads[w] = std::thread([&, w]() {
            for (size_t i = 0; i < 40U; ++i) {
                fw::Command cmd{.target = 1, .verb = (i % 2U == 0U) ? kVerbDefer : kVerbEcho};
                encodePayload(cmd, static_cast<uint32_t>((w << 8U) | i));
                rtos::Ticket ticket{};
                while (executor.post(cmd, rtos::Caller::kWeb, ticket) == fw::Result::kBusy) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (i % 3U == 0U) {
                    executor.cancel(ticket);
                } else if (i % 3U == 1U) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    executor.cancel(ticket);
                } else {
                    fw::CommandResult out{};
                    for (;;) {
                        const auto st = executor.poll(ticket, out);
                        if (st == rtos::PollState::kDone) {
                            break;
                        }
                        CHECK(st == rtos::PollState::kPending);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    // 等所有在途延迟命令排空完成，然后连续占满 8 槽确认无泄漏。
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    std::array<rtos::Ticket, rtos::kMaxPending> checkTickets{};
    fw::Command probeCmd{.target = 1, .verb = kVerbSlowDefer};
    for (size_t i = 0; i < rtos::kMaxPending; ++i) {
        CHECK(executor.post(probeCmd, rtos::Caller::kInternal, checkTickets[i]) == fw::Result::kOk);
    }
    for (const auto &t : checkTickets) {
        executor.cancel(t);
    }
}

} // namespace

namespace rtos {
uint32_t nowMs() noexcept
{
    return pdTICKS_TO_MS(xTaskGetTickCount());
}
} // namespace rtos

extern "C" {
void fake_log(const char *, const char *, const char *, ...) {}
const char *esp_err_to_name(esp_err_t)
{
    return "test";
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return fake_current_task;
}
void vTaskDelay(TickType_t ticks)
{
    if (ticks > 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
    }
}
void vTaskDelete(TaskHandle_t) {}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t)
{
    return 2048U;
}
esp_err_t esp_task_wdt_add(TaskHandle_t)
{
    return ESP_OK;
}
esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (std::strcmp(argv[1], "multi_producer_post") == 0) {
        testMultiProducerPost();
    } else if (std::strcmp(argv[1], "concurrent_call") == 0) {
        testConcurrentCallAndTimeout();
    } else if (std::strcmp(argv[1], "cancel_stress") == 0) {
        testConcurrentCancelStress();
    } else {
        return 2;
    }
    const int fails = s_failures.load();
    std::printf("%s %s\n", fails == 0 ? "PASS" : "FAIL", argv[1]);
    return fails == 0 ? 0 : 1;
}
