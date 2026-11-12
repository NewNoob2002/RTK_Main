// golden 等价测试（ADR-0007 §9 门禁 2、任务卡 J2a）：同一输入同时喂旧 CommandDispatcher 与
// CommandRouter + 4 个服务，比较 DispatchResult、reply.bytes()、whitelist_changed、heartbeat，
// 以及 AlarmService::build_status_push 与旧 build_status_push 的字节。
// 一侧发现不一致时以旧实现为准修改服务，不得修改本测试的期望。
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// 旧实现已于 J2b 退役：下面这行经 -I reference/cpp-v3-dispatcher 指向 reference/ 里的只读基准（内容未改）。
#include "rtk/core/command_dispatcher.hpp"
#include "rtk/core/command_router.hpp"
#include "rtk/core/services/alarm_service.hpp"
#include "rtk/core/services/config_service.hpp"
#include "rtk/core/services/device_service.hpp"
#include "rtk/core/services/pair_service.hpp"
#include "rtk/proto/alarm_codec.hpp"
#include "rtk/proto/frame.hpp"
#include "unity.h"

namespace ac = rtk::proto::alarm;
namespace mt = rtk::proto::msg_type;
namespace proto = rtk::proto;
namespace svc = rtk::core::services;

using rtk::core::Actions;
using rtk::core::AlarmSource;
using rtk::core::BtAddr;
using rtk::core::CommandDispatcher;
using rtk::core::CommandRouter;
using rtk::core::DispatchResult;
using rtk::core::LinkManager;
using rtk::core::Policy;
using rtk::core::Reply;
using rtk::core::Role;
using rtk::core::RouteTable;
using rtk::core::SessionHandle;

