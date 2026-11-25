// 真实组合根、LinkService/解析器与 SppTransport 的 host 回归；仅替换 IDF/存储/字节传输边界。
// 虚拟时钟验证非阻塞提交与 RX 预算；驱动事件/并发在 t13 检查真实实现。
#include <algorithm>
#include <charconv>
#include <chrono>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <semaphore>
#include "support/ConsoleSupport.hpp"

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
}

#include "AppTestAdapter.hpp"
#include "app/PowerRequests.hpp"
#include "board/BatteryAdc.hpp"
#include "board/BoardProfile.hpp"
#include "board/BoardStartup.hpp"
#include "board/Charger.hpp"
#include "driver/gpio.h"
#include "def/GnssSubDef.hpp"
#include "modules/debug/TraceText.hpp"
#include "modules/debug/DebugKeys.hpp"
#include "modules/gnss/RtcmPipe.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "modules/storage/StoreCatalog.hpp"
#include "cli/Out.hpp"
#include "fw/Log.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "SppServerPairing.h"
#include "modules/storage/StoreWriteGate.hpp"
#include "modules/system/FactoryReset.hpp"
#include "modules/system/SystemModule.hpp"

namespace app {
/** @brief 主机测试桩：未链接 FactoryCmds 时默认工厂命令保持锁定。 @retval false 总是。 */
bool factoryUnlocked() noexcept
{
    return false;
}
} // namespace app

// 测试向量、故障注入时刻与协议原始数值直接对照断言；不用于产品逻辑。
namespace {
std::atomic<uint32_t> s_clock{100U};
thread_local const std::chrono::steady_clock::time_point *s_wall_clock_start = nullptr;

struct PowerWaitClock {
    uint32_t firstGapMs = 0U;
    unsigned waits = 0U;
};
thread_local PowerWaitClock *s_power_wait_clock = nullptr;
uint32_t s_write_delay = 0U;
std::array<size_t, 2> s_tx_used{};
std::array<bool, 2> s_tx_reserved{};
bool s_tx_hold = false;
uint32_t s_tx_serial = 0;
uint32_t s_tx_cancels = 0;
size_t s_write_calls = 0U;
std::array<uint32_t, 64> s_write_handles{};
std::array<uint32_t, 64> s_write_timeouts{};
std::array<std::array<uint8_t, proto::kMaxFrame>, 64> s_write_bytes{};
std::array<size_t, 64> s_write_sizes{};
std::array<size_t, 64> s_store_at_write{};
size_t s_store_calls = 0U;
esp_err_t s_store_result = ESP_OK;
esp_err_t s_nvs_init_result = ESP_OK;
unsigned s_nvs_init_calls = 0U;
unsigned s_user_erases = 0U;
unsigned s_factory_erases = 0U;
std::atomic<unsigned> s_restarts{0U};
esp_err_t s_erase_result = ESP_OK;
std::array<uint8_t, core::kAddrLen> s_bt_mac{0x10U, 0x20U, 0x30U, 0x40U, 0xB5U, 0xF2U};
esp_err_t s_bt_mac_result = ESP_OK;
esp_err_t s_spp_init_result = ESP_OK;
unsigned s_spp_init_calls = 0U;
std::array<char, app::kDeviceNameBuf> s_spp_init_name{};
spp_server_pair_query_t s_pair_query = nullptr;
size_t s_saved_count = 0U;
app::WhitelistRaw s_saved{};
std::array<std::array<uint8_t, 4096>, 2> s_input{};
std::array<size_t, 2> s_input_size{};
std::array<size_t, 2> s_input_offset{};
std::array<spp_server_evt_t, 32> s_events{};
size_t s_event_size = 0U;
size_t s_event_offset = 0U;
uint32_t s_read_delay = 0U;
uint32_t s_poll_wait = 0U;
spp_server_rx_wake_fn s_rx_wake_fn = nullptr;
void *s_rx_wake_ctx = nullptr;
uint32_t s_rx_wake_calls = 0U;
StaticTask_t s_host_task{};
bool s_use_host_task = false;
std::array<int, 40U> s_gpioLevels{};
unsigned s_gpio4Presses = 0U;
bool s_chargerBusy = false;
bool s_chargerReady = true;
bool s_usbGpioConfigFails = false;
fw::Status s_safeOutputsStatus = fw::Status::kOk;
bool s_batteryReadFails = false;
uint32_t s_batteryMv = 0U;
uint8_t s_chargerReg = app::board::kSgmReg08PgBit;
unsigned s_chargerReads = 0U;
esp_err_t s_scanResult = ESP_OK;
bool s_scanModePaused = false;
unsigned s_scanCalls = 0U;
void (*s_consoleDelay)() noexcept = nullptr;
unsigned s_disconnectCalls = 0U;
int s_failures = 0;
constexpr core::BtAddr kPhone{{1, 2, 3, 4, 5, 6}};
constexpr core::BtAddr kAlarm{{7, 8, 9, 10, 11, 12}};
constexpr core::BtAddr kOtherAlarm{{13, 14, 15, 16, 17, 18}};
void check(bool value, const char *expression, int line)
{
    if (!value) {
        std::array<char, 16> text{};
        const auto converted = std::to_chars(text.data(), &text.back(), line);
        *converted.ptr = '\0';
        std::fputs("FAIL ", stderr);
        std::fputs(text.data(), stderr);
        std::fputs(": ", stderr);
        std::fputs(expression, stderr);
        std::fputc('\n', stderr);
        ++s_failures;
    }
}
#define CHECK(value) check((value), #value, __LINE__)

void setup(std::span<const core::BtAddr> whitelist = {},
           core::Policy policy = core::Policy::kConcurrent)
{
    using namespace app;
    appContext().data.init();
    LinkStatus link{};
    link.policy = policy;
    link.whitelistCount = whitelist.size();
    std::copy(whitelist.begin(), whitelist.end(), link.whitelist.begin());
    appContext().data.linkStatus.write(link);
    SystemStatus system{};
    system.btReady = true;
    appContext().data.systemStatus.write(system);
    CHECK(appContext().bt_link.init() == fw::Result::kOk);
    CHECK(s_command_queue.init() == fw::Status::kOk);
    CHECK(s_result_queue.init() == fw::Status::kOk);
}

void event(spp_server_evt_type_t type, uint32_t handle, const core::BtAddr &addr = {})
{
    spp_server_evt_t evt{};
    evt.type = type;
    evt.handle = handle;
    for (size_t idx = 0U; idx < addr.bytes.size(); ++idx) {
        evt.addr[idx] = addr.bytes[idx];
    }
    s_events[s_event_size++] = evt;
}

void open(uint32_t handle, const core::BtAddr &addr)
{
    spp_server_evt_t evt{};
    evt.type = SPP_SERVER_EVT_OPEN;
    evt.handle = handle;
    for (size_t idx = 0U; idx < addr.bytes.size(); ++idx) {
        evt.addr[idx] = addr.bytes[idx];
    }
    app::handleEvent(evt, s_clock);
}

void append(size_t session, uint16_t msg, uint8_t sender, uint8_t type,
            std::span<const uint8_t> payload = {})
{
    size_t count = 0U;
    auto &bytes = s_input[session];
    CHECK(proto::buildFrame(std::span<uint8_t>{bytes}.subspan(s_input_size[session]), msg, sender,
                            type, payload, count) == fw::Status::kOk);
    s_input_size[session] += count;
}

void dirty(size_t session)
{
    spp_server_evt_t evt{};
    evt.type = SPP_SERVER_EVT_DATA;
    evt.handle = session + 1U;
    app::handleEvent(evt, s_clock);
}

void saveRecovery()
{
    using namespace app;
    const std::array whitelist{kAlarm};
    setup(whitelist);
    open(1U, kPhone);
    const std::array<uint8_t, 1> clear{0};
    append(0U, 0x44, 0, proto::msg_type::kSet, clear);
    dirty(0U);
    s_store_result = ESP_FAIL;
    uint32_t tick = s_clock;
    link_round(tick);
    CHECK(s_write_calls == 1U); // RAM 接受 ACK 仍先于 app 保存。
    CHECK(s_store_calls == 0U && s_whitelist_pending);
    CHECK(s_link_manager.whitelist().empty());
    CHECK(s_snapshot.whitelistPending && s_snapshot.whitelistError == ESP_OK);
    s_clock += 499U;
    persistWhitelist(s_clock);
    CHECK(s_store_calls == 0U);
    ++s_clock;
    persistWhitelist(s_clock);
    publish_snapshot(s_clock);
    CHECK(s_store_calls == 1U && s_snapshot.whitelistError == ESP_FAIL);
    s_store_result = ESP_OK;
    s_clock = s_whitelist_attempt_ms + 999U;
    persistWhitelist(s_clock);
    CHECK(s_store_calls == 1U && s_whitelist_pending);
    ++s_clock;
    persistWhitelist(s_clock);
    CHECK(s_store_calls == 2U && !s_whitelist_pending && s_saved_count == 0U);

    // 配对升级本身触发 dirty，第一次落盘失败仍保留，重试不需要新 dirty。
    s_link_manager.pairOpen(s_clock);
    open(2U, kAlarm);
    const std::array<uint8_t, 8> hello{1, 0, 80, 0, 0, 0, 0, 0};
    append(1U, core::kHelloMsgId, proto::sender::kAlarm, proto::msg_type::kPush, hello);
    dirty(1U);
    drainRx(s_clock);
    collectWhitelistDirty();
    CHECK(s_link_manager.whitelist().size() == 1U && s_whitelist_pending);
    s_store_result = ESP_FAIL;
    s_clock += 500U;
    persistWhitelist(s_clock);
    const size_t before = s_store_calls;
    CHECK(s_whitelist_pending);
    s_clock = s_whitelist_attempt_ms + 999U;
    persistWhitelist(s_clock);
    CHECK(s_store_calls == before);
    s_store_result = ESP_OK;
    ++s_clock;
    persistWhitelist(s_clock);
    CHECK(s_store_calls == before + 1U && !s_whitelist_pending);
    CHECK(s_saved_count == 1U && s_saved[0] == kAlarm.bytes);

    // 控制台清空走相同 pending，失败仍返回 StoreError；之后覆盖落盘为空表。
    Command command{};
    command.id = CommandId::kWhitelistClear;
    command.seq = 7U;
    CHECK(s_command_queue.send(command, 0U));
    s_store_result = ESP_FAIL;
    process_commands(s_clock);
    CommandResult result{};
    CHECK(s_result_queue.receive(result, 0U));
    CHECK(result.seq == 7U && result.status == CommandStatus::kStoreError);
    CHECK(s_whitelist_pending && s_link_manager.whitelist().empty());
    s_clock += kWhitelistRetryMs;
    s_store_result = ESP_OK;
    persistWhitelist(s_clock);
    CHECK(!s_whitelist_pending && s_saved_count == 0U);
    publish_snapshot(s_clock);
    CHECK(!s_snapshot.whitelistPending && s_snapshot.whitelistError == ESP_OK);
    CHECK(s_store_at_write[0] == 0U); // 0x44 接受 ACK 未等待落盘。

    // 已有 pending 的冷却比较容忍 32 位毫秒回绕。
    s_whitelist_pending = true;
    s_store_result = ESP_FAIL;
    s_clock = UINT32_MAX - 500U;
    persistWhitelist(s_clock, true);
    CHECK(s_whitelist_pending);
    s_store_result = ESP_OK;
    s_clock += kWhitelistRetryMs;
    persistWhitelist(s_clock);
    CHECK(!s_whitelist_pending && s_saved_count == 0U);
}

void storageBackoff()
{
    using namespace app;
    setup();
    const std::array whitelist{kAlarm};
    markWhitelistDirty(whitelist, s_clock);
    s_store_result = ESP_FAIL;
    s_clock += 499U;
    persistWhitelist(s_clock);
    CHECK(s_store_calls == 0U);
    ++s_clock;
    persistWhitelist(s_clock);
    CHECK(s_store_calls == 1U);
    const std::array<uint32_t, 7> delays{1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (const auto delay : delays) {
        CHECK(appContext().data.storeStatus.read().retryMs == delay);
        const size_t attempts = s_store_calls;
        s_clock = s_whitelist_attempt_ms + 5U;
        markWhitelistDirty({}, s_clock); // 重试仍保存最新 RAM 值，变化不缩短退避。
        s_clock = s_whitelist_attempt_ms + delay - 1U;
        persistWhitelist(s_clock);
        CHECK(s_store_calls == attempts);
        ++s_clock;
        persistWhitelist(s_clock);
        CHECK(s_store_calls == attempts + 1U && s_whitelist_pending);
    }
    s_store_result = ESP_OK;
    s_clock += 30000U;
    persistWhitelist(s_clock);
    CHECK(!s_whitelist_pending && s_saved_count == 0U);
    CHECK(appContext().data.storeStatus.read().failures == 8U &&
          appContext().data.storeStatus.read().retryMs == 0U);
}

/** @brief 关机首次冲刷缩短去抖，连续轮询保留本次失败的退避。 */
void powerSaveBackoff()
{
    using namespace app;
    setup();
    auto &storage = appContext().storage;
    storage.markWhitelistDirty(std::array{kAlarm}, s_clock);
    s_store_result = ESP_FAIL;
    CHECK(power::s_power_requests.requestShutdown(board::BootMode::kNormal,
                                                  board::ShutdownReason::kKey));
    static_cast<void>(storage.poll(s_clock));
    CHECK(s_store_calls == 0U);
    storage.expeditePendingWrites();
    static_cast<void>(storage.poll(s_clock));
    CHECK(s_store_calls == 1U && storage.hasPendingWrites());
    for (unsigned step = 0U; step < 99U; ++step) {
        s_clock += 10U;
        static_cast<void>(storage.poll(s_clock));
    }
    CHECK(s_store_calls == 1U && storage.hasPendingWrites());
    s_store_result = ESP_OK;
    s_clock += 10U;
    static_cast<void>(storage.poll(s_clock));
    CHECK(s_store_calls == 2U && !storage.hasPendingWrites());
    power::s_power_requests.releaseAction();
}

/** @brief 配置真实电源模块，GPIO、充电芯片和时间由主机提供。 @param mode: 本次运行档。 */
void setupPower(app::board::BootMode mode)
{
    using namespace app;
    setup();
    auto &ctx = appContext();
    ctx.boardProfile = board::byId(board::BoardId::kP2V141);
    ctx.bootMode = mode;
    s_gpioLevels[18] = 1;
    s_gpioLevels[27] = (s_chargerReg & board::kSgmReg08PgBit) != 0U ? 1 : 0;
    CHECK(fw::isOk(ctx.power.init()));
}

/**
 * @brief 按固定周期推进真实电源模块
 * @param ticks: 轮数
 * @param stepMs: 每轮间隔（ms）
 */
void pollPower(unsigned ticks, uint32_t stepMs = 10U)
{
    for (unsigned n = 0U; n < ticks && s_restarts == 0U; ++n) {
        s_clock += stepMs;
        static_cast<void>(app::appContext().power.poll(s_clock));
    }
}

/** @brief 验证待机拒绝关机、保持超时释放 GPIO4、再次长按成功后才重启。 */
void powerStandby()
{
    using namespace app;
    setupPower(board::BootMode::kChargeStandby);
    CHECK(!power::requestShutdown(board::ShutdownReason::kConsole));
    pollPower(4U);
    CHECK(s_gpio4Presses == 0U);
    s_gpioLevels[18] = 0;
    pollPower(251U);
    CHECK(s_gpioLevels[4] == 1 && s_gpio4Presses == 1U && s_restarts == 0U);
    pollPower(500U);
    CHECK(s_gpioLevels[4] == 0 && s_restarts == 0U);
    s_gpioLevels[18] = 1;
    pollPower(4U);
    s_gpioLevels[18] = 0;
    pollPower(251U);
    CHECK(s_gpioLevels[4] == 1 && s_gpio4Presses == 2U);
    CHECK(appContext().data.uiEvents.readKeyHold().standbyConfirm);
    s_gpioLevels[25] = 1;
    pollPower(3U);
    CHECK(s_gpioLevels[4] == 0 && s_restarts == 0U);
    CHECK(!power::s_power_requests.claimRestart());
    // 触发重启后松开物理按键，standbyConfirm 必须保持 true（防止待机唤醒环回退）。
    s_gpioLevels[18] = 1;
    pollPower(1U);
    CHECK(appContext().data.uiEvents.readKeyHold().standbyConfirm);
    pollPower(15U);
    CHECK(s_restarts == 1U);
}

/** @brief 验证无充电芯片时插拔 USB（GPIO27 变化）更新快照并立即唤醒 OLED 任务。 */
void powerUsbChangeWakesUi()
{
    using namespace app;
    s_chargerReady = false;
    setupPower(board::BootMode::kNormal);
    s_gpioLevels[25] = 1;
    s_gpioLevels[27] = 0;
    auto &ctx = appContext();
    pollPower(2U);
    CHECK(!ctx.data.powerStatus.read().usbPresent && !ctx.data.powerStatus.read().chargerPresent);
    unsigned wakes = 0U;
    ctx.data.uiEvents.setWakeHook([](void *arg) noexcept { ++(*static_cast<unsigned *>(arg)); },
                                  &wakes);
    s_gpioLevels[27] = 1;
    pollPower(1U);
    CHECK(ctx.data.powerStatus.read().usbPresent && wakes == 1U);
    s_gpioLevels[27] = 0;
    pollPower(1U);
    CHECK(!ctx.data.powerStatus.read().usbPresent && wakes == 2U);
    ctx.data.uiEvents.setWakeHook(nullptr, nullptr);
}

/** @brief 验证 PowerModule 短按经 UiEventBus 通知 UI，且关机阶段唤醒必在 powerStatus 发布之后（ROL1-03）。 */
void powerShortKeyUi()
{
    using namespace app;
    setupPower(board::BootMode::kNormal);
    s_gpioLevels[25] = 1; // U3 上电保持中，直到测试显式模拟 U3 关断
    auto &ctx = appContext();
    struct WakeObserver {
        AppContext *app = nullptr;
        uint32_t wakes = 0U;
        uint32_t sawAnnounceOnWake = 0U;
        uint32_t sawCutPowerOnWake = 0U;
        board::ShutdownStage lastStage = board::ShutdownStage::kIdle;
    };
    WakeObserver obs{.app = &ctx};
    ctx.data.uiEvents.setWakeHook(
        [](void *arg) noexcept {
            // 测试观察者固定为 WakeObserver*。
            auto &o = *static_cast<WakeObserver *>(arg);
            ++o.wakes;
            o.lastStage = o.app->data.powerStatus.read().shutdownStage;
            if (o.lastStage == board::ShutdownStage::kAnnounce) {
                ++o.sawAnnounceOnWake;
            } else if (o.lastStage == board::ShutdownStage::kCutPower) {
                ++o.sawCutPowerOnWake;
            }
        },
        &obs);
    // 初始先观测松开（GPIO18=1）以完成开机按住保护。
    s_gpioLevels[18] = 1;
    pollPower(4U);
    CHECK(ctx.data.uiEvents.shortKeyCount() == 0U && obs.wakes == 0U);
    // 按下 100 ms 后松开 -> 触发 KeyEvent::kShort。
    s_gpioLevels[18] = 0;
    pollPower(10U);
    s_gpioLevels[18] = 1;
    pollPower(4U);
    CHECK(ctx.data.uiEvents.shortKeyCount() == 1U && obs.wakes >= 1U);
    CHECK(ctx.data.powerStatus.read().shortPresses == 1U);
    // OLED 在线时：正常模式长按 2.5 s 仅通知 UI（longKeyCount 递增），不直接发起关机。
    ctx.data.uiEvents.setOledActive(true);
    s_gpioLevels[18] = 0;
    pollPower(100U); // 1000 ms：应处于 holding 态
    CHECK(ctx.data.uiEvents.readKeyHold().active);
    pollPower(160U); // 2600 ms：越过 2500 ms 长按阈值
    CHECK(ctx.data.uiEvents.longKeyCount() == 1U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kIdle);
    s_gpioLevels[18] = 1;
    pollPower(5U);
    CHECK(!ctx.data.uiEvents.readKeyHold().active);
    ctx.data.uiEvents.setOledActive(false);
    // 发起关机 -> 唤醒回调执行瞬间 powerStatus.shutdownStage 必须已发布为 kAnnounce（不得为旧值 kIdle）。
    CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
    pollPower(1U);
    CHECK(obs.sawAnnounceOnWake == 1U);
    CHECK(ctx.data.powerStatus.read().shutdownDoneMask ==
          board::shutdownDoneBit(board::ShutdownDoneBit::kAnnounced));
    // 推进至 kCutPower -> 唤醒瞬间 powerStatus.shutdownStage 必须已发布为 kCutPower。
    pollPower(80U);
    CHECK(obs.sawCutPowerOnWake == 1U);
    CHECK((ctx.data.powerStatus.read().shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kCutPower)) != 0U);
    // USB 路径 + OLED 在线：U3 翻转前全程保持 s_restarts == 0（不清零，R2OL2-05）；
    // U3 掉电后先发布 kUsbOffReady，越过 30 ms 日志排空点后再过 20 ms（共 50 ms）仍被拦住，
    // OLED 标记终帧刷出后，为用户保留 300 ms 停留。
    CHECK(s_restarts == 0U);
    ctx.data.uiEvents.setOledActive(true);
    s_gpioLevels[27] = 1; // USB 在位
    s_gpioLevels[25] = 0; // U3 关断
    pollPower(1U);
    CHECK((ctx.data.powerStatus.read().shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady)) != 0U);
    CHECK(s_restarts == 0U);
    pollPower(3U); // +30 ms：日志排空点到达，锁存日志放行并开始计 OLED 30 ms 绝对截止
    CHECK(s_restarts == 0U);
    pollPower(2U); // +50 ms（日志排空后 +20 ms < 30 ms 上限）：仍不得重启
    CHECK(s_restarts == 0U);
    ctx.data.uiEvents.markShutdownFinalFrameFlushed();
    pollPower(1U, 5U); // +55 ms：观察刷出并开始停留
    pollPower(1U, board::kShutdownFinalVisibleMs - 1U);
    CHECK(s_restarts == 0U);
    pollPower(1U, 1U);
    CHECK(s_restarts == 1U);
    ctx.data.uiEvents.setOledActive(false);
    ctx.data.uiEvents.setWakeHook(nullptr, nullptr);
}

