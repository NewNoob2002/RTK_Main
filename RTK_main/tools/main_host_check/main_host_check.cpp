// 真实组合根、LinkService/解析器与 SppTransport 的 host 回归；仅替换 IDF/存储/字节传输边界。
// 虚拟时钟精确注入 200 ms 超时，不以 wall clock 调度噪声代替产品时间预算证据。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
}

#include "app_test_adapter.hpp"
#include "modules/debug/trace_text.hpp"

namespace {
uint32_t s_clock = 100U;
uint32_t s_write_delay = 0U;
size_t s_write_calls = 0U;
std::array<uint32_t, 64> s_write_handles{};
std::array<uint32_t, 64> s_write_timeouts{};
std::array<std::array<uint8_t, proto::kMaxFrame>, 64> s_write_bytes{};
std::array<size_t, 64> s_write_sizes{};
std::array<size_t, 64> s_store_at_write{};
size_t s_store_calls = 0U;
esp_err_t s_store_result = ESP_OK;
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
int s_failures = 0;
constexpr core::BtAddr kPhone{{1, 2, 3, 4, 5, 6}};
constexpr core::BtAddr kAlarm{{7, 8, 9, 10, 11, 12}};
constexpr core::BtAddr kOtherAlarm{{13, 14, 15, 16, 17, 18}};
#define CHECK(value)                                                                                                   \
    do {                                                                                                               \
        if (!(value)) {                                                                                                \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #value);                                       \
            ++s_failures;                                                                                              \
        }                                                                                                              \
    } while (false)

void setup(std::span<const core::BtAddr> whitelist = {}, core::Policy policy = core::Policy::kConcurrent)
{
    using namespace app;
    app_context().data.init();
    LinkStatus link{};
    link.policy = policy;
    link.whitelist_count = whitelist.size();
    std::copy(whitelist.begin(), whitelist.end(), link.whitelist.begin());
    app_context().data.link_status.write(link);
    SystemStatus system{};
    system.bt_ready = true;
    app_context().data.system_status.write(system);
    CHECK(app_context().bt_link.init() == fw::Result::kOk);
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
    app::handle_event(evt, s_clock);
}

void append(size_t session, uint16_t msg, uint8_t sender, uint8_t type, std::span<const uint8_t> payload = {})
{
    size_t count = 0U;
    auto &bytes = s_input[session];
    CHECK(proto::build_frame(
              std::span<uint8_t>(bytes.data() + s_input_size[session], bytes.size() - s_input_size[session]), msg,
              sender, type, payload, count) == fw::Status::kOk);
    s_input_size[session] += count;
}

void dirty(size_t session)
{
    spp_server_evt_t evt{};
    evt.type = SPP_SERVER_EVT_DATA;
    evt.handle = session + 1U;
    app::handle_event(evt, s_clock);
}

void save_recovery()
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
    CHECK(s_snapshot.whitelist_pending && s_snapshot.whitelist_error == ESP_OK);
    s_clock += 499U;
    persist_whitelist(s_clock);
    CHECK(s_store_calls == 0U);
    ++s_clock;
    persist_whitelist(s_clock);
    publish_snapshot(s_clock);
    CHECK(s_store_calls == 1U && s_snapshot.whitelist_error == ESP_FAIL);
    s_store_result = ESP_OK;
    s_clock = s_whitelist_attempt_ms + 999U;
    persist_whitelist(s_clock);
    CHECK(s_store_calls == 1U && s_whitelist_pending);
    ++s_clock;
    persist_whitelist(s_clock);
    CHECK(s_store_calls == 2U && !s_whitelist_pending && s_saved_count == 0U);

