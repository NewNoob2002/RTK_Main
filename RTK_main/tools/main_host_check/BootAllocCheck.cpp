// Host 单元测试：验证 BootArena 状态机/台账/故障注入无泄漏、BootTask/BootStreamBuffer 与业务模块外供存储绑定。
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <source_location>
#include <span>
#include <string_view>

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "data/AppData.hpp"
#include "data/GnssOutput.hpp"
#include "esp_err.h"
#include "fw/Lock.hpp"
#include "modules/debug/DebugModule.hpp"
#include "modules/gnss/RtcmPipe.hpp"
#include "modules/system/SystemMemory.hpp"
#include "rtos/BootAlloc.hpp"
#include "rtos/BootStreamBuffer.hpp"
#include "rtos/BootTask.hpp"
#include "rtos/EspStatus.hpp"
#include "SppServer.h"
#include "SppServerStorage.h"

// 同一组合根的真实 owner 计划与绑定/回滚逻辑（R8a 拆分为 app/ 下的 TU 后直接链接，不再整文件包含）。
#include "app/App.hpp"
#include "app/BootBuffers.hpp"

namespace {
bool s_task_create_fail = false;
uint32_t s_stack_bytes = 0U;
} // namespace

extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming, bugprone-reserved-identifier): GNU ld --wrap 要求的精确符号。
TaskHandle_t __wrap_xTaskCreateStaticPinnedToCore(TaskFunction_t, const char *, uint32_t bytes, void *, UBaseType_t,
                                                  StackType_t *, StaticTask_t *tcb, BaseType_t)
{
    s_stack_bytes = bytes;
    return s_task_create_fail ? nullptr : tcb;
}
void fake_log(const char *, const char *, const char *, ...) {}
esp_err_t esp_task_wdt_status(TaskHandle_t)
{
    return ESP_ERR_NOT_FOUND;
}
esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}
esp_err_t esp_task_wdt_delete(TaskHandle_t)
{
    return ESP_OK;
}
void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}
}

namespace rtos {
uint32_t now_ms() noexcept
{
    constexpr uint32_t kTestNowMs = 100U;
    return kTestNowMs;
}
} // namespace rtos

namespace app {
fw::Result StorageModule::save_keys(std::span<const fw::StoreKey> /*keys*/) noexcept
{
    return fw::Result::kOk;
}
} // namespace app

void check(bool passed, const char *expression, std::source_location location = std::source_location::current())
{
    if (!passed) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): host 断言的固定格式错误报告。
        std::fprintf(stderr, "CHECK FAILED (%s:%u): %s\n", location.file_name(), location.line(), expression);
        std::abort();
    }
}
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): 保留失败表达式文本；检查函数只求值一次。
#define CHECK(cond) check((cond), #cond)

