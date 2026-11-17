// golden 等价测试（ADR-0007 §9 门禁 2、任务卡 J2a）：同一输入同时喂旧 CommandDispatcher 与
// CommandRouter + 4 个服务，比较 DispatchResult、reply.bytes()、whitelistChanged、heartbeat，
// 以及 AlarmService::buildStatusPush 与旧 buildStatusPush 的字节。
// 一侧发现不一致时以旧实现为准修改服务，不得修改本测试的期望。
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// 旧实现已于 J2b 退役：下面这行经 -I reference/cpp-v3-dispatcher 指向 reference/ 里的只读基准（内容未改）。
#include "core/command_dispatcher.hpp"
#include "core/CommandRouter.hpp"
#include "core/services/AlarmService.hpp"
#include "core/services/ConfigService.hpp"
#include "core/services/DeviceService.hpp"
#include "core/services/PairService.hpp"
#include "proto/AlarmCodec.hpp"
#include "proto/Frame.hpp"
#include "unity.h"

namespace ac = proto::alarm;
namespace mt = proto::msg_type;
namespace svc = core::services;

using core::Actions;
using core::AlarmSource;
using core::BtAddr;
using core::CommandDispatcher;
using core::CommandRouter;
using core::DispatchResult;
using core::LinkManager;
using core::Policy;
using core::Reply;
using core::Role;
using core::RouteTable;
using core::SessionHandle;

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

// msgId 扫描范围：0x00–0xFF。
constexpr uint32_t kLastSweptMsgId = 0xFF;

// 各状态序列的起始时间（彼此不同，便于在日志里区分）。
constexpr uint32_t kPairNowMs = 1000;
constexpr uint32_t kDeviceNowMs = 2000;
constexpr uint32_t kConfigNowMs = 3000;
constexpr uint32_t kAlarmNowMs = 4000;

// 4 张表合计 13 条路由（设备 5 + 配置 3 + 报警 4 + 配对 1）；0x04/0x05 在 K4 移交 GnssService。
constexpr size_t kRouteCount = 13;

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

const char *roleName(Role role)
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

size_t descText(size_t pos, std::string_view text)
{
    for (size_t i = 0; (i < text.size()) && ((pos + 1U) < s_desc.size()); ++i) {
        s_desc[pos] = text[i];
        ++pos;
    }
    s_desc[pos] = '\0';
    return pos;
}

/** 追加固定位数的十六进制（大写，无前缀）；digits ≤ 4，位运算不越界。 */
size_t descHex(size_t pos, uint32_t value, size_t digits)
{
    for (size_t i = digits; i > 0; --i) {
        const uint32_t nibble = (value >> ((i - 1U) * 4U)) & 0x0FU;
        const char digit = (nibble < 10U) ? static_cast<char>('0' + nibble)
                                          : static_cast<char>('A' + (nibble - 10U));
        const std::array<char, 2> one = {digit, '\0'};
        pos = descText(pos, one.data());
    }
    return pos;
}

/** 失败信息："id=0x0044 role=PHONE type=0x02 payload=pair_open1"。不用 vararg（项目规范禁止）。 */
const char *describe(Role role, uint16_t msgId, uint8_t type, const char *shape)
{
    s_desc.fill('\0');
    size_t pos = descText(0, "id=0x");
    pos = descHex(pos, msgId, 4);
    pos = descText(pos, " role=");
    pos = descText(pos, roleName(role));
    pos = descText(pos, " type=0x");
    pos = descHex(pos, type, 2);
    pos = descText(pos, " payload=");
    static_cast<void>(descText(pos, shape));
    return s_desc.data();
}

/** 一次调用的可观测结果。 */
struct Outcome {
    DispatchResult result = DispatchResult::kNoReply;
    size_t len = 0;
    std::array<uint8_t, proto::kMaxFrame> bytes{};
    bool whitelistChanged = false;
    bool hasHeartbeat = false;
    ac::Heartbeat heartbeat{};
};

/** 旧实现与新实现的共同夹具：共享同一份 AlarmSource / LinkManager（两者都只持有引用）。 */
struct Harness {
    AlarmSource alarm{};
    LinkManager link{};
    CommandDispatcher oldDisp;
    svc::DeviceService dev{};
    svc::ConfigService cfg{};
    svc::AlarmService alarmSvc;
    svc::PairService pairSvc;
    CommandRouter router{};
    Reply oldReply{};
    Reply newReply{};