/** @brief 验证无 OLED（oledActive=false）时正常模式长按 2.5 s 保持直接关机行为。 */
void powerLongKeyNoOled()
{
    using namespace app;
    setupPower(board::BootMode::kNormal);
    s_gpioLevels[25] = 1;
    auto &ctx = appContext();
    ctx.data.uiEvents.setOledActive(false);
    // 初始观测松开解除开机按住保护。
    s_gpioLevels[18] = 1;
    pollPower(4U);
    // 长按 2.4 s 松开不关机、不发短按。
    s_gpioLevels[18] = 0;
    pollPower(240U);
    CHECK(ctx.data.uiEvents.readKeyHold().active);
    s_gpioLevels[18] = 1;
    pollPower(5U);
    CHECK(!ctx.data.uiEvents.readKeyHold().active);
    CHECK(ctx.data.uiEvents.shortKeyCount() == 0U && ctx.data.uiEvents.longKeyCount() == 0U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kIdle);
    // 长按满 2.5 s 且无 OLED：直接发起 kKey 关机。
    s_gpioLevels[18] = 0;
    pollPower(260U);
    CHECK(ctx.data.powerStatus.read().shutdownStage != board::ShutdownStage::kIdle);
    CHECK(ctx.data.powerStatus.read().lastShutdown == board::ShutdownReason::kKey);
}
/** @brief 验证 OLED 在线但 UI 不消费长按事件时，持续按住 8 s 触发强制关机兜底。 */
void powerForceOffFallback()
{
    using namespace app;
    setupPower(board::BootMode::kNormal);
    s_gpioLevels[25] = 1;
    auto &ctx = appContext();
    ctx.data.uiEvents.setOledActive(true);
    s_gpioLevels[18] = 1;
    pollPower(4U);
    // 按住 2.5 s：投递长按给 UI，但不消费且不关机。
    s_gpioLevels[18] = 0;
    pollPower(260U);
    CHECK(ctx.data.uiEvents.longKeyCount() == 1U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kIdle);
    // 持续按住至 7.9 s：仍未达 8 s 兜底阈值。
    pollPower(530U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kIdle);
    // 达到 8.0 s（kForcePowerOffHoldMs）：不经 UI 直接触发 kKey 关机。
    pollPower(20U);
    CHECK(ctx.data.powerStatus.read().shutdownStage != board::ShutdownStage::kIdle);
    CHECK(ctx.data.powerStatus.read().lastShutdown == board::ShutdownReason::kKey);
}

/** @brief 验证 OLED 由在线转离线（清除 kOledActive）后，后续 2.5 s 长按直接走关机。 */
void powerOledOfflineDirectShutdown()
{
    using namespace app;
    setupPower(board::BootMode::kNormal);
    s_gpioLevels[25] = 1;
    auto &ctx = appContext();
    ctx.data.uiEvents.setOledActive(true);
    s_gpioLevels[18] = 1;
    pollPower(4U);
    // 在线时长按 2.5 s 仅通知 UI，松开不关机。
    s_gpioLevels[18] = 0;
    pollPower(260U);
    CHECK(ctx.data.uiEvents.longKeyCount() == 1U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kIdle);
    s_gpioLevels[18] = 1;
    pollPower(5U);
    // OLED 连续 I2C 失败进入离线态后清除 kOledActive，再次长按 2.5 s 直接关机。
    ctx.data.uiEvents.setOledActive(false);
    s_gpioLevels[18] = 0;
    pollPower(260U);
    CHECK(ctx.data.powerStatus.read().shutdownStage != board::ShutdownStage::kIdle);
    CHECK(ctx.data.powerStatus.read().lastShutdown == board::ShutdownReason::kKey);
}
/**
 * @brief 驱动电源模块完成正常关机前序阶段并触发 USB 关机 U3 掉电进入 restartPending
 * @param oledActive: 模拟 OLED 刷新任务是否在线
 */
void driveToUsbCutRestart(bool oledActive)
{
    using namespace app;
    CHECK(s_restarts == 0U);
    setupPower(board::BootMode::kNormal);
    s_gpioLevels[25] = 1; // U3 上电保持中
    auto &ctx = appContext();
    ctx.data.uiEvents.setOledActive(oledActive);
    CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
    pollPower(160U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kCutPower);
    // R2OL2-05：绝不清零 s_restarts，在 U3 翻转前显式断言未发生过误重启。
    CHECK(s_restarts == 0U);
    s_gpioLevels[27] = 1; // USB 在位
    s_gpioLevels[25] = 0; // U3 关断
    pollPower(1U);
    CHECK((ctx.data.powerStatus.read().shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady)) != 0U);
    CHECK(s_restarts == 0U);
}

/**
 * @brief 验证关机各阶段从确认刷出算起停留，电池断电等待终帧或走超时兜底
 * @param flush: 是否模拟电池终帧写出成功
 */
void powerBatteryDisplayShutdown(bool flush)
{
    using namespace app;
    namespace b = board;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    setupPower(b::BootMode::kNormal);
    auto &ctx = appContext();
    auto &events = ctx.data.uiEvents;
    events.setOledActive(true);
    CHECK(power::requestShutdown(b::ShutdownReason::kConsole));
    pollPower(1U);
    for (const auto stage :
         {b::ShutdownStage::kAnnounce, b::ShutdownStage::kSave, b::ShutdownStage::kQuiesce}) {
        CHECK(ctx.data.powerStatus.read().shutdownStage == stage);
        pollPower(2U);
        events.markShutdownStageFlushed(stage);
        pollPower(1U, 1U);
        pollPower(1U, b::kMinStageVisibleMs - 1U);
        CHECK(ctx.data.powerStatus.read().shutdownStage == stage);
        CHECK(s_gpio4Presses == 0U);
        pollPower(1U, 1U);
    }
    const auto snapshot = ctx.data.powerStatus.read();
    CHECK(snapshot.shutdownStage == b::ShutdownStage::kCutPower);
    CHECK((snapshot.shutdownDoneMask & b::shutdownDoneBit(b::ShutdownDoneBit::kBatteryOffReady)) !=
          0U);
    CHECK(s_gpio4Presses == 0U);
    events.markShutdownFinalFrameFlushed(true); // USB 凭据不能放行电池路径。
    if (flush) {
        pollPower(1U, 40U);
        CHECK(s_gpio4Presses == 0U);
        events.markShutdownFinalFrameFlushed(false);
        pollPower(1U, 1U);
        pollPower(1U, b::kShutdownFinalVisibleMs - 1U);
    } else {
        pollPower(1U, b::kShutdownDisplayGraceMs - 1U);
    }
    CHECK(s_gpio4Presses == 0U);
    pollPower(1U, 1U);
    CHECK(s_gpio4Presses == 1U && s_gpioLevels[4] == 1 && s_restarts == 0U);
    pollPower(1U, b::kShutdownCutInitHighMs - 1U);
    CHECK(s_gpioLevels[4] == 1);
    pollPower(1U, 1U);
    CHECK(s_gpioLevels[4] == 0);
}

/** @brief 验证 USB 关机 + OLED 在线：日志排空后等待终帧，刷出后再停留 300 ms。 */
void powerOledUsbFinalFlush()
{
    driveToUsbCutRestart(true);
    auto &ctx = app::appContext();
    pollPower(3U, 10U); // +30 ms 日志排空点
    CHECK(s_restarts == 0U);
    pollPower(2U, 10U); // 日志排空后 +20 ms：OLED 尚未刷完终帧，不得重启
    CHECK(s_restarts == 0U);
    ctx.data.uiEvents.markShutdownFinalFrameFlushed();
    pollPower(1U, 5U); // 观察终帧已写出
    pollPower(1U, app::board::kShutdownFinalVisibleMs - 1U);
    CHECK(s_restarts == 0U);
    pollPower(1U, 1U);
    CHECK(s_restarts == 1U);
    ctx.data.uiEvents.setOledActive(false);
}

/** @brief 验证 USB 关机 + OLED 终帧永不置位：日志排空后严格等满 30 ms 额外上限才兜底重启（ROL2-01/02）。 */
void powerOledUsbFinalTimeout()
{
    driveToUsbCutRestart(true);
    pollPower(3U, 10U); // +30 ms 日志排空点，启动 30 ms OLED 上限计时
    CHECK(s_restarts == 0U);
    pollPower(1U, 29U); // 日志排空后 +29 ms：尚未满 30 ms 上限，不得重启
    CHECK(s_restarts == 0U);
    pollPower(1U, 1U); // 日志排空后恰好 +30 ms：触发超时兜底重启
    CHECK(s_restarts == 1U);
    app::appContext().data.uiEvents.setOledActive(false);
}

/** @brief 验证无 OLED 板型 USB 关机：在 30 ms 日志排空点 0 额外等待立即重启（ROL2-01/02）。 */
void powerOledUsbAbsentNoWait()
{
    driveToUsbCutRestart(false);
    pollPower(2U, 10U); // +20 ms：日志最小等待尚未满足
    CHECK(s_restarts == 0U);
    pollPower(1U, 10U); // +30 ms：日志排空即刻重启（0 ms 额外等待）
    CHECK(s_restarts == 1U);
}

/** @brief 验证 OLED 在日志排空前已刷出终帧：日志放行后进入终帧停留。 */
void powerOledUsbAlreadyFlushed()
{
    driveToUsbCutRestart(true);
    auto &ctx = app::appContext();
    pollPower(1U, 10U); // +10 ms：OLED 已提前刷完终帧
    CHECK(s_restarts == 0U);
    ctx.data.uiEvents.markShutdownFinalFrameFlushed();
    pollPower(1U, 10U); // +20 ms：仍等日志排空
    CHECK(s_restarts == 0U);
    pollPower(1U, 10U); // +30 ms：日志放行并观察终帧
    pollPower(1U, app::board::kShutdownFinalVisibleMs - 1U);
    CHECK(s_restarts == 0U);
    pollPower(1U, 1U);
    CHECK(s_restarts == 1U);
    ctx.data.uiEvents.setOledActive(false);
}

/** @brief 验证首次日志排空锁存后即便日志环重新变忙，仍在 +30 ms 绝对截止重启而非拖到 +150 ms（R2OL2-01）。 */
void powerOledUsbLogRebusy()
{
    using namespace app;
    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    auto &debug = ctx.debug;
    std::array<fw::TraceEvent, DebugModule::kRingEvents> trace{};
    std::array<uint8_t, DebugModule::kLogRingBytes> log{};
    std::array<StackType_t, DebugModule::kTaskStackBytes / sizeof(StackType_t)> stack{};
    CHECK(fw::isOk(debug.bindStorage({trace, log, stack})));
    CHECK(fw::isOk(debug.init()));
    CHECK(fw::isOk(debug.attachLog()));

    s_gpioLevels[25] = 1;
    ctx.data.uiEvents.setOledActive(true);
    CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
    pollPower(160U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kCutPower);
    CHECK(s_restarts == 0U);

    s_gpioLevels[27] = 1;
    s_gpioLevels[25] = 0;
    pollPower(1U);
    CHECK(s_restarts == 0U);
    // 排空日志并推进至 +30 ms：首次排空锁存放行，开启 30 ms OLED 截止计时。
    while (ModuleTestAccess::pumpLogs()) {
    }
    CHECK(debug.logOutputIdle());
    pollPower(3U, 10U); // +30 ms
    CHECK(s_restarts == 0U);

    // 注入新日志令日志环重新变忙（!logOutputIdle()）：不得推翻已锁存的放行状态。
    FW_LOGI(fw::TraceCat::kSys, "late log during oled final flush");
    CHECK(!debug.logOutputIdle());
    pollPower(1U, 29U); // +59 ms（+29 ms）
    CHECK(s_restarts == 0U);
    pollPower(1U, 1U); // +60 ms（+30 ms）：绝对截止触发重启，而非拖到 +150 ms
    CHECK(s_restarts == 1U);
    ctx.data.uiEvents.setOledActive(false);
}

/**
 * @brief 验证非整除 11 ms 轮询与真实任务唤醒：终帧完成事件或精确 30 ms 截止直接唤醒重启路径（R2OL2-01）
 * @param wakeByFlush: true 验证终帧完成即时唤醒；false 验证 30 ms 绝对截止唤醒（无需 +66 ms 轮询）
 */
void powerOledUsbNonalignedWake(bool wakeByFlush)
{
    using namespace app;
    driveToUsbCutRestart(true);
    auto &ctx = appContext();
    // 按 11 ms 非整除序列推进至 +22 ms（尚未到达 30 ms 日志最小窗口）。
    pollPower(2U, 11U);
    CHECK(s_restarts == 0U);

    struct WakeTaskArg {
        AppContext *app = nullptr;
        std::binary_semaphore entered{0};
        std::binary_semaphore finished{0};
        uint32_t elapsedWallMs = 0U;
    };
    static StaticTask_t s_wakeTcb{};
    static std::array<StackType_t, 64U> s_wakeStack{};
    WakeTaskArg arg{.app = &ctx};
    const TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        [](void *raw) {
            auto &a = *static_cast<WakeTaskArg *>(raw);
            const auto t0 = std::chrono::steady_clock::now();
            s_wall_clock_start = &t0;
            a.entered.release();
            s_clock += 11U; // +33 ms 非整除轮询点：锁存放行并在真实任务中进入 waitOledFinalFrame
            static_cast<void>(a.app->power.poll(s_clock));
            s_wall_clock_start = nullptr;
            const auto t1 = std::chrono::steady_clock::now();
            a.elapsedWallMs = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
            a.finished.release();
        },
        "pwr_wake", static_cast<uint32_t>(s_wakeStack.size()), &arg, 10U, s_wakeStack.data(),
        &s_wakeTcb, 1);
    CHECK(handle != nullptr);
    arg.entered.acquire();
    if (wakeByFlush) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(s_restarts == 0U);
        ctx.data.uiEvents.markShutdownFinalFrameFlushed();
    }
    arg.finished.acquire();
    if (wakeByFlush) {
        CHECK(s_restarts == 0U);
        pollPower(31U);
        CHECK(s_restarts == 1U);
        // 由终帧完成钩子直接唤醒，明显短于 30 ms 截止上限。
        CHECK(arg.elapsedWallMs < 25U);
    } else {
        CHECK(s_restarts == 1U);
        // 由 30 ms 截止 tick 直接唤醒并重启，无需等到下一个 11 ms 轮询点（+66 ms）。
        CHECK(arg.elapsedWallMs >= 25U && arg.elapsedWallMs <= 80U);
    }
    ctx.data.uiEvents.setOledActive(false);
}

/**
 * @brief 模拟从读取截止时钟到进入 RTOS 相对等待之间被抢占
 * @param gapMs: 首次等待前流逝的时间，覆盖未到期和已过期两种恢复场景
 */
void powerOledUsbPreemptedWait(uint32_t gapMs)
{
    driveToUsbCutRestart(true);
    pollPower(2U, 11U);
    s_clock += 11U;
    const uint32_t gateMs = s_clock;
    PowerWaitClock clock{.firstGapMs = gapMs};
    s_use_host_task = true;
    s_power_wait_clock = &clock;
    static_cast<void>(app::appContext().power.poll(gateMs));
    s_power_wait_clock = nullptr;
    s_use_host_task = false;

    const uint32_t elapsed = s_clock - gateMs;
    CHECK(clock.waits > 0U);
    CHECK(elapsed >= std::max(30U, gapMs));
    CHECK(elapsed <= std::max(30U, gapMs) + 1U);
    CHECK(s_restarts == 1U);
}

/** @brief 验证保存超时、射频静默超时与安全输出电平失败不伪造完成位，独立暴露降级掩码并沿用基线兜底重启（R2OL2-02）。 */
void powerShutdownDegradedMasks()
{
    using namespace app;
    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    s_gpioLevels[25] = 1;
    // 1. 注入保存失败（待写 NVS 不清）、BT 连接不响应暂停、外设安全输出失败。
    ctx.storage.markWhitelistDirty(std::array{kAlarm}, s_clock);
    s_store_result = ESP_FAIL;
    open(1U, kPhone);
    s_safeOutputsStatus = fw::Status::kIoError;

    CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
    // 推进 3.15 s 令 kSave 超时进入 kQuiesce（kShutdownSaveTimeoutMs = 3000 ms）。
    pollPower(315U);
    const PowerSnapshot qSnap = ctx.data.powerStatus.read();
    CHECK(qSnap.shutdownStage == board::ShutdownStage::kQuiesce);
    CHECK((qSnap.shutdownTotalCount > 0U) && (qSnap.shutdownDoneCount < qSnap.shutdownTotalCount));
    CHECK((qSnap.shutdownDoneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed)) ==
          0U);
    CHECK((qSnap.shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe)) == 0U);
    CHECK((qSnap.shutdownDegradedMask &
           board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout)) != 0U);
    CHECK((qSnap.shutdownDegradedMask &
           board::shutdownDegradedBit(board::ShutdownDegradedBit::kPeripheralsFailed)) != 0U);

    // 推进 200 ms 令 BT 静默超时进入 kCutPower：射频完成位必须仍为 0，降级位含 kRadioTimeout。
    pollPower(20U);
    const PowerSnapshot cSnap = ctx.data.powerStatus.read();
    CHECK(cSnap.shutdownStage == board::ShutdownStage::kCutPower);
    CHECK((cSnap.shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced)) == 0U);
    CHECK((cSnap.shutdownDegradedMask &
           board::shutdownDegradedBit(board::ShutdownDegradedBit::kRadioTimeout)) != 0U);
    CHECK(s_restarts == 0U);

    // USB 在位且 U3 关断：完成位仍不含失败项、降级位保留，并按基线策略执行 USB 兜底重启。
    s_gpioLevels[27] = 1;
    s_gpioLevels[25] = 0;
    pollPower(1U);
    const PowerSnapshot uSnap = ctx.data.powerStatus.read();
    CHECK((uSnap.shutdownDoneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kUsbOffReady)) !=
          0U);
    CHECK((uSnap.shutdownDoneMask & board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed)) ==
          0U);
    CHECK((uSnap.shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe)) == 0U);
    CHECK((uSnap.shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kRadioQuiesced)) == 0U);
    CHECK((uSnap.shutdownDegradedMask &
           board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout)) != 0U);
    CHECK((uSnap.shutdownDegradedMask &
           board::shutdownDegradedBit(board::ShutdownDegradedBit::kPeripheralsFailed)) != 0U);
    CHECK((uSnap.shutdownDegradedMask &
           board::shutdownDegradedBit(board::ShutdownDegradedBit::kRadioTimeout)) != 0U);
    pollPower(4U);
    CHECK(s_restarts == 1U);
    s_safeOutputsStatus = fw::Status::kOk;
}

struct CapturedLogBuf {
    std::array<char, 4096> data{};
    size_t len = 0U;
};

CapturedLogBuf *s_capturedLog = nullptr;

/**
 * @brief 测试日志输出钩子，将 DebugModule 吐出的文本追加到静态缓冲。
 * @param text: 日志片段指针
 * @param len: 日志片段长度
 */
void captureLogSink(const char *text, size_t len) noexcept
{
    if ((s_capturedLog == nullptr) || (text == nullptr) || (len == 0U)) {
        return;
    }
    const size_t avail = s_capturedLog->data.size() - 1U - s_capturedLog->len;
    const size_t copyLen = (len < avail) ? len : avail;
    if (copyLen > 0U) {
        std::memcpy(s_capturedLog->data.data() + s_capturedLog->len, text, copyLen);
        s_capturedLog->len += copyLen;
        s_capturedLog->data[s_capturedLog->len] = '\0';
    }
}