    // 配对升级本身触发 dirty，第一次落盘失败仍保留，重试不需要新 dirty。
    s_link_manager.pair_open(s_clock);
    open(2U, kAlarm);
    const std::array<uint8_t, 8> hello{1, 0, 80, 0, 0, 0, 0, 0};
    append(1U, core::kHelloMsgId, proto::sender::kAlarm, proto::msg_type::kPush, hello);
    dirty(1U);
    drain_rx(s_clock);
    collect_whitelist_dirty();
    CHECK(s_link_manager.whitelist().size() == 1U && s_whitelist_pending);
    s_store_result = ESP_FAIL;
    s_clock += 500U;
    persist_whitelist(s_clock);
    const size_t before = s_store_calls;
    CHECK(s_whitelist_pending);
    s_clock = s_whitelist_attempt_ms + 999U;
    persist_whitelist(s_clock);
    CHECK(s_store_calls == before);
    s_store_result = ESP_OK;
    ++s_clock;
    persist_whitelist(s_clock);
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
    persist_whitelist(s_clock);
    CHECK(!s_whitelist_pending && s_saved_count == 0U);
    publish_snapshot(s_clock);
    CHECK(!s_snapshot.whitelist_pending && s_snapshot.whitelist_error == ESP_OK);
    CHECK(s_store_at_write[0] == 0U); // 0x44 接受 ACK 未等待落盘。

    // 已有 pending 的冷却比较容忍 32 位毫秒回绕。
    s_whitelist_pending = true;
    s_store_result = ESP_FAIL;
    s_clock = UINT32_MAX - 500U;
    persist_whitelist(s_clock, true);
    CHECK(s_whitelist_pending);
    s_store_result = ESP_OK;
    s_clock += kWhitelistRetryMs;
    persist_whitelist(s_clock);
    CHECK(!s_whitelist_pending && s_saved_count == 0U);
}