    Harness() noexcept : oldDisp(alarm, link), alarmSvc(alarm, link, dev.info()), pairSvc(link)
    {
        link.init(Policy::kConcurrent, {});
    }

    /** 组装根：4 张路由表 → 1 个路由器（与 main 的组装方式一致）。 */
    fw::Status initRouter() noexcept
    {
        // 旧 dispatcher 的 0x02 固定 GNSS=0x81，对照时按 GNSS 运行态比较。
        dev.info().gnssRunning = true;
        const std::array<RouteTable, 4> tables = {dev.table(), cfg.table(), alarmSvc.table(),
                                                  pairSvc.table()};
        return router.init(tables);
    }
};

void armReply(Reply &reply)
{
    reply.buf.fill(kSentinelByte);
    reply.len = kSentinelLen;
    reply.whitelistChanged = true;
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
    out.whitelistChanged = reply.whitelistChanged;
    out.hasHeartbeat = reply.heartbeat.has_value();
    if (reply.heartbeat.has_value()) {
        out.heartbeat = *reply.heartbeat;
    }
    return out;
}

void compareOutcome(const Outcome &oldOut, const Outcome &newOut, const char *desc)
{
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(static_cast<uint8_t>(oldOut.result),
                                    static_cast<uint8_t>(newOut.result), desc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(static_cast<unsigned>(oldOut.len),
                                   static_cast<unsigned>(newOut.len), desc);
    if (oldOut.len > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(oldOut.bytes.data(), newOut.bytes.data(), oldOut.len,
                                             desc);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(oldOut.whitelistChanged ? 1U : 0U,
                                    newOut.whitelistChanged ? 1U : 0U, desc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(oldOut.hasHeartbeat ? 1U : 0U, newOut.hasHeartbeat ? 1U : 0U,
                                    desc);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(oldOut.heartbeat.seq, newOut.heartbeat.seq, desc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(oldOut.heartbeat.batteryPct, newOut.heartbeat.batteryPct, desc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(oldOut.heartbeat.localState, newOut.heartbeat.localState, desc);
}

/** 一次等价比较：先跑旧 dispatcher，再跑新路由器（两者共享 alarm/link 状态）。 */
void runCase(Harness &harness, Role role, uint16_t msgId, uint8_t type, const uint8_t *payload,
             size_t len, uint32_t nowMs, const char *desc)
{
    proto::FrameView frame{};
    frame.msgId = msgId;
    frame.type = type;
    frame.sender = proto::sender::kApp;
    frame.payload = std::span<const uint8_t>(payload, len);

    armReply(harness.oldReply);
    const DispatchResult oldResult = harness.oldDisp.handle(role, frame, nowMs, harness.oldReply);
    const Outcome oldOut = observe(harness.oldReply, oldResult);

    armReply(harness.newReply);
    const DispatchResult newResult = harness.router.handle(role, frame, nowMs, harness.newReply);
    const Outcome newOut = observe(harness.newReply, newResult);

    compareOutcome(oldOut, newOut, desc);
}

/** 0x40 主动推送：旧 buildStatusPush 与 AlarmService::buildStatusPush 逐字节比较。 */
void compareStatusPush(Harness &harness, uint16_t seq, uint32_t nowMs, const char *desc)
{
    armReply(harness.oldReply);
    const bool oldOk = harness.oldDisp.buildStatusPush(seq, nowMs, harness.oldReply);
    const Outcome oldOut = observe(harness.oldReply, DispatchResult::kReply);

    armReply(harness.newReply);
    const bool newOk = harness.alarmSvc.buildStatusPush(seq, nowMs, harness.newReply);
    const Outcome newOut = observe(harness.newReply, DispatchResult::kReply);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(oldOk ? 1U : 0U, newOk ? 1U : 0U, desc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(static_cast<unsigned>(oldOut.len),
                                   static_cast<unsigned>(newOut.len), desc);
    if (oldOut.len > 0) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(oldOut.bytes.data(), newOut.bytes.data(), oldOut.len,
                                             desc);
    }
}

/** 负载形状：前 headLen 字节用显式值覆盖，其余按图案填充。 */
struct PayloadShape {
    const char *name;
    size_t len;
    size_t headLen;
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
    {"wm44", core::kWorkModeSetLen, 0, 0, 0, 0},
    {"typical50", kTypicalLen, 1, kTestSet, 0, 0},
    {"full256", proto::kMaxPayload, 1, kTestSet, 0, 0},
}};

void fillPayload(const PayloadShape &shape)
{
    for (size_t i = 0; i < s_payload.size(); ++i) {
        s_payload[i] = static_cast<uint8_t>((i * kPayloadStride) + kPayloadSeed);
    }
    if (shape.headLen > 0) {
        s_payload[0] = shape.b0;
    }
    if (shape.headLen > 1) {
        s_payload[1] = shape.b1;
    }
    if (shape.headLen > 2) {
        s_payload[2] = shape.b2;
    }
}

void testMsgIdSweep()
{
    constexpr std::array<uint8_t, 4> kTypes = {mt::kQuery, mt::kSet, mt::kPush, kUnknownType};
    // 旧 dispatcher 对 kNone/kPending 一律 kDenied；新路由器按 RoleMask 判定，同样要比一轮。
    constexpr std::array<Role, 4> kRoles = {Role::kPhone, Role::kAlarm, Role::kNone,
                                            Role::kPending};

    Harness harness;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Status::kOk),
                            static_cast<uint8_t>(harness.initRouter()));
    TEST_ASSERT_EQUAL_UINT(kRouteCount, harness.router.routeCount());

    uint32_t nowMs = kFirstNowMs;
    size_t cases = 0;
    for (uint32_t rawId = 0; (rawId <= kLastSweptMsgId); ++rawId) {
        // rawId ≤ 0xFF，uint16_t 截断安全。
        const uint16_t msgId = static_cast<uint16_t>(rawId);
        for (const Role role : kRoles) {
            for (const uint8_t type : kTypes) {
                for (const PayloadShape &shape : kShapes) {
                    fillPayload(shape);
                    if ((msgId == kMsgGga) || (msgId == kMsgSat)) {
                        proto::FrameView frame{};
                        frame.msgId = msgId;
                        frame.type = type;
                        frame.sender = proto::sender::kApp;
                        frame.payload = std::span<const uint8_t>(s_payload.data(), shape.len);
                        armReply(harness.newReply);
                        const DispatchResult res =
                            harness.router.handle(role, frame, nowMs, harness.newReply);
                        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
                            static_cast<uint8_t>(DispatchResult::kNoReply),
                            static_cast<uint8_t>(res), describe(role, msgId, type, shape.name));
                    } else {
                        runCase(harness, role, msgId, type, s_payload.data(), shape.len, nowMs,
                                describe(role, msgId, type, shape.name));
                    }
                    nowMs += kNowStep;
                    ++cases;
                }
            }
        }
    }
    // 期望用例数 = (0x00–0xFF) × 角色 × type × 负载形状。
    const size_t expected = (kLastSweptMsgId + 1U) * kRoles.size() * kTypes.size() * kShapes.size();
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(expected), static_cast<unsigned>(cases));
}