/** @brief 验证新增收尾参与者阻塞断电直到超时且超时日志列出名称，并校验全部 ShutdownReason trace 文案（PW1 A/C）。 */
void powerParticipantBlocksAndTimeoutNames()
{
    using namespace app;
    power::clearExtraShutdownParticipants();
    // 注册边界校验：非法参数与重名必须被拒绝。
    struct ParticipantState {
        uint32_t beginCount = 0U;
        bool done = false;
    };
    ParticipantState saveState{};
    ParticipantState quiesceState{};
    const auto onBegin = [](board::ShutdownStage /*stage*/, void *arg) noexcept {
        // 由本测试显式绑定为 ParticipantState*。
        ++static_cast<ParticipantState *>(arg)->beginCount;
    };
    const auto checkDone = [](void *arg) noexcept {
        // 由本测试显式绑定为 ParticipantState*。
        return static_cast<const ParticipantState *>(arg)->done;
    };
    using fw::Status;
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = nullptr, .isDone = checkDone, .stage = board::ShutdownStage::kSave}) ==
          Status::kInvalidArg);
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = "", .isDone = checkDone, .stage = board::ShutdownStage::kSave}) ==
          Status::kInvalidArg);
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = "bad_null_fn", .isDone = nullptr, .stage = board::ShutdownStage::kSave}) ==
          Status::kInvalidArg);
    // 外部登记只接受 kSave / kQuiesce（评审 1）：其余阶段一律 kInvalidArg。
    for (const auto badStage : {board::ShutdownStage::kIdle, board::ShutdownStage::kAnnounce,
                                board::ShutdownStage::kCutPower}) {
        CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
                  .name = "bad_stage", .isDone = checkDone, .stage = badStage}) ==
              Status::kInvalidArg);
    }
    // 名字过长（33 字符）与内置同名（评审 6）必须拒绝。
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = "name_that_is_exactly_33_chars_xxx",
              .isDone = checkDone,
              .stage = board::ShutdownStage::kSave}) == Status::kInvalidArg);
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = "safe_gpio", .isDone = checkDone, .stage = board::ShutdownStage::kQuiesce}) ==
          Status::kInvalidArg);
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = "oled_final_usb",
              .isDone = checkDone,
              .stage = board::ShutdownStage::kQuiesce}) == Status::kInvalidArg);
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = "test_extra_save",
              .begin = onBegin,
              .isDone = checkDone,
              .userCtx = &saveState,
              .stage = board::ShutdownStage::kSave,
          }) == Status::kOk);
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = "test_extra_save",
              .isDone = checkDone,
              .stage = board::ShutdownStage::kSave,
          }) == Status::kBusy);
    CHECK(power::registerShutdownParticipant(power::ShutdownParticipant{
              .name = "test_extra_quiesce",
              .begin = onBegin,
              .isDone = checkDone,
              .userCtx = &quiesceState,
              .stage = board::ShutdownStage::kQuiesce,
          }) == Status::kOk);

    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    std::array<fw::TraceEvent, DebugModule::kRingEvents> trace{};
    std::array<uint8_t, DebugModule::kLogRingBytes> log{};
    std::array<StackType_t, DebugModule::kTaskStackBytes / sizeof(StackType_t)> stack{};
    CHECK(fw::isOk(ctx.debug.bindStorage({trace, log, stack})));
    CHECK(fw::isOk(ctx.debug.init()));
    CHECK(fw::isOk(ctx.debug.attachLog()));

    CapturedLogBuf captured{};
    s_capturedLog = &captured;
    cli::setOutSink(&captureLogSink, false);
    s_gpioLevels[25] = 1;

    CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
    // 2.95 s 未达 3 s 保存上限：未完成的 test_extra_save 必须阻塞进入 kQuiesce 与断电。
    pollPower(295U);
    const PowerSnapshot midSave = ctx.data.powerStatus.read();
    CHECK(midSave.shutdownStage == board::ShutdownStage::kSave);
    CHECK(saveState.beginCount == 1U);
    CHECK((midSave.shutdownTotalCount > 0U) &&
          (midSave.shutdownDoneCount < midSave.shutdownTotalCount));
    CHECK((s_gpio4Presses == 0U) && (s_restarts == 0U));

    // 跨过 3 s 保存上限（总 3.05 s，刚进入 kQuiesce 约 40 ms）：超时降级并列出未完成参与者名。
    pollPower(10U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kQuiesce);
    CHECK((ctx.data.powerStatus.read().shutdownDegradedMask &
           board::shutdownDegradedBit(board::ShutdownDegradedBit::kSaveTimeout)) != 0U);
    CHECK(quiesceState.beginCount == 1U);
    CHECK(ModuleTestAccess::pumpLogs());
    const std::string_view saveLogs{captured.data.data(), captured.len};
    CHECK(saveLogs.find("test_extra_save") != std::string_view::npos);

    // 约 90 ms 仍未达 150 ms 静默上限：test_extra_quiesce 持续阻塞 kCutPower。
    pollPower(5U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kQuiesce);
    CHECK(s_gpio4Presses == 0U);

    // 跨过 150 ms 静默上限：超时降级进入 kCutPower，日志列出 test_extra_quiesce。
    pollPower(10U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kCutPower);
    CHECK(ModuleTestAccess::pumpLogs());
    const std::string_view quiesceLogs{captured.data.data(), captured.len};
    CHECK(quiesceLogs.find("test_extra_quiesce") != std::string_view::npos);

    cli::setOutSink(nullptr, false);
    s_capturedLog = nullptr;
    power::clearExtraShutdownParticipants();

    // 校验 trace 文本覆盖全部 ShutdownReason（含预留 kWeb）。
    const std::array<std::pair<board::ShutdownReason, const char *>, 5U> reasons{{
        {board::ShutdownReason::kKey, "reason=key"},
        {board::ShutdownReason::kConsole, "reason=console"},
        {board::ShutdownReason::kLowBattery, "reason=low_battery"},
        {board::ShutdownReason::kPhone, "reason=phone"},
        {board::ShutdownReason::kWeb, "reason=web"},
    }};
    std::array<char, 128> line{};
    for (const auto &[reason, expected] : reasons) {
        const fw::TraceEvent stageEv{
            .cat = fw::TraceCat::kSys,
            .code = static_cast<uint8_t>(PowerTrace::kShutdownStage),
            .a = static_cast<uint32_t>(board::ShutdownStage::kSave),
            .b = static_cast<uint32_t>(reason),
        };
        debug::formatEvent(stageEv, line.data(), line.size());
        const std::string_view text{line.data()};
        CHECK(text.find("stage=save") != std::string_view::npos);
        CHECK(text.find(expected) != std::string_view::npos);

        const fw::TraceEvent abortEv{
            .cat = fw::TraceCat::kSys,
            .code = static_cast<uint8_t>(PowerTrace::kShutdownAbort),
            .a = static_cast<uint32_t>(reason),
            .b = 30000U,
        };
        debug::formatEvent(abortEv, line.data(), line.size());
        CHECK(std::string_view{line.data()}.find(expected) != std::string_view::npos);
    }
}

/** @brief 验证电池终帧停留期间插入 USB 不复用电池终帧状态，仍完整等待 USB RESTARTING 终帧与 300 ms 停留（PW1 D）。 */
void powerOledBatteryToUsbFinalFrame()
{
    using namespace app;
    namespace b = board;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    s_gpioLevels[27] = 0;
    setupPower(b::BootMode::kNormal);
    auto &ctx = appContext();
    auto &events = ctx.data.uiEvents;
    events.setOledActive(true);

    CHECK(power::requestShutdown(b::ShutdownReason::kConsole));
    pollPower(1U);
    for (const auto stage :
         {b::ShutdownStage::kAnnounce, b::ShutdownStage::kSave, b::ShutdownStage::kQuiesce}) {
        CHECK(ctx.data.powerStatus.read().shutdownStage == stage);
        pollPower(2U);
        events.markShutdownStageFlushed(stage);
        pollPower(1U, 1U);
        pollPower(1U, b::kMinStageVisibleMs);
    }
    CHECK(ctx.data.powerStatus.read().shutdownStage == b::ShutdownStage::kCutPower);
    CHECK(s_gpio4Presses == 0U && s_restarts == 0U);

    // 电池路径终帧与 kCutPower 阶段已刷出，停留 250 ms（满足阶段可见但仍处 300 ms 电池终帧停留期）。
    events.markShutdownStageFlushed(b::ShutdownStage::kCutPower);
    events.markShutdownFinalFrameFlushed(false);
    pollPower(1U, 1U);
    pollPower(1U, b::kMinStageVisibleMs);
    CHECK(s_gpio4Presses == 0U && s_restarts == 0U);

    // 此时插入 USB 且 U3 掉电：进入 USB RESTARTING 路径，不得沿用电池终帧标志跳过 RESTARTING 帧。
    s_gpioLevels[27] = 1;
    s_gpioLevels[25] = 0;
    pollPower(1U, 10U);
    CHECK((ctx.data.powerStatus.read().shutdownDoneMask &
           b::shutdownDoneBit(b::ShutdownDoneBit::kUsbOffReady)) != 0U);
    CHECK(s_restarts == 0U);

    // 等待 30 ms 日志排空后再推 20 ms：USB 终帧尚未刷出，即使已超过原电池终帧停留期限也不得重启。
    pollPower(3U, 10U);
    CHECK(s_restarts == 0U);
    pollPower(2U, 10U);
    CHECK(s_restarts == 0U);

    // 刷出 USB RESTARTING 终帧后，必须完整停留 kShutdownFinalVisibleMs（300 ms）。
    events.markShutdownFinalFrameFlushed(true);
    pollPower(1U, 5U);
    pollPower(1U, b::kShutdownFinalVisibleMs - 1U);
    CHECK(s_restarts == 0U);
    pollPower(1U, 1U);
    CHECK(s_restarts == 1U);
}

/** @brief 验证 HIL 模拟电量接口可触发低电关机且清除后恢复真实 ADC，不改产品默认阈值（PW1 HIL）。 */
void powerSimBatteryLowShutdown()
{
    using namespace app;
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
    s_batteryMv = 4150U;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    s_gpioLevels[27] = 0;
    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    CHECK(ctx.data.powerStatus.read().batteryPct >= 90U);

    // 注入 5% 模拟电量，未满 120 s 前不得关机。
    power::setSimulatedBatteryPct(5U);
    pollPower(1U, 100U);
    CHECK(ctx.data.powerStatus.read().batteryPct == 5U);
    pollPower(1198U, 100U);
    CHECK(power::s_power_requests.canScheduleRestart());

    // 清除模拟后低电计时归零并恢复真实 ADC 高电量。
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
    pollPower(120U, 10U);
    CHECK(ctx.data.powerStatus.read().batteryPct >= 90U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kIdle);

    // 再次注入 5% 并持续满 120 s：触发 kLowBattery 关机。
    power::setSimulatedBatteryPct(5U);
    pollPower(1200U, 100U);
    CHECK(power::s_power_requests.canScheduleRestart());
    pollPower(2U, 100U);
    const PowerSnapshot snap = ctx.data.powerStatus.read();
    CHECK(snap.shutdownStage == board::ShutdownStage::kSave);
    CHECK(snap.lastShutdown == board::ShutdownReason::kLowBattery);
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
}

/** @brief 驱动 OLED 在线的真实电源模块走完 kAnnounce/kSave/kQuiesce，停在 kCutPower 入口。 */
void driveOledShutdownToCutPower()
{
    using namespace app;
    namespace b = board;
    auto &ctx = appContext();
    auto &events = ctx.data.uiEvents;
    events.setOledActive(true);
    CHECK(power::requestShutdown(b::ShutdownReason::kConsole));
    pollPower(1U);
    for (const auto stage :
         {b::ShutdownStage::kAnnounce, b::ShutdownStage::kSave, b::ShutdownStage::kQuiesce}) {
        CHECK(ctx.data.powerStatus.read().shutdownStage == stage);
        pollPower(2U);
        events.markShutdownStageFlushed(stage);
        pollPower(1U, 1U);
        pollPower(1U, b::kMinStageVisibleMs);
    }
    CHECK(ctx.data.powerStatus.read().shutdownStage == b::ShutdownStage::kCutPower);
}

/**
 * @brief 验证 USB 终帧停留从确认被观察到之后才开始计时（评审 4，任务卡 D）。
 * @note 模拟 kLink 取时后被抢占：poll 入参比确认事件早 100 ms，停留必须仍满 300 ms。
 */
void powerOledUsbFinalFrameStaleClock()
{
    using namespace app;
    namespace b = board;
    s_chargerReg = b::kSgmReg08PgBit;
    s_gpioLevels[25] = 1;
    s_gpioLevels[27] = 1;
    setupPower(b::BootMode::kNormal);
    auto &ctx = appContext();
    auto &events = ctx.data.uiEvents;
    driveOledShutdownToCutPower();
    // USB 在位：GPIO4 动作由 U3 掉电后触发 USB 重启路径；先发 kCutPower 阶段确认。
    events.markShutdownStageFlushed(b::ShutdownStage::kCutPower);
    pollPower(1U, 1U);
    pollPower(1U, b::kMinStageVisibleMs);
    // GPIO4 动作先于 U3 关断：确认已拉高后才模拟 U3 掉电。
    CHECK(s_gpio4Presses == 1U && s_gpioLevels[4] == 1);
    s_gpioLevels[25] = 0;
    pollPower(1U, 10U);
    CHECK((ctx.data.powerStatus.read().shutdownDoneMask &
           b::shutdownDoneBit(b::ShutdownDoneBit::kUsbOffReady)) != 0U);
    // 等日志排空后进入等待 USB 终帧确认的有界窗口（30 ms），确认前不重启。
    pollPower(3U, 10U);
    CHECK(s_restarts == 0U);

    // 先推进时钟 100 ms 模拟「取时后被抢占」，再发布确认，最后用旧时间调用 poll。
    s_clock += 100U;
    const uint32_t confirmMs = s_clock;
    events.markShutdownFinalFrameFlushed(true);
    static_cast<void>(appContext().power.poll(confirmMs - 100U));
    CHECK(s_restarts == 0U);
    // 以确认时刻为基准：+250 / +299 ms 不得重启，+300 ms 才允许。
    s_clock = confirmMs + 249U;
    static_cast<void>(appContext().power.poll(s_clock));
    pollPower(1U, 1U);
    CHECK(s_restarts == 0U);
    pollPower(1U, 49U);
    CHECK(s_restarts == 0U);
    pollPower(1U, 1U);
    CHECK(s_restarts == 1U);
}

/** @brief 验证电池终帧停留同样从确认被观察到之后才开始计时（评审 4）。 */
void powerOledBatteryFinalFrameStaleClock()
{
    using namespace app;
    namespace b = board;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    s_gpioLevels[27] = 0;
    setupPower(b::BootMode::kNormal);
    auto &ctx = appContext();
    auto &events = ctx.data.uiEvents;
    driveOledShutdownToCutPower();
    events.markShutdownStageFlushed(b::ShutdownStage::kCutPower);
    pollPower(1U, 1U);
    s_clock += 100U;
    const uint32_t confirmMs = s_clock;
    events.markShutdownFinalFrameFlushed(false);
    static_cast<void>(appContext().power.poll(confirmMs - 100U));
    // 确认后 299 ms 内不得驱动 GPIO4（电池路径断电动作由终帧握手放行）。
    s_clock = confirmMs + 298U;
    static_cast<void>(appContext().power.poll(s_clock));
    pollPower(1U, 1U);
    CHECK(s_gpio4Presses == 0U && s_gpioLevels[4] == 0);
    pollPower(1U, 1U);
    CHECK(s_gpio4Presses == 1U);
}

/** @brief 验证满 4 槽外部参与者（32 字符名）超时日志每个未完成名字完整可见；safe_gpio 失败不波及额外项（评审 2/6）。 */
void powerParticipantFourSlotNames()
{
    using namespace app;
    power::clearExtraShutdownParticipants();
    struct SlotState {
        bool done = false;
    };
    SlotState saveDone{true};
    SlotState savePending{false};
    SlotState quiesceDone{true};
    SlotState quiescePending{false};
    const auto checkDone = [](void *arg) noexcept {
        // 由本测试显式绑定为 SlotState*。
        return static_cast<const SlotState *>(arg)->done;
    };
    constexpr const char *kSaveDoneName = "slot1_save_done_xxxxxxxxxxxxxxxx";
    constexpr const char *kSavePendingName = "slot2_save_pending_xxxxxxxxxxxxx";
    constexpr const char *kQuiesceDoneName = "slot3_quiesce_done_xxxxxxxxxxxxx";
    constexpr const char *kQuiescePendingName = "slot4_quiesce_pending_xxxxxxxxxx";
    const auto reg = [&](const char *name, SlotState *state, board::ShutdownStage stage,
                         power::ShutdownMilestone milestone) {
        return power::registerShutdownParticipant(
            power::ShutdownParticipant{.name = name,
                                       .isDone = checkDone,
                                       .userCtx = state,
                                       .stage = stage,
                                       .milestone = milestone});
    };
    CHECK(reg(kSaveDoneName, &saveDone, board::ShutdownStage::kSave,
              power::ShutdownMilestone::kSaveFlush) == fw::Status::kOk);
    CHECK(reg(kSavePendingName, &savePending, board::ShutdownStage::kSave,
              power::ShutdownMilestone::kSaveFlush) == fw::Status::kOk);
    CHECK(reg(kQuiesceDoneName, &quiesceDone, board::ShutdownStage::kQuiesce,
              power::ShutdownMilestone::kSafeGpio) == fw::Status::kOk);
    CHECK(reg(kQuiescePendingName, &quiescePending, board::ShutdownStage::kQuiesce,
              power::ShutdownMilestone::kSafeGpio) == fw::Status::kOk);
    CHECK(reg("slot5_overflow", &saveDone, board::ShutdownStage::kSave,
              power::ShutdownMilestone::kNone) == fw::Status::kNoSpace);

    s_safeOutputsStatus =
        fw::Status::kIoError; // 内置 safe_gpio 失败：不得连带短路额外 kSafeGpio 项。
    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    std::array<fw::TraceEvent, DebugModule::kRingEvents> trace{};
    std::array<uint8_t, DebugModule::kLogRingBytes> log{};
    std::array<StackType_t, DebugModule::kTaskStackBytes / sizeof(StackType_t)> stack{};
    CHECK(fw::isOk(ctx.debug.bindStorage({trace, log, stack})));
    CHECK(fw::isOk(ctx.debug.init()));
    CHECK(fw::isOk(ctx.debug.attachLog()));
    CapturedLogBuf captured{};
    s_capturedLog = &captured;
    cli::setOutSink(&captureLogSink, false);
    s_gpioLevels[25] = 1;

    CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
    pollPower(305U); // 超过 3 s 保存上限，刚进入 kQuiesce。
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kQuiesce);
    CHECK((ctx.data.powerStatus.read().shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kSaveFlushed)) == 0U);
    // 额外 kSafeGpio 项与内置失败无关：未完成时必须按 150 ms 通用超时等待，不能零等待放行。
    pollPower(5U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kQuiesce);
    pollPower(12U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kCutPower);
    const PowerSnapshot snap = ctx.data.powerStatus.read();
    CHECK((snap.shutdownDegradedMask &
           board::shutdownDegradedBit(board::ShutdownDegradedBit::kPeripheralsFailed)) != 0U);
    CHECK((snap.shutdownDoneMask &
           board::shutdownDoneBit(board::ShutdownDoneBit::kPeripheralsSafe)) == 0U);

    CHECK(ModuleTestAccess::pumpLogs());
    const std::string_view logs{captured.data.data(), captured.len};
    CHECK(logs.find(kSavePendingName) != std::string_view::npos);
    CHECK(logs.find(kQuiescePendingName) != std::string_view::npos);
    CHECK(logs.find("safe_gpio") != std::string_view::npos);
    CHECK(logs.find(kSaveDoneName) == std::string_view::npos);
    CHECK(logs.find(kQuiesceDoneName) == std::string_view::npos);

    cli::setOutSink(nullptr, false);
    s_capturedLog = nullptr;
    power::clearExtraShutdownParticipants();
    s_safeOutputsStatus = fw::Status::kOk;
}

/** @brief 验证模拟电量低电计时边界：119999 ms 不触发、120000 ms 触发（评审 3）。 */
void powerSimBatteryBoundary()
{
    using namespace app;
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
    s_batteryMv = 4150U;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    s_gpioLevels[27] = 0;
    setupPower(board::BootMode::kNormal);
    power::setSimulatedBatteryPct(5U);
    pollPower(1U, 100U); // 计时起点
    pollPower(1U, 119999U);
    CHECK(power::s_power_requests.canScheduleRestart());
    pollPower(1U, 1U);
    CHECK(!power::s_power_requests.canScheduleRestart());
    CHECK(appContext().data.powerStatus.read().lastShutdown == board::ShutdownReason::kLowBattery);
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
}

/** @brief 验证真实 ADC 成功读到 0 mV 时低电计时不被重置（评审 3）。 */
void powerLowBatteryRealZeroMv()
{
    using namespace app;
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
    s_batteryMv = 3390U;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    s_gpioLevels[27] = 0;
    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    CHECK(ctx.data.powerStatus.read().batteryPct == 5U);
    pollPower(600U, 100U);
    s_batteryMv = 0U; // 读取成功但电压为 0：不是模拟会话，计时必须继续累计。
    pollPower(550U, 100U);
    CHECK(power::s_power_requests.canScheduleRestart());
    pollPower(120U, 100U);
    CHECK(!power::s_power_requests.canScheduleRestart());
    CHECK(ctx.data.powerStatus.read().lastShutdown == board::ShutdownReason::kLowBattery);
}

/** @brief 验证两次模拟注入之间的「清除后立即重注入」不会合并低电计时（评审 8）。 */
void powerSimBatteryReinject()
{
    using namespace app;
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
    s_batteryMv = 4150U;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    s_gpioLevels[27] = 0;
    setupPower(board::BootMode::kNormal);
    power::setSimulatedBatteryPct(5U);
    pollPower(1100U, 100U); // 110 s
    CHECK(power::s_power_requests.canScheduleRestart());
    // 两次调用发生在同一个 poll 间隔内，消费者看不到中间的「未知」值，但会话代号必须变化。
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
    power::setSimulatedBatteryPct(5U);
    pollPower(1100U, 100U); // 若被合并，累计 220 s 早已关机；正确实现只从重注入后算起。
    CHECK(power::s_power_requests.canScheduleRestart());
    pollPower(110U, 100U);
    CHECK(!power::s_power_requests.canScheduleRestart());
    power::setSimulatedBatteryPct(board::kBatteryPctUnknown);
}

struct ExecutorRound {
    std::binary_semaphore ready{0};
    std::binary_semaphore run{0};
    std::array<uint8_t, 8> verbs{};
    size_t count = 0U;
};

constinit ExecutorRound s_linkRound{};
constinit ExecutorRound s_ioRound{};

