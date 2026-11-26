// 4G 链路 FSM 黑盒测试（ADR-0021 §2.5/§2.9）：用假 EC600U 应答器驱动 AtEngine + LinkFsm，
// 覆盖 Off→Ready 全路径、L1/L2/L3 触发与退避上限、SIM/注册异常、通道失效、关机序列与上电门控。
// 时间由测试推进（10 ms 一步），不依赖真实时钟。

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "cellular/AtEngine.hpp"
#include "cellular/LinkFsm.hpp"
#include "unity.h"

namespace {

using cellular::AtEngine;
using cellular::LinkFsm;
using cellular::Phase;
using cellular::RecoveryLevel;
using cellular::RegStatus;
using cellular::SimStatus;

constexpr uint32_t kStepMs = 10U;
constexpr uint32_t kRespondMs = 10U; // 假模块应答延迟

/** 假 EC600U：按命令文本应答，可配置故障。 */
struct Sim final {
    AtEngine engine;
    LinkFsm fsm;
    uint32_t now = 0U;

    // 模块侧状态与故障开关。
    bool powered = false;
    uint32_t poweredAtMs = 0U;
    uint32_t bootDelayMs = 0U;
    uint32_t moduleBaud = 115200U;
    bool resetBaudOnPower = true;
    bool silent = false;
    bool echo = true;
    bool iprFail = false;
    bool simAbsent = false;
    int regStat = 1;
    int qiactFails = 0;
    bool ipActive = false;
    bool poweredDownUrc = true;
    int qiopenErr = 0;
    bool autoUrcOpen = true; // QIOPEN OK 后自动给 +QIOPEN URC
    std::string rxData;      // 模块侧待读数据

    // 主机侧观测。
    uint32_t hostBaud = 115200U;
    std::string inbuf;
    size_t payloadWait = 0U;
    std::string payloadGot;
    std::vector<std::string> payloads;
    std::vector<std::string> cmds;
    std::vector<uint32_t> cmdTimes;
    struct PowerEv final {
        uint32_t t;
        bool on;
    };
    std::vector<PowerEv> power;
    std::vector<uint32_t> baudCalls;
    std::vector<std::string> events;
    std::string rxCollected;

