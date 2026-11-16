// 模块模板主机回归：验证 TemplateModule 接入 Executor 生命周期、CommandBus 路由与控制台命令表。
#include "AppContext.hpp"
#include "modules/_template/TemplateModule.hpp"
#include "fw/Module.hpp"
#include "rtos/EspStatus.hpp"
#include "SppServer.h"
#include "NvsStore.h"

#include <array>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace app {
namespace {
constinit AppContext s_test_app{};
}
AppContext &app_context() noexcept
{
    return s_test_app;
}
// 原有执行器场景已完成启动；组合根启动门控在 boot_alloc_check 单独验证。
bool AppContext::await_boot(BootTask, bool) noexcept
{
    return true;
}
} // namespace app

// 测试向量直接表达时刻、指令号与预期值，便于对照生命周期断言。
// NOLINTBEGIN(readability-magic-numbers)
namespace {
constexpr fw::ModuleId kTemplateId = 10U;
constexpr size_t kBitsPerByte = 8U;
uint32_t s_clock = 100U;
int s_failures = 0;
int s_critical = 0;
TaskHandle_t s_current = nullptr;
TaskHandle_t s_created = nullptr;
uint32_t s_round_limit = 1U;
uint32_t s_rounds = 0U;
bool s_pump_receive = false;
std::jmp_buf s_return{};
void check(bool value, const char *expression)
{
    if (!value) {
        std::fputs("FAIL ", stderr);
        std::fputs(expression, stderr);
        std::fputc('\n', stderr);
        ++s_failures;
    }
}
#define CHECK(value) check((value), #value)

void pump()
{
    const TaskHandle_t previous = s_current;
    s_current = s_created;
    s_rounds = 0U;
    if (setjmp(s_return) == 0) { // NOLINT(cppcoreguidelines-pro-bounds-array-to-pointer-decay) -- setjmp C
        // ABI，测试用来结束执行器无限循环。
        s_created->fn(s_created->arg);
    }
    s_current = previous;
}

fw::Result bt_stub_init(void *) noexcept
{
    return fw::Result::kOk;
}
constexpr fw::ModuleOps kBtStubOps{bt_stub_init, nullptr, nullptr, nullptr};

void wait_advance(void *, uint32_t max_ms) noexcept
{
    s_clock += max_ms;
}

fw::Command make_limit_cmd(fw::ModuleId target, uint32_t limit_ms) noexcept
{
    fw::Command cmd{};
    cmd.target = target;
    cmd.verb = app::verb(app::TemplateVerb::kSetLimit);
    cmd.arg_len = static_cast<uint8_t>(sizeof(uint32_t));
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        cmd.args[i] = static_cast<uint8_t>(limit_ms >> (i * kBitsPerByte));
    }
    return cmd;
}

