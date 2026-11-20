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

extern "C" {
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
}

#include "AppTestAdapter.hpp"
#include "board/BoardProfile.hpp"
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

// 测试向量、故障注入时刻与协议原始数值直接对照断言；不用于产品逻辑。
namespace {
uint32_t s_clock = 100U;
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
unsigned s_restarts = 0U;
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
    return s_clock;
}
fw::Status fromEspErr(esp_err_t err) noexcept
{
    return (err == ESP_OK) ? fw::Status::kOk : fw::Status::kTimeout;
}
} // namespace rtos

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
esp_err_t spp_server_disconnect(uint32_t)
{
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
    CHECK(storage.setPhoneSnPn(snPn, s_clock));
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
 * @note 单线程主机测试，每进程调用一次，非 ISR。
 */
void checkDevInfoProduct(const app::board::Profile *profile, std::string_view expected)
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
    checkDevInfoProduct(app::board::byId(app::board::BoardId::kP2V141), "P2");
}

} // namespace

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
        checkDevInfoProduct(app::board::byId(app::board::BoardId::kE1DevKit), "E1");
    } else if (std::strcmp(argv[1], "device_name_unknown") == 0) {
        checkDevInfoProduct(nullptr, "RTK");
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