    struct Out final {
        uint32_t due;
        std::string text;
        bool powerDown;
    };
    std::vector<Out> outq;
};

/** 排队一段模块输出。 */
void schedule(Sim &s, const std::string &text, uint32_t delayMs, bool powerDown = false)
{
    s.outq.push_back({s.now + delayMs, text, powerDown});
}

/** 一条命令的应答（含回显与结尾 OK）。 */
void respondOk(Sim &s, const std::string &cmd, const std::string &body = "")
{
    std::string out;
    if (s.echo) {
        out += cmd + "\r";
    }
    out += "\r\n" + body + "\r\nOK\r\n";
    schedule(s, out, kRespondMs);
}

/** 一条命令的 ERROR 类应答。 */
void respondErr(Sim &s, const std::string &cmd, const std::string &err)
{
    std::string out;
    if (s.echo) {
        out += cmd + "\r";
    }
    out += "\r\n" + err + "\r\n";
    schedule(s, out, kRespondMs);
}

/** 假模块处理一条完整命令行。 */
void handleCommand(Sim &s, const std::string &cmd)
{
    s.cmds.push_back(cmd);
    s.cmdTimes.push_back(s.now);
    if (cmd == "AT") {
        respondOk(s, cmd);
    } else if (cmd == "ATE0") {
        respondOk(s, cmd);
        s.echo = false;
    } else if (cmd == "AT+CMEE=2") {
        respondOk(s, cmd);
    } else if (cmd == "AT+IPR=460800") {
        if (s.iprFail) {
            respondErr(s, cmd, "ERROR");
        } else {
            respondOk(s, cmd);
            s.moduleBaud = 460800U;
        }
    } else if (cmd == "AT+GSN") {
        respondOk(s, cmd, "860000000000001");
    } else if (cmd == "AT+QCCID") {
        respondOk(s, cmd, "+QCCID: 89860000000000000001");
    } else if (cmd == "AT+CPIN?") {
        if (s.simAbsent) {
            respondErr(s, cmd, "+CME ERROR: 10");
        } else {
            respondOk(s, cmd, "+CPIN: READY");
        }
    } else if (cmd == "AT+CEREG?") {
        respondOk(s, cmd, "+CEREG: 0," + std::to_string(s.regStat));
    } else if (cmd == "AT+CREG?") {
        respondOk(s, cmd, "+CREG: 0," + std::to_string(s.regStat));
    } else if (cmd.rfind("AT+QICSGP=", 0) == 0) {
        respondOk(s, cmd);
    } else if (cmd == "AT+QIACT?") {
        respondOk(s, cmd, s.ipActive ? "+QIACT: 1,1,1,\"10.0.0.2\"" : "");
    } else if (cmd == "AT+QIACT=1") {
        if (s.qiactFails > 0) {
            --s.qiactFails;
            respondErr(s, cmd, "ERROR");
        } else {
            s.ipActive = true;
            respondOk(s, cmd);
        }
    } else if (cmd == "AT+QIDEACT=1") {
        s.ipActive = false;
        respondOk(s, cmd);
    } else if (cmd == "AT+CSQ") {
        respondOk(s, cmd, "+CSQ: 21,99");
    } else if (cmd.rfind("AT+QIOPEN=", 0) == 0) {
        respondOk(s, cmd);
        if (s.autoUrcOpen) {
            const int id = cmd[10] == '1' ? (cmd[12] - '0') : 0; // "AT+QIOPEN=1,<id>,..."
            schedule(s,
                     "\r\n+QIOPEN: " + std::to_string(id) + "," + std::to_string(s.qiopenErr) +
                         "\r\n",
                     100U);
        }
    } else if (cmd.rfind("AT+QICLOSE=", 0) == 0) {
        respondOk(s, cmd);
    } else if (cmd.rfind("AT+QIRD=", 0) == 0) {
        if (s.rxData.empty()) {
            respondOk(s, cmd, "+QIRD: 0");
        } else {
            std::string body = "+QIRD: " + std::to_string(s.rxData.size()) + "\r\n" + s.rxData;
            s.rxData.clear();
            respondOk(s, cmd, body);
        }
    } else if (cmd.rfind("AT+QISEND=", 0) == 0) {
        s.payloadWait = static_cast<size_t>(std::stoul(cmd.substr(cmd.find(',') + 1U)));
        s.payloadGot.clear();
        schedule(s, "\r\n> ", kRespondMs);
    } else if (cmd == "AT+QPOWD=1") {
        respondOk(s, cmd);
        if (s.poweredDownUrc) {
            schedule(s, "\r\nPOWERED DOWN\r\n", 300U, true);
        }
    } else {
        respondErr(s, cmd, "ERROR");
    }
}

/** 引擎写钩子：模块只在上电、启动完成、波特率一致且非静默时处理。 */
bool writeHook(void *ctx, std::span<const uint8_t> bytes) noexcept
{
    auto *s = static_cast<Sim *>(ctx);
    const bool alive = s->powered && !s->silent && ((s->now - s->poweredAtMs) >= s->bootDelayMs) &&
                       (s->hostBaud == s->moduleBaud);
    if (!alive) {
        return true;
    }
    for (const uint8_t b : bytes) {
        if (s->payloadWait > 0U) {
            s->payloadGot.push_back(static_cast<char>(b));
            if (--s->payloadWait == 0U) {
                s->payloads.push_back(s->payloadGot);
                schedule(*s, "\r\nSEND OK\r\n", kRespondMs);
            }
        } else if (b == '\r') {
            if (!s->inbuf.empty()) {
                const std::string cmd = s->inbuf;
                s->inbuf.clear();
                handleCommand(*s, cmd);
            }
        } else if (b != '\n') {
            s->inbuf.push_back(static_cast<char>(b));
        }
    }
    return true;
}

/** URC 钩子：转给 FSM。 */
void urcHook(void *ctx, std::string_view line) noexcept
{
    auto *s = static_cast<Sim *>(ctx);
    s->fsm.onUrc(line, s->now);
}

/** PWREN 钩子。 */
void powerHook(void *ctx, bool on) noexcept
{
    auto *s = static_cast<Sim *>(ctx);
    s->power.push_back({s->now, on});
    s->outq.clear();
    s->inbuf.clear();
    s->payloadWait = 0U;
    if (on) {
        s->powered = true;
        s->poweredAtMs = s->now;
        s->echo = true;
        s->ipActive = false;
        if (s->resetBaudOnPower) {
            s->moduleBaud = 115200U;
        }
    } else {
        s->powered = false;
    }
}

/** 波特率钩子。 */
bool baudHook(void *ctx, uint32_t baud) noexcept
{
    auto *s = static_cast<Sim *>(ctx);
    s->hostBaud = baud;
    s->baudCalls.push_back(baud);
    return true;
}

void onOpened(void *ctx, uint8_t id, bool ok) noexcept
{
    static_cast<Sim *>(ctx)->events.push_back("open" + std::to_string(id) + (ok ? ":1" : ":0"));
}

void onClosed(void *ctx, uint8_t id) noexcept
{
    static_cast<Sim *>(ctx)->events.push_back("closed" + std::to_string(id));
}

void onData(void *ctx, uint8_t id, std::span<const uint8_t> data) noexcept
{
    static_cast<void>(id);
    static_cast<Sim *>(ctx)->rxCollected.append(reinterpret_cast<const char *>(data.data()),
                                                data.size());
}

void onSendDone(void *ctx, uint8_t id, bool ok) noexcept
{
    static_cast<Sim *>(ctx)->events.push_back("sent" + std::to_string(id) + (ok ? ":1" : ":0"));
}

/** 初始化仿真：绑定引擎与 FSM，使能并允许上电。 */
void setup(Sim &s)
{
    cellular::AtPort port;
    port.ctx = &s;
    port.write = &writeHook;
    port.onUrc = &urcHook;
    port.urcPrefixes = std::span<const std::string_view>(cellular::kUrcPrefixes);
    s.engine.bind(port);
    s.engine.setDeadThreshold(cellular::kPolicy.deadTimeouts);
    cellular::LinkHooks hooks;
    hooks.ctx = &s;
    hooks.setPower = &powerHook;
    hooks.setBaud = &baudHook;
    cellular::ConnEvents ev;
    ev.ctx = &s;
    ev.onOpened = &onOpened;
    ev.onClosed = &onClosed;
    ev.onData = &onData;
    ev.onSendDone = &onSendDone;
    s.fsm.bind(s.engine, hooks, ev);
    s.fsm.setEnabled(true);
    s.fsm.setPowerAllowed(true);
}

/** 推进一步：投递到期输出、引擎与 FSM 轮询。 */
void step(Sim &s)
{
    s.now += kStepMs;
    for (size_t i = 0U; i < s.outq.size();) {
        if (static_cast<int32_t>(s.now - s.outq[i].due) >= 0) {
            const Sim::Out out = s.outq[i];
            s.outq.erase(s.outq.begin() + static_cast<std::ptrdiff_t>(i));
            if (out.powerDown) {
                s.powered = false;
            }
            s.engine.feed(std::span<const uint8_t>(
                              reinterpret_cast<const uint8_t *>(out.text.data()), out.text.size()),
                          s.now);
        } else {
            ++i;
        }
    }
    s.engine.poll(s.now);
    s.fsm.poll(s.now);
}

/** 推进指定毫秒。 */
void runFor(Sim &s, uint32_t ms)
{
    const uint32_t end = s.now + ms;
    while (static_cast<int32_t>(end - s.now) > 0) {
        step(s);
    }
}

/** 推进直到条件成立或超时。 */
template <typename F> bool runUntil(Sim &s, uint32_t limitMs, F pred)
{
    const uint32_t end = s.now + limitMs;
    while (!pred()) {
        if (static_cast<int32_t>(end - s.now) <= 0) {
            return false;
        }
        step(s);
    }
    return true;
}

/** 统计以 prefix 开头的命令条数。 */
size_t countCmds(const Sim &s, std::string_view prefix)
{
    size_t n = 0U;
    for (const std::string &c : s.cmds) {
        if (std::string_view{c}.starts_with(prefix)) {
            ++n;
        }
    }
    return n;
}

/** 取第 nth 条以 prefix 开头命令的下标，没有返回 -1。 */
int indexOfCmd(const Sim &s, std::string_view prefix, size_t nth = 0U)
{
    size_t seen = 0U;
    for (size_t i = 0U; i < s.cmds.size(); ++i) {
        if (std::string_view{s.cmds[i]}.starts_with(prefix)) {
            if (seen == nth) {
                return static_cast<int>(i);
            }
            ++seen;
        }
    }
    return -1;
}

/** 计 PWREN 拉低次数。 */
size_t powerOffCount(const Sim &s)
{
    size_t n = 0U;
    for (const auto &ev : s.power) {
        if (!ev.on) {
            ++n;
        }
    }
    return n;
}

/** 跑到 Ready。 */
bool toReady(Sim &s, uint32_t limitMs = 20000U)
{
    return runUntil(s, limitMs, [&s]() { return s.fsm.phase() == Phase::kReady; });
}

/** 跑到 Ready 且打开连接 0（URC 已回）。 */
bool toConnOpen(Sim &s)
{
    if (!toReady(s)) {
        return false;
    }
    if (!s.fsm.openConn(0U, "ntrip.example.com", 2101U)) {
        return false;
    }
    return runUntil(s, 2000U, [&s]() { return s.fsm.connOpen(0U); });
}

void testBackoffTables()
{
    using cellular::backoffMs;
    using cellular::kPolicy;
    const std::array<uint32_t, 7> l1{2000U, 4000U, 8000U, 16000U, 32000U, 60000U, 60000U};
    for (size_t i = 0U; i < l1.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT32(l1[i], backoffMs(kPolicy.l1, static_cast<uint8_t>(i)));
    }
    const std::array<uint32_t, 6> l2{10000U, 20000U, 40000U, 80000U, 120000U, 120000U};
    for (size_t i = 0U; i < l2.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT32(l2[i], backoffMs(kPolicy.l2, static_cast<uint8_t>(i)));
    }
    const std::array<uint32_t, 6> l3{30000U, 60000U, 120000U, 300000U, 300000U, 300000U};
    for (size_t i = 0U; i < l3.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT32(l3[i], cellular::l3BackoffMs(static_cast<uint8_t>(i)));
    }
    TEST_ASSERT_EQUAL_UINT32(300000U, cellular::l3BackoffMs(255U));
}

void testHappyPathToReady()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    const std::array<const char *, 11> expect{
        "AT",       "ATE0",     "AT+CMEE=2", "AT+IPR=460800", "AT",        "AT+GSN",
        "AT+QCCID", "AT+CPIN?", "AT+CEREG?", "AT+QIACT?",     "AT+QIACT=1"};
    for (size_t i = 0U; i < expect.size(); ++i) {
        TEST_ASSERT_TRUE(i < s.cmds.size());
        TEST_ASSERT_EQUAL_STRING(expect[i], s.cmds[i].c_str());
    }
    TEST_ASSERT_EQUAL_INT(-1, indexOfCmd(s, "AT+QICSGP")); // APN 为空：不下发
    const cellular::LinkSnapshot snap = s.fsm.snapshot(s.now);
    TEST_ASSERT_EQUAL(Phase::kReady, snap.phase);
    TEST_ASSERT_EQUAL(SimStatus::kReady, snap.sim);
    TEST_ASSERT_EQUAL(RegStatus::kHome, snap.reg);
    TEST_ASSERT_TRUE(snap.pdpUp);
    TEST_ASSERT_EQUAL_UINT32(460800U, snap.baud);
    TEST_ASSERT_EQUAL_STRING("860000000000001", snap.imei.data());
    TEST_ASSERT_EQUAL_STRING("89860000000000000001", snap.iccid.data());
    TEST_ASSERT_EQUAL(RecoveryLevel::kNone, snap.level);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.power.size()));
}