void test_lifecycle()
{
    auto &ctx = app::app_context();
    ctx.bind_contexts();
    ctx.data.init();
    app::TemplateModule tmpl{ctx};

    const auto ops = fw::make_ops<app::TemplateModule>();
    CHECK(ops.init != nullptr && ops.start == nullptr && ops.poll != nullptr && ops.on_command != nullptr);

    // 未 init 前的保护分支。
    CHECK(!tmpl.poll(s_clock));
    fw::CommandResult pre_res{};
    tmpl.on_command(fw::Command{.verb = app::verb(app::TemplateVerb::kReset)}, pre_res);
    CHECK(pre_res.status == fw::Result::kInvalidState);

    static constinit std::array<fw::Module, 2> modules{{
        {"bt_stub", app::id(app::ModuleKind::kBtLink), app::id(app::ExecKind::kLink), 0U, nullptr, &kBtStubOps},
        {"template", kTemplateId, app::id(app::ExecKind::kLink), 0U, nullptr, nullptr},
    }};
    modules[1].self = &tmpl;
    modules[1].ops = &ops;

    static rtos::Executor s_exec{};
    static std::array<StackType_t, app::AppContext::kLinkStackBytes / sizeof(StackType_t)> s_link_stack{};
    ctx.link_executor = &s_exec;

    const rtos::ExecutorConfig cfg{
        .name = "tmpl_exec", .priority = 5U, .core = 1, .wait = wait_advance, .wait_ctx = nullptr};
    CHECK(ctx.link_executor->init(cfg, modules, app::view_of(ctx.link_storage, s_link_stack)) == fw::Status::kOk);
    CHECK(tmpl.state().initialized && tmpl.state().limit_ms == 1000U && tmpl.state().ticks == 0U);
    CHECK(ctx.link_executor->start() == fw::Status::kOk);

    // 第 1 轮：ticks=1；第 2 轮只过 100 ms（< 1000 ms）不变；推进到 >= 1000 ms 后 ticks=2。
    s_round_limit = 2U;
    pump();
    CHECK(tmpl.state().ticks == 1U);
    s_clock += 1000U;
    s_round_limit = 1U;
    pump();
    CHECK(tmpl.state().ticks == 2U);

    // 通过 Executor CommandBus 验证合法/非法命令。
    s_pump_receive = true;
    fw::CommandResult res{};
    CHECK(ctx.link_executor->call(make_limit_cmd(kTemplateId, 2500U), res, 100U) == fw::Result::kOk);
    CHECK(res.status == fw::Result::kOk && tmpl.state().limit_ms == 2500U);

    CHECK(ctx.link_executor->call(make_limit_cmd(kTemplateId, 0U), res, 100U) == fw::Result::kOk);
    CHECK(res.status == fw::Result::kInvalidArg);
    CHECK(ctx.link_executor->call(make_limit_cmd(kTemplateId, 60001U), res, 100U) == fw::Result::kOk);
    CHECK(res.status == fw::Result::kInvalidArg);

    fw::Command bad_len = make_limit_cmd(kTemplateId, 500U);
    bad_len.arg_len = 2U;
    CHECK(ctx.link_executor->call(bad_len, res, 100U) == fw::Result::kOk && res.status == fw::Result::kInvalidArg);

    const fw::Command reset_bad{.target = kTemplateId, .verb = app::verb(app::TemplateVerb::kReset), .arg_len = 1U};
    CHECK(ctx.link_executor->call(reset_bad, res, 100U) == fw::Result::kOk && res.status == fw::Result::kInvalidArg);

    const fw::Command reset_ok{.target = kTemplateId, .verb = app::verb(app::TemplateVerb::kReset), .arg_len = 0U};
    CHECK(ctx.link_executor->call(reset_ok, res, 100U) == fw::Result::kOk && res.status == fw::Result::kOk);
    CHECK(tmpl.state().ticks == 0U);

    const fw::Command bad_verb{.target = kTemplateId, .verb = 99U};
    CHECK(ctx.link_executor->call(bad_verb, res, 100U) == fw::Result::kOk && res.status == fw::Result::kInvalidArg);
}