/** @brief 暂停执行器在本轮出队前。 @param ctx: 测试轮次控制。 @param maxMs: 执行器等待预算。 */
void waitExecutorRound(void *ctx, uint32_t maxMs) noexcept
{
    static_cast<void>(maxMs);
    // 两个测试执行器各绑定其对应的控制对象。
    auto &round = *static_cast<ExecutorRound *>(ctx);
    round.ready.release();
    round.run.acquire();
}

/** @brief 放行一个完整轮次并等到下轮边界。 @param round: 测试轮次控制。 */
void runExecutorRound(ExecutorRound &round)
{
    round.run.release();
    round.ready.acquire();
}

/** @brief 用真实队列验证关机保存栅栏的 FIFO、等待以及超时取消。 @param timeout: 注入 kIo 超时。 */
void powerNoReopen(bool timeout)
{
    using namespace app;
    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    constexpr auto kLink = id(ExecKind::kLink);
    constexpr auto kIo = id(ExecKind::kIo);
    constexpr auto kSetOut = static_cast<uint8_t>(GnssSubVerb::kSetOut);
    constexpr auto kSetGga = static_cast<uint8_t>(GnssSubVerb::kSetGgaGate);
    constexpr auto kFlushGga = static_cast<uint8_t>(GnssSubVerb::kFlushGgaGate);
    constexpr auto kTrust = verb(LinkVerb::kWhitelistAdd);
    static constexpr fw::ModuleOps kLinkOps{
        nullptr, nullptr, nullptr,
        [](void *, const fw::Command &cmd, fw::CommandResult &result) noexcept {
            s_linkRound.verbs.at(s_linkRound.count++) = cmd.verb;
            if (cmd.target == id(ModuleKind::kStorage)) {
                appContext().storage.onCommand(cmd, result);
            } else {
                appContext().bt_link.onCommand(cmd, result);
            }
        }};
    static constexpr fw::ModuleOps kIoOps{
        nullptr, nullptr, nullptr,
        [](void *, const fw::Command &cmd, fw::CommandResult &) noexcept {
            s_ioRound.verbs.at(s_ioRound.count++) = cmd.verb;
        }};
    static constinit std::array<fw::Module, 3> modules{{
        {"store", id(ModuleKind::kStorage), kLink, 0U, nullptr, &kLinkOps},
        {"link", id(ModuleKind::kBtLink), kLink, 0U, nullptr, &kLinkOps},
        {"gnss", id(ModuleKind::kGnss), kIo, 0U, nullptr, &kIoOps},
    }};
    static constinit rtos::Executor link{};
    static constinit rtos::Executor io{};
    static constinit rtos::ExecutorStorage<4096, 8> linkStorage{};
    static constinit rtos::ExecutorStorage<4096, 8> ioStorage{};
    const rtos::ExecutorConfig linkCfg{.name = "save_link",
                                       .id = kLink,
                                       .yieldTicks = 0U,
                                       .wait = &waitExecutorRound,
                                       .waitCtx = &s_linkRound};
    const rtos::ExecutorConfig ioCfg{.name = "save_io",
                                     .id = kIo,
                                     .yieldTicks = 0U,
                                     .wait = &waitExecutorRound,
                                     .waitCtx = &s_ioRound};
    CHECK(fw::isOk(ctx.storage.init()));
    CHECK(fw::isOk(link.init(linkCfg, modules, viewOf(linkStorage))));
    CHECK(fw::isOk(io.init(ioCfg, modules, viewOf(ioStorage))));
    CHECK(fw::isOk(link.start()));
    CHECK(fw::isOk(io.start()));
    s_linkRound.ready.acquire();
    s_ioRound.ready.acquire();
    ctx.linkExecutor = &link;
    ctx.ioExecutor = &io;
    s_gpioLevels[25] = 1;

    // 控制台已准入的信任命令在等待回执期间关闭设置门。命令必须仍能落盘。
    fw::Command trust{.target = id(ModuleKind::kBtLink), .verb = kTrust, .argLen = core::kAddrLen};
    std::ranges::copy(kAlarm.bytes, trust.args.begin());
    fw::CommandResult result{};
    s_consoleDelay = +[]() noexcept {
        CHECK(power::s_power_requests.settingsSubmitted());
        CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
        pollPower(3U); // 发出两个队尾凭据，队列消费由测试控制。
        CHECK(appContext().data.powerStatus.read().shutdownStage == board::ShutdownStage::kSave);
        runExecutorRound(s_linkRound);
    };

    rtos::Ticket gga{};
    CHECK(fw::isOk(io.post(fw::Command{.target = id(ModuleKind::kGnss), .verb = kSetGga},
                           rtos::Caller::kWeb, gga)));
    CHECK(submitRawTo(link, ModuleKind::kBtLink, nullptr, trust, result));
    CHECK(fw::isOk(result.status) && s_saved_count == 1U && s_saved[0] == kAlarm.bytes);
    CHECK(!submitRawTo(link, ModuleKind::kBtLink, nullptr, trust, result));
    CHECK(!submitRawTo(io, ModuleKind::kGnss, nullptr,
                       fw::Command{.target = id(ModuleKind::kGnss), .verb = kSetGga}, result));
    CHECK(!submitRawTo(io, ModuleKind::kGnss, nullptr,
                       fw::Command{.target = id(ModuleKind::kGnss), .verb = kSetOut}, result));
    CHECK(s_linkRound.count == 2U && s_linkRound.verbs[0] == kTrust &&
          s_linkRound.verbs[1] == StorageModule::kSaveFence);
    CHECK(s_gpio4Presses == 0U && s_restarts == 0U);

    if (timeout) {
        pollPower(300U);
        CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kQuiesce);
    }
    runExecutorRound(s_ioRound);
    CHECK(io.poll(gga, result) == rtos::PollState::kDone);
    CHECK(s_ioRound.count == (timeout ? 1U : 2U) && s_ioRound.verbs[0] == kSetGga);
    CHECK(timeout || s_ioRound.verbs[1] == kFlushGga);
    s_gpioLevels[25] = 0;
    pollPower(35U);
    CHECK(s_gpio4Presses == 0U && s_restarts == 1U);
}

/** @brief 交给 UART 前观察最后一条日志已离开环。 @param text: 待输出文本。 @param len: 文本长度。 */
void observeLastPowerLog(const char *text, size_t len) noexcept
{
    if (std::string_view{text, len}.find("restarting") == std::string_view::npos) {
        return;
    }
    auto &ctx = app::appContext();
    CHECK(ctx.debug.logRingUsed() == 0U && !ctx.debug.logOutputIdle());
    pollPower(3U);
    CHECK(s_restarts == 0U);
}

/** @brief 模拟日志任务在 pop 与提交 UART 之间被抢占。 */
void powerLogDrain()
{
    using namespace app;
    setupPower(board::BootMode::kChargeStandby);
    auto &debug = appContext().debug;
    std::array<fw::TraceEvent, DebugModule::kRingEvents> trace{};
    std::array<uint8_t, DebugModule::kLogRingBytes> log{};
    std::array<StackType_t, DebugModule::kTaskStackBytes / sizeof(StackType_t)> stack{};
    CHECK(fw::isOk(debug.bindStorage({trace, log, stack})));
    CHECK(fw::isOk(debug.init()));
    CHECK(fw::isOk(debug.attachLog()));
    pollPower(4U);
    s_gpioLevels[18] = 0;
    pollPower(251U);
    s_gpioLevels[25] = 1;
    pollPower(3U);
    CHECK(s_restarts == 0U && !debug.logOutputIdle());
    cli::setOutSink(&observeLastPowerLog, false);
    CHECK(ModuleTestAccess::pumpLogs());
    cli::setOutSink(nullptr, false);
    CHECK(debug.logOutputIdle() && s_restarts == 0U);
    pollPower(1U);
    CHECK(s_restarts == 1U);
}

/** @brief 复现切断入口 REG08 争用，检查下一轮重试且无 GPIO4 高沿。 */
void powerBusyAtCut()
{
    using namespace app;
    s_chargerReg = 0U;
    s_usbGpioConfigFails = true;
    s_gpioLevels[2] = 1;
    s_gpioLevels[25] = 1;
    setupPower(board::BootMode::kNormal);
    CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
    pollPower(3U);
    CHECK(appContext().data.powerStatus.read().shutdownStage == board::ShutdownStage::kQuiesce);
    s_chargerReg = board::kSgmReg08PgBit;
    s_gpioLevels[25] = 0;
    s_chargerBusy = true;
    pollPower(4U);
    CHECK(appContext().data.powerStatus.read().shutdownStage == board::ShutdownStage::kCutPower);
    CHECK(s_gpioLevels[4] == 0 && s_gpio4Presses == 0U && s_restarts == 0U);
    const auto reads = s_chargerReads;
    s_chargerBusy = false;
    pollPower(1U);
    CHECK(s_chargerReads == reads + 1U && appContext().data.powerStatus.read().usbPresent);
    pollPower(15U);
    CHECK(s_gpio4Presses == 0U && s_restarts == 1U);
}

/**
 * @brief 查询手机 0x03 并校验供电来源与电量字节
 * @param onBattery: 预期的电池供电标记
 * @param batteryPct: 预期的电量字节
 */
void checkBatteryReport(uint8_t onBattery, uint8_t batteryPct)
{
    using namespace app;
    const size_t index = s_write_calls;
    append(0U, 0x03U, 0U, proto::msg_type::kQuery);
    dirty(0U);
    uint32_t tick = s_clock;
    link_round(tick);
    CHECK(s_write_calls == index + 1U);
    proto::FrameParser parser{};
    proto::FrameView reply{};
    bool gotFrame = false;
    for (size_t i = 0U; i < s_write_sizes[index]; ++i) {
        if (parser.feed(s_write_bytes[index][i], reply) == proto::ParseResult::kFrame) {
            gotFrame = true;
        }
    }
    CHECK(gotFrame && reply.msgId == 0x03U && reply.payload.size() == 16U);
    if (gotFrame && reply.payload.size() == 16U) {
        CHECK(reply.payload[0] == onBattery && reply.payload[2] == batteryPct);
    }
}

/** @brief 持续低电时插 USB 取消计时；拔出后重新计满并进入 B2 保存阶段。 */
void powerLowBattery()
{
    using namespace app;
    s_batteryMv = 3390U;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    const auto initial = ctx.data.powerStatus.read();
    CHECK(initial.batteryMv == 3390U && initial.batteryPct == 5U);
    CHECK(!initial.usbPresent && initial.usbSource == board::UsbSource::kGpio);
    open(1U, kPhone);
    checkBatteryReport(1U, 5U);
    pollPower(1200U, 100U);
    s_gpioLevels[27] = 1;
    pollPower(1205U, 100U);
    checkBatteryReport(0U, 5U);
    CHECK(power::s_power_requests.canScheduleRestart());
    s_gpioLevels[27] = 0;
    pollPower(1200U, 100U);
    CHECK(power::s_power_requests.canScheduleRestart());
    pollPower(2U, 100U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kSave);
    CHECK(!power::s_power_requests.canScheduleRestart());
    CHECK(s_chargerReads == 0U);
}

/** @brief 验证运行期 ADC 失败清空电量、上报 0xFF，恢复后重新计满 120 s。 */
void powerBatteryReadFailure()
{
    using namespace app;
    s_batteryMv = 3390U;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    setupPower(board::BootMode::kNormal);
    auto &ctx = appContext();
    open(1U, kPhone);
    checkBatteryReport(1U, 5U);
    pollPower(1190U, 100U);
    s_batteryReadFails = true;
    pollPower(130U);
    const auto invalid = ctx.data.powerStatus.read();
    CHECK(invalid.batteryMv == 0U && invalid.batteryPct == 0xFFU);
    CHECK(invalid.shutdownStage == board::ShutdownStage::kIdle);
    checkBatteryReport(1U, 0xFFU);

    s_batteryReadFails = false;
    for (unsigned i = 0U; i < 130U && ctx.data.powerStatus.read().batteryPct == 0xFFU; ++i) {
        pollPower(1U);
    }
    CHECK(ctx.data.powerStatus.read().batteryPct == 5U);
    pollPower(1199U, 100U);
    CHECK(power::s_power_requests.canScheduleRestart());
    pollPower(2U, 100U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kSave);
}

/**
 * @brief 验证 USB 未知或充电待机均可阻止持续低电关机
 * @param mode: 正常模式测试 USB 未知，待机模式测试已知无 USB
 */
void powerLowBatteryInhibited(app::board::BootMode mode)
{
    using namespace app;
    const bool unknownUsb = mode == board::BootMode::kNormal;
    s_batteryMv = 3390U;
    s_chargerReg = 0U;
    s_chargerReady = !unknownUsb;
    s_usbGpioConfigFails = unknownUsb;
    s_gpioLevels[2] = 1;
    s_gpioLevels[25] = 1;
    setupPower(mode);
    pollPower(1215U, 100U);
    const auto snapshot = appContext().data.powerStatus.read();
    CHECK(snapshot.batteryPct == 5U && !snapshot.usbPresent);
    CHECK(snapshot.usbSource == (unknownUsb ? board::UsbSource::kStat : board::UsbSource::kGpio));
    CHECK(snapshot.shutdownStage == board::ShutdownStage::kIdle);
    CHECK(power::s_power_requests.shutdownReason() == board::ShutdownReason::kNone);
    CHECK(s_chargerReads == 0U && s_restarts == 0U);
}

/** @brief 验证新电量使用 20 点中值，可忽略组内单次电压尖峰。 */
void powerBatteryMedian()
{
    using namespace app;
    s_batteryMv = 3390U;
    setupPower(board::BootMode::kNormal);
    auto &powerStatus = appContext().data.powerStatus;
    CHECK(powerStatus.read().batteryMv == 3390U);
    pollPower(99U);
    s_batteryMv = 4400U;
    pollPower(1U);
    s_batteryMv = 3750U;
    pollPower(18U);
    CHECK(powerStatus.read().batteryMv == 3390U);
    pollPower(1U);
    const auto filtered = powerStatus.read();
    CHECK(filtered.batteryMv == 3750U && filtered.batteryPct == 60U);
}

/** @brief 验证慢轮询的有效样本可维持低电计时，采样中断超期后重新起算。 */
void powerBatteryFreshness()
{
    using namespace app;
    s_batteryMv = 3390U;
    s_chargerReg = 0U;
    s_gpioLevels[25] = 1;
    setupPower(board::BootMode::kNormal);
    pollPower(476U, 250U);
    CHECK(power::s_power_requests.canScheduleRestart());
    // 两秒内无成功采样，即使本轮恢复 ADC，也要重新累积 120 s。
    pollPower(1U, 2000U);
    CHECK(power::s_power_requests.canScheduleRestart());
    pollPower(479U, 250U);
    CHECK(power::s_power_requests.canScheduleRestart());
    pollPower(3U, 250U);
    CHECK(appContext().data.powerStatus.read().shutdownStage == board::ShutdownStage::kSave);
}

/** @brief 验证保存截止先拦重试，扫描提交拥塞下仍断电，30 s 后恢复写和 BT。 */
void powerTimeoutRecovery()
{
    using namespace app;
    setupPower(board::BootMode::kNormal);
    uint32_t traceElapsed = 0U;
    const auto traceShutdownAbort = [](void *data, const fw::TraceEvent &event) noexcept {
        if (event.cat == fw::TraceCat::kSys &&
            event.code == static_cast<uint8_t>(PowerTrace::kShutdownAbort)) {
            // traceInstall 原样转发本场景提供的 uint32_t 指针。
            *static_cast<uint32_t *>(data) = event.b;
        }
    };
    CHECK(fw::isOk(fw::traceInstall(traceShutdownAbort, &traceElapsed, &rtos::nowMs)));
    fw::traceSetMask(fw::traceBit(fw::TraceCat::kSys));
    auto &ctx = appContext();
    s_gpioLevels[25] = 1;
    s_scanResult = ESP_ERR_NOT_FINISHED;
    ctx.storage.markWhitelistDirty(std::array{kAlarm}, s_clock);
    s_store_result = ESP_FAIL;
    CHECK(power::requestShutdown(board::ShutdownReason::kKey));
    pollPower(2U);
    const uint32_t saveStart = s_clock;
    static_cast<void>(ctx.storage.poll(saveStart));
    CHECK(s_store_calls == 1U && ctx.storage.hasPendingWrites());
    // Storage 先轮询，精确到保存截止 tick 时重试恰好到期。
    s_clock = saveStart + board::kShutdownSaveTimeoutMs;
    s_store_result = ESP_OK;
    static_cast<void>(ctx.storage.poll(s_clock));
    CHECK(s_store_calls == 1U && ctx.storage.hasPendingWrites());
    static_cast<void>(ctx.power.poll(s_clock));
    CHECK(!ctx.bt_link.powerPauseDone() && s_scanCalls == 1U);
    open(1U, kPhone);
    CHECK(s_disconnectCalls == 1U);
    pollPower(15U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kCutPower);
    CHECK(s_gpioLevels[4] == 1 && s_restarts == 0U);
    static_cast<void>(ctx.storage.poll(s_clock));
    CHECK(s_store_calls == 1U);
    const uint32_t cutStart = s_clock;
    s_clock = cutStart + board::kShutdownCutTimeoutMs - 10U;
    static_cast<void>(ctx.power.poll(s_clock));
    CHECK(s_gpioLevels[4] == 1);
    pollPower(1U);
    CHECK(s_gpioLevels[4] == 0 &&
          ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kIdle);
    CHECK(traceElapsed == board::kShutdownCutTimeoutMs);
    CHECK(s_restarts == 0U && power::s_power_requests.canScheduleRestart());
    s_scanResult = ESP_OK;
    static_cast<void>(ctx.bt_link.poll(s_clock));
    CHECK(!s_scanModePaused);
    open(1U, kPhone);
    CHECK(s_disconnectCalls == 1U);
    static_cast<void>(ctx.storage.poll(s_clock));
    CHECK(s_store_calls == 2U && !ctx.storage.hasPendingWrites());
    CHECK(power::requestShutdown(board::ShutdownReason::kConsole));
    pollPower(3U);
    CHECK(ctx.data.powerStatus.read().shutdownStage == board::ShutdownStage::kQuiesce);
    CHECK(s_scanModePaused);
}

/** @brief 白名单清表实际走手机协议，验证失败码、RAM 和已写 NVS。 @param admitted: 关门前发送。 */
void powerPhoneWhitelist(bool admitted)
{
    using namespace app;
    s_saved[0] = kAlarm.bytes;
    s_saved_count = 1U;
    setup(std::array{kAlarm});
    appContext().bootConfig.userNvsReady = true;
    open(1U, kPhone);
    if (!admitted) {
        CHECK(power::s_power_requests.requestShutdown(board::BootMode::kNormal,
                                                      board::ShutdownReason::kKey));
    }
    const std::array<uint8_t, 1> clear{0U};
    append(0U, 0x44U, 0U, proto::msg_type::kSet, clear);
    dirty(0U);
    uint32_t tick = s_clock;
    link_round(tick);
    CHECK(s_write_calls == 1U);
    proto::FrameParser parser{};
    proto::FrameView reply{};
    for (const uint8_t byte : std::span{s_write_bytes[0]}.first(s_write_sizes[0])) {
        static_cast<void>(parser.feed(byte, reply));
    }
    const uint8_t expected = admitted ? 1U : 2U;
    CHECK(reply.msgId == 0x44U && reply.payload.size() == 2U && reply.payload[0] == expected &&
          reply.payload[1] == expected);
    CHECK(s_link_manager.whitelist().empty() == admitted);
    CHECK(s_whitelist_pending == admitted && power::s_power_requests.settingsSubmitted());
    if (admitted) {
        CHECK(power::s_power_requests.requestShutdown(board::BootMode::kNormal,
                                                      board::ShutdownReason::kKey));
    }
    persistWhitelist(s_clock, admitted);
    CHECK(s_store_calls == (admitted ? 1U : 0U));
    CHECK(s_saved_count == (admitted ? 0U : 1U));

    s_link_manager.pairOpen(s_clock);
    open(2U, kOtherAlarm);
    std::array<uint8_t, 8U> hello{};
    append(1U, core::kHelloMsgId, proto::sender::kAlarm, proto::msg_type::kPush, hello);
    dirty(1U);
    link_round(tick);
    CHECK(s_link_manager.whitelist().size() == s_saved_count && !s_whitelist_pending);
    CHECK(!ModuleTestAccess::phoneBlobWrites(s_clock));
    CHECK(!appContext().storage.hasPendingWrites());
}

/** @brief 蓝牙静默等待 CLOSE、拒绝晚到 OPEN，关机回退后接受新连接。 */
void powerBtPause()
{
    using namespace app;
    setup();
    auto &link = appContext().bt_link;
    open(1U, kPhone);
    link.setPowerPaused(true);
    CHECK(s_disconnectCalls == 1U);
    CHECK(!link.powerPauseDone());
    open(2U, kOtherAlarm);
    CHECK(s_disconnectCalls == 2U);
    event(SPP_SERVER_EVT_CLOSE, 1U);
    link.wait(0U);
    CHECK(link.powerPauseDone());
    link.setPowerPaused(false);
    open(1U, kPhone);
    CHECK(s_disconnectCalls == 2U);
    CHECK(!link.powerPauseDone());
}