void testApnConfigCommand()
{
    Sim s;
    setup(s);
    s.fsm.setConfig("cmnet", "u", "p", 1U);
    TEST_ASSERT_TRUE(toReady(s));
    const int apn = indexOfCmd(s, "AT+QICSGP=");
    TEST_ASSERT_TRUE(apn > 0);
    TEST_ASSERT_EQUAL_STRING("AT+QICSGP=1,1,\"cmnet\",\"u\",\"p\",1",
                             s.cmds[static_cast<size_t>(apn)].c_str());
    TEST_ASSERT_TRUE(apn > indexOfCmd(s, "AT+CEREG?"));
    TEST_ASSERT_TRUE(apn < indexOfCmd(s, "AT+QIACT?"));
}

void testIprFailureStaysAtBootBaud()
{
    Sim s;
    setup(s);
    s.iprFail = true;
    TEST_ASSERT_TRUE(toReady(s));
    TEST_ASSERT_EQUAL_UINT32(115200U, s.fsm.snapshot(s.now).baud);
    for (const uint32_t b : s.baudCalls) {
        TEST_ASSERT_NOT_EQUAL(460800U, b);
    }
}

void testModuleAlreadyFastBaudSwapsProbe()
{
    Sim s;
    setup(s);
    s.moduleBaud = 460800U;
    s.resetBaudOnPower = false;
    TEST_ASSERT_TRUE(toReady(s));
    TEST_ASSERT_EQUAL_INT(-1, indexOfCmd(s, "AT+IPR")); // 已在 460800：无需再切
    TEST_ASSERT_EQUAL_UINT32(460800U, s.fsm.snapshot(s.now).baud);
}