void storage_backoff()
{
    using namespace app;
    setup();
    const std::array whitelist{kAlarm};
    mark_whitelist_dirty(whitelist, s_clock);
    s_store_result = ESP_FAIL;
    s_clock += 499U;
    persist_whitelist(s_clock);
    CHECK(s_store_calls == 0U);
    ++s_clock;
    persist_whitelist(s_clock);
    CHECK(s_store_calls == 1U);
    const std::array<uint32_t, 7> delays{1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (const auto delay : delays) {
        CHECK(app_context().data.store_status.read().retry_ms == delay);
        const size_t attempts = s_store_calls;
        s_clock = s_whitelist_attempt_ms + 5U;
        mark_whitelist_dirty({}, s_clock); // 重试仍保存最新 RAM 值，变化不缩短退避。
        s_clock = s_whitelist_attempt_ms + delay - 1U;
        persist_whitelist(s_clock);
        CHECK(s_store_calls == attempts);
        ++s_clock;
        persist_whitelist(s_clock);
        CHECK(s_store_calls == attempts + 1U && s_whitelist_pending);
    }
    s_store_result = ESP_OK;
    s_clock += 30000U;
    persist_whitelist(s_clock);
    CHECK(!s_whitelist_pending && s_saved_count == 0U);
    CHECK(app_context().data.store_status.read().failures == 8U &&
          app_context().data.store_status.read().retry_ms == 0U);
}

void rx_fairness()
{
    using namespace app;
    const std::array whitelist{kAlarm};
    setup(whitelist);
    open(1U, kPhone);
    open(2U, kAlarm);
    for (size_t idx = 0U; idx < 10U; ++idx) {
        append(0U, 1U, 0U, proto::msg_type::kQuery);
    }
    const std::array<uint8_t, 8> heartbeat{5, 0, 80, 0, 0, 0, 0, 0};
    append(1U, proto::alarm::kMsgHeartbeat, proto::sender::kAlarm, proto::msg_type::kPush, heartbeat);
    dirty(0U);
    dirty(1U);
    s_write_delay = 200U;
    uint32_t tick = s_clock;
    const uint32_t start = s_clock;
    link_round(tick);
    CHECK(s_write_calls == 1U && s_clock - start == 200U);
    CHECK(s_sessions[0].drain_pending && s_sessions[0].rx_offset < s_sessions[0].rx_size);
    Command cmd{};
    cmd.id = CommandId::kAlarmSet;
    cmd.state = 1U;
    cmd.level = 2U;
    CHECK(s_command_queue.send(cmd, 0U));
    link_round(tick); // tick 优先，ALARM 推送耗尽预算，因此 RX 留在下一轮。
    CHECK(s_alarm.is_override());
    CHECK(s_write_calls == 2U && s_write_handles[1] == 2U);
    link_round(tick);
    CHECK(s_service->last_heartbeat().has_value());
    if (s_service->last_heartbeat().has_value()) {
        CHECK(s_service->last_heartbeat()->seq == 5U);
    }
    CHECK(s_service->stats().frames_rx == 2U);
    for (size_t idx = 0U; idx < 16U && s_service->stats().frames_rx < 11U; ++idx) {
        const uint32_t before = s_clock;
        const uint32_t frames = s_service->stats().frames_rx;
        link_round(tick);
        CHECK(s_clock - before <= 200U);
        CHECK(s_service->stats().frames_rx <= frames + 1U);
    }
    CHECK(s_service->stats().frames_rx == 11U);
    CHECK(s_write_timeouts[0] == core::kWriteTimeoutMs);
}

void rx_fast_partial()
{
    using namespace app;
    setup();
    open(1U, kPhone);
    for (size_t idx = 0U; idx < 10U; ++idx) {
        append(0U, 1U, 0U, proto::msg_type::kQuery);
    }
    dirty(0U);
    uint32_t tick = s_clock;
    for (size_t idx = 0U; idx < 10U; ++idx) {
        link_round(tick);
    }
    CHECK(s_write_calls == 10U && s_poll_wait == 0U);
    CHECK(s_service->stats().frames_rx == 10U);
    link_round(tick); // 非空标志直到 read=0 才清除。
    CHECK(!s_sessions[0].drain_pending);

    append(0U, 1U, 0U, proto::msg_type::kQuery);
    const size_t full = s_input_size[0];
    s_input_size[0] -= 5U;
    dirty(0U);
    link_round(tick);
    CHECK(s_service->stats().frames_rx == 10U);
    s_input_size[0] = full;
    dirty(0U);
    link_round(tick);
    CHECK(s_service->stats().frames_rx == 11U);

    append(0U, 1U, 0U, proto::msg_type::kQuery);
    dirty(0U);
    s_read_delay = 20U; // 完成一次非阻塞读前发生抢占，整轮预算已耗尽。
    const uint32_t frames = s_service->stats().frames_rx;
    link_round(tick);
    CHECK(s_service->stats().frames_rx == frames);
    CHECK(s_sessions[0].rx_offset == 0U && s_sessions[0].rx_size > 0U);
    s_read_delay = 0U;
    if (s_service->stats().frames_rx == frames) {
        link_round(tick);
    }
    CHECK(s_service->stats().frames_rx == frames + 1U);
}

void rx_tick_budget()
{
    using namespace app;
    const std::array whitelist{kAlarm, kOtherAlarm};
    setup(whitelist);
    open(1U, kAlarm);
    open(2U, kOtherAlarm);
    const std::array<uint8_t, 8> heartbeat{5, 0, 80, 0, 0, 0, 0, 0};
    append(0U, proto::alarm::kMsgHeartbeat, proto::sender::kAlarm, proto::msg_type::kPush, heartbeat);
    dirty(0U);
    s_write_delay = 200U;
    uint32_t tick = 0U;
    const uint32_t before = s_clock;
    link_round(tick);
    CHECK(s_write_calls == 2U && s_clock - before == 400U);
    CHECK(s_service->stats().frames_rx == 0U && s_sessions[0].drain_pending);
    CHECK(s_write_timeouts[0] == 200U && s_write_timeouts[1] == 200U);
}

// 自测 oracle 用逐位 CRC，独立于产品的 CRC 查表实现。
uint32_t crc24_reference(std::span<const uint8_t> wire)
{
    uint32_t crc = 0;
    for (uint8_t byte : wire) {
        crc ^= static_cast<uint32_t>(byte) << 16U; // uint8_t 扩展到 uint32_t 无损。
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc << 1U) ^ ((crc & 0x800000U) != 0U ? 0x1864CFBU : 0U);
        }
    }
    return crc & 0xFFFFFFU;
}