void testDeviceSequence()
{
    Harness harness;
    TEST_ASSERT_TRUE(isOk(harness.initRouter()));

    uint32_t nowMs = kDeviceNowMs;
    constexpr std::array<uint16_t, 5> kIds = {kMsgDevInfo, kMsgDevState, kMsgBattery, kMsgSatTrack,
                                              kMsgCom};
    // 1. 默认值（构造函数默认值必须与旧 dispatcher 相同）。
    for (const uint16_t id : kIds) {
        runCase(harness, Role::kPhone, id, mt::kQuery, s_payload.data(), 0, nowMs,
                describe(Role::kPhone, id, mt::kQuery, "default"));
        nowMs += kNowStep;
    }

    // 2. 两侧显式写入同一组设备信息（golden 前提）。
    harness.oldDisp.info().sn.assign("SN0001");
    harness.dev.info().sn.assign("SN0001");
    harness.oldDisp.info().fwVer.assign("V6.1.0");
    harness.dev.info().fwVer.assign("V6.1.0");
    harness.oldDisp.info().boardSn.assign("B0001");
    harness.dev.info().boardSn.assign("B0001");
    harness.oldDisp.info().boardFw.assign("R1.0");
    harness.dev.info().boardFw.assign("R1.0");
    harness.oldDisp.info().radioFw.assign("RF1");
    harness.dev.info().radioFw.assign("RF1");
    harness.oldDisp.info().onBattery = false;
    harness.dev.info().onBattery = false;
    harness.oldDisp.info().batteryPct = kBatteryPct;
    harness.dev.info().batteryPct = kBatteryPct;
    harness.oldDisp.info().batteryTempC = kBatteryTempC;
    harness.dev.info().batteryTempC = kBatteryTempC;
    harness.oldDisp.info().comBaud = kComBaud;
    harness.dev.info().comBaud = kComBaud;

    for (const uint16_t id : {kMsgDevInfo, kMsgBattery, kMsgCom}) {
        runCase(harness, Role::kPhone, id, mt::kQuery, s_payload.data(), 0, nowMs,
                describe(Role::kPhone, id, mt::kQuery, "custom"));
        nowMs += kNowStep;
    }
    // 0x06 与 0x0D 的 SET 分支（0x0D 按 5.4 行为不回复）。
    runCase(harness, Role::kPhone, kMsgSatTrack, mt::kSet, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, kMsgSatTrack, mt::kSet, "none"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgCom, mt::kSet, s_payload.data(), core::kComLen, nowMs,
            describe(Role::kPhone, kMsgCom, mt::kSet, "32B"));
}