void testSlowBootStillSyncsWithinWindow()
{
    Sim s;
    setup(s);
    s.bootDelayMs = 4000U;
    TEST_ASSERT_TRUE(toReady(s));
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.power.size())); // 窗口内同步成功，没有 L3
    TEST_ASSERT_TRUE(countCmds(s, "AT") > 3U);
}

void testSyncWindowTimeoutEscalatesL3WithCappedBackoff()
{
    Sim s;
    setup(s);
    s.silent = true;
    // 连续 5 次 L3：保持时间 30/60/120/300/300 s（≥ 2 s 下限）。
    TEST_ASSERT_TRUE(runUntil(s, 1000000U, [&s]() { return s.power.size() >= 11U; }));
    const std::array<uint32_t, 5> hold{30000U, 60000U, 120000U, 300000U, 300000U};
    for (size_t i = 0U; i < hold.size(); ++i) {
        const uint32_t offAt = s.power[1U + (2U * i)].t;
        const uint32_t onAt = s.power[2U + (2U * i)].t;
        TEST_ASSERT_FALSE(s.power[1U + (2U * i)].on);
        TEST_ASSERT_TRUE(s.power[2U + (2U * i)].on);
        TEST_ASSERT_TRUE((onAt - offAt) >= hold[i]);
        TEST_ASSERT_TRUE((onAt - offAt) <= (hold[i] + 50U));
    }
    // 每次窗口约 10 s（含最后一条命令的 300 ms 超时），不早于 10 s。
    TEST_ASSERT_TRUE((s.power[1].t - s.power[0].t) >= 10000U);
    TEST_ASSERT_TRUE((s.power[1].t - s.power[0].t) <= 11000U);
    TEST_ASSERT_EQUAL(RecoveryLevel::kL3Module, s.fsm.snapshot(s.now).level);
    TEST_ASSERT_TRUE(s.fsm.snapshot(s.now).recoveryCount >= 5U);
}

void testPowerCycleBackoffVisibleInSnapshot()
{
    Sim s;
    setup(s);
    s.silent = true;
    TEST_ASSERT_TRUE(runUntil(s, 20000U, [&s]() { return s.fsm.phase() == Phase::kPowerCycle; }));
    const cellular::LinkSnapshot snap = s.fsm.snapshot(s.now);
    TEST_ASSERT_TRUE(snap.backoffRemainingMs > 29000U);
    TEST_ASSERT_TRUE(snap.backoffRemainingMs <= 30000U);
    TEST_ASSERT_EQUAL(RecoveryLevel::kL3Module, snap.level);
    TEST_ASSERT_EQUAL_UINT32(1U, snap.recoveryCount);
}

void testSimAbsentStaysInSimCheckThenRecoveryIsL3()
{
    Sim s;
    setup(s);
    s.simAbsent = true;
    runFor(s, 40000U);
    TEST_ASSERT_EQUAL(Phase::kSimCheck, s.fsm.phase()); // 无卡不触发恢复
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.power.size()));
    TEST_ASSERT_EQUAL(SimStatus::kAbsent, s.fsm.snapshot(s.now).sim);
    TEST_ASSERT_EQUAL(RecoveryLevel::kNone, s.fsm.snapshot(s.now).level);
    // 插卡：模块需整机重启才能重新读卡 → L3，随后到 Ready。
    s.simAbsent = false;
    TEST_ASSERT_TRUE(runUntil(s, 5000U, [&s]() { return powerOffCount(s) == 1U; }));
    TEST_ASSERT_EQUAL(RecoveryLevel::kL3Module, s.fsm.snapshot(s.now).level);
    TEST_ASSERT_TRUE(toReady(s, 60000U));
}

void testRegisterTimeoutIs180sThenL3()
{
    Sim s;
    setup(s);
    s.regStat = 2; // 搜网中，永不注册
    TEST_ASSERT_TRUE(runUntil(s, 20000U, [&s]() { return s.fsm.phase() == Phase::kRegister; }));
    const uint32_t regAt = s.now;
    TEST_ASSERT_TRUE(runUntil(s, 200000U, [&s]() { return powerOffCount(s) == 1U; }));
    const uint32_t offAt = s.power[1].t;
    TEST_ASSERT_TRUE((offAt - regAt) >= 179990U);
    TEST_ASSERT_TRUE((offAt - regAt) <= 180100U);
    TEST_ASSERT_EQUAL(RegStatus::kUnknown, s.fsm.snapshot(s.now).reg);
}

void testCregFallbackWhenCeregNotRegistered()
{
    Sim s;
    setup(s);
    s.regStat = 5; // 漫游
    TEST_ASSERT_TRUE(toReady(s));
    TEST_ASSERT_EQUAL(RegStatus::kRoaming, s.fsm.snapshot(s.now).reg);
    s.cmds.clear();
    Sim t;
    setup(t);
    t.regStat = 3; // 拒绝
    runFor(t, 5000U);
    TEST_ASSERT_TRUE(countCmds(t, "AT+CREG?") > 0U); // CEREG 未注册时补查 CREG
    TEST_ASSERT_EQUAL(Phase::kRegister, t.fsm.phase());
}