uint32_t crc32_reference(std::span<const uint8_t> wire, uint32_t initial = 0)
{
    uint32_t crc = initial ^ UINT32_MAX;
    for (uint8_t byte : wire) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ UINT32_MAX;
}

std::span<const uint8_t> append_rtcm(size_t session, uint16_t message, size_t payload_size = 8)
{
    CHECK(payload_size >= 2 && payload_size <= 1023);
    auto wire = std::span{s_input[session]}.subspan(s_input_size[session], payload_size + 6);
    wire[0] = 0xD3;
    // 载荷长度 <= 1023，上半部分 <= 3，掩码后的下半部分 <= 255，收窄安全。
    wire[1] = static_cast<uint8_t>(payload_size >> 8U);
    wire[2] = static_cast<uint8_t>(payload_size & 0xFFU);
    std::fill(wire.begin() + 3, wire.end() - 3, 0x11);
    // 测试消息号限制为 12 bit，上 8 位及掩码后的低半字节收窄安全。
    CHECK(message <= 4095);
    wire[3] = static_cast<uint8_t>(message >> 4U);
    wire[4] = static_cast<uint8_t>((message & 0xFU) << 4U);
    const uint32_t crc = crc24_reference(wire.first(payload_size + 3));
    // CRC-24Q 的三个字节逐一取低 8 bit，均在 uint8_t 范围内。
    wire[payload_size + 3] = static_cast<uint8_t>((crc >> 16U) & 0xFFU);
    wire[payload_size + 4] = static_cast<uint8_t>((crc >> 8U) & 0xFFU);
    wire[payload_size + 5] = static_cast<uint8_t>(crc & 0xFFU);
    s_input_size[session] += wire.size();
    return wire;
}

void append_bytes(size_t session, std::span<const uint8_t> bytes)
{
    CHECK(bytes.size() <= s_input[session].size() - s_input_size[session]);
    std::memcpy(std::span{s_input[session]}.subspan(s_input_size[session]).data(), bytes.data(), bytes.size());
    s_input_size[session] += bytes.size();
}

void check_reply(size_t index, uint16_t message)
{
    CHECK(index < s_write_calls);
    proto::FrameParser parser;
    proto::FrameView frame;
    size_t frames = 0;
    for (uint8_t byte : std::span{s_write_bytes[index]}.first(s_write_sizes[index])) {
        if (parser.feed(byte, frame) == proto::ParseResult::kFrame) {
            ++frames;
            CHECK(frame.msg_id == message);
            CHECK(frame.sender == proto::sender::kDevice && frame.type == proto::msg_type::kQueryRes);
        }
    }
    CHECK(frames == 1 && parser.crc_errors() == 0 && parser.length_errors() == 0);
    CHECK(s_write_handles[index] == 1);
}

void rtcm_mix()
{
    using namespace app;
    setup({}, policy_from_config());
    open(1, kPhone);
    append(0, 1, 0, proto::msg_type::kQuery);
    const auto first = append_rtcm(0, 1005);
    const auto second = append_rtcm(0, 1077, 11);
    const uint32_t expected_crc = crc32_reference(second, crc32_reference(first));
    constexpr std::array<uint8_t, 4> kGarbage{0, 0x7F, 0xFF, 0x55};
    append_bytes(0, kGarbage);
    append(0, 2, 0, proto::msg_type::kQuery);
    event(SPP_SERVER_EVT_DATA, 1);
    uint32_t tick = s_clock;
    link_round(tick);
    CHECK(s_write_calls == 1 && s_service->stats().frames_rx == 1);
    CHECK(s_snapshot.rtcm_frames == 0);
    CHECK(s_sessions[0].rx_offset < s_sessions[0].rx_size);
    link_round(tick);
    CHECK(s_write_calls == 2 && s_service->stats().frames_rx == 2);
    check_reply(0, 1);
    check_reply(1, 2);
    CHECK(s_snapshot.rtcm_frames == 2 && s_snapshot.rtcm_bytes == first.size() + second.size());
    CHECK(s_snapshot.rtcm_crc32 == expected_crc && s_snapshot.rtcm_last_msg == 1077);
    CHECK(s_snapshot.rtcm_rejected_role == 0 && s_snapshot.demux_unknown_bytes == kGarbage.size());
    CHECK(s_snapshot.parser_crc_errors == 0 && s_snapshot.parser_length_errors == 0);
    CHECK(s_snapshot.rx_dropped == 0 && s_poll_wait == 0);
}