void test_commands_before_exec()
{
    auto &ctx = app::app_context();
    ctx.data.init();
    app::SystemStatus sys{};
    sys.bt_ready = true;
    ctx.data.system_status.write(sys);
    rtos::ExecStats exec{};
    exec.rounds = 42U;
    ctx.data.exec_stats.write(exec);

    const auto cmds = app::template_cmds();
    const auto verbs = app::template_verbs();
    CHECK(cmds.size() == 1U && std::strcmp(cmds[0].name, "tmpl") == 0 && cmds[0].handler != nullptr);
    CHECK(verbs.size() == 2U && std::strcmp(verbs[0].name, "status") == 0 && std::strcmp(verbs[1].name, "limit") == 0);
    const auto fn = cmds[0].handler;

    auto prog = std::to_array("tmpl");
    auto sub_status = std::to_array("status");
    auto sub_limit = std::to_array("limit");
    auto sub_bad = std::to_array("bogus");
    auto arg_zero = std::to_array("0");
    auto arg_over = std::to_array("60001");
    auto arg_nan = std::to_array("abc");
    auto arg_ok = std::to_array("2500");

    std::array<char *, 1> only_prog{prog.data()};
    std::array<char *, 2> status_ok{prog.data(), sub_status.data()};
    std::array<char *, 3> status_extra{prog.data(), sub_status.data(), arg_ok.data()};
    std::array<char *, 2> bad_sub{prog.data(), sub_bad.data()};
    std::array<char *, 2> limit_missing{prog.data(), sub_limit.data()};
    std::array<char *, 3> limit_zero{prog.data(), sub_limit.data(), arg_zero.data()};
    std::array<char *, 3> limit_over{prog.data(), sub_limit.data(), arg_over.data()};
    std::array<char *, 3> limit_nan{prog.data(), sub_limit.data(), arg_nan.data()};
    std::array<char *, 3> limit_ok{prog.data(), sub_limit.data(), arg_ok.data()};
    std::array<char *, 4> limit_extra{prog.data(), sub_limit.data(), arg_ok.data(), arg_ok.data()};

    CHECK(fn(nullptr, -1, status_ok.data()) == 1);
    CHECK(fn(nullptr, 2, nullptr) == 1);
    CHECK(fn(nullptr, 1, only_prog.data()) == 1);
    CHECK(fn(nullptr, 2, status_ok.data()) == 0);
    CHECK(fn(nullptr, 3, status_extra.data()) == 1);
    CHECK(fn(nullptr, 2, bad_sub.data()) == 1);
    CHECK(fn(nullptr, 2, limit_missing.data()) == 1);
    CHECK(fn(nullptr, 3, limit_zero.data()) == 1);
    CHECK(fn(nullptr, 3, limit_over.data()) == 1);
    CHECK(fn(nullptr, 3, limit_nan.data()) == 1);
    CHECK(fn(nullptr, 4, limit_extra.data()) == 1);
    // 执行器未运行且目标为占位符时应安全拒绝并返回非 0。
    CHECK(fn(nullptr, 3, limit_ok.data()) == 1);
}

void test_commands_after_exec()
{
    const auto cmds = app::template_cmds();
    const auto fn = cmds[0].handler;
    auto prog = std::to_array("tmpl");
    auto sub_status = std::to_array("status");
    auto sub_limit = std::to_array("limit");
    auto arg_ok = std::to_array("2500");
    std::array<char *, 2> status_ok{prog.data(), sub_status.data()};
    std::array<char *, 3> limit_ok{prog.data(), sub_limit.data(), arg_ok.data()};

    // 执行器运行中：status 读取真实 DataCenter；占位目标 0xFF 经 submit() 安全返回 kNotFound -> 1。
    CHECK(fn(nullptr, 2, status_ok.data()) == 0);
    s_pump_receive = true;
    CHECK(fn(nullptr, 3, limit_ok.data()) == 1);
}
} // namespace