void testQiactFailureL2ThenSuccess()
{
    Sim s;
    setup(s);
    s.qiactFails = 2;
    TEST_ASSERT_TRUE(runUntil(
        s, 20000U, [&s]() { return s.fsm.snapshot(s.now).level == RecoveryLevel::kL2Bearer; }));
    const uint32_t firstFail = s.now;
    const cellular::LinkSnapshot snap = s.fsm.snapshot(s.now);
    TEST_ASSERT_EQUAL(Phase::kPdpUp, snap.phase);
    TEST_ASSERT_TRUE(snap.backoffRemainingMs > 9000U);
    TEST_ASSERT_TRUE(snap.backoffRemainingMs <= 10000U);
    TEST_ASSERT_TRUE(toReady(s, 120000U));
    TEST_ASSERT_TRUE((s.now - firstFail) >= 10000U + 20000U); // 10 s、20 s 两级退避
    TEST_ASSERT_EQUAL_UINT32(2U, s.fsm.snapshot(s.now).recoveryCount);
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(countCmds(s, "AT+QIDEACT=1")));
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.power.size())); // 未升级 L3
}

void testQiactThreeConsecutiveFailuresEscalateL3()
{
    Sim s;
    setup(s);
    s.qiactFails = 3;
    TEST_ASSERT_TRUE(runUntil(s, 120000U, [&s]() { return powerOffCount(s) == 1U; }));
    TEST_ASSERT_EQUAL(RecoveryLevel::kL3Module, s.fsm.snapshot(s.now).level);
    TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(countCmds(s, "AT+QIACT=1")));
    // L3 之后重新上电并成功到 Ready。
    TEST_ASSERT_TRUE(toReady(s, 120000U));
}

void testPdpDeactUrcTriggersL2WithBackoff()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    schedule(s, "\r\n+QIURC: \"pdpdeact\",1\r\n", 10U);
    TEST_ASSERT_TRUE(runUntil(s, 200U, [&s]() { return s.fsm.phase() == Phase::kPdpUp; }));
    TEST_ASSERT_EQUAL(RecoveryLevel::kL2Bearer, s.fsm.snapshot(s.now).level);
    const uint32_t urcAt = s.now;
    TEST_ASSERT_TRUE(toReady(s, 30000U));
    TEST_ASSERT_TRUE((s.now - urcAt) >= 10000U);
    TEST_ASSERT_TRUE(countCmds(s, "AT+QIDEACT=1") >= 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.power.size()));
}

void testRegistrationLostThenRecoveredIsL2()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    s.regStat = 2;
    TEST_ASSERT_TRUE(
        runUntil(s, 40000U, [&s]() { return s.fsm.snapshot(s.now).reg == RegStatus::kSearching; }));
    TEST_ASSERT_EQUAL(Phase::kReady, s.fsm.phase()); // 仅掉网：不立即动作
    s.regStat = 1;
    TEST_ASSERT_TRUE(runUntil(s, 40000U, [&s]() { return s.fsm.phase() == Phase::kPdpUp; }));
    TEST_ASSERT_EQUAL(RecoveryLevel::kL2Bearer, s.fsm.snapshot(s.now).level);
    TEST_ASSERT_TRUE(toReady(s, 60000U));
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.power.size()));
}

void testRegistrationUrcLostThenRecoveredIsL2()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    runFor(s, 1000U); // 等进入 Ready 后的首次巡检（CSQ/CEREG?/CPIN?）完成，避免其应答覆盖注入的 URC
    schedule(s, "\r\n+CEREG: 2\r\n", 10U);
    runFor(s, 100U);
    TEST_ASSERT_EQUAL(RegStatus::kSearching, s.fsm.snapshot(s.now).reg);
    TEST_ASSERT_EQUAL(Phase::kReady, s.fsm.phase());
    schedule(s, "\r\n+CEREG: 1\r\n", 10U);
    runFor(s, 100U);
    TEST_ASSERT_EQUAL(Phase::kPdpUp, s.fsm.phase());
    TEST_ASSERT_EQUAL(RecoveryLevel::kL2Bearer, s.fsm.snapshot(s.now).level);
}

void testChannelDeadEscalatesL3AndRecovers()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    s.silent = true;
    TEST_ASSERT_TRUE(runUntil(s, 60000U, [&s]() { return powerOffCount(s) == 1U; }));
    TEST_ASSERT_TRUE(s.fsm.snapshot(s.now).channelDead);
    TEST_ASSERT_EQUAL(RecoveryLevel::kL3Module, s.fsm.snapshot(s.now).level);
    s.silent = false;
    TEST_ASSERT_TRUE(toReady(s, 120000U));
    TEST_ASSERT_FALSE(s.fsm.snapshot(s.now).channelDead);
}

void testHealthCheckEvery30s()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    const uint32_t readyAt = s.now;
    runFor(s, 95000U);
    TEST_ASSERT_EQUAL_UINT32(
        4U, static_cast<uint32_t>(countCmds(s, "AT+CSQ"))); // 进入 Ready 立即一次，之后每 30 s
    TEST_ASSERT_TRUE(countCmds(s, "AT+CPIN?") >= 4U);
    TEST_ASSERT_EQUAL_UINT8(21U, s.fsm.snapshot(s.now).csq);
    const int second = indexOfCmd(s, "AT+CSQ", 1U);
    TEST_ASSERT_TRUE(second > 0);
    TEST_ASSERT_TRUE((s.cmdTimes[static_cast<size_t>(second)] - readyAt) >= 30000U);
    TEST_ASSERT_TRUE((s.cmdTimes[static_cast<size_t>(second)] - readyAt) < 31000U);
}

void testSimRemovedInReadyReturnsToSimCheckThenL3()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    s.simAbsent = true;
    schedule(s, "\r\n+CPIN: NOT READY\r\n", 10U);
    TEST_ASSERT_TRUE(runUntil(s, 500U, [&s]() { return s.fsm.phase() == Phase::kSimCheck; }));
    TEST_ASSERT_FALSE(s.fsm.snapshot(s.now).pdpUp);
    runFor(s, 6000U);
    TEST_ASSERT_EQUAL(Phase::kSimCheck, s.fsm.phase());
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.power.size()));
    s.simAbsent = false;
    TEST_ASSERT_TRUE(runUntil(s, 5000U, [&s]() { return powerOffCount(s) == 1U; }));
}

