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
#include "esp_log.h"
#include "esp_task_wdt.h"
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
#include "app/BootReasons.hpp"
#include "board/BoardProfile.hpp"
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
#include "modules/web/WebOtaTransfer.hpp"
#endif

namespace {
bool s_task_create_fail = false;
uint32_t s_stack_bytes = 0U;
} // namespace

extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming, bugprone-reserved-identifier): GNU ld --wrap 要求的精确符号。
TaskHandle_t __wrap_xTaskCreateStaticPinnedToCore(TaskFunction_t, const char *, uint32_t bytes,
                                                  void *, UBaseType_t, StackType_t *,
                                                  StaticTask_t *tcb, BaseType_t)
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
uint32_t nowMs() noexcept
{
    constexpr uint32_t kTestNowMs = 100U;
    return kTestNowMs;
}
} // namespace rtos

namespace app {
fw::Result StorageModule::saveKeys(std::span<const fw::StoreKey> /*keys*/) noexcept
{
    return fw::Result::kOk;
}
} // namespace app

void check(bool passed, const char *expression,
           std::source_location location = std::source_location::current())
{
    if (!passed) {
        std::fprintf(stderr, "CHECK FAILED (%s:%u): %s\n", location.file_name(), location.line(),
                     expression);
        std::abort();
    }
}
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): 保留失败表达式文本；检查函数只求值一次。
#define CHECK(cond) check((cond), #cond)