namespace rtos {
uint32_t now_ms() noexcept
{
    CHECK(s_critical == 0);
    return s_clock;
}
fw::Status from_esp_err(esp_err_t err) noexcept
{
    return (err == ESP_OK) ? fw::Status::kOk : fw::Status::kTimeout;
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
uint32_t esp_get_free_heap_size(void)
{
    return 0U;
}
uint32_t esp_get_minimum_free_heap_size(void)
{
    return 0U;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    CHECK(s_critical == 0);
    return s_current;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *, uint32_t, void *arg, UBaseType_t,
                                           StackType_t *, StaticTask_t *tcb, BaseType_t)
{
    CHECK(s_critical == 0);
    tcb->fn = fn;
    tcb->arg = arg;
    s_created = tcb;
    return tcb;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item, uint8_t *area, StaticQueue_t *queue)
{
    CHECK(s_critical == 0);
    queue->buf = area;
    queue->capacity = length;
    queue->item_size = item;
    queue->head = queue->count = 0U;
    return queue;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t)
{
    CHECK(s_critical == 0);
    if (queue->count == queue->capacity) {
        return pdFALSE;
    }
    const size_t tail = (queue->head + queue->count) % queue->capacity;
    const std::span bytes{queue->buf, queue->capacity * queue->item_size};
    std::memcpy(bytes.subspan(tail * queue->item_size).data(), item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks)
{
    CHECK(s_critical == 0);
    if ((queue->count == 0U) && s_pump_receive && (s_current == nullptr) && (ticks != 0U)) {
        pump();
    }
    if (queue->count == 0U) {
        s_clock += ticks;
        return pdFALSE;
    }
    const std::span bytes{queue->buf, queue->capacity * queue->item_size};
    std::memcpy(item, bytes.subspan(queue->head * queue->item_size).data(), queue->item_size);
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t)
{
    CHECK(s_critical == 0);
}
void vTaskDelete(TaskHandle_t)
{
    /* host 替身：pthread 入口返回即结束线程；追踪任务不在 host 场景启动，这里只是链接占位。 */
}
void vTaskDelay(TickType_t ticks)
{
    CHECK(s_critical == 0);
    s_clock += ticks;
    if (++s_rounds == s_round_limit) {
        // C ABI：配对 pump() 的 setjmp，测试用来结束执行器无限循环。
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
        std::longjmp(s_return, 1);
    }
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t)
{
    CHECK(s_critical == 0);
    return 2048U;
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
bool spp_server_poll_event(spp_server_evt_t *, uint32_t)
{
    return false;
}
size_t spp_server_read(uint32_t, uint8_t *, size_t)
{
    return 0U;
}
esp_err_t spp_server_write(uint32_t, const uint8_t *, size_t, uint32_t)
{
    return ESP_OK;
}
esp_err_t spp_server_reply_reserve(uint32_t, spp_server_tx_token_t *token)
{
    *token = {};
    return ESP_OK;
}
esp_err_t spp_server_tx_commit(const spp_server_tx_token_t *, const uint8_t *, size_t)
{
    return ESP_OK;
}
esp_err_t spp_server_tx_commit2(const spp_server_tx_token_t *, const uint8_t *, size_t, const uint8_t *, size_t)
{
    return ESP_OK;
}
void spp_server_tx_cancel(const spp_server_tx_token_t *) {}
bool spp_server_reply_ready(uint32_t)
{
    return true;
}
esp_err_t spp_server_push_latest(uint32_t, const uint8_t *, size_t)
{
    return ESP_OK;
}
esp_err_t spp_server_stream_bind(uint32_t, bool)
{
    return ESP_OK;
}
esp_err_t spp_server_stream_reserve(uint32_t, size_t, spp_server_tx_token_t *token)
{
    *token = {};
    return ESP_OK;
}
void spp_server_tx_stats(spp_server_tx_stats_t *stats)
{
    *stats = {};
}
esp_err_t spp_server_disconnect(uint32_t)
{
    return ESP_OK;
}
/* host 无 SPP 驱动：追踪钩子与 RX 查询替身（DebugModule/BtLinkModule::init 会调用）。 */
void spp_server_set_trace_hook(spp_server_trace_fn) {}
void spp_server_set_rx_wake_hook(spp_server_rx_wake_fn, void *) {}
bool spp_server_rx_ready(uint32_t)
{
    return false;
}
bool spp_server_any_rx_ready(void)
{
    return false;
}
void spp_server_rx_stats(spp_server_rx_stats_t *stats)
{
    if (stats != nullptr) {
        *stats = {};
    }
}
void xTaskNotifyGive(TaskHandle_t) {}
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t)
{
    return 0U;
}
esp_err_t nvs_store_init(void)
{
    return ESP_OK;
}
esp_err_t nvs_store_get_sn(char *, size_t)
{
    return ESP_OK;
}
esp_err_t nvs_store_set_sn(const char *)
{
    return ESP_OK;
}
esp_err_t nvs_store_set_alarms(const uint8_t (*)[NVS_STORE_ADDR_LEN], size_t)
{
    return ESP_OK;
}
esp_err_t nvs_store_get_alarms(uint8_t (*)[NVS_STORE_ADDR_LEN], size_t, size_t *count)
{
    *count = 0U;
    return ESP_OK;
}
}

int main(int argc, const char **raw_args)
{
    const std::span<const char *> argv{raw_args, static_cast<size_t>(argc)}; // argc>=0，无符号转换安全。
    if ((argc != 2) || (std::strcmp(argv[1], "template_module") != 0)) {
        return 2;
    }
    test_commands_before_exec();
    test_lifecycle();
    test_commands_after_exec();
    std::fputs((s_failures == 0) ? "PASS " : "FAIL ", stdout);
    std::puts(argv[1]);
    return (s_failures == 0) ? 0 : 1;
}
// NOLINTEND(readability-magic-numbers)