namespace {

// Reply 哨兵值：两侧调用前都置成同一状态，用来验证 handle 先 clear()。
constexpr uint8_t kSentinelByte = 0x5A;
constexpr size_t kSentinelLen = 99;
constexpr size_t kDescCapacity = 96;

// 时间与负载图案（图案保证每个字节位置内容不同，能暴露偏移错误）。
constexpr uint32_t kFirstNowMs = 0x0800;
constexpr uint32_t kNowStep = 16;
constexpr uint32_t kPayloadSeed = 0x11;
constexpr size_t kPayloadStride = 7;

// msg_id 扫描范围：0x00–0xFF。
constexpr uint32_t kLastSweptMsgId = 0xFF;

// 各状态序列的起始时间（彼此不同，便于在日志里区分）。
constexpr uint32_t kPairNowMs = 1000;
constexpr uint32_t kDeviceNowMs = 2000;
constexpr uint32_t kConfigNowMs = 3000;
constexpr uint32_t kAlarmNowMs = 4000;

// 4 张表合计 15 条路由（设备 7 + 配置 3 + 报警 4 + 配对 1），与旧 is_known_id 的集合一致。
constexpr size_t kRouteCount = 15;

constexpr SessionHandle kPhoneHandle = 0x81;
constexpr SessionHandle kAlarmHandle = 0x82;
constexpr BtAddr kPhoneAddr{{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}};
constexpr BtAddr kAlarmAddr{{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5}};

// phone_protocol.md §4 之外的非法 type。
constexpr uint8_t kUnknownType = 0x07;

// §4 命令 ID（便于阅读的局部别名，取值仍来自协议文档）。
constexpr uint16_t kMsgDevInfo = 0x0001;
constexpr uint16_t kMsgDevState = 0x0002;
constexpr uint16_t kMsgBattery = 0x0003;
constexpr uint16_t kMsgGga = 0x0004;
constexpr uint16_t kMsgSat = 0x0005;
constexpr uint16_t kMsgSatTrack = 0x0006;
constexpr uint16_t kMsgWorkMode = 0x0007;
constexpr uint16_t kMsgCom = 0x000D;
constexpr uint16_t kMsgRadio = 0x0014;
constexpr uint16_t kMsgLog = 0x0015;

// 0x44 / 0x42 的动作字节与状态/等级（§5）。
constexpr uint8_t kPairOpen = 0x01;
constexpr uint8_t kPairClear = 0x00;
constexpr uint8_t kPairBadAction = 0x09;
constexpr uint8_t kTestSet = 0x01;
constexpr uint8_t kTestClear = 0x00;
constexpr uint8_t kTestLevel = 0x03;
constexpr uint8_t kIllegalState = 0x09;

// 测试里显式写入的设备信息与心跳取值。
constexpr uint8_t kBatteryPct = 64;
constexpr int8_t kBatteryTempC = -5;
constexpr uint32_t kComBaud = 9600;
constexpr uint8_t kHeartbeatPct = 55;
constexpr uint8_t kHeartbeatState = 0x02;
constexpr uint8_t kPushBatteryPct = 42;
constexpr uint16_t kHeartbeatSeq = 0x0007;
constexpr uint16_t kPushSeq = 0xBEEF;
constexpr uint16_t kPushSeq2 = 0xBEF0;
constexpr uint16_t kPushSeq3 = 0xFFFF;
constexpr uint8_t kStateAlert = ac::kStateAlert;

// 文件作用域可变静态统一加 s_ 前缀（AGENTS.md §2）。
std::array<uint8_t, proto::kMaxPayload> s_payload{};
std::array<char, kDescCapacity> s_desc{};

const char *role_name(Role role)
{
    switch (role) {
    case Role::kNone:
        return "NONE";
    case Role::kPhone:
        return "PHONE";
    case Role::kAlarm:
        return "ALARM";
    case Role::kPending:
        return "PENDING";
    default:
        return "?";
    }
}

size_t desc_text(size_t pos, std::string_view text)
{
    for (size_t i = 0; (i < text.size()) && ((pos + 1U) < s_desc.size()); ++i) {
        s_desc[pos] = text[i];
        ++pos;
    }
    s_desc[pos] = '\0';
    return pos;
}

/** 追加固定位数的十六进制（大写，无前缀）；digits ≤ 4，位运算不越界。 */
size_t desc_hex(size_t pos, uint32_t value, size_t digits)
{
    for (size_t i = digits; i > 0; --i) {
        const uint32_t nibble = (value >> ((i - 1U) * 4U)) & 0x0FU;
        const char digit = (nibble < 10U) ? static_cast<char>('0' + nibble) : static_cast<char>('A' + (nibble - 10U));
        const std::array<char, 2> one = {digit, '\0'};
        pos = desc_text(pos, one.data());
    }
    return pos;
}

/** 失败信息："id=0x0044 role=PHONE type=0x02 payload=pair_open1"。不用 vararg（项目规范禁止）。 */
const char *describe(Role role, uint16_t msg_id, uint8_t type, const char *shape)
{
    s_desc.fill('\0');
    size_t pos = desc_text(0, "id=0x");
    pos = desc_hex(pos, msg_id, 4);
    pos = desc_text(pos, " role=");
    pos = desc_text(pos, role_name(role));
    pos = desc_text(pos, " type=0x");
    pos = desc_hex(pos, type, 2);
    pos = desc_text(pos, " payload=");
    static_cast<void>(desc_text(pos, shape));
    return s_desc.data();
}

/** 一次调用的可观测结果。 */
struct Outcome {
    DispatchResult result = DispatchResult::kNoReply;
    size_t len = 0;
    std::array<uint8_t, proto::kMaxFrame> bytes{};
    bool whitelist_changed = false;
    bool has_heartbeat = false;
    ac::Heartbeat heartbeat{};
};

/** 旧实现与新实现的共同夹具：共享同一份 AlarmSource / LinkManager（两者都只持有引用）。 */
struct Harness {
    AlarmSource alarm{};
    LinkManager link{};
    CommandDispatcher old_disp;
    svc::DeviceService dev{};
    svc::ConfigService cfg{};
    svc::AlarmService alarm_svc;
    svc::PairService pair_svc;
    CommandRouter router{};
    Reply old_reply{};
    Reply new_reply{};

    Harness() noexcept : old_disp(alarm, link), alarm_svc(alarm, link, dev.info()), pair_svc(link)
    {
        link.init(Policy::kConcurrent, {});
    }