// 数字是故障位置、边界值和协议测试向量；host 替身手动分配/释放用来核对 arena 的释放责任。
// NOLINTBEGIN(readability-magic-numbers, cppcoreguidelines-no-malloc)
namespace {

struct FaultAllocCtx {
    size_t fail_on_call = SIZE_MAX;
    size_t call_count = 0U;
    size_t live_allocs = 0U;
    size_t free_calls = 0U;
    size_t alloc_calls = 0U;
    size_t last_req_bytes = 0U;
};

void *fault_alloc(size_t align, size_t bytes, void *raw_ctx) noexcept
{
    auto *ctx = static_cast<FaultAllocCtx *>(raw_ctx);
    const size_t idx = ctx->call_count;
    ++ctx->call_count;
    if (idx >= ctx->fail_on_call) {
        return nullptr;
    }
    const size_t safe_align = (align < sizeof(void *)) ? sizeof(void *) : align;
    const size_t rounded = ((bytes + safe_align - 1U) / safe_align) * safe_align;
    void *ptr = std::aligned_alloc(safe_align, rounded);
    if (ptr != nullptr) {
        ++ctx->live_allocs;
        ++ctx->alloc_calls;
        ctx->last_req_bytes = rounded;
    }
    return ptr;
}

void fault_free(void *ptr, void *raw_ctx) noexcept
{
    auto *ctx = static_cast<FaultAllocCtx *>(raw_ctx);
    if (ptr != nullptr) {
        std::free(ptr);
        --ctx->live_allocs;
        ++ctx->free_calls;
    }
}

size_t fault_actual_size(void * /*ptr*/, void *raw_ctx) noexcept
{
    const auto *ctx = static_cast<const FaultAllocCtx *>(raw_ctx);
    return ctx->last_req_bytes + 16U;
}

rtos::BootAllocatorOps make_ops(FaultAllocCtx &ctx) noexcept
{
    return rtos::BootAllocatorOps{
        .alloc = &fault_alloc,
        .free = &fault_free,
        .allocated_size = &fault_actual_size,
        .ctx = &ctx,
    };
}

void test_arena_lifecycle_and_validation() noexcept
{
    FaultAllocCtx ctx{};
    const auto ops = make_ops(ctx);
    rtos::BootArena arena{};

    CHECK(arena.state() == rtos::ArenaState::kUninit);
    CHECK(arena.total_bytes() == 0U);
    CHECK(arena.total_actual_bytes() == 0U);
    CHECK(arena.entries().empty());
    CHECK(!arena.degraded());

    void *ptr = nullptr;
    CHECK(arena.allocate("early", 64U, 4U, ptr) == fw::Status::kInvalidState);
    CHECK(arena.seal() == fw::Status::kInvalidState);
    CHECK(arena.abort("uninit") == fw::Status::kOk);
    CHECK(arena.state() == rtos::ArenaState::kAborted);
    CHECK(arena.degraded());

    rtos::BootArena arena2{};
    CHECK(arena2.open(&ops) == fw::Status::kOk);
    CHECK(arena2.state() == rtos::ArenaState::kOpen);
    CHECK(arena2.open(&ops) == fw::Status::kInvalidState);

    // 参数非法拒绝
    CHECK(arena2.allocate(nullptr, 64U, 4U, ptr) == fw::Status::kInvalidArg);
    CHECK(arena2.allocate("", 64U, 4U, ptr) == fw::Status::kInvalidArg);
    CHECK(arena2.allocate("zero", 0U, 4U, ptr) == fw::Status::kInvalidArg);
    CHECK(arena2.allocate("align3", 64U, 3U, ptr) == fw::Status::kInvalidArg);
    CHECK(arena2.allocate("align6", 64U, 6U, ptr) == fw::Status::kInvalidArg);
    CHECK(arena2.allocate("round_overflow", SIZE_MAX, 16U, ptr) == fw::Status::kInvalidArg);
    std::span<uint64_t> too_big{};
    CHECK(arena2.alloc_array<uint64_t>("count_overflow", SIZE_MAX, too_big) == fw::Status::kInvalidArg);
    CHECK(too_big.empty());
    CHECK(arena2.alloc_array<uint64_t>("under_aligned", 1U, too_big, alignof(uint64_t) / 2U) ==
          fw::Status::kInvalidArg);
    CHECK(too_big.empty() && arena2.entries().empty() && ctx.alloc_calls == 0U);

    // 默认宿主分配器（ops=nullptr）与 align=0 退化为平台指针宽度对齐
    rtos::BootArena arena_def{};
    CHECK(arena_def.open(nullptr) == fw::Status::kOk);
    CHECK(arena_def.allocate("align0", 16U, 0U, ptr) == fw::Status::kOk);
    CHECK(ptr != nullptr);
    CHECK(arena_def.abort("cleanup_def") == fw::Status::kOk);

    // 正常分配与重复 tag 拒绝
    std::span<uint8_t> buf0{};
    CHECK(arena2.alloc_array<uint8_t>("buf.a", 128U, buf0, 16U) == fw::Status::kOk);
    CHECK(buf0.size() == 128U && buf0.data() != nullptr);
    CHECK(arena2.allocate("buf.a", 64U, 4U, ptr) == fw::Status::kInvalidArg);
    CHECK(arena2.total_bytes() == 128U);
    CHECK(arena2.total_actual_bytes() == 144U);
    CHECK(arena2.entries().size() == 1U);

    // seal 后只读，abort 也不释放已封存内存
    CHECK(arena2.seal() == fw::Status::kOk);
    CHECK(arena2.state() == rtos::ArenaState::kSealed);
    CHECK(arena2.allocate("late", 64U, 4U, ptr) == fw::Status::kInvalidState);
    CHECK(arena2.seal() == fw::Status::kInvalidState);
    CHECK(arena2.abort("after_seal") == fw::Status::kInvalidState);
    CHECK(ctx.live_allocs == 1U);
    CHECK(ctx.free_calls == 0U);
    CHECK(!arena2.degraded());

    // 运行期可标记非致命降级原因（例如 bt_not_ready）
    arena2.set_degraded_reason("bt_not_ready");
    CHECK(arena2.degraded());
    CHECK(std::string_view{arena2.reason()} == "bt_not_ready");

    // 测试收尾手动释放以避免 ASan 报告人工测试对象本身泄漏
    std::free(buf0.data());
}

void test_arena_max_entries_and_fault_injection() noexcept
{
    // 1. 超过 kMaxEntries (32) 拒绝
    {
        FaultAllocCtx ctx{};
        const auto ops = make_ops(ctx);
        rtos::BootArena arena{};
        CHECK(arena.open(&ops) == fw::Status::kOk);
        static constexpr std::array<const char *, 33> kTags{{
            "t00", "t01", "t02", "t03", "t04", "t05", "t06", "t07", "t08", "t09", "t10",
            "t11", "t12", "t13", "t14", "t15", "t16", "t17", "t18", "t19", "t20", "t21",
            "t22", "t23", "t24", "t25", "t26", "t27", "t28", "t29", "t30", "t31", "t32",
        }};
        for (size_t i = 0U; i < rtos::BootArena::kMaxEntries; ++i) {
            void *p = nullptr;
            CHECK(arena.allocate(kTags[i], 32U, 4U, p) == fw::Status::kOk);
        }
        void *extra = nullptr;
        CHECK(arena.allocate("overflow", 32U, 4U, extra) == fw::Status::kNoSpace);
        CHECK(ctx.live_allocs == rtos::BootArena::kMaxEntries);
        CHECK(arena.abort("max_entries_test") == fw::Status::kOk);
        CHECK(ctx.live_allocs == 0U);
        CHECK(ctx.free_calls == rtos::BootArena::kMaxEntries);
    }
}

void reset_test_arena() noexcept
{
    // 测试没有创建消费者；仅在所有引用已撤销、块已回收后重建台账。
    std::destroy_at(&app::s_app.boot_arena);
    std::construct_at(&app::s_app.boot_arena);
}

void test_production_allocation_plan() noexcept
{
    FaultAllocCtx probe{};
    auto ops = make_ops(probe);
    app::BootBuffers buffers{};
    CHECK(app::allocate_boot_buffers(buffers, &ops));
    const auto entries = app::s_app.boot_arena.entries();
    const size_t owners = entries.size();
    std::array<const char *, rtos::BootArena::kMaxEntries> names{};
    size_t requested = 0U;
    size_t stream_count = 0U;
    bool has_gnss = false;
    bool has_trace = false;
    for (size_t i = 0; i < owners; ++i) {
        const auto &entry = entries[i];
        names[i] = entry.tag;
        requested += entry.bytes;
        CHECK(entry.ptr != nullptr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 检查分配地址的数值对齐，不解引用。
        CHECK((reinterpret_cast<uintptr_t>(entry.ptr) % entry.align) == 0U);
        CHECK(entry.actual_bytes >= entry.bytes);
        const std::string_view name{entry.tag};
        if (name.starts_with("spp.tx.stream")) {
            ++stream_count;
            CHECK(name == "spp.tx.stream.phone" && entry.bytes == SPP_SERVER_STREAM_CAPACITY);
        }
        if (name.starts_with("spp.tx.reply")) {
            CHECK(entry.bytes == SPP_SERVER_REPLY_CAPACITY);
        }
        if (name == "spp.tx.merge") {
            CHECK(entry.bytes == SPP_SERVER_MERGE_CAPACITY);
        }
        has_gnss |= name == "gnss.latest";
        has_trace |= name == "trace.ring";
    }
    CHECK(stream_count == 1U);
    CHECK(has_gnss == bool(RTK_GNSS_ENABLED));
    CHECK(has_trace == bool(FW_TRACE_ENABLED));
    CHECK(requested == app::s_app.boot_arena.total_bytes());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): host 计划结果使用固定格式报告。
    std::printf("boot plan: owners=%zu requested=%zu actual=%zu gnss=%d trace=%d\n", owners, requested,
                app::s_app.boot_arena.total_actual_bytes(), RTK_GNSS_ENABLED, FW_TRACE_ENABLED);
    app::rollback_boot_bindings(buffers, "test:probe");
    CHECK(probe.live_allocs == 0U);
    reset_test_arena();

    // 对生产计划第一个至最后一个 owner 分别注入失败，验证先前块全部回收。
    for (size_t fail = 0; fail < owners; ++fail) {
        FaultAllocCtx ctx{.fail_on_call = fail};
        ops = make_ops(ctx);
        CHECK(!app::allocate_boot_buffers(buffers, &ops));
        CHECK(buffers.link_stack.empty() && buffers.spp_rx_0.empty() && buffers.spp_stream.empty());
        CHECK(ctx.live_allocs == 0U && ctx.free_calls == fail);
        CHECK(app::s_app.boot_arena.state() == rtos::ArenaState::kAborted);
        CHECK(app::s_app.boot_arena.entries().empty());
        std::array<char, rtos::BootArena::kMaxReasonBytes> expected{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): 构造受限长的预期错误文本。
        std::snprintf(expected.data(), expected.size(), "boot_alloc:%s", names[fail]);
        CHECK(std::string_view{app::s_app.boot_arena.reason()} == expected.data());
        reset_test_arena();
    }
}

void test_binding_rollback_and_sealed_lifetime() noexcept
{
    constexpr size_t kStages = 1U + (FW_TRACE_ENABLED ? 1U : 0U) + (RTK_GNSS_ENABLED ? 3U : 0U);
    for (size_t stage = 0; stage < kStages; ++stage) {
        FaultAllocCtx ctx{};
        const auto ops = make_ops(ctx);
        app::BootBuffers buffers{};
        CHECK(app::allocate_boot_buffers(buffers, &ops));
        auto invalid = buffers;
        size_t index = 0U;
        if (stage == index++) {
            invalid.spp_rx_1 = {};
        }
#if FW_TRACE_ENABLED
        if (stage == index++) {
            invalid.trace_ring = {};
        }
#endif
#if RTK_GNSS_ENABLED
        if (stage == index++) {
            invalid.gnss_latest = {};
        }
        if (stage == index++) {
            invalid.rtcm_pipe = {};
        }
        if (stage == index++) {
            invalid.gnss_demux = {};
        }
#endif
        const char *failure = app::bind_boot_buffers(invalid);
        CHECK(failure != nullptr);
        app::rollback_boot_bindings(buffers, failure);
        CHECK(app::s_app.boot_arena.state() == rtos::ArenaState::kAborted);
        CHECK(ctx.live_allocs == 0U);
        CHECK(spp_server_unbind_storage() == ESP_OK);
#if RTK_GNSS_ENABLED
        CHECK(app::s_app.rtcm_pipe.init() == fw::Status::kInvalidState);
        uint8_t reader = 0U;
        CHECK(app::s_app.data.gnss_app_ring.attach(1U, reader) == fw::Result::kInvalidState);
#endif
#if FW_TRACE_ENABLED
        CHECK(app::s_app.debug.init() == fw::Result::kInvalidState);
#endif
        reset_test_arena();
    }
    FaultAllocCtx ctx{};
    const auto ops = make_ops(ctx);
    app::BootBuffers buffers{};
    CHECK(app::allocate_boot_buffers(buffers, &ops));
    CHECK(app::bind_boot_buffers(buffers) == nullptr);
    CHECK(app::s_app.boot_arena.seal() == fw::Status::kOk);
    CHECK(app::s_app.boot_arena.abort("callback_alive") == fw::Status::kInvalidState);
    CHECK(ctx.free_calls == 0U);
    const uint8_t value = 0xA5U;
    buffers.spp_stream.back() = value;
    CHECK(buffers.spp_stream.back() == value);
    app::s_app.boot_arena.set_degraded_reason("task:create_failed");
    // 停机后块仍封存；host 不创建回调，撤销 view 后手动释放模拟重启。
    app::rollback_boot_bindings(buffers, "task:create_failed");
    CHECK(ctx.free_calls == 0U);
    for (const auto &entry : app::s_app.boot_arena.entries()) {
        fault_free(entry.ptr, &ctx);
    }
    CHECK(ctx.live_allocs == 0U);
    reset_test_arena();
}

int safe_command(void *ctx, int, char **) noexcept
{
    ++*static_cast<unsigned *>(ctx);
    return 0;
}

void test_console_gate_and_task_admission() noexcept
{
    unsigned calls = 0U;
    app::GatedCommand safe{&safe_command, &calls, true};
    app::GatedCommand business{&safe_command, &calls, false};
    app::s_app.boot_phase.store(app::BootPhase::kStarting);
    CHECK(app::gated_command(&safe, 0, nullptr) == cli::kCmdFail);
    app::set_console_only();
    CHECK(app::gated_command(&business, 0, nullptr) == cli::kCmdFail);
    CHECK(app::gated_command(&safe, 0, nullptr) == 0 && calls == 1U);
    CHECK(!app::s_app.await_boot(app::BootTask::kLink, true));
    CHECK((app::s_app.boot_stopped.load() & 1U) != 0U);
    app::s_app.boot_phase.store(app::BootPhase::kRunning);
    CHECK(app::gated_command(&business, 0, nullptr) == 0 && calls == 2U);
    CHECK(app::s_app.await_boot(app::BootTask::kIo, true));
    app::s_app.boot_phase.store(app::BootPhase::kStopping);
    CHECK(!app::s_app.await_boot(app::BootTask::kTrace, false));
    CHECK((app::s_app.boot_stopped.load() & 4U) != 0U);
}

std::array<char, 8192U> s_memory_output{};
size_t s_memory_output_size = 0U;

void memory_sink(const char *text, size_t bytes) noexcept
{
    CHECK(bytes < s_memory_output.size() - s_memory_output_size);
    std::memcpy(std::span{s_memory_output}.subspan(s_memory_output_size).data(), text, bytes);
    s_memory_output_size += bytes;
}

void test_memory_report() noexcept
{
    FaultAllocCtx ctx{};
    const auto ops = make_ops(ctx);
    rtos::BootArena arena{};
    CHECK(arena.open(&ops) == fw::Status::kOk);
    std::span<uint8_t> block{};
    CHECK(arena.alloc_array("test.owner", 256U, block) == fw::Status::kOk);
    CHECK(arena.seal() == fw::Status::kOk);
    std::array<app::MemoryHeapView, 4U> heaps{{
        {"internal_8bit", 1U, 200000U, 100000U, 81920U, 32768U},
        {"8bit", 2U, 200000U, 100000U, 81920U, 32768U},
        {"dma", 4U, 100000U, 50000U, 40000U, 20000U},
        {"default", 8U, 200000U, 100000U, 81920U, 32768U},
    }};
    cli::set_out_sink(&memory_sink, false);
    app::print_memory_report(arena, 100U, 200U, heaps);
    std::string_view output{s_memory_output.data(), s_memory_output_size};
    CHECK(output.find("test.owner") != output.npos && output.find("actual=272 B") != output.npos);
    CHECK(output.find("caps=0x") != output.npos && output.find("views overlap") != output.npos);
    CHECK(output.find("PASS (min>=81920 B, largest>=32768 B)") != output.npos);
    CHECK(output.find("normal") != output.npos);
    s_memory_output_size = 0U;
    heaps.front().minimum = 81919U;
    arena.set_degraded_reason("test:startup");
    app::print_memory_report(arena, 100U, 200U, heaps);
    output = {s_memory_output.data(), s_memory_output_size};
    CHECK(output.find("FAIL (min>=81920 B, largest>=32768 B)") != output.npos);
    CHECK(output.find("console-only: test:startup") != output.npos);
    s_memory_output_size = 0U;
    heaps.front().minimum = 81920U;
    heaps.front().largest = 32767U;
    app::print_memory_report(arena, 100U, 200U, heaps);
    output = {s_memory_output.data(), s_memory_output_size};
    CHECK(output.find("FAIL (min>=81920 B, largest>=32768 B)") != output.npos);
    cli::set_out_sink(nullptr, false);
    fault_free(block.data(), &ctx);
}

void dummy_task(void * /*arg*/) noexcept {}

void test_boot_task_and_stream_buffer() noexcept
{
    rtos::BootTask task{};
    CHECK(!task.running());
    CHECK(task.start({}, "t", 1U, 0, &dummy_task, nullptr) == fw::Status::kInvalidArg);
    std::array<StackType_t, 512U> stack{};
    CHECK(task.start(stack, nullptr, 1U, 0, &dummy_task, nullptr) == fw::Status::kInvalidArg);
    CHECK(task.start(stack, "boot_t", 1U, 0, nullptr, nullptr) == fw::Status::kInvalidArg);
    CHECK(task.start(stack, "boot_t", 1U, -1, &dummy_task, nullptr) == fw::Status::kInvalidArg);
    CHECK(task.start(stack, "boot_t", configMAX_PRIORITIES, 0, &dummy_task, nullptr) == fw::Status::kInvalidArg);
    s_task_create_fail = true;
    CHECK(task.start(stack, "boot_t", 1U, 0, &dummy_task, nullptr) == fw::Status::kNoSpace);
    CHECK(!task.running());
    s_task_create_fail = false;
    CHECK(task.start(stack, "boot_t", 1U, 0, &dummy_task, nullptr) == fw::Status::kOk);
    CHECK(s_stack_bytes == sizeof(stack));
    CHECK(task.running());
    CHECK(task.start(stack, "boot_t2", 1U, 0, &dummy_task, nullptr) == fw::Status::kBusy);

    rtos::BootStreamBuffer sb{};
    std::array<uint8_t, 64U> small{};
    CHECK(sb.init({}, 1U) == fw::Status::kInvalidArg);
    CHECK(sb.init(small, 0U) == fw::Status::kInvalidArg);
    CHECK(sb.init(small, 1U) == fw::Status::kOk);
    CHECK(sb.init(small, 1U) == fw::Status::kBusy);
    const std::array<uint8_t, 4> payload{1U, 2U, 3U, 4U};
    CHECK(sb.send(payload, 0U) == payload.size());
    CHECK(sb.available() == payload.size());
    CHECK(sb.space() < small.size());
    std::array<uint8_t, 8> rx{};
    CHECK(sb.receive(rx, 0U) == payload.size());
    CHECK(std::memcmp(rx.data(), payload.data(), payload.size()) == 0);
    CHECK(sb.available() == 0U);
    sb.deinit();
    CHECK(sb.init(small, 1U) == fw::Status::kOk);
    sb.deinit();
}

void test_consumer_storage_bindings() noexcept
{
    // 1. GnssLatestCache
    app::data::GnssLatestCache cache{};
    cache.bind_lock(fw::kNoLock);
    CHECK(cache.init({}) == fw::Status::kInvalidArg);
    std::array<uint8_t, app::data::kGnssLatestTotalBytes> latest_buf{};
    CHECK(cache.init(latest_buf) == fw::Status::kOk);
    constexpr auto kGga = std::to_array("$GNGGA,123519,4807.038,N*47\r\n");
    const auto gga = std::bit_cast<std::array<uint8_t, sizeof(kGga)>>(kGga);
    const std::span<const uint8_t> sentence{gga.data(), gga.size() - 1U};
    CHECK(cache.update(0U, sentence));
    std::array<uint8_t, 64U> out{};
    const auto q = cache.query(0U, out);
    CHECK(q.ok && q.len == sentence.size());

    // 2. DataCenter GNSS 缓冲绑定（当 RTK_GNSS_ENABLED=1 时启用）
#if RTK_GNSS_ENABLED
    app::data::DataCenter data{};
    data.init();
    std::array<uint8_t, app::kGnssAppRingBytes> ring_buf{};
    CHECK(data.init_gnss_buffers({}, latest_buf) == fw::Status::kInvalidArg);
    CHECK(data.init_gnss_buffers(ring_buf, {}) == fw::Status::kInvalidArg);
    CHECK(data.init_gnss_buffers(ring_buf, latest_buf) == fw::Status::kOk);
    uint8_t rid = 0U;
    CHECK(data.gnss_app_ring.attach(1U, rid) == fw::Result::kOk);
    CHECK(data.gnss_app_ring.write(0U, sentence) == fw::Result::kOk);
    fw::FanoutView view{};
    CHECK(data.gnss_app_ring.peek(rid, view) == fw::Result::kOk);
    CHECK(data.gnss_app_ring.commit(view.lease) == fw::Result::kOk);
#endif

    // 3. RtcmPipe 外供缓冲绑定
    app::RtcmPipe pipe{};
    pipe.init_lock();
    CHECK(pipe.init() == fw::Status::kInvalidState);
    CHECK(pipe.bind_storage({}) == fw::Status::kInvalidArg);
    std::array<uint8_t, app::kRtcmPipeBytes> pipe_buf{};
    CHECK(pipe.bind_storage(pipe_buf) == fw::Status::kOk);
    CHECK(pipe.init() == fw::Status::kOk);

    // 4. DebugModule 外供缓冲绑定
#if FW_TRACE_ENABLED
    static app::DebugModule dbg{};
    CHECK(dbg.init() == fw::Result::kInvalidState);
    static std::array<fw::TraceEvent, app::DebugModule::kRingEvents> trace_buf{};
    static std::array<uint8_t, app::DebugModule::kLogRingBytes> log_buf{};
    static std::array<StackType_t, app::DebugModule::kTaskStackBytes / sizeof(StackType_t)> task_stack{};
    CHECK(dbg.bind_storage({{}, log_buf, task_stack}) == fw::Status::kInvalidArg);
    CHECK(dbg.bind_storage({trace_buf, {}, task_stack}) == fw::Status::kInvalidArg);
    CHECK(dbg.bind_storage({trace_buf, log_buf, {}}) == fw::Status::kInvalidArg);
    CHECK(dbg.bind_storage({trace_buf, log_buf, task_stack}) == fw::Status::kOk);
    CHECK(dbg.init() == fw::Result::kOk);
#endif
}

} // namespace

// NOLINTEND(readability-magic-numbers, cppcoreguidelines-no-malloc)
int main()
{
    test_arena_lifecycle_and_validation();
    test_arena_max_entries_and_fault_injection();
    test_production_allocation_plan();
    test_binding_rollback_and_sealed_lifetime();
    test_console_gate_and_task_admission();
    test_memory_report();
    test_boot_task_and_stream_buffer();
    test_consumer_storage_bindings();
    std::puts("boot_alloc_check: all tests PASS");
    return 0;
}