void testSimRemovedDetectedByHealthCheck()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    s.simAbsent = true; // 没有 URC：靠 Ready 巡检里的 AT+CPIN? 发现
    TEST_ASSERT_TRUE(runUntil(s, 40000U, [&s]() { return s.fsm.phase() == Phase::kSimCheck; }));
    TEST_ASSERT_EQUAL(SimStatus::kAbsent, s.fsm.snapshot(s.now).sim);
}

void testRdyUrcResyncs()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    schedule(s, "\r\nRDY\r\n", 10U);
    TEST_ASSERT_TRUE(runUntil(s, 200U, [&s]() { return s.fsm.phase() == Phase::kSync; }));
    TEST_ASSERT_TRUE(toReady(s, 20000U));
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.power.size()));
}

void testManualResetUsesMinimumHold()
{
    Sim s;
    setup(s);
    TEST_ASSERT_FALSE(s.fsm.requestReset(s.now)); // 未上电不可复位
    TEST_ASSERT_TRUE(toReady(s));
    TEST_ASSERT_TRUE(s.fsm.requestReset(s.now));
    TEST_ASSERT_EQUAL(Phase::kPowerCycle, s.fsm.phase());
    TEST_ASSERT_EQUAL(RecoveryLevel::kL3Module, s.fsm.snapshot(s.now).level);
    TEST_ASSERT_TRUE(toReady(s, 20000U));
    const uint32_t gap = s.power[2].t - s.power[1].t;
    TEST_ASSERT_TRUE(gap >= 2000U);
    TEST_ASSERT_TRUE(gap < 2100U);
}

void testConnOpenRecvSendFlow()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toConnOpen(s));
    TEST_ASSERT_TRUE(indexOfCmd(s, "AT+QIOPEN=1,0,\"TCP\",\"ntrip.example.com\",2101,0,0") >= 0);
    TEST_ASSERT_EQUAL_STRING("open0:1", s.events.back().c_str());
    // 收到数据：recv URC → QIRD 读到空为止（chunk 1024）。
    s.rxData = "hello";
    schedule(s, "\r\n+QIURC: \"recv\",0\r\n", 10U);
    runFor(s, 500U);
    TEST_ASSERT_EQUAL_STRING("hello", s.rxCollected.c_str());
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(countCmds(s, "AT+QIRD=0,1024")));
    // 发送：QISEND + '>' + 定长数据。
    const std::array<uint8_t, 3> data{'a', 'b', 'c'};
    TEST_ASSERT_TRUE(s.fsm.sendConn(0U, data, s.now));
    TEST_ASSERT_FALSE(s.fsm.sendConn(0U, data, s.now)); // 在途：拒绝重入
    runFor(s, 200U);
    TEST_ASSERT_TRUE(indexOfCmd(s, "AT+QISEND=0,3") >= 0);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(s.payloads.size()));
    TEST_ASSERT_EQUAL_STRING("abc", s.payloads[0].c_str());
    TEST_ASSERT_EQUAL_STRING("sent0:1", s.events.back().c_str());
    // 参数校验。
    TEST_ASSERT_FALSE(s.fsm.sendConn(1U, data, s.now)); // 槽未打开
    TEST_ASSERT_FALSE(s.fsm.sendConn(3U, data, s.now)); // 越界
    std::array<uint8_t, 513> big{};
    TEST_ASSERT_FALSE(s.fsm.sendConn(0U, big, s.now));
    TEST_ASSERT_FALSE(s.fsm.openConn(0U, "x", 1U)); // 槽占用
    TEST_ASSERT_FALSE(s.fsm.openConn(3U, "x", 1U));
}

void testConnClosedUrcBackoffsThenEscalatesL2()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toConnOpen(s));
    s.events.clear();
    // 不收数据地反复被远端关闭：2/4/8/16 s 退避，第 5 次连续失败升级 L2。
    const std::array<uint32_t, 4> gap{2000U, 4000U, 8000U, 16000U};
    for (size_t i = 0U; i < gap.size(); ++i) {
        schedule(s, "\r\n+QIURC: \"closed\",0\r\n", 10U);
        runFor(s, 100U);
        TEST_ASSERT_FALSE(s.fsm.connOpen(0U));
        TEST_ASSERT_EQUAL(RecoveryLevel::kL1Conn, s.fsm.snapshot(s.now).level);
        const size_t before = countCmds(s, "AT+QIOPEN=");
        const uint32_t closedAt = s.now - 100U + 10U;
        TEST_ASSERT_TRUE(
            runUntil(s, gap[i] + 500U, [&]() { return countCmds(s, "AT+QIOPEN=") > before; }));
        const uint32_t reopenAt = s.cmdTimes.back();
        TEST_ASSERT_TRUE((reopenAt - closedAt) >= gap[i]);
        TEST_ASSERT_TRUE((reopenAt - closedAt) <= (gap[i] + 80U));
        TEST_ASSERT_TRUE(runUntil(s, 1000U, [&s]() { return s.fsm.connOpen(0U); }));
    }
    schedule(s, "\r\n+QIURC: \"closed\",0\r\n", 10U);
    TEST_ASSERT_TRUE(runUntil(s, 200U, [&s]() { return s.fsm.phase() == Phase::kPdpUp; }));
    TEST_ASSERT_EQUAL(RecoveryLevel::kL2Bearer, s.fsm.snapshot(s.now).level);
    // L2 恢复后自动重开已登记连接。
    TEST_ASSERT_TRUE(toReady(s, 60000U));
    TEST_ASSERT_TRUE(runUntil(s, 3000U, [&s]() { return s.fsm.connOpen(0U); }));
    // closed 回调只通知一次、不会额外报 open 失败。
    for (const std::string &e : s.events) {
        TEST_ASSERT_TRUE(e != "open0:0");
    }
}