void rtcm_roles()
{
    using namespace app;
    const std::array whitelist{kAlarm};
    setup(whitelist, policy_from_config());
    open(1, kAlarm);
    CHECK(s_link_manager.role(1) == core::Role::kAlarm);
    append_rtcm(0, 1005);
    dirty(0);
    drain_rx(s_clock);
    CHECK(s_rtcm.stats().rejected_role == 1 && s_rtcm.stats().frames == 0);
    spp_server_evt_t close{};
    close.type = SPP_SERVER_EVT_CLOSE;
    close.handle = 1;
    handle_event(close, s_clock);
    open(1, kPhone);
    s_link_manager.pair_open(s_clock);
    open(2, kOtherAlarm);
    CHECK(s_link_manager.role(2) == core::Role::kPending);
    append_rtcm(1, 1077);
    dirty(1);
    drain_rx(s_clock);
    CHECK(s_rtcm.stats().rejected_role == 2 && s_rtcm.stats().frames == 0);
    // 主动断开到物理 CLOSE 之间角色已经是 NONE，也不能接受 RTCM。
    s_service->on_close(2);
    append_rtcm(1, 1077);
    dirty(1);
    drain_rx(s_clock);
    CHECK(s_rtcm.stats().rejected_role == 3);
    publish_snapshot(s_clock);
    CHECK(s_snapshot.rtcm_frames == 0 && s_snapshot.rtcm_bytes == 0 && s_snapshot.rtcm_crc32 == 0);
    CHECK(s_snapshot.rtcm_last_msg == 0 && s_snapshot.rtcm_rejected_role == 3 && s_write_calls == 0);
}

void rtcm_replay_budget()
{
    using namespace app;
    setup({}, policy_from_config());
    open(1, kPhone);
    constexpr std::array<uint8_t, 3> kHeader{0xD3, 0, 0};
    append_bytes(0, kHeader);
    append(0, 1, 0, proto::msg_type::kQuery);
    const auto rtcm = append_rtcm(0, 1005);
    const uint32_t expected_crc = crc32_reference(rtcm);
    append(0, 2, 0, proto::msg_type::kQuery);
    // 外层假 RTCM 把两条产品帧暂时认作载荷；长度为 62，转换到 uint8_t 无损。
    CHECK(s_input_size[0] - 3 < 256);
    s_input[0][2] = static_cast<uint8_t>(s_input_size[0] - 3);
    CHECK(crc24_reference(std::span{s_input[0]}.first(s_input_size[0])) != 0);
    constexpr std::array<uint8_t, 3> kBadCrc{0, 0, 0};
    append_bytes(0, kBadCrc);
    dirty(0);
    uint32_t tick = s_clock;
    link_round(tick);
    CHECK(s_service->stats().frames_rx == 1 && s_write_calls == 1);
    CHECK(s_sessions[0].sink.pending() && s_snapshot.rtcm_frames == 0);
    link_round(tick);
    CHECK(s_service->stats().frames_rx == 2 && s_write_calls == 2);
    CHECK(s_snapshot.rtcm_frames == 1 && s_snapshot.rtcm_crc32 == expected_crc);
    CHECK(s_snapshot.demux_rtcm_rejects == 1 && s_snapshot.demux_unknown_bytes == 6);
    CHECK(s_snapshot.parser_crc_errors == 0 && s_snapshot.parser_length_errors == 0);
    check_reply(0, 1);
    check_reply(1, 2);
}