void rxFairness()
{
    using namespace app;
    const std::array whitelist{kAlarm};
    setup(whitelist);
    open(1U, kPhone);
    open(2U, kAlarm);
    // 30 条查询 (>512B 轮次字节预算) + ALARM 心跳：验证多帧连续 feed 与跨轮起点轮换公平性。
    for (size_t idx = 0U; idx < 30U; ++idx) {
        append(0U, static_cast<uint16_t>(idx + 1U), 0U, proto::msg_type::kQuery);
    }
    const std::array<uint8_t, 8> heartbeat{5, 0, 80, 0, 0, 0, 0, 0};
    append(1U, proto::alarm::kMsgHeartbeat, proto::sender::kAlarm, proto::msg_type::kPush,
           heartbeat);
    dirty(0U);
    dirty(1U);
    uint32_t tick = s_clock;
    const uint32_t start = s_clock;
    link_round(tick);
    // 支持单轮多帧连续消费，受 512B 字节预算限制而不是 1 帧上限。
    CHECK(s_write_calls > 1U && s_clock == start);
    CHECK(s_sessions[0].drainPending && s_input_offset[0] < s_input_size[0]);
    Command cmd{};
    cmd.id = CommandId::kAlarmSet;
    cmd.state = 1U;
    cmd.level = 2U;
    CHECK(s_command_queue.send(cmd, 0U));
    s_clock += app::kTickPeriodMs;
    // 第 2 轮起点轮换到 ALARM，优先投递心跳并响应命令，不让 PHONE 高频流饿死 ALARM。
    link_round(tick);
    CHECK(s_alarm.isOverride());
    CHECK(s_service->lastHeartbeat().has_value());
    if (s_service->lastHeartbeat().has_value()) {
        CHECK(s_service->lastHeartbeat()->seq == 5U);
    }
    for (size_t idx = 0U; idx < 6U && s_service->stats().framesRx < 31U; ++idx) {
        link_round(tick);
    }
    CHECK(s_service->stats().framesRx == 31U);
    CHECK(s_write_timeouts[0] == 0U);
}

void rxFastPartial()
{
    using namespace app;
    setup();
    open(1U, kPhone);
    for (size_t idx = 0U; idx < 10U; ++idx) {
        append(0U, 1U, 0U, proto::msg_type::kQuery);
    }
    dirty(0U);
    uint32_t tick = s_clock;
    // 多帧连续 feed：10 条小查询 (<=512B) 在首轮即可全部完成，无需等 10 轮。
    link_round(tick);
    CHECK(s_write_calls == 10U && s_poll_wait == 0U);
    CHECK(s_service->stats().framesRx == 10U);
    link_round(tick); // 非空标志直到 read=0 才清除。
    CHECK(!s_sessions[0].drainPending);

    append(0U, 1U, 0U, proto::msg_type::kQuery);
    const size_t full = s_input_size[0];
    s_input_size[0] -= 5U;
    dirty(0U);
    link_round(tick);
    CHECK(s_service->stats().framesRx == 10U);
    s_input_size[0] = full;
    dirty(0U);
    link_round(tick);
    CHECK(s_service->stats().framesRx == 11U);

    append(0U, 1U, 0U, proto::msg_type::kQuery);
    dirty(0U);
    s_read_delay = 20U; // 完成一次非阻塞读前发生抢占，整轮预算已耗尽。
    const uint32_t frames = s_service->stats().framesRx;
    link_round(tick);
    CHECK(s_service->stats().framesRx == frames);
    CHECK(s_sessions[0].rxOffset == 0U && s_sessions[0].rxSize > 0U);
    s_read_delay = 0U;
    if (s_service->stats().framesRx == frames) {
        link_round(tick);
    }
    CHECK(s_service->stats().framesRx == frames + 1U);
}

void rxTickBudget()
{
    using namespace app;
    const std::array whitelist{kAlarm, kOtherAlarm};
    setup(whitelist);
    open(1U, kAlarm);
    open(2U, kOtherAlarm);
    const std::array<uint8_t, 8> heartbeat{5, 0, 80, 0, 0, 0, 0, 0};
    append(0U, proto::alarm::kMsgHeartbeat, proto::sender::kAlarm, proto::msg_type::kPush,
           heartbeat);
    dirty(0U);
    s_write_delay = 200U;
    uint32_t tick = 0U;
    const uint32_t before = s_clock;
    link_round(tick);
    CHECK(s_write_calls == 2U && s_clock == before);
    CHECK(s_service->stats().framesRx == 1U && s_service->lastHeartbeat().has_value());
    CHECK(s_write_timeouts[0] == 0U && s_write_timeouts[1] == 0U);
}

// 自测 oracle 用逐位 CRC，独立于产品的 CRC 查表实现。
uint32_t crc24Reference(std::span<const uint8_t> wire)
{
    uint32_t crc = 0;
    for (const uint8_t byte : wire) {
        crc ^= static_cast<uint32_t>(byte) << 16U; // uint8_t 扩展到 uint32_t 无损。
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc << 1U) ^ ((crc & 0x800000U) != 0U ? 0x1864CFBU : 0U);
        }
    }
    return crc & 0xFFFFFFU;
}

uint32_t crc32Reference(std::span<const uint8_t> wire, uint32_t initial = 0)
{
    uint32_t crc = initial ^ UINT32_MAX;
    for (const uint8_t byte : wire) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ UINT32_MAX;
}

std::span<const uint8_t> appendRtcm(size_t session, uint16_t message, size_t payloadSize = 8)
{
    CHECK(payloadSize >= 2 && payloadSize <= 1023);
    const auto wire = std::span{s_input[session]}.subspan(s_input_size[session], payloadSize + 6);
    wire[0] = 0xD3;
    // 载荷长度 <= 1023，上半部分 <= 3，掩码后的下半部分 <= 255，收窄安全。
    wire[1] = static_cast<uint8_t>(payloadSize >> 8U);
    wire[2] = static_cast<uint8_t>(payloadSize & 0xFFU);
    std::fill(wire.begin() + 3, wire.end() - 3, 0x11);
    // 测试消息号限制为 12 bit，上 8 位及掩码后的低半字节收窄安全。
    CHECK(message <= 4095);
    wire[3] = static_cast<uint8_t>(message >> 4U);
    wire[4] = static_cast<uint8_t>((message & 0xFU) << 4U);
    const uint32_t crc = crc24Reference(wire.first(payloadSize + 3));
    // CRC-24Q 的三个字节逐一取低 8 bit，均在 uint8_t 范围内。
    wire[payloadSize + 3] = static_cast<uint8_t>((crc >> 16U) & 0xFFU);
    wire[payloadSize + 4] = static_cast<uint8_t>((crc >> 8U) & 0xFFU);
    wire[payloadSize + 5] = static_cast<uint8_t>(crc & 0xFFU);
    s_input_size[session] += wire.size();
    return wire;
}

void appendBytes(size_t session, std::span<const uint8_t> bytes)
{
    CHECK(bytes.size() <= s_input[session].size() - s_input_size[session]);
    std::memcpy(std::span{s_input[session]}.subspan(s_input_size[session]).data(), bytes.data(),
                bytes.size());
    s_input_size[session] += bytes.size();
}

void checkReply(size_t index, uint16_t message)
{
    CHECK(index < s_write_calls);
    proto::FrameParser parser;
    proto::FrameView frame;
    size_t frames = 0;
    for (const uint8_t byte : std::span{s_write_bytes[index]}.first(s_write_sizes[index])) {
        if (parser.feed(byte, frame) == proto::ParseResult::kFrame) {
            ++frames;
            CHECK(frame.msgId == message);
            CHECK(frame.sender == proto::sender::kDevice &&
                  frame.type == proto::msg_type::kQueryRes);
        }
    }
    CHECK(frames == 1 && parser.crcErrors() == 0 && parser.lengthErrors() == 0);
    CHECK(s_write_handles[index] == 1);
}

void rtcmMix()
{
    using namespace app;
    setup({}, policyFromConfig());
    open(1, kPhone);
    append(0, 1, 0, proto::msg_type::kQuery);
    const auto first = appendRtcm(0, 1005);
    const auto second = appendRtcm(0, 1077, 11);
    const uint32_t expectedCrc = crc32Reference(second, crc32Reference(first));
    constexpr std::array<uint8_t, 4> kGarbage{0, 0x7F, 0xFF, 0x55};
    appendBytes(0, kGarbage);
    append(0, 2, 0, proto::msg_type::kQuery);
    event(SPP_SERVER_EVT_DATA, 1);
    uint32_t tick = s_clock;
    link_round(tick);
    // 支持单轮多帧连续 feed（<=512B）：混杂的 2 条产品帧与 2 条 RTCM 帧在同一轮直推完成。
    CHECK(s_write_calls == 2 && s_service->stats().framesRx == 2);
    checkReply(0, 1);
    checkReply(1, 2);
    CHECK(s_snapshot.rtcm_frames == 2 && s_snapshot.rtcm_bytes == first.size() + second.size());
    CHECK(s_snapshot.rtcm_crc32 == expectedCrc && s_snapshot.rtcm_last_msg == 1077);
    CHECK(s_snapshot.rtcm_rejected_role == 0 && s_snapshot.demux_unknown_bytes == kGarbage.size());
    CHECK(s_snapshot.parserCrcErrors == 0 && s_snapshot.parserLengthErrors == 0);
    CHECK(s_snapshot.rx_dropped == 0 && s_poll_wait == 0);
}

void rtcmRoles()
{
    using namespace app;
    const std::array whitelist{kAlarm};
    setup(whitelist, policyFromConfig());
    open(1, kAlarm);
    CHECK(s_link_manager.role(1) == core::Role::kAlarm);
    appendRtcm(0, 1005);
    dirty(0);
    drainRx(s_clock);
    CHECK(s_rtcm.stats().rejectedRole == 1 && s_rtcm.stats().frames == 0);
    spp_server_evt_t close{};
    close.type = SPP_SERVER_EVT_CLOSE;
    close.handle = 1;
    handleEvent(close, s_clock);
    open(1, kPhone);
    s_link_manager.pairOpen(s_clock);
    open(2, kOtherAlarm);
    CHECK(s_link_manager.role(2) == core::Role::kPending);
    appendRtcm(1, 1077);
    dirty(1);
    drainRx(s_clock);
    CHECK(s_rtcm.stats().rejectedRole == 2 && s_rtcm.stats().frames == 0);
    // 主动断开到物理 CLOSE 之间角色已经是 NONE，也不能接受 RTCM。
    s_service->onClose(2);
    appendRtcm(1, 1077);
    dirty(1);
    drainRx(s_clock);
    CHECK(s_rtcm.stats().rejectedRole == 3);
    publish_snapshot(s_clock);
    CHECK(s_snapshot.rtcm_frames == 0 && s_snapshot.rtcm_bytes == 0 && s_snapshot.rtcm_crc32 == 0);
    CHECK(s_snapshot.rtcm_last_msg == 0 && s_snapshot.rtcm_rejected_role == 3 &&
          s_write_calls == 0);
}

void rtcmReplayBudget()
{
    using namespace app;
    setup({}, policyFromConfig());
    open(1, kPhone);
    constexpr std::array<uint8_t, 3> kHeader{0xD3, 0, 0};
    appendBytes(0, kHeader);
    append(0, 1, 0, proto::msg_type::kQuery);
    const auto rtcm = appendRtcm(0, 1005);
    const uint32_t expectedCrc = crc32Reference(rtcm);
    append(0, 2, 0, proto::msg_type::kQuery);
    // 外层假 RTCM 把两条产品帧暂时认作载荷；长度为 62，转换到 uint8_t 无损。
    CHECK(s_input_size[0] - 3 < 256);
    s_input[0][2] = static_cast<uint8_t>(s_input_size[0] - 3);
    CHECK(crc24Reference(std::span{s_input[0]}.first(s_input_size[0])) != 0);
    constexpr std::array<uint8_t, 3> kBadCrc{0, 0, 0};
    appendBytes(0, kBadCrc);
    dirty(0);
    uint32_t tick = s_clock;
    link_round(tick);
    CHECK(s_service->stats().framesRx == 2 && s_write_calls == 2);
    CHECK(s_snapshot.rtcm_frames == 1 && s_snapshot.rtcm_crc32 == expectedCrc);
    CHECK(s_snapshot.demux_rtcm_rejects == 1 && s_snapshot.demux_unknown_bytes == 6);
    CHECK(s_snapshot.parserCrcErrors == 0 && s_snapshot.parserLengthErrors == 0);
    checkReply(0, 1);
    checkReply(1, 2);
}

void rtcmResetLifecycle()
{
    using namespace app;
    setup({}, policyFromConfig());
    open(1, kPhone);
    constexpr std::array<uint8_t, 10> kInvalid{0xAA, 0x44, 0x18, 0x19, 0xD3,
                                               0xFC, 0,    0x7F, 0xFF, 0x55};
    appendBytes(0, kInvalid);
    const size_t badBegin = s_input_size[0];
    append(0, 1, 0, proto::msg_type::kQuery);
    const size_t badSize = s_input_size[0] - badBegin;
    CHECK(crc32Reference(std::span{s_input[0]}.subspan(badBegin, badSize - proto::kCrcLen)) != 0);
    const auto badCrc =
        std::span{s_input[0]}.subspan(s_input_size[0] - proto::kCrcLen, proto::kCrcLen);
    std::fill(badCrc.begin(), badCrc.end(), 0);
    const auto wire = appendRtcm(0, 1005);
    const size_t full = s_input_size[0];
    s_input_size[0] = full - wire.size() + 4;
    dirty(0);
    drainRx(s_clock);
    publish_snapshot(s_clock);
    CHECK(s_sessions[0].demux->busy());
    CHECK(s_snapshot.demux_unknown_bytes == kInvalid.size() + badSize);
    CHECK(s_snapshot.demux_product_rejects == 2 && s_snapshot.demux_rtcm_rejects == 1);
    CHECK(s_snapshot.parserCrcErrors == 0 && s_snapshot.parserLengthErrors == 0);
    Command command{};
    command.id = CommandId::kRtcmReset;
    command.seq = 17;
    CHECK(s_command_queue.send(command, 0));
    process_commands(s_clock);
    CommandResult result;
    CHECK(s_result_queue.receive(result, 0));
    CHECK(result.seq == 17 && result.status == CommandStatus::kOk);
    publish_snapshot(s_clock);
    CHECK(s_sessions[0].demux->busy());
    CHECK(s_snapshot.rtcm_frames == 0 && s_snapshot.rtcm_bytes == 0 && s_snapshot.rtcm_crc32 == 0);
    CHECK(s_snapshot.rtcm_last_msg == 0 && s_snapshot.rtcm_rejected_role == 0);
    CHECK(s_snapshot.demux_unknown_bytes == 0 && s_snapshot.demux_product_rejects == 0 &&
          s_snapshot.demux_rtcm_rejects == 0);
    s_input_size[0] = full;
    dirty(0);
    drainRx(s_clock);
    CHECK(s_rtcm.stats().frames == 1 && s_rtcm.stats().crc32 == crc32Reference(wire));

    constexpr std::array<uint8_t, 3> kPartial{0xD3, 0, 6};
    appendBytes(0, kPartial);
    dirty(0);
    drainRx(s_clock);
    CHECK(s_sessions[0].demux->busy());
    spp_server_evt_t close{};
    close.type = SPP_SERVER_EVT_CLOSE;
    close.handle = 1;
    handleEvent(close, s_clock);
    handleEvent(close, s_clock); // 重复 CLOSE 不得重复累计。
    publish_snapshot(s_clock);
    CHECK(s_snapshot.demux_unknown_bytes == 3 && s_snapshot.demux_rtcm_rejects == 1);
    CHECK(!s_sessions[0].used && !s_sessions[0].demux->busy());
    open(1, kPhone);
    const auto next = appendRtcm(0, 1077);
    dirty(0);
    drainRx(s_clock);
    publish_snapshot(s_clock);
    CHECK(s_snapshot.rtcm_frames == 2 && s_snapshot.rtcm_last_msg == 1077);
    CHECK(s_snapshot.rtcm_crc32 == crc32Reference(next, crc32Reference(wire)));
    CHECK(s_snapshot.demux_unknown_bytes == 3 && s_snapshot.demux_rtcm_rejects == 1);
    // 已关闭会话的统计也属于 rtcm reset 的清零范围。
    CHECK(s_command_queue.send(command, 0));
    process_commands(s_clock);
    publish_snapshot(s_clock);
    CHECK(s_snapshot.rtcm_frames == 0 && s_snapshot.rtcm_crc32 == 0 &&
          s_snapshot.demux_unknown_bytes == 0 && s_snapshot.demux_rtcm_rejects == 0);
}

void rtcmByteBudget()
{
    using namespace app;
    setup({}, policyFromConfig());
    open(1, kPhone);
    const auto first = appendRtcm(0, 1005, 1023);
    const auto second = appendRtcm(0, 1077);
    const uint32_t expectedCrc = crc32Reference(second, crc32Reference(first));
    dirty(0);
    uint32_t tick = s_clock;
    size_t consumed = 0;
    for (size_t round = 0; round < 6 && s_rtcm.stats().frames < 2; ++round) {
        link_round(tick);
        const auto &entry = s_sessions[0];
        const size_t next = s_input_offset[0] - (entry.rxSize - entry.rxOffset);
        CHECK(next - consumed <= kRxBudgetBytes);
        consumed = next;
        if (consumed < first.size()) {
            CHECK(s_rtcm.stats().frames == 0);
        }
    }
    CHECK(s_rtcm.stats().frames == 2 && s_rtcm.stats().bytes == first.size() + second.size());
    CHECK(s_rtcm.stats().crc32 == expectedCrc && s_write_calls == 0);
}

void lifecycleApplication()
{
    using namespace app;
    setup();
    event(SPP_SERVER_EVT_OPEN, 1U, kPhone);
    event(SPP_SERVER_EVT_CLOSE, 1U);
    event(SPP_SERVER_EVT_OPEN, 1U, kAlarm);
    event(SPP_SERVER_EVT_DATA, 1U);
    append(0U, 1U, 0U, proto::msg_type::kQuery);
    uint32_t tick = s_clock;
    link_round(tick);
    CHECK(s_link_manager.role(1U) == core::Role::kPhone);
    CHECK(s_snapshot.session_count == 1U && s_snapshot.sessions[0].addr == kAlarm);
    CHECK(s_service->stats().framesRx == 1U && s_write_calls == 1U);
}
} // namespace

namespace rtos {
uint32_t nowMs() noexcept
{
    const auto elapsed = s_wall_clock_start == nullptr
                             ? 0
                             : std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - *s_wall_clock_start)
                                   .count();
    return s_clock + static_cast<uint32_t>(elapsed);
}
fw::Status fromEspErr(esp_err_t err) noexcept
{
    return (err == ESP_OK) ? fw::Status::kOk : fw::Status::kTimeout;
}
} // namespace rtos

extern "C" uint32_t __real_ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticks);

/** @brief 在真实主机替身等待前注入可控的抢占间隙，事件通知用例仍走原替身。 */
extern "C" uint32_t __wrap_ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticks)
{
    if (s_power_wait_clock == nullptr) {
        return __real_ulTaskNotifyTake(clearOnExit, ticks);
    }
    if (s_power_wait_clock->waits++ == 0U) {
        s_clock += s_power_wait_clock->firstGapMs;
    }
    s_clock += ticks;
    return 0U;
}