void testConnDataResetsL1Backoff()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toConnOpen(s));
    schedule(s, "\r\n+QIURC: \"closed\",0\r\n", 10U);
    runFor(s, 100U);
    TEST_ASSERT_TRUE(runUntil(s, 3000U, [&s]() { return s.fsm.connOpen(0U); }));
    s.rxData = "x";
    schedule(s, "\r\n+QIURC: \"recv\",0\r\n", 10U); // 收到数据：失败计数清零
    runFor(s, 300U);
    const size_t before = countCmds(s, "AT+QIOPEN=");
    schedule(s, "\r\n+QIURC: \"closed\",0\r\n", 10U);
    const uint32_t t0 = s.now;
    TEST_ASSERT_TRUE(runUntil(s, 2600U, [&]() { return countCmds(s, "AT+QIOPEN=") > before; }));
    TEST_ASSERT_TRUE((s.cmdTimes.back() - t0) < 2200U); // 仍是首级 2 s，而不是 4 s
}

void testConnOpenFailureUrcBacksOffL1()
{
    Sim s;
    setup(s);
    s.qiopenErr = 552;
    TEST_ASSERT_TRUE(toReady(s));
    TEST_ASSERT_TRUE(s.fsm.openConn(2U, "host", 1883U));
    TEST_ASSERT_TRUE(runUntil(s, 1000U, [&s]() { return !s.events.empty(); }));
    TEST_ASSERT_EQUAL_STRING("open2:0", s.events.back().c_str());
    TEST_ASSERT_EQUAL(RecoveryLevel::kL1Conn, s.fsm.snapshot(s.now).level);
    TEST_ASSERT_EQUAL(Phase::kReady, s.fsm.phase());
    const size_t before = countCmds(s, "AT+QIOPEN=");
    runFor(s, 1000U);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(before),
                             static_cast<uint32_t>(countCmds(s, "AT+QIOPEN=")));
    s.qiopenErr = 0;
    TEST_ASSERT_TRUE(runUntil(s, 3000U, [&s]() { return s.fsm.connOpen(2U); }));
}

void testConnCloseRequest()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toConnOpen(s));
    TEST_ASSERT_TRUE(s.fsm.closeConn(0U));
    TEST_ASSERT_TRUE(runUntil(s, 1000U, [&s]() { return !s.fsm.connOpen(0U); }));
    TEST_ASSERT_TRUE(indexOfCmd(s, "AT+QICLOSE=0") >= 0);
    runFor(s, 10000U);
    TEST_ASSERT_EQUAL_UINT32(
        1U, static_cast<uint32_t>(countCmds(s, "AT+QIOPEN="))); // 主动关闭后不自动重开
    TEST_ASSERT_FALSE(s.fsm.closeConn(3U));
}

void testShutdownNormalWithPoweredDownUrc()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toConnOpen(s));
    const uint32_t t0 = s.now;
    s.fsm.beginShutdown(t0);
    TEST_ASSERT_TRUE(runUntil(s, 2500U, [&s]() { return s.fsm.shutdownDone(); }));
    TEST_ASSERT_TRUE((s.now - t0) <= 2000U);
    TEST_ASSERT_FALSE(s.power.back().on);
    TEST_ASSERT_EQUAL(Phase::kOff, s.fsm.phase());
    const int close = indexOfCmd(s, "AT+QICLOSE=0");
    const int powd = indexOfCmd(s, "AT+QPOWD=1");
    TEST_ASSERT_TRUE(close >= 0);
    TEST_ASSERT_TRUE(powd > close);
    // 关机后不再自动上电。
    const size_t n = s.power.size();
    runFor(s, 20000U);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(n), static_cast<uint32_t>(s.power.size()));
}

void testShutdownWithoutPoweredDownCutsWithin2s()
{
    Sim s;
    setup(s);
    s.poweredDownUrc = false;
    TEST_ASSERT_TRUE(toReady(s));
    const uint32_t t0 = s.now;
    s.fsm.beginShutdown(t0);
    TEST_ASSERT_TRUE(runUntil(s, 2500U, [&s]() { return s.fsm.shutdownDone(); }));
    TEST_ASSERT_TRUE((s.now - t0) <= 2000U);
    TEST_ASSERT_TRUE((s.now - t0) >= 1500U); // 等满 POWERED DOWN 窗口
    TEST_ASSERT_FALSE(s.power.back().on);
}

void testShutdownWhenAtDeadCutsWithin2s()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toConnOpen(s));
    s.silent = true;
    const uint32_t t0 = s.now;
    s.fsm.beginShutdown(t0);
    TEST_ASSERT_TRUE(runUntil(s, 2500U, [&s]() { return s.fsm.shutdownDone(); }));
    TEST_ASSERT_TRUE((s.now - t0) <= 2000U);
    TEST_ASSERT_FALSE(s.power.back().on);
}

void testShutdownBeforeModuleUpCutsImmediately()
{
    Sim s;
    setup(s);
    s.bootDelayMs = 8000U;
    runFor(s, 1500U);
    TEST_ASSERT_EQUAL(Phase::kSync, s.fsm.phase());
    s.fsm.beginShutdown(s.now);
    TEST_ASSERT_TRUE(s.fsm.shutdownDone());
    TEST_ASSERT_FALSE(s.power.back().on);
}

void testShutdownWhenOffOrCyclingIsImmediate()
{
    Sim s;
    setup(s);
    s.fsm.setEnabled(false);
    TEST_ASSERT_TRUE(s.fsm.shutdownDone()); // 从未上电：天然完成
    s.fsm.beginShutdown(s.now);
    TEST_ASSERT_TRUE(s.fsm.shutdownDone());
    runFor(s, 5000U);
    for (const auto &ev : s.power) {
        TEST_ASSERT_FALSE(ev.on); // 绝不上电
    }
}