void rtcm_reset_lifecycle()
{
    using namespace app;
    setup({}, policy_from_config());
    open(1, kPhone);
    constexpr std::array<uint8_t, 10> kInvalid{0xAA, 0x44, 0x18, 0x19, 0xD3, 0xFC, 0, 0x7F, 0xFF, 0x55};
    append_bytes(0, kInvalid);
    const size_t bad_begin = s_input_size[0];
    append(0, 1, 0, proto::msg_type::kQuery);
    const size_t bad_size = s_input_size[0] - bad_begin;
    CHECK(crc32_reference(std::span{s_input[0]}.subspan(bad_begin, bad_size - proto::kCrcLen)) != 0);
    auto bad_crc = std::span{s_input[0]}.subspan(s_input_size[0] - proto::kCrcLen, proto::kCrcLen);
    std::fill(bad_crc.begin(), bad_crc.end(), 0);
    const auto wire = append_rtcm(0, 1005);
    const size_t full = s_input_size[0];
    s_input_size[0] = full - wire.size() + 4;
    dirty(0);
    drain_rx(s_clock);
    publish_snapshot(s_clock);
    CHECK(s_sessions[0].demux.busy());
    CHECK(s_snapshot.demux_unknown_bytes == kInvalid.size() + bad_size);
    CHECK(s_snapshot.demux_product_rejects == 2 && s_snapshot.demux_rtcm_rejects == 1);
    CHECK(s_snapshot.parser_crc_errors == 0 && s_snapshot.parser_length_errors == 0);
    Command command{};
    command.id = CommandId::kRtcmReset;
    command.seq = 17;
    CHECK(s_command_queue.send(command, 0));
    process_commands(s_clock);
    CommandResult result;
    CHECK(s_result_queue.receive(result, 0));
    CHECK(result.seq == 17 && result.status == CommandStatus::kOk);
    publish_snapshot(s_clock);
    CHECK(s_sessions[0].demux.busy());
    CHECK(s_snapshot.rtcm_frames == 0 && s_snapshot.rtcm_bytes == 0 && s_snapshot.rtcm_crc32 == 0);
    CHECK(s_snapshot.rtcm_last_msg == 0 && s_snapshot.rtcm_rejected_role == 0);
    CHECK(s_snapshot.demux_unknown_bytes == 0 && s_snapshot.demux_product_rejects == 0 &&
          s_snapshot.demux_rtcm_rejects == 0);
    s_input_size[0] = full;
    dirty(0);
    drain_rx(s_clock);
    CHECK(s_rtcm.stats().frames == 1 && s_rtcm.stats().crc32 == crc32_reference(wire));

    constexpr std::array<uint8_t, 3> kPartial{0xD3, 0, 6};
    append_bytes(0, kPartial);
    dirty(0);
    drain_rx(s_clock);
    CHECK(s_sessions[0].demux.busy());
    spp_server_evt_t close{};
    close.type = SPP_SERVER_EVT_CLOSE;
    close.handle = 1;
    handle_event(close, s_clock);
    handle_event(close, s_clock); // 重复 CLOSE 不得重复累计。
    publish_snapshot(s_clock);
    CHECK(s_snapshot.demux_unknown_bytes == 3 && s_snapshot.demux_rtcm_rejects == 1);
    CHECK(!s_sessions[0].used && !s_sessions[0].demux.busy());
    open(1, kPhone);
    const auto next = append_rtcm(0, 1077);
    dirty(0);
    drain_rx(s_clock);
    publish_snapshot(s_clock);
    CHECK(s_snapshot.rtcm_frames == 2 && s_snapshot.rtcm_last_msg == 1077);
    CHECK(s_snapshot.rtcm_crc32 == crc32_reference(next, crc32_reference(wire)));
    CHECK(s_snapshot.demux_unknown_bytes == 3 && s_snapshot.demux_rtcm_rejects == 1);
    // 已关闭会话的统计也属于 rtcm reset 的清零范围。
    CHECK(s_command_queue.send(command, 0));
    process_commands(s_clock);
    publish_snapshot(s_clock);
    CHECK(s_snapshot.rtcm_frames == 0 && s_snapshot.rtcm_crc32 == 0 && s_snapshot.demux_unknown_bytes == 0 &&
          s_snapshot.demux_rtcm_rejects == 0);
}