void testConfigSequence()
{
    Harness harness;
    TEST_ASSERT_TRUE(isOk(harness.initRouter()));

    uint32_t nowMs = kConfigNowMs;
    for (size_t i = 0; i < s_payload.size(); ++i) {
        s_payload[i] = static_cast<uint8_t>((i * kPayloadStride) + kPayloadSeed);
    }

    // 0x07：SET 恰好 44 字节成功，QUERY 回读 48 字节，短包失败且不回滚。
    runCase(harness, Role::kPhone, kMsgWorkMode, mt::kSet, s_payload.data(), core::kWorkModeSetLen,
            nowMs, describe(Role::kPhone, kMsgWorkMode, mt::kSet, "44B"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgWorkMode, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, kMsgWorkMode, mt::kQuery, "readback"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgWorkMode, mt::kSet, s_payload.data(),
            core::kWorkModeSetLen - 1U, nowMs,
            describe(Role::kPhone, kMsgWorkMode, mt::kSet, "43B"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgWorkMode, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, kMsgWorkMode, mt::kQuery, "readback2"));
    nowMs += kNowStep;

    // 0x14：SET 15 字节成功（第 17 字节保持默认 0x05），满 28 字节覆盖全部字段。
    runCase(harness, Role::kPhone, kMsgRadio, mt::kSet, s_payload.data(), core::kRadioSetMinLen,
            nowMs, describe(Role::kPhone, kMsgRadio, mt::kSet, "15B"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgRadio, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, kMsgRadio, mt::kQuery, "readback"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgRadio, mt::kSet, s_payload.data(),
            core::kRadioSetMinLen - 1U, nowMs, describe(Role::kPhone, kMsgRadio, mt::kSet, "14B"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgRadio, mt::kSet, s_payload.data(), core::kRadioLen, nowMs,
            describe(Role::kPhone, kMsgRadio, mt::kSet, "28B"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgRadio, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, kMsgRadio, mt::kQuery, "readback2"));
    nowMs += kNowStep;

    // 0x15：SET 50 字节成功，49 字节失败；非 QUERY/SET 的 type 走只读分支。
    runCase(harness, Role::kPhone, kMsgLog, mt::kSet, s_payload.data(), core::kLogLen, nowMs,
            describe(Role::kPhone, kMsgLog, mt::kSet, "50B"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgLog, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, kMsgLog, mt::kQuery, "readback"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgLog, mt::kSet, s_payload.data(), core::kLogLen - 1U, nowMs,
            describe(Role::kPhone, kMsgLog, mt::kSet, "49B"));
    nowMs += kNowStep;
    runCase(harness, Role::kPhone, kMsgLog, mt::kPush, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, kMsgLog, mt::kPush, "push"));
}

void testPairSequence()
{
    Harness harness;
    TEST_ASSERT_TRUE(isOk(harness.initRouter()));

    Actions act;
    uint32_t nowMs = kPairNowMs;
    const std::array<uint8_t, 1> pairOpen = {kPairOpen};
    const std::array<uint8_t, 1> pairClear = {kPairClear};
    const std::array<uint8_t, 1> pairBad = {kPairBadAction};

    // 1. 打开配对窗口。
    runCase(harness, Role::kPhone, ac::kMsgPair, mt::kSet, pairOpen.data(), pairOpen.size(), nowMs,
            describe(Role::kPhone, ac::kMsgPair, mt::kSet, "open"));
    TEST_ASSERT_TRUE(harness.link.isPairing());
    nowMs += kNowStep;

    // 2. 报警器先以 PHONE 接入（地址还不在白名单），HELLO 之后转 ALARM 并加入白名单。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(Role::kPhone),
        static_cast<uint8_t>(harness.link.onOpen(kAlarmHandle, kAlarmAddr, nowMs, act)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(Role::kAlarm),
        static_cast<uint8_t>(harness.link.onFrame(kAlarmHandle, proto::sender::kAlarm,
                                                  core::kHelloMsgId, nowMs, act)));
    TEST_ASSERT_EQUAL_UINT(1, harness.link.whitelist().size());
    nowMs += kNowStep;

    // 3. 查询：1 字节数量 + 6 字节 MAC。
    runCase(harness, Role::kPhone, ac::kMsgPair, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, ac::kMsgPair, mt::kQuery, "1addr"));
    nowMs += kNowStep;

    // 4. 清空白名单：whitelistChanged = true。
    runCase(harness, Role::kPhone, ac::kMsgPair, mt::kSet, pairClear.data(), pairClear.size(),
            nowMs, describe(Role::kPhone, ac::kMsgPair, mt::kSet, "clear"));
    TEST_ASSERT_TRUE(harness.oldReply.whitelistChanged);
    TEST_ASSERT_TRUE(harness.newReply.whitelistChanged);
    TEST_ASSERT_EQUAL_UINT(0, harness.link.whitelist().size());
    nowMs += kNowStep;

    runCase(harness, Role::kPhone, ac::kMsgPair, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, ac::kMsgPair, mt::kQuery, "0addr"));
    nowMs += kNowStep;

    // 5. 非法动作字节 → 失败应答。
    runCase(harness, Role::kPhone, ac::kMsgPair, mt::kSet, pairBad.data(), pairBad.size(), nowMs,
            describe(Role::kPhone, ac::kMsgPair, mt::kSet, "bad_action"));
}

void testAlarmSequence()
{
    Harness harness;
    TEST_ASSERT_TRUE(isOk(harness.initRouter()));

    Actions act;
    uint32_t nowMs = kAlarmNowMs;

    // 1. 手机在线 → 状态帧 flags bit0。
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(Role::kPhone),
        static_cast<uint8_t>(harness.link.onOpen(kPhoneHandle, kPhoneAddr, nowMs, act)));
    TEST_ASSERT_TRUE(harness.link.hasRole(Role::kPhone));

    // 2. 无覆盖：推送与查询。
    compareStatusPush(harness, 0x0000, nowMs, "push idle");
    nowMs += kNowStep;
    runCase(harness, Role::kAlarm, ac::kMsgStatus, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kAlarm, ac::kMsgStatus, mt::kQuery, "idle"));
    nowMs += kNowStep;

    // 3. 设置测试覆盖：alert / level 3。
    const std::array<uint8_t, 3> testSet = {kTestSet, kStateAlert, kTestLevel};
    runCase(harness, Role::kPhone, ac::kMsgTest, mt::kSet, testSet.data(), testSet.size(), nowMs,
            describe(Role::kPhone, ac::kMsgTest, mt::kSet, "set"));
    TEST_ASSERT_TRUE(harness.alarm.isOverride());
    TEST_ASSERT_EQUAL_HEX8(kStateAlert, harness.alarm.state());
    nowMs += kNowStep;

    // 4. 覆盖生效后的查询与推送。
    runCase(harness, Role::kAlarm, ac::kMsgStatus, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kAlarm, ac::kMsgStatus, mt::kQuery, "override"));
    nowMs += kNowStep;
    compareStatusPush(harness, kPushSeq, nowMs, "push override");
    nowMs += kNowStep;

    // 5. 心跳：解析结果写 reply.heartbeat，不回复。
    const std::array<uint8_t, ac::kHeartbeatLen> heartbeat = {
        0x07, 0x00, kHeartbeatPct, kHeartbeatState, 0x00, 0x00, 0x00, 0x00};
    runCase(harness, Role::kAlarm, ac::kMsgHeartbeat, mt::kPush, heartbeat.data(), heartbeat.size(),
            nowMs, describe(Role::kAlarm, ac::kMsgHeartbeat, mt::kPush, "hb"));
    TEST_ASSERT_TRUE(harness.oldReply.heartbeat.has_value());
    TEST_ASSERT_TRUE(harness.newReply.heartbeat.has_value());
    TEST_ASSERT_EQUAL_HEX16(kHeartbeatSeq, harness.newReply.heartbeat->seq);
    TEST_ASSERT_EQUAL_UINT8(kHeartbeatPct, harness.newReply.heartbeat->batteryPct);
    TEST_ASSERT_EQUAL_UINT8(kHeartbeatState, harness.newReply.heartbeat->localState);
    nowMs += kNowStep;

    // 6. 心跳之后（电池变化）再比一次推送。
    harness.oldDisp.info().batteryPct = kPushBatteryPct;
    harness.dev.info().batteryPct = kPushBatteryPct;
    compareStatusPush(harness, kPushSeq2, nowMs, "push after heartbeat");
    nowMs += kNowStep;

    // 7. 清除覆盖。
    const std::array<uint8_t, 3> testClear = {kTestClear, kStateAlert, kTestLevel};
    runCase(harness, Role::kPhone, ac::kMsgTest, mt::kSet, testClear.data(), testClear.size(),
            nowMs, describe(Role::kPhone, ac::kMsgTest, mt::kSet, "clear"));
    TEST_ASSERT_FALSE(harness.alarm.isOverride());
    nowMs += kNowStep;

    // 8. 非法状态 → 失败且状态不变。
    const std::array<uint8_t, 3> testBad = {kTestSet, kIllegalState, kTestLevel};
    runCase(harness, Role::kPhone, ac::kMsgTest, mt::kSet, testBad.data(), testBad.size(), nowMs,
            describe(Role::kPhone, ac::kMsgTest, mt::kSet, "bad_state"));
    TEST_ASSERT_FALSE(harness.alarm.isOverride());
    nowMs += kNowStep;

    // 9. 围栏（预留）与 0x40 / 0x41 的 type 限制。
    runCase(harness, Role::kPhone, ac::kMsgFence, mt::kQuery, s_payload.data(), 0, nowMs,
            describe(Role::kPhone, ac::kMsgFence, mt::kQuery, "reserved"));
    nowMs += kNowStep;
    runCase(harness, Role::kAlarm, ac::kMsgStatus, mt::kSet, s_payload.data(), 0, nowMs,
            describe(Role::kAlarm, ac::kMsgStatus, mt::kSet, "type_denied"));
    nowMs += kNowStep;
    runCase(harness, Role::kAlarm, ac::kMsgHeartbeat, mt::kQuery, heartbeat.data(),
            heartbeat.size(), nowMs,
            describe(Role::kAlarm, ac::kMsgHeartbeat, mt::kQuery, "type_denied"));
    nowMs += kNowStep;
    compareStatusPush(harness, kPushSeq3, nowMs, "push final");
}

} // namespace