extern "C" {
void fake_log(const char *, const char *, const char *, ...) {}
const char *esp_err_to_name(esp_err_t err)
{
    return (err == ESP_OK) ? "OK" : "FAIL";
}
uint32_t esp_get_free_heap_size(void)
{
    return 0U;
}
uint32_t esp_get_minimum_free_heap_size(void)
{
    return 0U;
}
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    if (mac == nullptr || type != ESP_MAC_BT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_bt_mac_result != ESP_OK) {
        return s_bt_mac_result;
    }
    std::memcpy(mac, s_bt_mac.data(), s_bt_mac.size());
    return ESP_OK;
}
esp_err_t spp_server_init(const char *device_name)
{
    ++s_spp_init_calls;
    s_spp_init_name.fill('\0');
    if (device_name != nullptr) {
        static_cast<void>(
            std::snprintf(s_spp_init_name.data(), s_spp_init_name.size(), "%s", device_name));
    }
    return s_spp_init_result;
}
esp_err_t esp_task_wdt_add(TaskHandle_t)
{
    return ESP_OK;
}
esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}
void vTaskDelete(TaskHandle_t)
{
    /* host 替身：pthread 入口返回即结束线程；追踪任务不在 host 场景启动，这里只是链接占位。 */
}
void vTaskDelay(TickType_t ticks)
{
    s_clock += ticks;
    if (ticks != 0U && s_consoleDelay != nullptr && fake_current_task == nullptr) {
        const auto callback = s_consoleDelay;
        s_consoleDelay = nullptr;
        callback();
    }
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return fake_current_task != nullptr ? fake_current_task
                                        : (s_use_host_task ? &s_host_task : nullptr);
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t)
{
    return 0U;
}
bool spp_server_poll_event(spp_server_evt_t *evt, uint32_t timeout)
{
    if (s_event_offset < s_event_size) {
        *evt = s_events[s_event_offset++];
        return true;
    }
    s_poll_wait += timeout;
    s_clock += timeout;
    return false;
}
size_t spp_server_read(uint32_t handle, uint8_t *bytes, size_t capacity)
{
    const size_t idx = handle - 1U;
    const size_t available = s_input_size[idx] - s_input_offset[idx];
    const size_t count = (available < capacity) ? available : capacity;
    std::memcpy(bytes, std::span<const uint8_t>{s_input[idx]}.subspan(s_input_offset[idx]).data(),
                count);
    s_input_offset[idx] += count;
    s_clock += s_read_delay;
    return count;
}
esp_err_t spp_server_write(uint32_t handle, const uint8_t *bytes, size_t size, uint32_t timeout)
{
    CHECK(s_write_calls < s_write_bytes.size() && size <= s_write_bytes[0].size());
    if (s_write_calls >= s_write_bytes.size() || size > s_write_bytes[0].size()) {
        return ESP_FAIL;
    }
    std::memcpy(s_write_bytes[s_write_calls].data(), bytes, size);
    s_write_sizes[s_write_calls] = size;
    s_write_handles[s_write_calls] = handle;
    s_store_at_write[s_write_calls] = s_store_calls;
    s_write_timeouts[s_write_calls++] = timeout;
    CHECK(timeout == 0U || timeout == 200U);
    s_clock += (timeout == 0U) ? 0U : s_write_delay;
    return (timeout != 0U && s_write_delay != 0U) ? ESP_ERR_TIMEOUT : ESP_OK;
}
esp_err_t spp_server_reply_reserve(uint32_t handle, spp_server_tx_token_t *token)
{
    if (handle < 1U || handle > 2U) {
        return ESP_ERR_NOT_FOUND;
    }
    const auto index = handle - 1U;
    if (s_tx_reserved[index] ||
        s_tx_used[index] + SPP_SERVER_REPLY_RESERVE > SPP_SERVER_REPLY_CAPACITY) {
        return ESP_ERR_TIMEOUT;
    }
    s_tx_reserved[index] = true;
    // 两槽索引<=1，收窄安全。
    *token = {1U, ++s_tx_serial, SPP_SERVER_REPLY_RESERVE, static_cast<uint8_t>(index), 0U};
    return ESP_OK;
}
esp_err_t spp_server_tx_commit(const spp_server_tx_token_t *token, const uint8_t *data, size_t size)
{
    CHECK(s_tx_reserved[token->slot] && size <= token->capacity);
    s_tx_reserved[token->slot] = false;
    s_tx_used[token->slot] += s_tx_hold ? size : 0U;
    return spp_server_write(token->slot + 1U, data, size, 0U);
}
esp_err_t spp_server_tx_commit2(const spp_server_tx_token_t *token, const uint8_t *a, size_t alen,
                                const uint8_t *b, size_t blen)
{
    // 替身按真实语义拼成一条记录后走单段路径，便于逐字节断言。
    static std::array<uint8_t, SPP_SERVER_STREAM_CAPACITY> joined{};
    CHECK(alen != 0U && blen != 0U && alen + blen <= joined.size());
    std::memcpy(joined.data(), a, alen);
    std::memcpy(joined.data() + alen, b, blen);
    return spp_server_tx_commit(token, joined.data(), alen + blen);
}
void spp_server_tx_cancel(const spp_server_tx_token_t *token)
{
    CHECK(s_tx_reserved[token->slot]);
    s_tx_reserved[token->slot] = false;
    ++s_tx_cancels;
}
bool spp_server_reply_ready(uint32_t handle)
{
    return handle >= 1U && handle <= 2U && !s_tx_reserved[handle - 1U] &&
           s_tx_used[handle - 1U] + SPP_SERVER_REPLY_RESERVE <= SPP_SERVER_REPLY_CAPACITY;
}
esp_err_t spp_server_push_latest(uint32_t handle, const uint8_t *data, size_t size)
{
    return spp_server_write(handle, data, size, 0U);
}
esp_err_t spp_server_stream_bind(uint32_t, bool)
{
    return ESP_OK;
}
esp_err_t spp_server_stream_reserve(uint32_t handle, size_t len, spp_server_tx_token_t *token)
{
    if ((handle < 1U) || (handle > 2U) || (len == 0U) || (len > SPP_SERVER_STREAM_CAPACITY) ||
        (token == nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto index = handle - 1U;
    if (s_tx_reserved[index]) {
        return ESP_ERR_TIMEOUT;
    }
    s_tx_reserved[index] = true;
    *token = {1U, ++s_tx_serial, static_cast<uint16_t>(len), static_cast<uint8_t>(index), 1U};
    return ESP_OK;
}
void spp_server_tx_stats(spp_server_tx_stats_t *stats)
{
    *stats = {};
}
/** @brief 模拟 GAP 投递结果。 @param paused: 目标扫描模式。 @retval SDK 投递结果。 */
extern "C" esp_err_t spp_server_scan_pause(bool paused)
{
    ++s_scanCalls;
    if (s_scanResult == ESP_OK) {
        s_scanModePaused = paused;
    }
    return s_scanResult;
}

esp_err_t spp_server_disconnect(uint32_t)
{
    ++s_disconnectCalls;
    return ESP_OK;
}
/* host 无 SPP 驱动：追踪钩子与 RX 查询/唤醒替身。 */
void spp_server_set_trace_hook(spp_server_trace_fn) {}
void spp_server_set_rx_wake_hook(spp_server_rx_wake_fn fn, void *ctx)
{
    s_rx_wake_fn = fn;
    s_rx_wake_ctx = ctx;
}
bool spp_server_rx_ready(uint32_t handle)
{
    if (handle < 1U || handle > 2U) {
        return false;
    }
    const size_t idx = handle - 1U;
    return s_input_offset[idx] < s_input_size[idx];
}
bool spp_server_any_rx_ready(void)
{
    return spp_server_rx_ready(1U) || spp_server_rx_ready(2U);
}
void spp_server_rx_stats(spp_server_rx_stats_t *stats)
{
    if (stats != nullptr) {
        *stats = {};
        stats->wake_calls = s_rx_wake_calls;
    }
}
void spp_server_set_pair_query(spp_server_pair_query_t query)
{
    s_pair_query = query;
}

void esp_restart(void)
{
    ++s_restarts;
    app::power::s_power_requests.releaseAction();
}

esp_err_t nvs_store_erase_user(void)
{
    ++s_user_erases;
    return s_erase_result;
}

esp_err_t nvs_store_erase_factory(void)
{
    ++s_factory_erases;
    return ESP_OK;
}

esp_err_t nvs_store_init(void)
{
    ++s_nvs_init_calls;
    return s_nvs_init_result;
}
esp_err_t nvs_store_get_sn(char *out, size_t size)
{
    constexpr char kSerial[] = "TEST";
    if (size < sizeof(kSerial)) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(out, std::data(kSerial), sizeof(kSerial));
    return ESP_OK;
}
esp_err_t nvs_store_set_sn(const char *)
{
    return ESP_OK;
}
esp_err_t nvs_store_set_alarms(const uint8_t (*alarms)[NVS_STORE_ADDR_LEN], size_t count)
{
    ++s_store_calls;
    if (s_store_result == ESP_OK) {
        s_saved_count = count;
        const std::span rows{alarms, count};
        for (size_t idx = 0U; idx < count; ++idx) {
            // C ABI 二维数组行交给 memcpy，长度固定为 NVS_STORE_ADDR_LEN。
            std::memcpy(s_saved[idx].data(), rows[idx], NVS_STORE_ADDR_LEN);
        }
    }
    return s_store_result;
}
esp_err_t nvs_store_get_alarms(uint8_t (*alarms)[NVS_STORE_ADDR_LEN], size_t maxCount,
                               size_t *count)
{
    const size_t n = std::min(s_saved_count, maxCount);
    const std::span rows{alarms, n};
    for (size_t idx = 0U; idx < n; ++idx) {
        std::memcpy(rows[idx], s_saved[idx].data(), NVS_STORE_ADDR_LEN);
    }
    *count = n;
    return ESP_OK;
}
}

// 真实 LinkService + SppTransport：16 请求跨过旧8项队列，满时保留尾帧并只暂停 PHONE。
void creditWindow()
{
    using namespace app;
    const std::array whitelist{kAlarm};
    setup(whitelist);
    open(1U, kPhone);
    open(2U, kAlarm);
    s_tx_hold = true;
    ModuleTestAccess::last_tick_ms() = s_clock; // 本场景固定时钟，推送时序由其它场景覆盖。
    constexpr uint16_t kSmallQuery = 6U; // 卫星跟踪占位应答26B，16条连同最坏预留仍小于2048B。
    for (size_t index = 0; index < 16U; ++index) {
        append(0U, kSmallQuery, proto::sender::kApp, proto::msg_type::kQuery);
    }
    dirty(0U);
    const auto round = [] {
        appContext().bt_link.wait(0U); // 交付生命周期/接收通知，不推进假时钟。
        static_cast<void>(appContext().bt_link.poll(s_clock));
    };
    const uint32_t start = s_clock;
    for (size_t index = 0; index < 20U; ++index) {
        round();
    }
    CHECK(s_clock == start && s_write_calls == 16U && s_service->stats().framesRx == 16U);
    CHECK(s_tx_used[0] < SPP_SERVER_REPLY_CAPACITY && !s_service->paused(1U));
    // 模拟另一生产者已占满容量；不得消费后续 RX 或重复路由完整 pending 帧。
    s_tx_used[0] = SPP_SERVER_REPLY_CAPACITY;
    append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    const std::array<uint8_t, 8> heartbeat{5, 0, 80, 0, 0, 0, 0, 0};
    append(1U, proto::alarm::kMsgHeartbeat, proto::sender::kAlarm, proto::msg_type::kPush,
           heartbeat);
    dirty(0U);
    dirty(1U);
    round();
    round(); // 全局每轮只分发一条产品帧：先服务 ALARM 后再触达 PHONE 的满窗口。
    CHECK(s_service->paused(1U) && s_write_calls == 16U);
    const size_t offset = s_input_offset[0];
    round();
    CHECK(s_service->lastHeartbeat().has_value() && !s_tx_reserved[1] && s_tx_cancels == 1U);
    CHECK(s_input_offset[0] == offset && s_service->paused(1U));
    for (size_t index = 0; index < 3U; ++index) {
        round();
    }
    CHECK(s_write_calls == 16U && s_input_offset[0] == offset);
    // 即使不发送 TX_SPACE，睡眠前重查 level 仍会恢复，且每个请求恰好一次。
    s_tx_used[0] = 0;
    round();
    round();
    CHECK(!s_service->paused(1U) && s_write_calls == 18U);
    s_tx_used[0] = SPP_SERVER_REPLY_CAPACITY;
    append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    dirty(0U);
    round();
    CHECK(s_service->paused(1U));
    event(SPP_SERVER_EVT_CLOSE, 1U);
    s_tx_used[0] = 0;
    round();
    CHECK(!s_service->paused(1U) && s_write_calls == 18U &&
          s_service->stats().dispatchCancelled == 1U);
}

// 验证 RX level 兜底、paused replyReady 复查与 BTC 通知唤醒（不依赖轮询睡死）。
void rxWakeLevelRecheck()
{
    using namespace app;
    setup();
    open(1U, kPhone);
    // 1) 模拟 evt_queue 满丢掉 DATA 事件：仅缓冲可读，wait(20) 仍须立即醒来并标记 drainPending。
    append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    appContext().bt_link.wait(20U);
    CHECK(s_poll_wait == 0U && s_sessions[0].drainPending);
    static_cast<void>(appContext().bt_link.poll(s_clock));
    CHECK(s_service->stats().framesRx == 1U && s_write_calls == 1U);

    // 2) 模拟 TX_SPACE 丢失：paused 下 replyReady 恢复时 wait(20) 立即返回。
    s_tx_hold = true;
    s_tx_used[0] = SPP_SERVER_REPLY_CAPACITY;
    append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    dirty(0U);
    appContext().bt_link.wait(0U);
    static_cast<void>(appContext().bt_link.poll(s_clock));
    CHECK(s_service->paused(1U) && s_write_calls == 1U);
    s_tx_used[0] = 0U;
    appContext().bt_link.wait(20U);
    static_cast<void>(appContext().bt_link.poll(s_clock));
    CHECK(!s_service->paused(1U) && s_write_calls == 2U);

    // 3) 真实任务通知唤醒：空等 wait(500)，后台线程注入帧并调用 rx_wake_fn 应立刻唤醒。
    s_tx_hold = false;
    // 清空上一帧残留的 drainPending，确保真正进入阻塞等待分支。
    appContext().bt_link.wait(0U);
    static_cast<void>(appContext().bt_link.poll(s_clock));
    CHECK(!s_sessions[0].drainPending);
    pthread_mutex_init(&s_host_task.lock, nullptr);
    pthread_condattr_t condAttr{};
    pthread_condattr_init(&condAttr);
    pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
    pthread_cond_init(&s_host_task.cond, &condAttr);
    pthread_condattr_destroy(&condAttr);
    s_host_task.notif = 0U;
    s_host_task.started = true;
    fake_current_task = &s_host_task;
    s_use_host_task = true;
    CHECK(s_rx_wake_fn != nullptr);

    std::thread waker([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
        ++s_rx_wake_calls;
        s_rx_wake_fn(s_rx_wake_ctx);
    });
    const auto t0 = std::chrono::steady_clock::now();
    appContext().bt_link.wait(500U);
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    waker.join();
    s_use_host_task = false;
    fake_current_task = nullptr;
    CHECK(elapsedMs < 250);
    static_cast<void>(appContext().bt_link.poll(s_clock));
    CHECK(s_service->stats().framesRx == 3U && s_write_calls == 3U);
}

// 验证 512B 读缓冲尾切片保留、单会话暂停不阻塞另一会话、以及 CLOSE 清理。
void rx512PartialAndPauseIsolation()
{
    using namespace app;
    const std::array whitelist{kAlarm};
    setup(whitelist);
    open(1U, kPhone);
    open(2U, kAlarm);
    s_tx_hold = true;
    // 预留空间只够 1 条回复，确保第 1 帧成功、第 2 帧在 512B 读缓冲中触发 pause。
    s_tx_used[0] = SPP_SERVER_REPLY_CAPACITY - SPP_SERVER_REPLY_RESERVE;
    for (size_t i = 0U; i < 20U; ++i) {
        append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    }
    const std::array<uint8_t, 8> heartbeat{9, 0, 85, 0, 0, 0, 0, 0};
    append(1U, proto::alarm::kMsgHeartbeat, proto::sender::kAlarm, proto::msg_type::kPush,
           heartbeat);
    dirty(0U);
    dirty(1U);
    uint32_t tick = s_clock;
    // 第 1 轮：单次 spp_server_read 读入 <=512B；PHONE 发 1 帧后第 2 帧 pause，同轮仍服务 ALARM 心跳。
    link_round(tick);
    CHECK(s_sessions[0].rxSize > 256U && s_sessions[0].rxOffset < s_sessions[0].rxSize);
    CHECK(s_service->paused(1U) && s_write_calls == 1U);
    CHECK(s_service->lastHeartbeat().has_value() && s_service->lastHeartbeat()->seq == 9U);
    const size_t pausedOffset = s_sessions[0].rxOffset;

    // 再次从 ALARM 发送心跳，不得被 PHONE 暂停阻塞。
    const std::array<uint8_t, 8> heartbeat2{10, 0, 86, 0, 0, 0, 0, 0};
    append(1U, proto::alarm::kMsgHeartbeat, proto::sender::kAlarm, proto::msg_type::kPush,
           heartbeat2);
    dirty(1U);
    link_round(tick);
    CHECK(s_service->lastHeartbeat()->seq == 10U && s_write_calls == 1U);
    CHECK(s_sessions[0].rxOffset == pausedOffset);

    // 释放 PHONE 回复信用后，保留在 512B 缓冲里的尾切片继续逐帧无损投递。
    s_tx_hold = false;
    s_tx_used[0] = 0U;
    for (size_t i = 0U; i < 6U && s_service->stats().framesRx < 22U; ++i) {
        link_round(tick);
    }
    CHECK(!s_service->paused(1U) && s_service->stats().framesRx == 22U && s_write_calls == 20U);

    // CLOSE 时取消在途挂起请求并丢弃未消费尾部。
    s_tx_used[0] = SPP_SERVER_REPLY_CAPACITY;
    append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    append(0U, 1U, proto::sender::kApp, proto::msg_type::kQuery);
    dirty(0U);
    link_round(tick);
    CHECK(s_service->paused(1U) && s_sessions[0].rxOffset < s_sessions[0].rxSize);
    event(SPP_SERVER_EVT_CLOSE, 1U);
    s_tx_used[0] = 0U;
    link_round(tick);
    CHECK(!s_service->paused(1U) && s_write_calls == 20U &&
          s_service->stats().dispatchCancelled == 1U);
}

// 验证 RtcmPipe 4096B 直推满帧整帧丢弃与 kIo 写状态统计契约。
void rtcmPipeDirectAndIoStats()
{
    app::RtcmPipe pipe;
    pipe.initLock();
    std::array<uint8_t, app::kRtcmPipeBytes> pipeBuf{};
    CHECK(pipe.bindStorage(pipeBuf) == fw::Status::kOk);
    CHECK(pipe.init() == fw::Status::kOk);
    std::array<uint8_t, 1029> frame{};
    frame[0] = 0xD3U;
    CHECK(pipe.push(frame));
    CHECK(pipe.push(frame));
    CHECK(pipe.push(frame));
    // 已占 3087 B，剩余 1009 B < 1029 B，第 4 帧整帧丢弃且不破坏已入队字节。
    CHECK(!pipe.push(frame));
    auto stats = pipe.stats();
    CHECK(stats.pushedFrames == 3U && stats.pushedBytes == 3087U);
    CHECK(stats.droppedFrames == 1U && stats.droppedBytes == 1029U);
    CHECK(pipe.available() == 3087U);

    std::array<uint8_t, 512> chunk{};
    CHECK(pipe.read(chunk) == 512U);
    pipe.noteWritten(256U);
    pipe.setPending(256U, true);
    auto io = pipe.ioStats();
    CHECK(io.pendingBytes == 256U && io.paused);
    pipe.noteWriteError(256U);
    pipe.noteDiscarded(128U);
    pipe.discard();
    io = pipe.ioStats();
    stats = pipe.stats();
    CHECK(pipe.available() == 0U && stats.writtenBytes == 256U);
    CHECK(io.uartWriteErrors == 1U && !io.paused && io.pendingBytes == 0U);
    CHECK(io.discardedBytes == (256U + 128U + (3087U - 512U)));
}

namespace {

struct StoreLogCapture {
    std::array<char, 2048U> log{};
    std::array<char, 1024U> console{};
    size_t logSize = 0U;
    size_t consoleSize = 0U;
    size_t warnings = 0U;
};

constinit StoreLogCapture s_store_log{};

/**
 * @brief 收集同一个主机测试中的启动存储日志。
 * @param ctx: 未使用。
 * @param hdr: 日志分类与级别。
 * @param payload: 日志正文。
 * @note 单线程主机测试。
 */
void captureStoreLog(void * /*ctx*/, const fw::LogHeader &hdr,
                     std::span<const uint8_t> payload) noexcept
{
    auto &out = s_store_log.log;
    auto &size = s_store_log.logSize;
    if (hdr.cat == fw::TraceCat::kStore && payload.size() < out.size() - size) {
        s_store_log.warnings += hdr.level == fw::LogLevel::kWarn ? 1U : 0U;
        std::memcpy(out.data() + size, payload.data(), payload.size());
        size += payload.size();
        out[size++] = '\n';
    }
}

/**
 * @brief 收集 config 命令写入控制台的文本。
 * @param text: 待输出字节。
 * @param len: 字节数。
 * @note 单线程主机测试。
 */
void captureStoreConsole(const char *text, size_t len) noexcept
{
    auto &out = s_store_log.console;
    auto &size = s_store_log.consoleSize;
    if (len <= out.size() - size) {
        std::memcpy(out.data() + size, text, len);
        size += len;
    }
}

/**
 * @brief 校验目录加载分类与 config、启动日志的逐键格式一致性。
 * @note 单线程主机场景，每次在新进程运行。
 */
void storageStartup()
{
    auto &storage = app::appContext().storage;
    const auto &keys = app::debug::kKeys;
    const auto defaults = storage.loadKeys(keys);
    CHECK(defaults.saved == 0U && defaults.defaults == keys.size() && defaults.failed == 0U);

    CHECK(app::store::writeValue(keys[app::debug::kIdxMask], 1U) == ESP_OK);
    const auto partial = storage.loadKeys(keys);
    CHECK(partial.saved == 1U && partial.defaults == keys.size() - 1U && partial.failed == 0U);

    const auto &report = keys[app::debug::kIdxReport];
    CHECK(app::store::writeValue(report, static_cast<uint32_t>(report.max) + 1U) == ESP_OK);
    const auto failed = storage.loadKeys(keys);
    CHECK(failed.saved == 1U && failed.defaults == keys.size() - 2U && failed.failed == 1U);

    CHECK(fw::isOk(fw::logInstallDirect(&captureStoreLog, nullptr, &rtos::nowMs)));
    app::loadEarlyConfig(app::appContext());
    CHECK(fw::isOk(storage.init()));
    cli::setOutSink(&captureStoreConsole, false);
    char command[] = "config";
    char group[] = "debug";
    std::array<char *, 2U> argv{command, group};
    const auto &config = app::configCmds().front();
    CHECK(config.handler(config.ctx, static_cast<int>(argv.size()), argv.data()) == 0);
    cli::setOutSink(nullptr, false);
    const std::string_view log{s_store_log.log.data(), s_store_log.logSize};
    std::string_view console{s_store_log.console.data(), s_store_log.consoleSize};
    CHECK(log.find("Could not load dbg/report: invalid value 3601\n") != std::string_view::npos);
    CHECK(log.find("debug: 1 saved, 2 default\n") != std::string_view::npos);
    const size_t header = console.find('\n');
    CHECK(header != std::string_view::npos);
    if (header != std::string_view::npos) {
        console.remove_prefix(header + 1U);
        CHECK(!console.empty() && log.find(console) != std::string_view::npos);
    }

    s_store_log.consoleSize = 0U;
    char buildGroup[] = "build";
    argv[1] = buildGroup;
    cli::setOutSink(&captureStoreConsole, false);
    CHECK(config.handler(config.ctx, static_cast<int>(argv.size()), argv.data()) == 0);
    cli::setOutSink(nullptr, false);
    const std::string_view build{s_store_log.console.data(), s_store_log.consoleSize};
    CHECK(build.starts_with("[build]\n  console        UART0 2000000 baud\n"
                            "  console_profile dev\n"));
}

/** @brief 验证天线高去抖与 NVS 重读，以及 PPP 启动恢复。 */
void phoneNvsReadback()
{
    auto &context = app::appContext();
    auto &storage = context.storage;
    context.data.init();
    app::loadEarlyConfig(context);
    CHECK(fw::isOk(storage.init()));
    CHECK(storage.phoneHeight() == core::AntennaHeight{});
    CHECK(!context.data.gnssPppEnabled.read().has_value());

    constexpr core::AntennaHeight kHeight{0, 0, 0, 0, 0, 0, 0xF8, 0x3F};
    CHECK(storage.setPhoneHeight(kHeight, s_clock));
    const auto group = std::ranges::find(app::storeCatalog(), "phone", &app::StoreGroup::name);
    CHECK(group != app::storeCatalog().end());
    const auto keys = group->keys;
    CHECK(storage.loadKeys(keys).defaults == keys.size());
    core::PhoneSnPn snPn{};
    std::memcpy(snPn.data(), "RTK20260001", 11U);
    std::memcpy(snPn.data() + core::kPhoneSnBytes, "1000190000002001", 16U);
    fw::Command snPnCmd{};
    snPnCmd.target = app::id(app::ModuleKind::kStorage);
    snPnCmd.verb = app::verb(app::StorageVerb::kSetSnPn);
    snPnCmd.argLen = static_cast<uint8_t>(snPn.size());
    std::copy(snPn.begin(), snPn.end(), snPnCmd.args.begin());
    fw::CommandResult snPnRes{};
    storage.onCommand(snPnCmd, snPnRes);
    CHECK(fw::isOk(snPnRes.status));
    core::PhoneNtripConfig ntrip{};
    ntrip[1U] = 1U;
    ntrip[8U] = 'n';
    ntrip[108U] = 3U;
    CHECK(storage.setPhoneNtrip(ntrip, s_clock));
    core::PhoneJsonServerConfig jsonSrv{};
    jsonSrv[0U] = 'j';
    jsonSrv[36U] = 1U;
    CHECK(storage.setPhoneJsonServer(jsonSrv, s_clock));
    core::PhoneThingsboardConfig tb{};
    tb[0U] = 1U;
    tb[2U] = 't';
    tb[85U] = 7U;
    CHECK(storage.setPhoneThingsboard(tb, s_clock));

    s_clock += 499U;
    static_cast<void>(storage.poll(s_clock));
    CHECK(storage.loadKeys(keys).defaults == keys.size());
    s_clock += 1U;
    static_cast<void>(storage.poll(s_clock));
    CHECK(app::store::writeValue(app::store::kPhonePppKey, 1U) == ESP_OK);
    const core::SatTrackConfig satCfg{
        .cutoffDeg = 12U, .gps = 1U, .glo = 0U, .bds = 1U, .gal = 1U, .sbas = 1U};
    CHECK(app::store::writeBlob(app::store::kPhoneSatCfgKey, core::encodeSatTrackConfig(satCfg)) ==
          ESP_OK);

    // 清空内存值后通过 StoreCatalog 重新回读，验证全部 7 个手机键完整持久化。
    CHECK(storage.setPhoneHeight(core::AntennaHeight{}, s_clock));
    CHECK(storage.setPhoneNtrip(core::PhoneNtripConfig{}, s_clock));
    CHECK(storage.setPhoneJsonServer(core::PhoneJsonServerConfig{}, s_clock));
    CHECK(storage.setPhoneThingsboard(core::PhoneThingsboardConfig{}, s_clock));
    context.data.gnssSatTrack.write(std::nullopt);
    const auto loaded = storage.loadKeys(keys);
    CHECK(loaded.saved == keys.size() && loaded.failed == 0U);
    CHECK(storage.phoneHeight() == kHeight);
    CHECK(context.data.phoneHeight.read() == kHeight);
    CHECK(context.data.gnssPppEnabled.read() == true);
    CHECK(context.data.gnssSatTrack.read() == satCfg);
    CHECK(storage.phoneSnPn() == snPn);
    CHECK(storage.phoneNtrip() == ntrip);
    CHECK(storage.phoneJsonServer() == jsonSrv);
    CHECK(storage.phoneThingsboard() == tb);

    // 验证 0x1E 延迟 >=200 ms 才执行重启与恢复出厂（先让应答发出）。
    auto sys = context.data.systemStatus.read();
    sys.btReady = true;
    context.data.systemStatus.write(sys);
    CHECK(fw::isOk(context.bt_link.init()));
    s_restarts = 0U;
    s_user_erases = 0U;
    CHECK(context.bt_link.schedulePhoneReset(false, true, s_clock));
    auto &requests = app::power::s_power_requests;
    using app::board::BootMode;
    using app::board::ShutdownReason;
    CHECK(requests.requestShutdown(BootMode::kNormal, ShutdownReason::kKey));
    CHECK(!context.bt_link.schedulePhoneReset(true, false, s_clock));
    s_clock += 250U;
    static_cast<void>(context.bt_link.poll(s_clock));
    CHECK(s_restarts == 0U && s_user_erases == 0U);
    requests.releaseAction();

    CHECK(context.bt_link.schedulePhoneReset(false, true, s_clock));
    s_clock += 249U;
    static_cast<void>(context.bt_link.poll(s_clock));
    CHECK(s_restarts == 0U && s_user_erases == 0U);
    s_clock += 1U;
    static_cast<void>(context.bt_link.poll(s_clock));
    CHECK(s_restarts == 1U && s_user_erases == 0U);

    CHECK(context.bt_link.schedulePhoneReset(true, false, s_clock));
    s_clock += 250U;
    static_cast<void>(context.bt_link.poll(s_clock));
    CHECK(s_restarts == 2U && s_user_erases == 1U);
}

/** @brief 验证启动计数、诊断校验、存储阶段只装载剩余键及不可用时的降级。 */
void earlyConfig(unsigned previous, bool nvsAvailable)
{
    auto &context = app::appContext();
    const auto &keys = app::debug::kKeys;
    const auto &mask = keys[app::debug::kIdxMask];
    const auto &report = keys[app::debug::kIdxReport];
    const auto &hex = keys[app::debug::kIdxHex];
    const auto &levels = keys[app::debug::kIdxLogLevel];
    constexpr fw::StoreKey kFails{.ns = "boot",
                                  .key = "fails",
                                  .type = fw::StoreType::kU8,
                                  .maxSize = 1U,
                                  .max = 255,
                                  .help = "boot count"};
    constexpr uint32_t kSavedLevels = fw::logLevelsAll(fw::LogLevel::kWarn);
    constexpr uint32_t kStoreInfo =
        fw::traceBit(fw::TraceCat::kStore) *
        (static_cast<uint32_t>(fw::LogLevel::kInfo) - static_cast<uint32_t>(fw::LogLevel::kWarn));
    constexpr uint32_t kFixtureLevels =
        kSavedLevels | (kStoreInfo << static_cast<unsigned>(fw::TraceCat::kStore));
    CHECK(app::store::writeValue(mask, 1U) == ESP_OK);
    CHECK(app::store::writeValue(report, 3601U) == ESP_OK);
    CHECK(app::store::writeValue(hex, fw::kTraceAllMask + 1U) == ESP_OK);
    CHECK(app::store::writeValue(levels, kFixtureLevels) == ESP_OK);
    if (previous != 0U) {
        CHECK(app::store::writeValue(kFails, previous) == ESP_OK);
    }
    s_nvs_init_result = nvsAvailable ? ESP_OK : ESP_FAIL;
    CHECK(fw::isOk(fw::logInstallDirect(&captureStoreLog, nullptr, &rtos::nowMs)));
    app::loadEarlyConfig(context);
    const bool safeDefaults = previous >= 3U;
    const bool readUser = nvsAvailable && !safeDefaults;
    CHECK(context.bootConfig.userNvsReady == nvsAvailable);
    CHECK(context.bootConfig.previousFailures == previous);
    CHECK(context.bootConfig.safeDefaults == safeDefaults);
    CHECK(context.bootConfig.traceMask == (readUser ? 1U : 0U));
    CHECK(context.bootConfig.debugCounts.failed == (readUser ? 2U : 0U));
    CHECK(fw::traceMask() == context.bootConfig.traceMask);
    CHECK(fw::logLevels() == (readUser ? kFixtureLevels : fw::kLogLevelsDefault));
    CHECK(fw::logHexMask() == 0U && context.debug.reportPeriodS() == 0U);
    CHECK(s_store_log.warnings == (readUser ? 2U : safeDefaults ? 1U : 0U));
    uint32_t failures = 0U;
    if (nvsAvailable) {
        CHECK(app::store::readValue(kFails, failures) == ESP_OK && failures == previous + 1U);
    } else {
        CHECK(app::store::readValue(kFails, failures) == ESP_ERR_NVS_NOT_FOUND);
    }

    CHECK(app::store::writeValue(mask, 2U) == ESP_OK);
    CHECK(fw::isOk(context.storage.init()));
    CHECK(s_nvs_init_calls == 1U);
    CHECK(fw::traceMask() == (readUser ? 1U : 0U));
    CHECK(context.debug.reportPeriodS() == 0U);
    if (safeDefaults) {
        uint32_t value = 0U;
        CHECK(app::store::readValue(mask, value) == ESP_OK && value == 2U);
        cli::setOutSink(&captureStoreConsole, false);
        char command[] = "config";
        char group[] = "debug";
        std::array<char *, 2U> argv{command, group};
        const auto &config = app::configCmds().front();
        CHECK(config.handler(config.ctx, static_cast<int>(argv.size()), argv.data()) == 0);
        cli::setOutSink(nullptr, false);
        const std::string_view console{s_store_log.console.data(), s_store_log.consoleSize};
        CHECK(console.find("safe defaults") != std::string_view::npos);
    }
    const std::string_view log{s_store_log.log.data(), s_store_log.logSize};
    CHECK((log.find("Previous boot did not complete") != std::string_view::npos) ==
          (previous != 0U));
    CHECK((log.find("Boot failed 3 times, using default settings") != std::string_view::npos) ==
          safeDefaults);
    CHECK((log.find("store init failed") != std::string_view::npos) == !nvsAvailable);
    app::clearBootFailures(context.bootConfig);
    if (nvsAvailable) {
        CHECK(app::store::readValue(kFails, failures) == ESP_OK && failures == 0U);
    }
}

/** @brief 经真实控制台处理器与执行器验证确认、排他与重启。 */
void factoryReset()
{
    using namespace app;
    auto &context = appContext();
    context.data.init();
    constexpr auto kStorage = id(ModuleKind::kStorage);
    constexpr auto kLink = id(ExecKind::kLink);
    static constexpr auto kOps = fw::makeOps<StorageModule>();
    static const std::array<fw::Module, 1> kModules{{
        {"storage", kStorage, kLink, 0U, &context.storage, &kOps},
    }};
    static constinit rtos::Executor sExecutor{};
    static std::array<StackType_t, AppContext::kLinkStackBytes / sizeof(StackType_t)> sStack{};
    context.linkExecutor = &sExecutor;
    const rtos::ExecutorConfig cfg{.name = "reset_test", .id = kLink, .priority = 5U, .core = 1};
    CHECK(fw::isOk(sExecutor.init(cfg, kModules, viewOf(context.linkStorage, sStack))));
    CHECK(fw::isOk(sExecutor.start()));
    pthread_mutex_init(&s_host_task.lock, nullptr);
    pthread_condattr_t condAttr{};
    pthread_condattr_init(&condAttr);
    pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
    pthread_cond_init(&s_host_task.cond, &condAttr);
    pthread_condattr_destroy(&condAttr);
    s_host_task.notif = 0U;
    s_host_task.started = true;
    fake_current_task = &s_host_task;
    s_use_host_task = true;
    cli::setOutSink(&captureStoreConsole, false);
    char command[] = "freset";
    char yes[] = "yes";
    char no[] = "no";
    std::array<char *, 2> args{command, no};
    const auto &entry = factoryResetCmds().front();
    CHECK(entry.handler(entry.ctx, 1, args.data()) == 0);
    CHECK(entry.handler(entry.ctx, 2, args.data()) != 0 && s_user_erases == 0U);
    args[1] = yes;
    {
        const store::WriteGuard busy;
        CHECK(busy.active());
        CHECK(entry.handler(entry.ctx, 2, args.data()) != 0 && s_user_erases == 0U);
    }
    s_erase_result = ESP_FAIL;
    CHECK(entry.handler(entry.ctx, 2, args.data()) != 0);
    CHECK(s_user_erases == 1U && s_factory_erases == 0U && s_restarts == 0U);
    s_erase_result = ESP_OK;
    CHECK(entry.handler(entry.ctx, 2, args.data()) == 0);
    CHECK(s_user_erases == 2U && s_factory_erases == 0U && s_restarts == 1U);
    const store::WriteGuard after;
    CHECK(!after.active());
    s_use_host_task = false;
    fake_current_task = nullptr;
    if (s_failures != 0) {
        static_cast<void>(
            std::fwrite(s_store_log.console.data(), 1U, s_store_log.consoleSize, stderr));
    }
    cli::setOutSink(nullptr, false);
}

/** @brief 应用开窗、GAP 查询和 LinkManager 角色转移共享同一个到期点。 */
void pairingWindow()
{
    using namespace app;
    const std::array whitelist{kOtherAlarm};
    setup(whitelist);
    auto &link = appContext().bt_link;
    link.installPairingPolicy();
    CHECK(s_pair_query(kPhone.bytes.data()) == SPP_SERVER_PAIR_DENY);
    CHECK(s_pair_query(kOtherAlarm.bytes.data()) == SPP_SERVER_PAIR_TRUSTED);
    s_link_manager.pairOpen(s_clock);
    CHECK(link.pairStatus(s_clock).remainingMs == core::kPairWindowMs);
    CHECK(s_pair_query(kPhone.bytes.data()) == SPP_SERVER_PAIR_ALLOW);
    fw::Command cmd{.verb = verb(LinkVerb::kPair)};
    cmd.args[0] = 0x58U;
    cmd.args[1] = 0x02U;
    cmd.args[2] = 1U;
    cmd.argLen = 3U;
    fw::CommandResult result{};
    link.onCommand(cmd, result);
    CHECK(fw::isOk(result.status));
    CHECK(link.pairStatus(s_clock).remainingMs == 600000U);
    CHECK(s_pair_query(kPhone.bytes.data()) == SPP_SERVER_PAIR_CONFIRM);
    core::Actions actions{};
    CHECK(s_link_manager.onOpen(1U, kPhone, s_clock, actions) == core::Role::kPhone);
    s_clock += 599999U;
    CHECK(s_link_manager.onOpen(2U, kAlarm, s_clock, actions) == core::Role::kPending);
    ++s_clock;
    CHECK(s_pair_query(kAlarm.bytes.data()) == SPP_SERVER_PAIR_DENY);
    CHECK(s_link_manager.onFrame(2U, core::kHelloSender, core::kHelloMsgId, s_clock, actions) ==
          core::Role::kNone);
    s_link_manager.tick(s_clock, actions);
    CHECK(!s_link_manager.isPairing());
    s_link_manager.pairOpen(s_clock);
    link.onCommand(fw::Command{.verb = verb(LinkVerb::kPairClose)}, result);
    CHECK(s_pair_query(kPhone.bytes.data()) == SPP_SERVER_PAIR_DENY);
}

/**
 * @brief 验证 S2：Wi-Fi 模式下经 storage 写白名单与 pair_next，重启进入 BT 模式读到同一白名单且 pair_next 仅生效一次
 * @note 单线程主机测试，非 ISR。
 */
void webS2StorageBt()
{
    using namespace app;
    auto &context = appContext();
    context.data.init();
    loadEarlyConfig(context);
    context.radioMode = RadioMode::kWifiConfig;
    s_saved_count = 0U;
    s_saved = {};
    s_store_result = ESP_OK;

    // 1. Wi-Fi 模式启动（不启动 bt_link）：经 storage 写白名单与 pair_next。
    CHECK(fw::isOk(context.storage.init()));
    CHECK(context.data.linkStatus.read().whitelistCount == 0U);
    CHECK(!context.storage.btPairNext());

    const auto makeWlCmd = [](StorageVerb v, const core::BtAddr &addr) {
        fw::Command cmd{};
        cmd.target = id(ModuleKind::kStorage);
        cmd.verb = verb(v);
        cmd.argLen = static_cast<uint8_t>(core::kAddrLen);
        std::copy(addr.bytes.begin(), addr.bytes.end(), cmd.args.begin());
        return cmd;
    };
    const core::BtAddr kThirdAlarm{{21U, 22U, 23U, 24U, 25U, 26U}};
    const core::BtAddr kFourthAlarm{{31U, 32U, 33U, 34U, 35U, 36U}};
    const core::BtAddr kFifthAlarm{{41U, 42U, 43U, 44U, 45U, 46U}};

    fw::CommandResult res{};
    context.storage.onCommand(makeWlCmd(StorageVerb::kWhitelistAdd, kAlarm), res);
    CHECK(res.status == fw::Result::kOk);
    context.storage.onCommand(makeWlCmd(StorageVerb::kWhitelistAdd, kAlarm), res);
    CHECK(res.status == fw::Result::kInvalidState);
    context.storage.onCommand(makeWlCmd(StorageVerb::kWhitelistAdd, kOtherAlarm), res);
    CHECK(res.status == fw::Result::kOk);
    context.storage.onCommand(makeWlCmd(StorageVerb::kWhitelistAdd, kThirdAlarm), res);
    CHECK(res.status == fw::Result::kOk);
    context.storage.onCommand(makeWlCmd(StorageVerb::kWhitelistAdd, kFourthAlarm), res);
    CHECK(res.status == fw::Result::kOk);
    context.storage.onCommand(makeWlCmd(StorageVerb::kWhitelistAdd, kFifthAlarm), res);
    CHECK(res.status == fw::Result::kNoSpace);

    context.storage.onCommand(makeWlCmd(StorageVerb::kWhitelistDel, kAlarm), res);
    CHECK(res.status == fw::Result::kOk);
    context.storage.onCommand(makeWlCmd(StorageVerb::kWhitelistDel, kAlarm), res);
    CHECK(res.status == fw::Result::kNotFound);

    const auto wifiLink = context.data.linkStatus.read();
    CHECK(wifiLink.whitelistCount == 3U);
    CHECK(wifiLink.whitelist[0] == kOtherAlarm);
    CHECK(wifiLink.whitelist[1] == kThirdAlarm);
    CHECK(wifiLink.whitelist[2] == kFourthAlarm);

    fw::Command pairCmd{};
    pairCmd.target = id(ModuleKind::kStorage);
    pairCmd.verb = verb(StorageVerb::kSetPairNext);
    pairCmd.argLen = 1U;
    pairCmd.args[0] = 1U;
    context.storage.onCommand(pairCmd, res);
    CHECK(res.status == fw::Result::kOk);
    CHECK(context.storage.btPairNext());
    CHECK(context.data.storeStatus.read().btPairNext);

    // 2. 模拟首次重启进入 BT 模式但 NVS 清零失败：保留 pair_next 待重试且本次不开配对窗口（P2 #7）。
    ModuleTestAccess::resetStorageForReboot();
    context.radioMode = RadioMode::kBluetooth;
    CHECK(fw::isOk(context.storage.init()));
    CHECK(context.storage.btPairNext());
    auto sys = context.data.systemStatus.read();
    sys.btReady = true;
    context.data.systemStatus.write(sys);
    nvs_shim_set_write_fail(ESP_FAIL);
    context.bt_link.installPairingPolicy();
    CHECK(fw::isOk(context.bt_link.init()));
    nvs_shim_set_write_fail(ESP_OK);
    CHECK(context.bt_link.pairStatus(s_clock).remainingMs == 0U);
    CHECK(context.storage.btPairNext());
    uint32_t persistedPairNext = 99U;
    CHECK(store::readValue(store::kBtPairNextKey, persistedPairNext) == ESP_OK);
    CHECK(persistedPairNext == 1U);

    // 3. 模拟 NVS 恢复后再次重启进入 BT 模式：读到同一白名单，先清零持久化再自动开启配对窗口。
    ModuleTestAccess::resetStorageForReboot();
    ModuleTestAccess::resetBtLinkForReboot();
    context.radioMode = RadioMode::kBluetooth;
    CHECK(fw::isOk(context.storage.init()));
    CHECK(context.storage.btPairNext());
    sys = context.data.systemStatus.read();
    sys.btReady = true;
    context.data.systemStatus.write(sys);
    context.bt_link.installPairingPolicy();
    CHECK(fw::isOk(context.bt_link.init()));

    const auto btWl = s_link_manager.whitelist();
    CHECK(btWl.size() == 3U);
    CHECK(btWl[0] == kOtherAlarm && btWl[1] == kThirdAlarm && btWl[2] == kFourthAlarm);
    CHECK(context.bt_link.pairStatus(s_clock).remainingMs == core::kPairWindowMs);
    CHECK(!context.storage.btPairNext());
    persistedPairNext = 99U;
    CHECK(store::readValue(store::kBtPairNextKey, persistedPairNext) == ESP_OK);
    CHECK(persistedPairNext == 0U);

    // 4. 模拟第三次 BT 重启：pair_next 已清零，不再自动开窗；白名单保持不变。
    ModuleTestAccess::resetStorageForReboot();
    ModuleTestAccess::resetBtLinkForReboot();
    CHECK(fw::isOk(context.storage.init()));
    CHECK(!context.storage.btPairNext());
    sys = context.data.systemStatus.read();
    sys.btReady = true;
    context.data.systemStatus.write(sys);
    context.bt_link.installPairingPolicy();
    CHECK(fw::isOk(context.bt_link.init()));
    CHECK(context.bt_link.pairStatus(s_clock).remainingMs == 0U);
    const auto btWl2 = s_link_manager.whitelist();
    CHECK(btWl2.size() == 3U && btWl2[0] == kOtherAlarm);
}

/**
 * @brief 验证 buildDeviceName 与 deriveSnFromMac 的 P2/E1/unknown、空值、换行与超长边界
 * @note 单线程主机测试，非 ISR。
 */
void checkDeviceNamePure()
{
    using namespace app;
    std::array<char, kDeviceNameBuf> name{};
    CHECK(buildDeviceName(board::productName(board::byId(board::BoardId::kP2V141)), "B5F2", name));
    CHECK(std::string_view{name.data()} == "P2-B5F2");
    CHECK(
        buildDeviceName(board::productName(board::byId(board::BoardId::kE1DevKit)), "A1B2", name));
    CHECK(std::string_view{name.data()} == "E1-A1B2");
    CHECK(buildDeviceName(board::productName(nullptr), "0000", name));
    CHECK(std::string_view{name.data()} == "RTK-0000");

    // 空 SN、空产品名、换行与 SN 超 16 B 均拒绝，且不改写既有输出。
    CHECK(!buildDeviceName("P2", "", name));
    CHECK(std::string_view{name.data()} == "RTK-0000");
    CHECK(!buildDeviceName("", "B5F2", name));
    CHECK(!buildDeviceName("P2\n", "B5F2", name));
    CHECK(!buildDeviceName("P2", "B5\rF2", name));
    CHECK(!buildDeviceName("P2", "12345678901234567", name));

    // 恰好 31 B 成功；32 B 及产品名本身超限返回 false 且首字节清零。
    CHECK(buildDeviceName("PRODUCT_14CHAR", "1234567890123456", name));
    CHECK(std::string_view{name.data()} == "PRODUCT_14CHAR-1234567890123456");
    CHECK(!buildDeviceName("PRODUCT_15CHARS", "1234567890123456", name));
    CHECK(name[0] == '\0');
    name[0] = 'X';
    CHECK(!buildDeviceName("1234567890123456789012345678901", "1", name));
    CHECK(name[0] == '\0');

    std::array<char, NVS_STORE_SN_MAX + 1U> sn{};
    const std::array<uint8_t, core::kAddrLen> mac{0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xB5U, 0xF2U};
    CHECK(deriveSnFromMac(mac, sn));
    CHECK(std::string_view{sn.data()} == "B5F2");
}

/**
 * @brief 验证 SystemModule::prepare() 的 P2/E1/unknown 命名、MAC 派生、失败回退与超长拒绝
 * @note 单线程主机测试，非 ISR。
 */
void checkSystemPrepare()
{
    using namespace app;
    auto &ctx = appContext();
    ctx.data.init();

    // P2 + 无 NVS SN：取 BT MAC 尾两字节派生 B5F2 → P2-B5F2。
    s_spp_init_calls = 0U;
    s_spp_init_result = ESP_OK;
    s_bt_mac = {0x10U, 0x20U, 0x30U, 0x40U, 0xB5U, 0xF2U};
    s_bt_mac_result = ESP_OK;
    ctx.boardProfile = board::byId(board::BoardId::kP2V141);
    ctx.data.systemStatus.write(SystemStatus{});
    ctx.system.prepare();
    ctx.system.startBt(); // W1：BT 模式下由启动流程在 prepare() 之后调用。
    auto st = ctx.data.systemStatus.read();
    CHECK(st.btReady && s_spp_init_calls == 1U);
    CHECK(std::string_view{st.sn.data()} == "B5F2");
    CHECK(std::string_view{st.deviceName.data()} == "P2-B5F2");
    CHECK(std::string_view{s_spp_init_name.data()} == "P2-B5F2");

    // E1 + 已有 NVS SN：直接使用 SN → E1-9988。
    SystemStatus preset{};
    static_cast<void>(std::snprintf(preset.sn.data(), preset.sn.size(), "%s", "9988"));
    ctx.boardProfile = board::byId(board::BoardId::kE1DevKit);
    ctx.data.systemStatus.write(preset);
    ctx.system.prepare();
    ctx.system.startBt(); // W1：BT 模式下由启动流程在 prepare() 之后调用。
    st = ctx.data.systemStatus.read();
    CHECK(st.btReady && s_spp_init_calls == 2U);
    CHECK(std::string_view{st.deviceName.data()} == "E1-9988");
    CHECK(std::string_view{s_spp_init_name.data()} == "E1-9988");

    // unknown 板型 + MAC 读取失败：SN 回退 0000，产品回退 RTK → RTK-0000。
    s_bt_mac_result = ESP_FAIL;
    ctx.boardProfile = nullptr;
    ctx.data.systemStatus.write(SystemStatus{});
    ctx.system.prepare();
    ctx.system.startBt(); // W1：BT 模式下由启动流程在 prepare() 之后调用。
    st = ctx.data.systemStatus.read();
    CHECK(st.btReady && s_spp_init_calls == 3U);
    CHECK(std::string_view{st.sn.data()} == "0000");
    CHECK(std::string_view{st.deviceName.data()} == "RTK-0000");
    CHECK(std::string_view{s_spp_init_name.data()} == "RTK-0000");

    // 总长超过 kDeviceNameMax：拒绝启动 SPP 且 btReady 保持 false。
    constexpr board::Profile kLongProduct{
        .id = board::BoardId::kUnknown, .name = "long", .product = "PRODUCT_15CHARS"};
    SystemStatus longSn{};
    static_cast<void>(std::snprintf(longSn.sn.data(), longSn.sn.size(), "%s", "1234567890123456"));
    ctx.boardProfile = &kLongProduct;
    ctx.data.systemStatus.write(longSn);
    ctx.system.prepare();
    ctx.system.startBt(); // W1：BT 模式下由启动流程在 prepare() 之后调用。
    st = ctx.data.systemStatus.read();
    CHECK(!st.btReady && s_spp_init_calls == 3U);
    CHECK(st.deviceName[0] == '\0');
}

/**
 * @brief 验证 BtLinkModule::init() 将指定板型的产品名注入 0x0001 设备信息应答的 product 字段
 * @param profile: 待注入的板型配置指针，nullptr 表示 unknown
 * @param expected: 期望在 0x0001 应答偏移 0 处读到的产品短名
 * @param hardwareVersion: 期望在偏移 24 处读到的硬件版本
 * @note 单线程主机测试，每进程调用一次，非 ISR。
 */
void checkDevInfoProduct(const app::board::Profile *profile, std::string_view expected,
                         std::string_view hardwareVersion)
{
    using namespace app;
    s_write_calls = 0U;
    s_input_size.fill(0U);
    s_input_offset.fill(0U);
    s_event_size = 0U;
    s_event_offset = 0U;
    s_tx_used.fill(0U);
    s_tx_reserved.fill(false);
    appContext().boardProfile = profile;
    setup();
    open(1U, kPhone);
    append(0U, 0x0001U, 0U, proto::msg_type::kQuery);
    dirty(0U);
    uint32_t tick = s_clock;
    link_round(tick);
    CHECK(s_write_calls == 1U);
    if (s_write_calls == 1U) {
        proto::FrameParser parser{};
        proto::FrameView reply{};
        bool gotFrame = false;
        for (size_t i = 0U; i < s_write_sizes[0]; ++i) {
            if (parser.feed(s_write_bytes[0][i], reply) == proto::ParseResult::kFrame) {
                gotFrame = true;
            }
        }
        CHECK(gotFrame && reply.msgId == 0x0001U && reply.payload.size() == core::kDevInfoLen);
        if (gotFrame && reply.payload.size() == core::kDevInfoLen) {
            std::array<char, 8> raw{};
            std::memcpy(raw.data(), reply.payload.data(), raw.size());
            const size_t len = strnlen(raw.data(), raw.size());
            const std::string_view actual{raw.data(), len};
            CHECK(actual == expected);
            constexpr size_t kHardwareVersionOffset = 24U; // 0x01 设备信息布局
            std::memcpy(raw.data(), reply.payload.data() + kHardwareVersionOffset, raw.size());
            CHECK(std::string_view(raw.data(), strnlen(raw.data(), raw.size())) == hardwareVersion);
        }
    }
}

/**
 * @brief 汇总执行设备名与 P2 产品名注入相关主机测试
 * @note 单线程主机测试，非 ISR。
 */
void deviceName()
{
    checkDeviceNamePure();
    checkSystemPrepare();
    checkDevInfoProduct(app::board::byId(app::board::BoardId::kP2V141), "P2", "V1.41");
}

} // namespace

/** @brief 读取主机模拟的 GPIO。 @param gpio: 板表引脚。 @retval 采样电平。 */
int gpio_get_level(gpio_num_t gpio)
{
    return s_gpioLevels.at(static_cast<size_t>(gpio));
}

/** @brief 记录 GPIO4 模拟按压。 @param gpio: 板表引脚。 @param level: 目标电平。 @retval ESP_OK 成功。 */
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    s_gpioLevels.at(static_cast<size_t>(gpio)) = level != 0U ? 1 : 0;
    if (gpio == 4 && level != 0U) {
        ++s_gpio4Presses;
    }
    return ESP_OK;
}