    /** 组装根：4 张路由表 → 1 个路由器（与 main 的组装方式一致）。 */
    rtk::Status init_router() noexcept
    {
        const std::array<RouteTable, 4> tables = {dev.table(), cfg.table(), alarm_svc.table(), pair_svc.table()};
        return router.init(tables);
    }
};

void arm_reply(Reply &reply)
{
    reply.buf.fill(kSentinelByte);
    reply.len = kSentinelLen;
    reply.whitelist_changed = true;
    reply.heartbeat = ac::Heartbeat{};
}

Outcome observe(const Reply &reply, DispatchResult result)
{
    Outcome out;
    out.result = result;
    out.len = reply.len;
    const std::span<const uint8_t> bytes = reply.bytes();
    for (size_t i = 0; (i < bytes.size()) && (i < out.bytes.size()); ++i) {
        out.bytes[i] = bytes[i];
    }
    out.whitelist_changed = reply.whitelist_changed;
    out.has_heartbeat = reply.heartbeat.has_value();
    if (reply.heartbeat.has_value()) {
        out.heartbeat = *reply.heartbeat;
    }
    return out;
}

void compare_outcome(const Outcome &old_out, const Outcome &new_out, const char *desc)
{
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(old_out.result), static_cast<uint8_t>(new_out.result), desc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(static_cast<unsigned>(old_out.len), static_cast<unsigned>(new_out.len), desc);
    if (old_out.len > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(old_out.bytes.data(), new_out.bytes.data(), old_out.len, desc);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(old_out.whitelist_changed ? 1U : 0U, new_out.whitelist_changed ? 1U : 0U, desc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(old_out.has_heartbeat ? 1U : 0U, new_out.has_heartbeat ? 1U : 0U, desc);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(old_out.heartbeat.seq, new_out.heartbeat.seq, desc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(old_out.heartbeat.battery_pct, new_out.heartbeat.battery_pct, desc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(old_out.heartbeat.local_state, new_out.heartbeat.local_state, desc);
}

/** 一次等价比较：先跑旧 dispatcher，再跑新路由器（两者共享 alarm/link 状态）。 */
void run_case(Harness &harness, Role role, uint16_t msg_id, uint8_t type, const uint8_t *payload, size_t len,
              uint32_t now_ms, const char *desc)
{
    proto::FrameView frame{};
    frame.msg_id = msg_id;
    frame.type = type;
    frame.sender = proto::sender::kApp;
    frame.payload = std::span<const uint8_t>(payload, len);

    arm_reply(harness.old_reply);
    const DispatchResult old_result = harness.old_disp.handle(role, frame, now_ms, harness.old_reply);
    const Outcome old_out = observe(harness.old_reply, old_result);

    arm_reply(harness.new_reply);
    const DispatchResult new_result = harness.router.handle(role, frame, now_ms, harness.new_reply);
    const Outcome new_out = observe(harness.new_reply, new_result);

    compare_outcome(old_out, new_out, desc);
}

/** 0x40 主动推送：旧 build_status_push 与 AlarmService::build_status_push 逐字节比较。 */
void compare_status_push(Harness &harness, uint16_t seq, uint32_t now_ms, const char *desc)
{
    arm_reply(harness.old_reply);
    const bool old_ok = harness.old_disp.build_status_push(seq, now_ms, harness.old_reply);
    const Outcome old_out = observe(harness.old_reply, DispatchResult::kReply);

    arm_reply(harness.new_reply);
    const bool new_ok = harness.alarm_svc.build_status_push(seq, now_ms, harness.new_reply);
    const Outcome new_out = observe(harness.new_reply, DispatchResult::kReply);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(old_ok ? 1U : 0U, new_ok ? 1U : 0U, desc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(static_cast<unsigned>(old_out.len), static_cast<unsigned>(new_out.len), desc);
    if (old_out.len > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(old_out.bytes.data(), new_out.bytes.data(), old_out.len, desc);
    }
}

/** 负载形状：前 head_len 字节用显式值覆盖，其余按图案填充。 */
struct PayloadShape {
    const char *name;
    size_t len;
    size_t head_len;
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
};

constexpr size_t kTypicalLen = 50;
constexpr std::array<PayloadShape, 9> kShapes = {{
    {"empty", 0, 0, 0, 0, 0},
    {"pair_open1", 1, 1, kPairOpen, 0, 0},
    {"pair_clear1", 1, 1, kPairClear, 0, 0},
    {"test_set3", 3, 3, kTestSet, kStateAlert, kTestLevel},
    {"test_clear3", 3, 3, kTestClear, kStateAlert, kTestLevel},
    {"short2", 2, 1, kPairOpen, 0, 0},
    {"wm44", rtk::core::kWorkModeSetLen, 0, 0, 0, 0},
    {"typical50", kTypicalLen, 1, kTestSet, 0, 0},
    {"full256", proto::kMaxPayload, 1, kTestSet, 0, 0},
}};

void fill_payload(const PayloadShape &shape)
{
    for (size_t i = 0; i < s_payload.size(); ++i) {
        s_payload[i] = static_cast<uint8_t>((i * kPayloadStride) + kPayloadSeed);
    }
    if (shape.head_len > 0) {
        s_payload[0] = shape.b0;
    }
    if (shape.head_len > 1) {
        s_payload[1] = shape.b1;
    }
    if (shape.head_len > 2) {
        s_payload[2] = shape.b2;
    }
}

void test_msg_id_sweep()
{
    constexpr std::array<uint8_t, 4> kTypes = {mt::kQuery, mt::kSet, mt::kPush, kUnknownType};
    // 旧 dispatcher 对 kNone/kPending 一律 kDenied；新路由器按 RoleMask 判定，同样要比一轮。
    constexpr std::array<Role, 4> kRoles = {Role::kPhone, Role::kAlarm, Role::kNone, Role::kPending};

    Harness harness;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(rtk::Status::kOk), static_cast<uint8_t>(harness.init_router()));
    TEST_ASSERT_EQUAL_UINT(kRouteCount, harness.router.route_count());

    uint32_t now_ms = kFirstNowMs;
    size_t cases = 0;
    for (uint32_t raw_id = 0; (raw_id <= kLastSweptMsgId); ++raw_id) {
        // raw_id ≤ 0xFF，uint16_t 截断安全。
        const uint16_t msg_id = static_cast<uint16_t>(raw_id);
        for (const Role role : kRoles) {
            for (const uint8_t type : kTypes) {
                for (const PayloadShape &shape : kShapes) {
                    fill_payload(shape);
                    run_case(harness, role, msg_id, type, s_payload.data(), shape.len, now_ms,
                             describe(role, msg_id, type, shape.name));
                    now_ms += kNowStep;
                    ++cases;
                }
            }
        }
    }
    // 期望用例数 = (0x00–0xFF) × 角色 × type × 负载形状。
    const size_t expected = (kLastSweptMsgId + 1U) * kRoles.size() * kTypes.size() * kShapes.size();
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(expected), static_cast<unsigned>(cases));
}

void test_device_sequence()
{
    Harness harness;
    TEST_ASSERT_TRUE(rtk::is_ok(harness.init_router()));

    uint32_t now_ms = kDeviceNowMs;
    constexpr std::array<uint16_t, 7> kIds = {kMsgDevInfo, kMsgDevState, kMsgBattery, kMsgGga,
                                              kMsgSat,     kMsgSatTrack, kMsgCom};
    // 1. 默认值（构造函数默认值必须与旧 dispatcher 相同）。
    for (const uint16_t id : kIds) {
        run_case(harness, Role::kPhone, id, mt::kQuery, s_payload.data(), 0, now_ms,
                 describe(Role::kPhone, id, mt::kQuery, "default"));
        now_ms += kNowStep;
    }

    // 2. 两侧显式写入同一组设备信息（golden 前提）。
    harness.old_disp.info().sn.assign("SN0001");
    harness.dev.info().sn.assign("SN0001");
    harness.old_disp.info().fw_ver.assign("V6.1.0");
    harness.dev.info().fw_ver.assign("V6.1.0");
    harness.old_disp.info().board_sn.assign("B0001");
    harness.dev.info().board_sn.assign("B0001");
    harness.old_disp.info().board_fw.assign("R1.0");
    harness.dev.info().board_fw.assign("R1.0");
    harness.old_disp.info().radio_fw.assign("RF1");
    harness.dev.info().radio_fw.assign("RF1");
    harness.old_disp.info().on_battery = false;
    harness.dev.info().on_battery = false;
    harness.old_disp.info().battery_pct = kBatteryPct;
    harness.dev.info().battery_pct = kBatteryPct;
    harness.old_disp.info().battery_temp_c = kBatteryTempC;
    harness.dev.info().battery_temp_c = kBatteryTempC;
    harness.old_disp.info().com_baud = kComBaud;
    harness.dev.info().com_baud = kComBaud;

    for (const uint16_t id : {kMsgDevInfo, kMsgBattery, kMsgCom}) {
        run_case(harness, Role::kPhone, id, mt::kQuery, s_payload.data(), 0, now_ms,
                 describe(Role::kPhone, id, mt::kQuery, "custom"));
        now_ms += kNowStep;
    }
    // 0x06 与 0x0D 的 SET 分支（0x0D 按 5.4 行为不回复）。
    run_case(harness, Role::kPhone, kMsgSatTrack, mt::kSet, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, kMsgSatTrack, mt::kSet, "none"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgCom, mt::kSet, s_payload.data(), rtk::core::kComLen, now_ms,
             describe(Role::kPhone, kMsgCom, mt::kSet, "32B"));
}

void test_config_sequence()
{
    Harness harness;
    TEST_ASSERT_TRUE(rtk::is_ok(harness.init_router()));

    uint32_t now_ms = kConfigNowMs;
    for (size_t i = 0; i < s_payload.size(); ++i) {
        s_payload[i] = static_cast<uint8_t>((i * kPayloadStride) + kPayloadSeed);
    }

    // 0x07：SET 恰好 44 字节成功，QUERY 回读 48 字节，短包失败且不回滚。
    run_case(harness, Role::kPhone, kMsgWorkMode, mt::kSet, s_payload.data(), rtk::core::kWorkModeSetLen, now_ms,
             describe(Role::kPhone, kMsgWorkMode, mt::kSet, "44B"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgWorkMode, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, kMsgWorkMode, mt::kQuery, "readback"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgWorkMode, mt::kSet, s_payload.data(), rtk::core::kWorkModeSetLen - 1U, now_ms,
             describe(Role::kPhone, kMsgWorkMode, mt::kSet, "43B"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgWorkMode, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, kMsgWorkMode, mt::kQuery, "readback2"));
    now_ms += kNowStep;

    // 0x14：SET 15 字节成功（第 17 字节保持默认 0x05），满 28 字节覆盖全部字段。
    run_case(harness, Role::kPhone, kMsgRadio, mt::kSet, s_payload.data(), rtk::core::kRadioSetMinLen, now_ms,
             describe(Role::kPhone, kMsgRadio, mt::kSet, "15B"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgRadio, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, kMsgRadio, mt::kQuery, "readback"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgRadio, mt::kSet, s_payload.data(), rtk::core::kRadioSetMinLen - 1U, now_ms,
             describe(Role::kPhone, kMsgRadio, mt::kSet, "14B"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgRadio, mt::kSet, s_payload.data(), rtk::core::kRadioLen, now_ms,
             describe(Role::kPhone, kMsgRadio, mt::kSet, "28B"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgRadio, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, kMsgRadio, mt::kQuery, "readback2"));
    now_ms += kNowStep;

    // 0x15：SET 50 字节成功，49 字节失败；非 QUERY/SET 的 type 走只读分支。
    run_case(harness, Role::kPhone, kMsgLog, mt::kSet, s_payload.data(), rtk::core::kLogLen, now_ms,
             describe(Role::kPhone, kMsgLog, mt::kSet, "50B"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgLog, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, kMsgLog, mt::kQuery, "readback"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgLog, mt::kSet, s_payload.data(), rtk::core::kLogLen - 1U, now_ms,
             describe(Role::kPhone, kMsgLog, mt::kSet, "49B"));
    now_ms += kNowStep;
    run_case(harness, Role::kPhone, kMsgLog, mt::kPush, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, kMsgLog, mt::kPush, "push"));
}

void test_pair_sequence()
{
    Harness harness;
    TEST_ASSERT_TRUE(rtk::is_ok(harness.init_router()));

    Actions act;
    uint32_t now_ms = kPairNowMs;
    const std::array<uint8_t, 1> pair_open = {kPairOpen};
    const std::array<uint8_t, 1> pair_clear = {kPairClear};
    const std::array<uint8_t, 1> pair_bad = {kPairBadAction};

    // 1. 打开配对窗口。
    run_case(harness, Role::kPhone, ac::kMsgPair, mt::kSet, pair_open.data(), pair_open.size(), now_ms,
             describe(Role::kPhone, ac::kMsgPair, mt::kSet, "open"));
    TEST_ASSERT_TRUE(harness.link.is_pairing());
    now_ms += kNowStep;

    // 2. 报警器先以 PHONE 接入（地址还不在白名单），HELLO 之后转 ALARM 并加入白名单。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Role::kPhone),
                            static_cast<uint8_t>(harness.link.on_open(kAlarmHandle, kAlarmAddr, now_ms, act)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Role::kAlarm),
                            static_cast<uint8_t>(harness.link.on_frame(kAlarmHandle, proto::sender::kAlarm,
                                                                       rtk::core::kHelloMsgId, now_ms, act)));
    TEST_ASSERT_EQUAL_UINT(1, harness.link.whitelist().size());
    now_ms += kNowStep;

    // 3. 查询：1 字节数量 + 6 字节 MAC。
    run_case(harness, Role::kPhone, ac::kMsgPair, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, ac::kMsgPair, mt::kQuery, "1addr"));
    now_ms += kNowStep;

    // 4. 清空白名单：whitelist_changed = true。
    run_case(harness, Role::kPhone, ac::kMsgPair, mt::kSet, pair_clear.data(), pair_clear.size(), now_ms,
             describe(Role::kPhone, ac::kMsgPair, mt::kSet, "clear"));
    TEST_ASSERT_TRUE(harness.old_reply.whitelist_changed);
    TEST_ASSERT_TRUE(harness.new_reply.whitelist_changed);
    TEST_ASSERT_EQUAL_UINT(0, harness.link.whitelist().size());
    now_ms += kNowStep;

    run_case(harness, Role::kPhone, ac::kMsgPair, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, ac::kMsgPair, mt::kQuery, "0addr"));
    now_ms += kNowStep;

    // 5. 非法动作字节 → 失败应答。
    run_case(harness, Role::kPhone, ac::kMsgPair, mt::kSet, pair_bad.data(), pair_bad.size(), now_ms,
             describe(Role::kPhone, ac::kMsgPair, mt::kSet, "bad_action"));
}

void test_alarm_sequence()
{
    Harness harness;
    TEST_ASSERT_TRUE(rtk::is_ok(harness.init_router()));

    Actions act;
    uint32_t now_ms = kAlarmNowMs;

    // 1. 手机在线 → 状态帧 flags bit0。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Role::kPhone),
                            static_cast<uint8_t>(harness.link.on_open(kPhoneHandle, kPhoneAddr, now_ms, act)));
    TEST_ASSERT_TRUE(harness.link.has_role(Role::kPhone));

    // 2. 无覆盖：推送与查询。
    compare_status_push(harness, 0x0000, now_ms, "push idle");
    now_ms += kNowStep;
    run_case(harness, Role::kAlarm, ac::kMsgStatus, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kAlarm, ac::kMsgStatus, mt::kQuery, "idle"));
    now_ms += kNowStep;

    // 3. 设置测试覆盖：alert / level 3。
    const std::array<uint8_t, 3> test_set = {kTestSet, kStateAlert, kTestLevel};
    run_case(harness, Role::kPhone, ac::kMsgTest, mt::kSet, test_set.data(), test_set.size(), now_ms,
             describe(Role::kPhone, ac::kMsgTest, mt::kSet, "set"));
    TEST_ASSERT_TRUE(harness.alarm.is_override());
    TEST_ASSERT_EQUAL_HEX8(kStateAlert, harness.alarm.state());
    now_ms += kNowStep;

    // 4. 覆盖生效后的查询与推送。
    run_case(harness, Role::kAlarm, ac::kMsgStatus, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kAlarm, ac::kMsgStatus, mt::kQuery, "override"));
    now_ms += kNowStep;
    compare_status_push(harness, kPushSeq, now_ms, "push override");
    now_ms += kNowStep;

    // 5. 心跳：解析结果写 reply.heartbeat，不回复。
    const std::array<uint8_t, ac::kHeartbeatLen> heartbeat = {0x07, 0x00, kHeartbeatPct, kHeartbeatState,
                                                              0x00, 0x00, 0x00,          0x00};
    run_case(harness, Role::kAlarm, ac::kMsgHeartbeat, mt::kPush, heartbeat.data(), heartbeat.size(), now_ms,
             describe(Role::kAlarm, ac::kMsgHeartbeat, mt::kPush, "hb"));
    TEST_ASSERT_TRUE(harness.old_reply.heartbeat.has_value());
    TEST_ASSERT_TRUE(harness.new_reply.heartbeat.has_value());
    TEST_ASSERT_EQUAL_HEX16(kHeartbeatSeq, harness.new_reply.heartbeat->seq);
    TEST_ASSERT_EQUAL_UINT8(kHeartbeatPct, harness.new_reply.heartbeat->battery_pct);
    TEST_ASSERT_EQUAL_UINT8(kHeartbeatState, harness.new_reply.heartbeat->local_state);
    now_ms += kNowStep;

    // 6. 心跳之后（电池变化）再比一次推送。
    harness.old_disp.info().battery_pct = kPushBatteryPct;
    harness.dev.info().battery_pct = kPushBatteryPct;
    compare_status_push(harness, kPushSeq2, now_ms, "push after heartbeat");
    now_ms += kNowStep;

    // 7. 清除覆盖。
    const std::array<uint8_t, 3> test_clear = {kTestClear, kStateAlert, kTestLevel};
    run_case(harness, Role::kPhone, ac::kMsgTest, mt::kSet, test_clear.data(), test_clear.size(), now_ms,
             describe(Role::kPhone, ac::kMsgTest, mt::kSet, "clear"));
    TEST_ASSERT_FALSE(harness.alarm.is_override());
    now_ms += kNowStep;

    // 8. 非法状态 → 失败且状态不变。
    const std::array<uint8_t, 3> test_bad = {kTestSet, kIllegalState, kTestLevel};
    run_case(harness, Role::kPhone, ac::kMsgTest, mt::kSet, test_bad.data(), test_bad.size(), now_ms,
             describe(Role::kPhone, ac::kMsgTest, mt::kSet, "bad_state"));
    TEST_ASSERT_FALSE(harness.alarm.is_override());
    now_ms += kNowStep;

    // 9. 围栏（预留）与 0x40 / 0x41 的 type 限制。
    run_case(harness, Role::kPhone, ac::kMsgFence, mt::kQuery, s_payload.data(), 0, now_ms,
             describe(Role::kPhone, ac::kMsgFence, mt::kQuery, "reserved"));
    now_ms += kNowStep;
    run_case(harness, Role::kAlarm, ac::kMsgStatus, mt::kSet, s_payload.data(), 0, now_ms,
             describe(Role::kAlarm, ac::kMsgStatus, mt::kSet, "type_denied"));
    now_ms += kNowStep;
    run_case(harness, Role::kAlarm, ac::kMsgHeartbeat, mt::kQuery, heartbeat.data(), heartbeat.size(), now_ms,
             describe(Role::kAlarm, ac::kMsgHeartbeat, mt::kQuery, "type_denied"));
    now_ms += kNowStep;
    compare_status_push(harness, kPushSeq3, now_ms, "push final");
}

} // namespace

// Unity 默认运行器会在每个用例前后调用这两个符号（外部链接，与 test/host 同样写法）。
// 每个用例自建 Harness，因此这里不需要复位状态。
extern "C" void setUp(void) {}

extern "C" void tearDown(void) {}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_msg_id_sweep);
    RUN_TEST(test_device_sequence);
    RUN_TEST(test_config_sequence);
    RUN_TEST(test_pair_sequence);
    RUN_TEST(test_alarm_sequence);
    return UNITY_END();
}