// Unity 默认运行器会在每个用例前后调用这两个符号（外部链接，与 test/host 同样写法）。
// 每个用例自建 Harness，因此这里不需要复位状态。
extern "C" void setUp(void) {}

extern "C" void tearDown(void) {}

/** 0x02 GNSS 字节随运行态变化：kRunning→0x81，其它→0x01；其余字节与旧 dispatcher 一致。 */
void testDevStateGnssByte()
{
    Harness harness;
    TEST_ASSERT_TRUE(isOk(harness.initRouter()));
    proto::FrameView frame{};
    frame.msgId = kMsgDevState;
    frame.type = mt::kQuery;
    frame.sender = proto::sender::kApp;
    constexpr size_t kGnssByte = proto::kHeaderLen + 2U;
    for (const bool running : {true, false}) {
        harness.dev.info().gnssRunning = running;
        armReply(harness.oldReply);
        const Outcome oldOut =
            observe(harness.oldReply,
                    harness.oldDisp.handle(Role::kPhone, frame, kDeviceNowMs, harness.oldReply));
        armReply(harness.newReply);
        const Outcome newOut =
            observe(harness.newReply,
                    harness.router.handle(Role::kPhone, frame, kDeviceNowMs, harness.newReply));
        TEST_ASSERT_EQUAL_UINT(oldOut.len, newOut.len);
        TEST_ASSERT_EQUAL_HEX8(running ? 0x81U : 0x01U, newOut.bytes[kGnssByte]);
        // 只比较 header+payload，CRC 随 GNSS 字节变化。
        for (size_t i = 0; i + proto::kCrcLen < newOut.len; ++i) {
            if (i != kGnssByte) {
                TEST_ASSERT_EQUAL_HEX8(oldOut.bytes[i], newOut.bytes[i]);
            }
        }
    }
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testMsgIdSweep);
    RUN_TEST(testDeviceSequence);
    RUN_TEST(testDevStateGnssByte);
    RUN_TEST(testConfigSequence);
    RUN_TEST(testPairSequence);
    RUN_TEST(testAlarmSequence);
    return UNITY_END();
}