/**
 * @brief 模拟输入配置，允许注入 GPIO27 不可用
 * @param config: 待配置的 GPIO 与模式
 * @retval ESP_OK 成功
 * @retval ESP_FAIL 注入 GPIO27 配置失败
 */
esp_err_t gpio_config(const gpio_config_t *config)
{
    if (config->pin_bit_mask == (1ULL << 27U)) {
        CHECK(config->mode == GPIO_MODE_INPUT && config->pull_up_en == GPIO_PULLUP_DISABLE &&
              config->pull_down_en == GPIO_PULLDOWN_DISABLE);
        if (s_usbGpioConfigFails) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

namespace app::board {

/** @brief 模拟外设回到安全电平。 @retval kOk 成功。 */
fw::Status setSafeOutputLevels(const Profile &) noexcept
{
    s_gpioLevels[4] = 0;
    return s_safeOutputsStatus;
}

/** @brief 本用例可启用虚拟 ADC。 @param gpio: 板级 ADC 输入。 @retval kOk 已启用。 @retval kNotFound 无模拟传感器。 */
fw::Status initBatteryAdc(int gpio) noexcept
{
    CHECK(gpio == 34);
    return s_batteryMv == 0U ? fw::Status::kNotFound : fw::Status::kOk;
}

/** @brief 获取模拟电池电压。 @param[out] mv: 成功时写入电池毫伏。 @retval kOk 成功。 @retval kIoError 注入读失败。 */
fw::Status readBatteryAdcMv(uint32_t &mv) noexcept
{
    if (s_batteryReadFails) {
        return fw::Status::kIoError;
    }
    mv = s_batteryMv;
    return fw::Status::kOk;
}

namespace charger {

/** @brief 模拟充电芯片就绪状态。 @retval true 已就绪。 @retval false 注入离线。 */
bool isReady() noexcept
{
    return s_chargerReady;
}

/** @brief 提供 USB 在位的充电芯片样本。 @param[out] last: 成功样本。 */
fw::Status refreshInputStatus(ChargerInputStatus &last) noexcept
{
    ++s_chargerReads;
    if (s_chargerBusy) {
        return fw::Status::kBusy;
    }
    last = decodeSgmReg08(s_chargerReg);
    return fw::Status::kOk;
}

} // namespace charger
} // namespace app::board

int main(int argc, const char **raw_args)
{
    const std::span<const char *> argv{raw_args,
                                       static_cast<size_t>(argc)}; // argc>=0，转换不丢失。
    if (argc != 2) {
        return 2;
    }
    if (std::strcmp(argv[1], "policy_failure") == 0) {
        app::note_bt_failure();
        app::StatusSnapshot snap{};
        app::read_snapshot(snap);
#if defined(CONFIG_RTK_LINK_POLICY_PHONE_PREEMPT)
        CHECK(snap.policy == core::Policy::kPhonePreempt);
#else
        CHECK(snap.policy == core::Policy::kConcurrent);
#endif
        CHECK(!snap.btReady && !snap.linkRunning);
    } else if (std::strcmp(argv[1], "debug_cat_names") == 0) {
        // 控制台习惯输小写，表里是大写短名：两种写法都必须解析（上板发现小写被拒）。
        fw::TraceCat cat = fw::TraceCat::kLink;
        CHECK(app::debug::parseCat("gnss", cat) && cat == fw::TraceCat::kGnss);
        CHECK(app::debug::parseCat("GNSS", cat) && cat == fw::TraceCat::kGnss);
        CHECK(app::debug::parseCat("Spp", cat) && cat == fw::TraceCat::kSpp);
        CHECK(!app::debug::parseCat("gns", cat) && !app::debug::parseCat("gnssx", cat));
    } else if (std::strcmp(argv[1], "save_recovery") == 0) {
        saveRecovery();
    } else if (std::strcmp(argv[1], "storage_backoff") == 0) {
        storageBackoff();
    } else if (std::strcmp(argv[1], "power_save_backoff") == 0) {
        powerSaveBackoff();
    } else if (std::strcmp(argv[1], "power_standby") == 0) {
        powerStandby();
    } else if (std::strcmp(argv[1], "power_short_key_ui") == 0) {
        powerShortKeyUi();
    } else if (std::strcmp(argv[1], "power_long_key_no_oled") == 0) {
        powerLongKeyNoOled();
    } else if (std::strcmp(argv[1], "power_force_off_fallback") == 0) {
        powerForceOffFallback();
    } else if (std::strcmp(argv[1], "power_oled_offline_direct_shutdown") == 0) {
        powerOledOfflineDirectShutdown();
    } else if (std::strcmp(argv[1], "power_usb_change_wakes_ui") == 0) {
        powerUsbChangeWakesUi();
    } else if (std::strcmp(argv[1], "power_oled_usb_final_flush") == 0) {
        powerOledUsbFinalFlush();
    } else if (std::strcmp(argv[1], "power_oled_battery_final_flush") == 0) {
        powerBatteryDisplayShutdown(true);
    } else if (std::strcmp(argv[1], "power_oled_battery_final_timeout") == 0) {
        powerBatteryDisplayShutdown(false);
    } else if (std::strcmp(argv[1], "power_oled_usb_final_timeout") == 0) {
        powerOledUsbFinalTimeout();
    } else if (std::strcmp(argv[1], "power_oled_usb_absent_no_wait") == 0) {
        powerOledUsbAbsentNoWait();
    } else if (std::strcmp(argv[1], "power_oled_usb_already_flushed") == 0) {
        powerOledUsbAlreadyFlushed();
    } else if (std::strcmp(argv[1], "power_oled_usb_log_rebusy") == 0) {
        powerOledUsbLogRebusy();
    } else if (std::strcmp(argv[1], "power_oled_usb_wake_on_flush") == 0) {
        powerOledUsbNonalignedWake(true);
    } else if (std::strcmp(argv[1], "power_oled_usb_wake_on_deadline") == 0) {
        powerOledUsbNonalignedWake(false);
    } else if (std::strcmp(argv[1], "power_oled_usb_preempt_before_deadline") == 0) {
        powerOledUsbPreemptedWait(5U);
    } else if (std::strcmp(argv[1], "power_oled_usb_preempt_across_deadline") == 0) {
        powerOledUsbPreemptedWait(31U);
    } else if (std::strcmp(argv[1], "power_shutdown_degraded_masks") == 0) {
        powerShutdownDegradedMasks();
    } else if (std::strcmp(argv[1], "power_participant_blocks_and_timeout_names") == 0) {
        powerParticipantBlocksAndTimeoutNames();
    } else if (std::strcmp(argv[1], "power_oled_battery_to_usb_final_frame") == 0) {
        powerOledBatteryToUsbFinalFrame();
    } else if (std::strcmp(argv[1], "power_sim_battery_low_shutdown") == 0) {
        powerSimBatteryLowShutdown();
    } else if (std::strcmp(argv[1], "power_oled_usb_final_frame_stale_clock") == 0) {
        powerOledUsbFinalFrameStaleClock();
    } else if (std::strcmp(argv[1], "power_oled_battery_final_frame_stale_clock") == 0) {
        powerOledBatteryFinalFrameStaleClock();
    } else if (std::strcmp(argv[1], "power_participant_four_slot_names") == 0) {
        powerParticipantFourSlotNames();
    } else if (std::strcmp(argv[1], "power_sim_battery_boundary") == 0) {
        powerSimBatteryBoundary();
    } else if (std::strcmp(argv[1], "power_low_battery_real_zero_mv") == 0) {
        powerLowBatteryRealZeroMv();
    } else if (std::strcmp(argv[1], "power_sim_battery_reinject") == 0) {
        powerSimBatteryReinject();
    } else if (std::strcmp(argv[1], "power_log_drain") == 0) {
        powerLogDrain();
    } else if (std::strcmp(argv[1], "power_low_battery") == 0) {
        powerLowBattery();
    } else if (std::strcmp(argv[1], "power_battery_read_failure") == 0) {
        powerBatteryReadFailure();
    } else if (std::strcmp(argv[1], "power_low_battery_unknown_usb") == 0) {
        powerLowBatteryInhibited(app::board::BootMode::kNormal);
    } else if (std::strcmp(argv[1], "power_low_battery_standby") == 0) {
        powerLowBatteryInhibited(app::board::BootMode::kChargeStandby);
    } else if (std::strcmp(argv[1], "power_battery_median") == 0) {
        powerBatteryMedian();
    } else if (std::strcmp(argv[1], "power_battery_freshness") == 0) {
        powerBatteryFreshness();
    } else if (std::strcmp(argv[1], "power_busy_at_cut") == 0) {
        powerBusyAtCut();
    } else if (std::strcmp(argv[1], "power_timeout_recovery") == 0) {
        powerTimeoutRecovery();
    } else if (std::strcmp(argv[1], "power_phone_whitelist_closed") == 0) {
        powerPhoneWhitelist(false);
    } else if (std::strcmp(argv[1], "power_phone_whitelist_admitted") == 0) {
        powerPhoneWhitelist(true);
    } else if (std::strcmp(argv[1], "power_bt_pause") == 0) {
        powerBtPause();
    } else if (std::strcmp(argv[1], "power_no_reopen") == 0) {
        powerNoReopen(false);
    } else if (std::strcmp(argv[1], "power_fence_timeout") == 0) {
        powerNoReopen(true);
    } else if (std::strcmp(argv[1], "storage_startup") == 0) {
        storageStartup();
    } else if (std::strcmp(argv[1], "phone_nvs_readback") == 0) {
        phoneNvsReadback();
    } else if (std::strcmp(argv[1], "early_config_0") == 0) {
        earlyConfig(0U, true);
    } else if (std::strcmp(argv[1], "early_config_2") == 0) {
        earlyConfig(2U, true);
    } else if (std::strcmp(argv[1], "early_config_3") == 0) {
        earlyConfig(3U, true);
    } else if (std::strcmp(argv[1], "early_config_nvs_failure") == 0) {
        earlyConfig(0U, false);
    } else if (std::strcmp(argv[1], "factory_reset") == 0) {
        factoryReset();
    } else if (std::strcmp(argv[1], "pairing_window") == 0) {
        pairingWindow();
    } else if (std::strcmp(argv[1], "web_s2_storage_bt") == 0) {
        webS2StorageBt();
    } else if (std::strcmp(argv[1], "device_name") == 0) {
        deviceName();
    } else if (std::strcmp(argv[1], "device_name_e1") == 0) {
        checkDevInfoProduct(app::board::byId(app::board::BoardId::kE1DevKit), "E1", "V1.1.0");
    } else if (std::strcmp(argv[1], "device_name_unknown") == 0) {
        checkDevInfoProduct(nullptr, "RTK", "UNKNOWN");
    } else if (std::strcmp(argv[1], "credit_window") == 0) {
        creditWindow();
    } else if (std::strcmp(argv[1], "rx_fairness") == 0) {
        rxFairness();
    } else if (std::strcmp(argv[1], "rx_fast_partial") == 0) {
        rxFastPartial();
    } else if (std::strcmp(argv[1], "rx_tick_budget") == 0) {
        rxTickBudget();
    } else if (std::strcmp(argv[1], "rx_wake_level_recheck") == 0) {
        rxWakeLevelRecheck();
    } else if (std::strcmp(argv[1], "rx_512_partial_and_pause_isolation") == 0) {
        rx512PartialAndPauseIsolation();
    } else if (std::strcmp(argv[1], "rtcm_pipe_direct_and_io_stats") == 0) {
        rtcmPipeDirectAndIoStats();
    } else if (std::strcmp(argv[1], "rtcm_mix") == 0) {
        rtcmMix();
    } else if (std::strcmp(argv[1], "rtcm_roles") == 0) {
        rtcmRoles();
    } else if (std::strcmp(argv[1], "rtcm_replay_budget") == 0) {
        rtcmReplayBudget();
    } else if (std::strcmp(argv[1], "rtcm_reset_lifecycle") == 0) {
        rtcmResetLifecycle();
    } else if (std::strcmp(argv[1], "rtcm_byte_budget") == 0) {
        rtcmByteBudget();
    } else if (std::strcmp(argv[1], "lifecycle_application") == 0) {
        lifecycleApplication();
    } else {
        return 2;
    }
    std::fputs(s_failures ? "FAIL " : "PASS ", stdout);
    std::puts(argv[1]);
    return s_failures ? 1 : 0;
}