void rtcm_byte_budget()
{
    using namespace app;
    setup({}, policy_from_config());
    open(1, kPhone);
    const auto first = append_rtcm(0, 1005, 1023);
    const auto second = append_rtcm(0, 1077);
    const uint32_t expected_crc = crc32_reference(second, crc32_reference(first));
    dirty(0);
    uint32_t tick = s_clock;
    size_t consumed = 0;
    for (size_t round = 0; round < 6 && s_rtcm.stats().frames < 2; ++round) {
        link_round(tick);
        const auto &entry = s_sessions[0];
        const size_t next = s_input_offset[0] - (entry.rx_size - entry.rx_offset);
        CHECK(next - consumed <= kRxBudgetBytes);
        consumed = next;
        if (consumed < first.size()) {
            CHECK(s_rtcm.stats().frames == 0);
        }
    }
    CHECK(s_rtcm.stats().frames == 2 && s_rtcm.stats().bytes == first.size() + second.size());
    CHECK(s_rtcm.stats().crc32 == expected_crc && s_write_calls == 0);
}

void lifecycle_application()
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
    CHECK(s_service->stats().frames_rx == 1U && s_write_calls == 1U);
}
} // namespace

namespace rtos {
uint32_t now_ms() noexcept
{
    return s_clock;
}
fw::Status from_esp_err(esp_err_t err) noexcept
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
    std::memcpy(bytes, s_input[idx].data() + s_input_offset[idx], count);
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
    CHECK(timeout == 200U);
    s_clock += s_write_delay;
    return s_write_delay ? ESP_ERR_TIMEOUT : ESP_OK;
}
esp_err_t spp_server_disconnect(uint32_t)
{
    return ESP_OK;
}
/* host 无 SPP 驱动：追踪钩子登记接受即返回（DebugModule::init 会调用）。 */
void spp_server_set_trace_hook(spp_server_trace_fn) {}
esp_err_t nvs_store_init(void)
{
    return ESP_OK;
}
esp_err_t nvs_store_get_sn(char *out, size_t size)
{
    std::snprintf(out, size, "%s", "TEST");
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
        for (size_t idx = 0U; idx < count; ++idx) {
            std::memcpy(s_saved[idx].data(), alarms[idx], NVS_STORE_ADDR_LEN);
        }
    }
    return s_store_result;
}
esp_err_t nvs_store_get_alarms(uint8_t (*)[NVS_STORE_ADDR_LEN], size_t, size_t *count)
{
    *count = 0U;
    return ESP_OK;
}
}