void testDisabledNeverPowers()
{
    Sim s;
    setup(s);
    s.fsm.setEnabled(false);
    runFor(s, 30000U);
    TEST_ASSERT_TRUE(s.power.empty());
    TEST_ASSERT_EQUAL(Phase::kOff, s.fsm.phase());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.fsm.msToNextWork(s.now));
}

void testPowerNotAllowedNeverPowersThenAllows()
{
    Sim s;
    setup(s);
    s.fsm.setPowerAllowed(false); // 低电量 <10% 或 Wi-Fi 配网模式
    runFor(s, 30000U);
    TEST_ASSERT_TRUE(s.power.empty());
    s.fsm.setPowerAllowed(true);
    TEST_ASSERT_TRUE(toReady(s));
}

void testPowerRevokedGracefullyStopsThenRepowersAfterMinOff()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    const uint32_t t0 = s.now;
    s.fsm.setPowerAllowed(false);
    TEST_ASSERT_TRUE(runUntil(s, 2500U, [&s]() { return s.fsm.phase() == Phase::kOff; }));
    TEST_ASSERT_TRUE((s.now - t0) <= 2000U);
    TEST_ASSERT_TRUE(indexOfCmd(s, "AT+QPOWD=1") >= 0);
    TEST_ASSERT_FALSE(s.power.back().on);
    const uint32_t offAt = s.power.back().t;
    s.fsm.setPowerAllowed(true);
    TEST_ASSERT_TRUE(runUntil(s, 5000U, [&s]() { return s.power.back().on; }));
    TEST_ASSERT_TRUE((s.power.back().t - offAt) >= 2000U); // 断电至少保持 2 s
}

void testDisableAtRuntimeStopsModule()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    s.fsm.setEnabled(false);
    TEST_ASSERT_TRUE(runUntil(s, 2500U, [&s]() { return s.fsm.phase() == Phase::kOff; }));
    runFor(s, 10000U);
    TEST_ASSERT_FALSE(s.power.back().on);
}

void testMsToNextWorkIsBoundedWhenReady()
{
    Sim s;
    setup(s);
    TEST_ASSERT_TRUE(toReady(s));
    runFor(s, 1000U);
    const uint32_t wait = s.fsm.msToNextWork(s.now);
    TEST_ASSERT_TRUE(wait <= 30000U);
    TEST_ASSERT_TRUE(wait > 0U);
}

void testConfigTruncatesOverlongFields()
{
    Sim s;
    setup(s);
    const std::string longApn(60U, 'a');
    s.fsm.setConfig(longApn, "", "", 0U);
    TEST_ASSERT_TRUE(toReady(s));
    const int apn = indexOfCmd(s, "AT+QICSGP=");
    TEST_ASSERT_TRUE(apn >= 0);
    // 截断到 31 字符，命令仍然完整。
    TEST_ASSERT_EQUAL_STRING(
        ("AT+QICSGP=1,1,\"" + std::string(31U, 'a') + "\",\"\",\"\",0").c_str(),
        s.cmds[static_cast<size_t>(apn)].c_str());
}

} // namespace

void setUp() {}

void tearDown() {}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testBackoffTables);
    RUN_TEST(testHappyPathToReady);
    RUN_TEST(testApnConfigCommand);
    RUN_TEST(testIprFailureStaysAtBootBaud);
    RUN_TEST(testModuleAlreadyFastBaudSwapsProbe);
    RUN_TEST(testSlowBootStillSyncsWithinWindow);
    RUN_TEST(testSyncWindowTimeoutEscalatesL3WithCappedBackoff);
    RUN_TEST(testPowerCycleBackoffVisibleInSnapshot);
    RUN_TEST(testSimAbsentStaysInSimCheckThenRecoveryIsL3);
    RUN_TEST(testRegisterTimeoutIs180sThenL3);
    RUN_TEST(testCregFallbackWhenCeregNotRegistered);
    RUN_TEST(testQiactFailureL2ThenSuccess);
    RUN_TEST(testQiactThreeConsecutiveFailuresEscalateL3);
    RUN_TEST(testPdpDeactUrcTriggersL2WithBackoff);
    RUN_TEST(testRegistrationLostThenRecoveredIsL2);
    RUN_TEST(testRegistrationUrcLostThenRecoveredIsL2);
    RUN_TEST(testChannelDeadEscalatesL3AndRecovers);
    RUN_TEST(testHealthCheckEvery30s);
    RUN_TEST(testSimRemovedInReadyReturnsToSimCheckThenL3);
    RUN_TEST(testSimRemovedDetectedByHealthCheck);
    RUN_TEST(testRdyUrcResyncs);
    RUN_TEST(testManualResetUsesMinimumHold);
    RUN_TEST(testConnOpenRecvSendFlow);
    RUN_TEST(testConnClosedUrcBackoffsThenEscalatesL2);
    RUN_TEST(testConnDataResetsL1Backoff);
    RUN_TEST(testConnOpenFailureUrcBacksOffL1);
    RUN_TEST(testConnCloseRequest);
    RUN_TEST(testShutdownNormalWithPoweredDownUrc);
    RUN_TEST(testShutdownWithoutPoweredDownCutsWithin2s);
    RUN_TEST(testShutdownWhenAtDeadCutsWithin2s);
    RUN_TEST(testShutdownBeforeModuleUpCutsImmediately);
    RUN_TEST(testShutdownWhenOffOrCyclingIsImmediate);
    RUN_TEST(testDisabledNeverPowers);
    RUN_TEST(testPowerNotAllowedNeverPowersThenAllows);
    RUN_TEST(testPowerRevokedGracefullyStopsThenRepowersAfterMinOff);
    RUN_TEST(testDisableAtRuntimeStopsModule);
    RUN_TEST(testMsToNextWorkIsBoundedWhenReady);
    RUN_TEST(testConfigTruncatesOverlongFields);
    return UNITY_END();
}