// 数字是故障位置、边界值和协议测试向量；host 替身手动分配/释放用来核对 arena 的释放责任。
// NOLINTBEGIN(cppcoreguidelines-no-malloc): 注入失败的 host 分配器必须调用 malloc/free 模拟后端。
namespace {

struct FaultAllocCtx {
    size_t failOnCall = SIZE_MAX;
    size_t callCount = 0U;
    size_t liveAllocs = 0U;
    size_t freeCalls = 0U;
    size_t allocCalls = 0U;
    size_t lastReqBytes = 0U;
};

void *faultAlloc(size_t align, size_t bytes, void *rawCtx) noexcept
{
    auto *ctx = static_cast<FaultAllocCtx *>(rawCtx);
    const size_t idx = ctx->callCount;
    ++ctx->callCount;
    if (idx >= ctx->failOnCall) {
        return nullptr;
    }
    const size_t safeAlign = (align < sizeof(void *)) ? sizeof(void *) : align;
    const size_t rounded = ((bytes + safeAlign - 1U) / safeAlign) * safeAlign;
    void *ptr = std::aligned_alloc(safeAlign, rounded);
    if (ptr != nullptr) {
        ++ctx->liveAllocs;
        ++ctx->allocCalls;
        ctx->lastReqBytes = rounded;
    }
    return ptr;
}

void faultFree(void *ptr, void *rawCtx) noexcept
{
    auto *ctx = static_cast<FaultAllocCtx *>(rawCtx);
    if (ptr != nullptr) {
        std::free(ptr);
        --ctx->liveAllocs;
        ++ctx->freeCalls;
    }
}

size_t faultActualSize(void * /*ptr*/, void *rawCtx) noexcept
{
    const auto *ctx = static_cast<const FaultAllocCtx *>(rawCtx);
    return ctx->lastReqBytes + 16U;
}

rtos::BootAllocatorOps makeOps(FaultAllocCtx &ctx) noexcept
{
    return rtos::BootAllocatorOps{
        .alloc = &faultAlloc,
        .free = &faultFree,
        .allocatedSize = &faultActualSize,
        .ctx = &ctx,
    };
}

void testArenaLifecycleAndValidation() noexcept
{
    FaultAllocCtx ctx{};
    const auto ops = makeOps(ctx);
    rtos::BootArena arena{};

    CHECK(arena.state() == rtos::ArenaState::kUninit);
    CHECK(arena.totalBytes() == 0U);
    CHECK(arena.totalActualBytes() == 0U);
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
    std::span<uint64_t> tooBig{};
    CHECK(arena2.allocArray<uint64_t>("count_overflow", SIZE_MAX, tooBig) ==
          fw::Status::kInvalidArg);
    CHECK(tooBig.empty());
    CHECK(arena2.allocArray<uint64_t>("under_aligned", 1U, tooBig, alignof(uint64_t) / 2U) ==
          fw::Status::kInvalidArg);
    CHECK(tooBig.empty() && arena2.entries().empty() && ctx.allocCalls == 0U);

    // 默认宿主分配器（ops=nullptr）与 align=0 退化为平台指针宽度对齐
    rtos::BootArena arenaDef{};
    CHECK(arenaDef.open(nullptr) == fw::Status::kOk);
    CHECK(arenaDef.allocate("align0", 16U, 0U, ptr) == fw::Status::kOk);
    CHECK(ptr != nullptr);
    CHECK(arenaDef.abort("cleanup_def") == fw::Status::kOk);

    // 正常分配与重复 tag 拒绝
    std::span<uint8_t> buf0{};
    CHECK(arena2.allocArray<uint8_t>("buf.a", 128U, buf0, 16U) == fw::Status::kOk);
    CHECK(buf0.size() == 128U && buf0.data() != nullptr);
    CHECK(arena2.allocate("buf.a", 64U, 4U, ptr) == fw::Status::kInvalidArg);
    CHECK(arena2.totalBytes() == 128U);
    CHECK(arena2.totalActualBytes() == 144U);
    CHECK(arena2.entries().size() == 1U);

    // seal 后只读，abort 也不释放已封存内存
    CHECK(arena2.seal() == fw::Status::kOk);
    CHECK(arena2.state() == rtos::ArenaState::kSealed);
    CHECK(arena2.allocate("late", 64U, 4U, ptr) == fw::Status::kInvalidState);
    CHECK(arena2.seal() == fw::Status::kInvalidState);
    CHECK(arena2.abort("after_seal") == fw::Status::kInvalidState);
    CHECK(ctx.liveAllocs == 1U);
    CHECK(ctx.freeCalls == 0U);
    CHECK(!arena2.degraded());

    // 运行期可标记非致命降级原因（例如 bt_not_ready）
    arena2.setDegradedReason("bt_not_ready");
    CHECK(arena2.degraded());
    CHECK(std::string_view{arena2.reason()} == "bt_not_ready");

    // 测试收尾手动释放以避免 ASan 报告人工测试对象本身泄漏
    std::free(buf0.data());
}

void testArenaMaxEntriesAndFaultInjection() noexcept
{
    // 1. 超过 kMaxEntries (32) 拒绝
    {
        FaultAllocCtx ctx{};
        const auto ops = makeOps(ctx);
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
        CHECK(ctx.liveAllocs == rtos::BootArena::kMaxEntries);
        CHECK(arena.abort("max_entries_test") == fw::Status::kOk);
        CHECK(ctx.liveAllocs == 0U);
        CHECK(ctx.freeCalls == rtos::BootArena::kMaxEntries);
    }
}

void resetTestArena() noexcept
{
    // 测试没有创建消费者；仅在所有引用已撤销、块已回收后重建台账。
    std::destroy_at(&app::s_app.bootArena);
    std::construct_at(&app::s_app.bootArena);
}

void testProductionAllocationPlan() noexcept
{
    FaultAllocCtx probe{};
    auto ops = makeOps(probe);
    app::BootBuffers buffers{};
    CHECK(app::allocateBootBuffers(buffers, &ops));
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    CHECK(buffers.webOta.empty());
#endif
    const auto entries = app::s_app.bootArena.entries();
    const size_t owners = entries.size();
    std::array<const char *, rtos::BootArena::kMaxEntries> names{};
    size_t requested = 0U;
    size_t streamCount = 0U;
    bool hasGnss = false;
    bool hasTrace = false;
    for (size_t i = 0; i < owners; ++i) {
        const auto &entry = entries[i];
        names[i] = entry.tag;
        requested += entry.bytes;
        CHECK(entry.ptr != nullptr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 检查分配地址的数值对齐，不解引用。
        CHECK((reinterpret_cast<uintptr_t>(entry.ptr) % entry.align) == 0U);
        CHECK(entry.actualBytes >= entry.bytes);
        const std::string_view name{entry.tag};
        if (name.starts_with("spp.tx.stream")) {
            ++streamCount;
            CHECK(name == "spp.tx.stream.phone" && entry.bytes == SPP_SERVER_STREAM_CAPACITY);
        }
        if (name.starts_with("spp.tx.reply")) {
            CHECK(entry.bytes == SPP_SERVER_REPLY_CAPACITY);
        }
        if (name == "spp.tx.merge") {
            CHECK(entry.bytes == SPP_SERVER_MERGE_CAPACITY);
        }
        hasGnss |= name == "gnss.latest";
        hasTrace |= name == "trace.ring";
    }
    CHECK(streamCount == 1U);
    CHECK(hasGnss == bool(RTK_GNSS_ENABLED));
    CHECK(hasTrace == bool(FW_TRACE_ENABLED));
    CHECK(requested == app::s_app.bootArena.totalBytes());
    std::printf("boot plan: owners=%zu requested=%zu actual=%zu gnss=%d trace=%d\n", owners,
                requested, app::s_app.bootArena.totalActualBytes(), RTK_GNSS_ENABLED,
                FW_TRACE_ENABLED);
    app::rollbackBootBindings(buffers, "test:probe");
    CHECK(probe.liveAllocs == 0U);
    resetTestArena();

    // 对生产计划第一个至最后一个 owner 分别注入失败，验证先前块全部回收。
    for (size_t fail = 0; fail < owners; ++fail) {
        FaultAllocCtx ctx{.failOnCall = fail};
        ops = makeOps(ctx);
        CHECK(!app::allocateBootBuffers(buffers, &ops));
        CHECK(buffers.linkStack.empty() && buffers.sppRx0.empty() && buffers.sppStream.empty());
        CHECK(ctx.liveAllocs == 0U && ctx.freeCalls == fail);
        CHECK(app::s_app.bootArena.state() == rtos::ArenaState::kAborted);
        CHECK(app::s_app.bootArena.entries().empty());
        std::array<char, rtos::BootArena::kMaxReasonBytes> expected{};
        std::snprintf(expected.data(), expected.size(), "boot_alloc:%s", names[fail]);
        CHECK(std::string_view{app::s_app.bootArena.reason()} == expected.data());
        resetTestArena();
    }
}

/**
 * @brief 验证 Wi-Fi 模式分配、绑定和回滚 OTA 工作区，充电待机跳过分配
 * @note 单线程主机测试，非 ISR。
 */
void testWifiAllocationPlan() noexcept
{
    app::s_app.radioMode = app::RadioMode::kWifiConfig;
    FaultAllocCtx ctx{};
    auto ops = makeOps(ctx);
    app::BootBuffers buffers{};
    CHECK(app::allocateBootBuffers(buffers, &ops));
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    size_t otaIndex = 0U;
#endif
    for (const auto &entry : app::s_app.bootArena.entries()) {
        const std::string_view tag{entry.tag};
        CHECK(!tag.starts_with("spp.") && tag != "task.spp_worker");
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
        if (tag != "web.ota") {
            ++otaIndex;
        } else {
            CHECK(entry.ptr == buffers.webOta.data() && entry.bytes == app::web_ota::kBufferBytes);
        }
#endif
    }
#if RTK_GNSS_ENABLED
    CHECK(!buffers.gnssDemux.empty() && !buffers.rtcmPipe.empty());
#endif
    CHECK(app::bindBootBuffers(buffers) == nullptr);
#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    CHECK(otaIndex + 1U == app::s_app.bootArena.count());
    CHECK(buffers.webOta.size_bytes() == app::web_ota::kBufferBytes);
#endif
    app::rollbackBootBindings(buffers, "test:wifi");
    CHECK(ctx.liveAllocs == 0U);
    resetTestArena();

#if defined(CONFIG_RTK_OTA_ENABLE) && CONFIG_RTK_OTA_ENABLE
    ctx = {.failOnCall = otaIndex};
    CHECK(!app::allocateBootBuffers(buffers, &ops));
    CHECK(std::string_view{app::s_app.bootArena.reason()} == "boot_alloc:web.ota");
    CHECK(buffers.webFrames.empty() && buffers.webOta.empty() && ctx.liveAllocs == 0U);
    resetTestArena();

    ctx = {};
    CHECK(app::allocateBootBuffers(buffers, &ops));
    auto invalid = buffers;
    invalid.webOta = invalid.webOta.first(1U);
    const auto *failure = app::bindBootBuffers(invalid);
    CHECK(failure != nullptr && std::string_view{failure} == app::boot_reason::kBindWebOta);
    app::rollbackBootBindings(buffers, app::boot_reason::kBindWebOta);
    CHECK(buffers.webFrames.empty() && buffers.webOta.empty() && ctx.liveAllocs == 0U);
    resetTestArena();

    app::s_app.bootMode = app::board::BootMode::kChargeStandby;
    CHECK(app::allocateBootBuffers(buffers, &ops));
    CHECK(buffers.webFrames.empty() && buffers.webOta.empty());
    app::rollbackBootBindings(buffers, "test:standby");
    CHECK(ctx.liveAllocs == 0U);
    resetTestArena();
    app::s_app.bootMode = app::board::BootMode::kNormal;
#endif
    app::s_app.radioMode = app::RadioMode::kBluetooth;
}

/**
 * @brief 验证 OLED 栈与 1 KB 帧缓冲仅在含 Feature::kOled 板型上从 BootArena 分配、绑定与回滚（ROL1-04）
 * @note 单线程主机测试，非 ISR。
 */
void testOledBootAllocationPlan() noexcept
{
    // 1. E1 DevKit（无 Feature::kOled）：零 OLED BootArena 分配。
    app::s_app.boardProfile = app::board::byId(app::board::BoardId::kE1DevKit);
    CHECK(app::s_app.boardProfile != nullptr);
    {
        FaultAllocCtx ctx{};
        const auto ops = makeOps(ctx);
        app::BootBuffers buffers{};
        CHECK(app::allocateBootBuffers(buffers, &ops));
        CHECK(buffers.oledStack.empty() && buffers.oledFb.empty());
        for (const auto &entry : app::s_app.bootArena.entries()) {
            const std::string_view tag{entry.tag};
            CHECK(tag != "task.oled" && tag != "oled.fb");
        }
        CHECK(app::bindBootBuffers(buffers) == nullptr);
        app::rollbackBootBindings(buffers, "test:e1_no_oled");
        CHECK(ctx.liveAllocs == 0U);
        resetTestArena();
    }

    // 2. P2 v1.41（含 Feature::kOled）：正常与充电待机均分配 task.oled + oled.fb，故障注入与绑定失败零泄漏。
    app::s_app.boardProfile = app::board::byId(app::board::BoardId::kP2V141);
    CHECK(app::s_app.boardProfile != nullptr);
    size_t oledStackIdx = SIZE_MAX;
    size_t oledFbIdx = SIZE_MAX;
    {
        FaultAllocCtx ctx{};
        const auto ops = makeOps(ctx);
        app::BootBuffers buffers{};
        CHECK(app::allocateBootBuffers(buffers, &ops));
        CHECK(buffers.oledStack.size_bytes() == app::OledModule::kTaskStackBytes);
        CHECK(buffers.oledFb.size_bytes() == app::OledModule::kFrameBufferBytes);
        const auto entries = app::s_app.bootArena.entries();
        for (size_t i = 0U; i < entries.size(); ++i) {
            const std::string_view tag{entries[i].tag};
            if (tag == "task.oled") {
                oledStackIdx = i;
                CHECK(entries[i].bytes == app::OledModule::kTaskStackBytes);
                CHECK(entries[i].align == 16U);
            } else if (tag == "oled.fb") {
                oledFbIdx = i;
                CHECK(entries[i].bytes == app::OledModule::kFrameBufferBytes);
                CHECK(entries[i].align >= 4U);
            }
        }
        CHECK(oledStackIdx != SIZE_MAX && oledFbIdx != SIZE_MAX);
        CHECK(app::bindBootBuffers(buffers) == nullptr);
        app::rollbackBootBindings(buffers, "test:p2_oled");
        CHECK(ctx.liveAllocs == 0U);
        resetTestArena();
    }

    // 故障注入：task.oled 与 oled.fb 失败均全量回滚。
    for (const auto &[idx, expected] : std::array<std::pair<size_t, const char *>, 2U>{
             {{oledStackIdx, "boot_alloc:task.oled"}, {oledFbIdx, "boot_alloc:oled.fb"}}}) {
        FaultAllocCtx ctx{.failOnCall = idx};
        const auto ops = makeOps(ctx);
        app::BootBuffers buffers{};
        CHECK(!app::allocateBootBuffers(buffers, &ops));
        CHECK(std::string_view{app::s_app.bootArena.reason()} == expected);
        CHECK(buffers.oledStack.empty() && buffers.oledFb.empty() && ctx.liveAllocs == 0U);
        resetTestArena();
    }

    // 绑定失败：截断 oledFb 必须返回 bind:oled 并安全回滚。
    {
        FaultAllocCtx ctx{};
        const auto ops = makeOps(ctx);
        app::BootBuffers buffers{};
        CHECK(app::allocateBootBuffers(buffers, &ops));
        auto invalid = buffers;
        invalid.oledFb = invalid.oledFb.first(16U);
        const char *failure = app::bindBootBuffers(invalid);
        CHECK(failure != nullptr && std::string_view{failure} == app::boot_reason::kBindOled);
        app::rollbackBootBindings(buffers, failure);
        CHECK(buffers.oledStack.empty() && buffers.oledFb.empty() && ctx.liveAllocs == 0U);
        resetTestArena();
    }

    // 充电待机下 P2 仍分配 OLED 缓冲以展示充电画面。
    app::s_app.bootMode = app::board::BootMode::kChargeStandby;
    {
        FaultAllocCtx ctx{};
        const auto ops = makeOps(ctx);
        app::BootBuffers buffers{};
        CHECK(app::allocateBootBuffers(buffers, &ops));
        CHECK(buffers.oledStack.size_bytes() == app::OledModule::kTaskStackBytes);
        CHECK(buffers.oledFb.size_bytes() == app::OledModule::kFrameBufferBytes);
        CHECK(app::bindBootBuffers(buffers) == nullptr);
        app::rollbackBootBindings(buffers, "test:p2_standby_oled");
        CHECK(ctx.liveAllocs == 0U);
        resetTestArena();
    }
    app::s_app.bootMode = app::board::BootMode::kNormal;
    app::s_app.boardProfile = nullptr;
}

void testBindingRollbackAndSealedLifetime() noexcept
{
    constexpr size_t kStages = 1U + (FW_TRACE_ENABLED ? 1U : 0U) + (RTK_GNSS_ENABLED ? 3U : 0U);
    for (size_t stage = 0; stage < kStages; ++stage) {
        FaultAllocCtx ctx{};
        const auto ops = makeOps(ctx);
        app::BootBuffers buffers{};
        CHECK(app::allocateBootBuffers(buffers, &ops));
        auto invalid = buffers;
        size_t index = 0U;
        if (stage == index++) {
            invalid.sppRx1 = {};
        }
#if FW_TRACE_ENABLED
        if (stage == index++) {
            invalid.traceRing = {};
        }
#endif
#if RTK_GNSS_ENABLED
        if (stage == index++) {
            invalid.gnssLatest = {};
        }
        if (stage == index++) {
            invalid.rtcmPipe = {};
        }
        if (stage == index++) {
            invalid.gnssDemux = {};
        }
#endif
        const char *failure = app::bindBootBuffers(invalid);
        CHECK(failure != nullptr);
        app::rollbackBootBindings(buffers, failure);
        CHECK(app::s_app.bootArena.state() == rtos::ArenaState::kAborted);
        CHECK(ctx.liveAllocs == 0U);
        CHECK(spp_server_unbind_storage() == ESP_OK);
#if RTK_GNSS_ENABLED
        CHECK(app::s_app.rtcmPipe.init() == fw::Status::kInvalidState);
        uint8_t reader = 0U;
        CHECK(app::s_app.data.gnssAppRing.attach(1U, reader) == fw::Result::kInvalidState);
#endif
#if FW_TRACE_ENABLED
        CHECK(app::s_app.debug.init() == fw::Result::kInvalidState);
#endif
        resetTestArena();
    }
    FaultAllocCtx ctx{};
    const auto ops = makeOps(ctx);
    app::BootBuffers buffers{};
    CHECK(app::allocateBootBuffers(buffers, &ops));
    CHECK(app::bindBootBuffers(buffers) == nullptr);
    CHECK(app::s_app.bootArena.seal() == fw::Status::kOk);
    CHECK(app::s_app.bootArena.abort("callback_alive") == fw::Status::kInvalidState);
    CHECK(ctx.freeCalls == 0U);
    const uint8_t value = 0xA5U;
    buffers.sppStream.back() = value;
    CHECK(buffers.sppStream.back() == value);
    app::s_app.bootArena.setDegradedReason("task:create_failed");
    // 停机后块仍封存；host 不创建回调，撤销 view 后手动释放模拟重启。
    app::rollbackBootBindings(buffers, "task:create_failed");
    CHECK(ctx.freeCalls == 0U);
    for (const auto &entry : app::s_app.bootArena.entries()) {
        faultFree(entry.ptr, &ctx);
    }
    CHECK(ctx.liveAllocs == 0U);
    resetTestArena();
}

int safeCommand(void *ctx, int, char **) noexcept
{
    ++*static_cast<unsigned *>(ctx);
    return 0;
}

void testConsoleGateAndTaskAdmission() noexcept
{
    unsigned calls = 0U;
    app::GatedCommand safe{&safeCommand, &calls, true};
    app::GatedCommand business{&safeCommand, &calls, false};
    app::s_app.bootPhase.store(app::BootPhase::kStarting);
    CHECK(app::gatedCommand(&safe, 0, nullptr) == cli::kCmdFail);
    app::setConsoleOnly();
    CHECK(app::gatedCommand(&business, 0, nullptr) == cli::kCmdFail);
    CHECK(app::gatedCommand(&safe, 0, nullptr) == 0 && calls == 1U);
    CHECK(!app::s_app.awaitBoot(app::BootTask::kLink, true));
    CHECK((app::s_app.bootStopped.load() & 1U) != 0U);
    app::s_app.bootPhase.store(app::BootPhase::kRunning);
    CHECK(app::gatedCommand(&business, 0, nullptr) == 0 && calls == 2U);
    CHECK(app::s_app.awaitBoot(app::BootTask::kIo, true));
    app::s_app.bootPhase.store(app::BootPhase::kStopping);
    CHECK(!app::s_app.awaitBoot(app::BootTask::kTrace, false));
    CHECK((app::s_app.bootStopped.load() & 4U) != 0U);
}

std::array<char, 8192U> s_memory_output{};
size_t s_memory_output_size = 0U;

void memorySink(const char *text, size_t bytes) noexcept
{
    CHECK(bytes < s_memory_output.size() - s_memory_output_size);
    std::memcpy(std::span{s_memory_output}.subspan(s_memory_output_size).data(), text, bytes);
    s_memory_output_size += bytes;
}

/**
 * @brief 验证两种构建的堆门槛、诊断格式及降级原因
 * @note 仅单线程 host 测试调用，会更换全局控制台输出端。
 */
void testMemoryReport() noexcept
{
    FaultAllocCtx ctx{};
    const auto ops = makeOps(ctx);
    rtos::BootArena arena{};
    CHECK(arena.open(&ops) == fw::Status::kOk);
    std::span<uint8_t> block{};
    CHECK(arena.allocArray("test.owner", 256U, block) == fw::Status::kOk);
    CHECK(arena.seal() == fw::Status::kOk);
    std::array<app::MemoryHeapView, 4U> heaps{{
        {"internal_8bit", 1U, 200000U, 100000U, 81920U, 32768U},
        {"8bit", 2U, 200000U, 100000U, 81920U, 32768U},
        {"dma", 4U, 100000U, 50000U, 40000U, 20000U},
        {"default", 8U, 200000U, 100000U, 81920U, 32768U},
    }};
    cli::setOutSink(&memorySink, false);
    app::printMemoryReport(arena, 100U, 200U, heaps, false);
    std::string_view output{s_memory_output.data(), s_memory_output_size};
    CHECK(output.find("test.owner") != output.npos && output.find("actual=272 B") != output.npos);
    CHECK(output.find("caps=0x") != output.npos && output.find("views overlap") != output.npos);
    CHECK(output.find("normal") != output.npos);
    arena.setDegradedReason("test:startup");
    struct BudgetCase {
        bool wifiEnabled;
        size_t minimum;
        size_t largest;
        const char *verdict;
    };
    constexpr std::array<BudgetCase, 6U> kCases{{
        {false, 81920U, 32768U, "PASS (min>=81920 B, largest>=32768 B)"},
        {false, 81919U, 32768U, "FAIL (min>=81920 B, largest>=32768 B)"},
        {false, 81920U, 32767U, "FAIL (min>=81920 B, largest>=32768 B)"},
        {true, 40960U, 32768U, "PASS (min>=40960 B, largest>=32768 B)"},
        {true, 40959U, 32768U, "FAIL (min>=40960 B, largest>=32768 B)"},
        {true, 40960U, 32767U, "FAIL (min>=40960 B, largest>=32768 B)"},
    }};
    for (const auto &test : kCases) {
        s_memory_output_size = 0U;
        heaps.front().minimum = test.minimum;
        heaps.front().largest = test.largest;
        app::printMemoryReport(arena, 100U, 200U, heaps, test.wifiEnabled);
        output = {s_memory_output.data(), s_memory_output_size};
        CHECK(output.find(test.verdict) != output.npos);
        CHECK(output.find(test.wifiEnabled ? "adr0017" : "adr0011") != output.npos);
        CHECK(output.find("console-only: test:startup") != output.npos);
    }
    s_memory_output_size = 0U;
    app::printMemoryReport(arena, 100U, 200U, {}, true);
    output = {s_memory_output.data(), s_memory_output_size};
    CHECK(output.find("FAIL (min>=40960 B, largest>=32768 B)") != output.npos);
    CHECK(output.find("adr0017") != output.npos);
    constexpr auto kWifiBudget = app::internalHeapBudget(true);
    constexpr auto kBtBudget = app::internalHeapBudget(false);
    static_assert(kWifiBudget.minimum == 40960U && kWifiBudget.largest == 32768U);
    static_assert(kBtBudget.minimum == 81920U && kBtBudget.largest == 32768U);
    cli::setOutSink(nullptr, false);
    faultFree(block.data(), &ctx);
}

void dummyTask(void * /*arg*/) noexcept {}

void testBootTaskAndStreamBuffer() noexcept
{
    rtos::BootTask task{};
    CHECK(!task.running());
    CHECK(task.start({}, "t", 1U, 0, &dummyTask, nullptr) == fw::Status::kInvalidArg);
    std::array<StackType_t, 512U> stack{};
    CHECK(task.start(stack, nullptr, 1U, 0, &dummyTask, nullptr) == fw::Status::kInvalidArg);
    CHECK(task.start(stack, "boot_t", 1U, 0, nullptr, nullptr) == fw::Status::kInvalidArg);
    CHECK(task.start(stack, "boot_t", 1U, -1, &dummyTask, nullptr) == fw::Status::kInvalidArg);
    CHECK(task.start(stack, "boot_t", configMAX_PRIORITIES, 0, &dummyTask, nullptr) ==
          fw::Status::kInvalidArg);
    s_task_create_fail = true;
    CHECK(task.start(stack, "boot_t", 1U, 0, &dummyTask, nullptr) == fw::Status::kNoSpace);
    CHECK(!task.running());
    s_task_create_fail = false;
    CHECK(task.start(stack, "boot_t", 1U, 0, &dummyTask, nullptr) == fw::Status::kOk);
    CHECK(s_stack_bytes == sizeof(stack));
    CHECK(task.running());
    CHECK(task.start(stack, "boot_t2", 1U, 0, &dummyTask, nullptr) == fw::Status::kBusy);

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

void testConsumerStorageBindings() noexcept
{
    // 1. GnssLatestCache
    app::data::GnssLatestCache cache{};
    cache.bindLock(fw::kNoLock);
    CHECK(cache.init({}) == fw::Status::kInvalidArg);
    std::array<uint8_t, app::data::kGnssLatestTotalBytes> latestBuf{};
    CHECK(cache.init(latestBuf) == fw::Status::kOk);
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
    std::array<uint8_t, app::kGnssAppRingBytes> ringBuf{};
    CHECK(data.initGnssBuffers({}, latestBuf) == fw::Status::kInvalidArg);
    CHECK(data.initGnssBuffers(ringBuf, {}) == fw::Status::kInvalidArg);
    CHECK(data.initGnssBuffers(ringBuf, latestBuf) == fw::Status::kOk);
    uint8_t rid = 0U;
    CHECK(data.gnssAppRing.attach(1U, rid) == fw::Result::kOk);
    CHECK(data.gnssAppRing.write(0U, sentence) == fw::Result::kOk);
    fw::FanoutView view{};
    CHECK(data.gnssAppRing.peek(rid, view) == fw::Result::kOk);
    CHECK(data.gnssAppRing.commit(view.lease) == fw::Result::kOk);
#endif

    // 3. RtcmPipe 外供缓冲绑定
    app::RtcmPipe pipe{};
    pipe.initLock();
    CHECK(pipe.init() == fw::Status::kInvalidState);
    CHECK(pipe.bindStorage({}) == fw::Status::kInvalidArg);
    std::array<uint8_t, app::kRtcmPipeBytes> pipeBuf{};
    CHECK(pipe.bindStorage(pipeBuf) == fw::Status::kOk);
    CHECK(pipe.init() == fw::Status::kOk);

    // 4. DebugModule 外供缓冲绑定
#if FW_TRACE_ENABLED
    static app::DebugModule dbg{};
    CHECK(dbg.init() == fw::Result::kInvalidState);
    static std::array<fw::TraceEvent, app::DebugModule::kRingEvents> traceBuf{};
    static std::array<uint8_t, app::DebugModule::kLogRingBytes> logBuf{};
    static std::array<StackType_t, app::DebugModule::kTaskStackBytes / sizeof(StackType_t)>
        taskStack{};
    CHECK(dbg.bindStorage({{}, logBuf, taskStack}) == fw::Status::kInvalidArg);
    CHECK(dbg.bindStorage({traceBuf, {}, taskStack}) == fw::Status::kInvalidArg);
    CHECK(dbg.bindStorage({traceBuf, logBuf, {}}) == fw::Status::kInvalidArg);
    CHECK(dbg.bindStorage({traceBuf, logBuf, taskStack}) == fw::Status::kOk);
    CHECK(dbg.init() == fw::Result::kOk);
#endif
}

} // namespace

// NOLINTEND(cppcoreguidelines-no-malloc)
int main()
{
    testArenaLifecycleAndValidation();
    testArenaMaxEntriesAndFaultInjection();
    testProductionAllocationPlan();
    testWifiAllocationPlan();
    testOledBootAllocationPlan();
    testBindingRollbackAndSealedLifetime();
    testConsoleGateAndTaskAdmission();
    testMemoryReport();
    testBootTaskAndStreamBuffer();
    testConsumerStorageBindings();
    std::puts("boot_alloc_check: all tests PASS");
    return 0;
}