// 异步发送装饰器：未启动时同步直通；启动后入队即返回、FIFO、满队列 kBusy、底层失败计数。
class GateTransport final : public core::Transport {
public:
    fw::Status write(core::SessionHandle handle, std::span<const uint8_t> data, uint32_t timeout_ms) noexcept override
    {
        entered.store(true);
        while (!open.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(timeout_ms == 200U && data.size() == 1U);
        order[count.load()] = data[0];
        count.fetch_add(1U);
        return (handle == 0xDEADU) ? fw::Status::kTimeout : fw::Status::kOk;
    }
    fw::Status disconnect(core::SessionHandle) noexcept override
    {
        return fw::Status::kOk;
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> open{true};
    std::atomic<uint32_t> count{0};
    std::array<uint8_t, 32> order{};
};

void async_tx()
{
    static GateTransport inner;
    static app::AsyncTxTransport tx{inner};
    const std::array<uint8_t, 1> first{0};
    CHECK(tx.write(1U, first, 200U) == fw::Status::kOk && inner.count.load() == 1U); // 未启动：同步
    inner.open.store(false);
    inner.entered.store(false);
    CHECK(tx.start(1U, 0) == fw::Status::kOk && tx.start(1U, 0) == fw::Status::kBusy);
    const std::array<uint8_t, 1> one{1};
    CHECK(tx.write(1U, one, 200U) == fw::Status::kOk);
    while (!inner.entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // 发送任务卡在底层 write 内：再入 kDepth 条成功，下一条满。
    for (uint8_t i = 0; i < app::AsyncTxTransport::kDepth; ++i) {
        const std::array<uint8_t, 1> b{static_cast<uint8_t>(2U + i)};
        CHECK(tx.write((i == 0U) ? 0xDEADU : 1U, b, 200U) == fw::Status::kOk);
    }
    const std::array<uint8_t, 1> overflow{99};
    CHECK(tx.write(1U, overflow, 200U) == fw::Status::kBusy);
    CHECK(tx.write(1U, std::span<const uint8_t>{}, 200U) == fw::Status::kInvalidArg);
    inner.open.store(true);
    const uint32_t total = 2U + app::AsyncTxTransport::kDepth;
    for (int spin = 0; spin < 2000 && inner.count.load() < total; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(inner.count.load() == total);
    for (uint32_t i = 0; i < total; ++i) {
        CHECK(inner.order[i] == i);
    }
    const auto st = tx.stats();
    CHECK(st.sent == total - 2U && st.failed == 1U && st.queue_full == 1U &&
          st.max_depth >= app::AsyncTxTransport::kDepth);
}

int main(int argc, char **argv)
{
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
        CHECK(!snap.bt_ready && !snap.link_running);
    } else if (std::strcmp(argv[1], "debug_cat_names") == 0) {
        // 控制台习惯输小写，表里是大写短名：两种写法都必须解析（T4 上板发现小写被拒）。
        fw::TraceCat cat = fw::TraceCat::kLink;
        CHECK(app::debug::parse_cat("gnss", cat) && cat == fw::TraceCat::kGnss);
        CHECK(app::debug::parse_cat("GNSS", cat) && cat == fw::TraceCat::kGnss);
        CHECK(app::debug::parse_cat("Spp", cat) && cat == fw::TraceCat::kSpp);
        CHECK(!app::debug::parse_cat("gns", cat) && !app::debug::parse_cat("gnssx", cat));
    } else if (std::strcmp(argv[1], "save_recovery") == 0) {
        save_recovery();
    } else if (std::strcmp(argv[1], "storage_backoff") == 0) {
        storage_backoff();
    } else if (std::strcmp(argv[1], "async_tx") == 0) {
        async_tx();
    } else if (std::strcmp(argv[1], "rx_fairness") == 0) {
        rx_fairness();
    } else if (std::strcmp(argv[1], "rx_fast_partial") == 0) {
        rx_fast_partial();
    } else if (std::strcmp(argv[1], "rx_tick_budget") == 0) {
        rx_tick_budget();
    } else if (std::strcmp(argv[1], "rtcm_mix") == 0) {
        rtcm_mix();
    } else if (std::strcmp(argv[1], "rtcm_roles") == 0) {
        rtcm_roles();
    } else if (std::strcmp(argv[1], "rtcm_replay_budget") == 0) {
        rtcm_replay_budget();
    } else if (std::strcmp(argv[1], "rtcm_reset_lifecycle") == 0) {
        rtcm_reset_lifecycle();
    } else if (std::strcmp(argv[1], "rtcm_byte_budget") == 0) {
        rtcm_byte_budget();
    } else if (std::strcmp(argv[1], "lifecycle_application") == 0) {
        lifecycle_application();
    } else {
        return 2;
    }
    std::printf("%s %s\n", s_failures ? "FAIL" : "PASS", argv[1]);
    return s_failures ? 1 : 0;
}
