// AT 引擎黑盒测试（ADR-0021 §2.4）：分帧、终结行、URC、提示符数据、定长读、超时与通道失效。

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "cellular/AtEngine.hpp"
#include "cellular/AtParse.hpp"
#include "cellular/LinkFsm.hpp"
#include "unity.h"

namespace {

using cellular::AtCommand;
using cellular::AtEngine;
using cellular::AtResponse;
using cellular::AtResult;

struct Harness final {
    AtEngine engine;
    std::string tx;
    std::vector<std::string> lines;
    std::vector<std::string> urcs;
    std::vector<AtResponse> done;
    std::string raw;
    bool writeOk = true;
};

Harness *g_h = nullptr;

bool writeHook(void *ctx, std::span<const uint8_t> bytes) noexcept
{
    auto *h = static_cast<Harness *>(ctx);
    if (!h->writeOk) {
        return false;
    }
    h->tx.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return true;
}

void urcHook(void *ctx, std::string_view line) noexcept
{
    static_cast<Harness *>(ctx)->urcs.emplace_back(line);
}

void lineHook(void *ctx, std::string_view line) noexcept
{
    static_cast<Harness *>(ctx)->lines.emplace_back(line);
}

void doneHook(void *ctx, const AtResponse &rsp) noexcept
{
    static_cast<Harness *>(ctx)->done.push_back(rsp);
}

void rawHook(void *ctx, std::span<const uint8_t> chunk) noexcept
{
    static_cast<Harness *>(ctx)->raw.append(reinterpret_cast<const char *>(chunk.data()),
                                            chunk.size());
}

void setupHarness(Harness &h)
{
    g_h = &h;
    cellular::AtPort port;
    port.ctx = &h;
    port.write = &writeHook;
    port.onUrc = &urcHook;
    port.urcPrefixes = std::span<const std::string_view>{cellular::kUrcPrefixes};
    h.engine.bind(port);
}

AtCommand makeCmd(Harness &h, std::string_view text, uint32_t timeoutMs = 1000U)
{
    AtCommand cmd;
    cmd.text = text;
    cmd.timeoutMs = timeoutMs;
    cmd.onLine = &lineHook;
    cmd.onDone = &doneHook;
    cmd.onRaw = &rawHook;
    cmd.ctx = &h;
    return cmd;
}

void feedStr(Harness &h, std::string_view s, uint32_t now)
{
    h.engine.feed({reinterpret_cast<const uint8_t *>(s.data()), s.size()}, now);
}

void testOkAndStickyHalfLines()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ"), 0U));
    TEST_ASSERT_EQUAL_STRING("AT+CSQ\r", h.tx.c_str());
    // 半行 + 粘包：一次喂 "\r\n+CSQ: 2" 再喂 "0,0\r\n\r\nOK\r\n"。
    feedStr(h, "\r\n+CSQ: 2", 10U);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.done.size()));
    feedStr(h, "0,0\r\n\r\nOK\r\n", 11U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[0].result);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.lines.size()));
    TEST_ASSERT_EQUAL_STRING("+CSQ: 20,0", h.lines[0].c_str());
    TEST_ASSERT_FALSE(h.engine.busy());
}

void testEchoDropped()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "ATE0"), 0U));
    feedStr(h, "ATE0\r\r\nOK\r\n", 5U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.lines.size()));
}

void testUrcMidResponse()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ"), 0U));
    // 响应中间插入 URC：URC 分发给 onUrc，不进入命令的行回调。
    feedStr(h, "+QIURC: \"recv\",0\r\n+CSQ: 18,0\r\nOK\r\n", 3U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.urcs.size()));
    TEST_ASSERT_EQUAL_STRING("+QIURC: \"recv\",0", h.urcs[0].c_str());
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.lines.size()));
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[0].result);
    // 空闲时的 URC 同样分发；非 URC 游离行计入 stray。
    feedStr(h, "RDY\r\njunk\r\n", 4U);
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(h.urcs.size()));
    TEST_ASSERT_EQUAL_UINT32(1U, h.engine.stats().stray);
}

void testQueryResponseNotTreatedAsUrc()
{
    Harness h;
    setupHarness(h);
    AtCommand cmd = makeCmd(h, "AT+CPIN?");
    cmd.match = &cellular::isCpinLine;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    feedStr(h, "+CPIN: READY\r\nOK\r\n", 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.urcs.size()));
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.lines.size()));
}

void testErrorAndCmeError()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+X"), 0U));
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CPIN?"), 0U));
    feedStr(h, "ERROR\r\n", 1U);
    TEST_ASSERT_EQUAL(AtResult::kError, h.done[0].result);
    // 队列中的第二条在第一条完成后自动发出。
    TEST_ASSERT_EQUAL_STRING("AT+X\rAT+CPIN?\r", h.tx.c_str());
    feedStr(h, "+CME ERROR: 10\r\n", 2U);
    TEST_ASSERT_EQUAL(AtResult::kCmeError, h.done[1].result);
    TEST_ASSERT_EQUAL_INT32(10, h.done[1].errorCode);
}

void testPromptDataPhase()
{
    Harness h;
    setupHarness(h);
    const std::array<uint8_t, 4> payload{'a', 'b', 'c', 'd'};
    AtCommand cmd = makeCmd(h, "AT+QISEND=0,4");
    cmd.payload = payload;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    TEST_ASSERT_EQUAL_STRING("AT+QISEND=0,4\r", h.tx.c_str());
    feedStr(h, "\r\n> ", 1U);
    TEST_ASSERT_EQUAL_STRING("AT+QISEND=0,4\rabcd", h.tx.c_str());
    feedStr(h, "\r\nSEND OK\r\n", 2U);
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[0].result);
    // 超限 payload 被拒绝。
    std::array<uint8_t, AtEngine::kMaxPayload + 1U> big{};
    AtCommand bad = makeCmd(h, "AT+QISEND=0,513");
    bad.payload = big;
    TEST_ASSERT_FALSE(h.engine.submit(bad, 3U));
}

void testFixedLengthRawRead()
{
    Harness h;
    setupHarness(h);
    AtCommand cmd = makeCmd(h, "AT+QIRD=0,1024");
    cmd.rawPrefix = "+QIRD:";
    cmd.rawMax = 1024U;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    // 原始数据含 CR/LF/"OK" 字样，必须按长度读取，不被分帧误吞；且数据分多块到达。
    feedStr(h, "+QIRD: 8\r\nab\r\nOK", 1U);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.done.size()));
    feedStr(h, "\r\n\r\nOK\r\n", 2U);
    TEST_ASSERT_EQUAL_STRING("ab\r\nOK\r\n", h.raw.c_str());
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[0].result);
}

void testTimeoutAndChannelDead()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 300U), 0U));
    TEST_ASSERT_EQUAL_UINT32(300U, h.engine.msToDeadline(0U));
    h.engine.poll(299U);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.done.size()));
    h.engine.poll(300U);
    TEST_ASSERT_EQUAL(AtResult::kTimeout, h.done[0].result);
    TEST_ASSERT_FALSE(h.engine.dead());
    // 第二次超时之间插入一次成功应答 → 连续计数清零，不判失效。
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 300U), 400U));
    h.engine.poll(700U);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 300U), 700U));
    feedStr(h, "OK\r\n", 710U);
    TEST_ASSERT_FALSE(h.engine.dead());
    // 连续 3 次无响应 → 通道失效，后续排队命令以 kAborted 收尾，不再接受新命令。
    for (uint32_t i = 0U; i < 3U; ++i) {
        TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 100U), 1000U + (i * 200U)));
        h.engine.poll(1100U + (i * 200U));
    }
    TEST_ASSERT_TRUE(h.engine.dead());
    TEST_ASSERT_FALSE(h.engine.submit(makeCmd(h, "AT"), 2000U));
    h.engine.reset(2000U);
    TEST_ASSERT_FALSE(h.engine.dead());
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT"), 2001U));
}

void testWriteFailureTimesOut()
{
    Harness h;
    setupHarness(h);
    h.writeOk = false;
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 500U), 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, h.engine.stats().writeFails);
    h.engine.poll(1U);
    TEST_ASSERT_EQUAL(AtResult::kTimeout, h.done[0].result);
}

void testOverlongLineDropped()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+X"), 0U));
    const std::string longLine(AtEngine::kLineBytes + 50U, 'x');
    feedStr(h, longLine, 1U);
    feedStr(h, "\r\nOK\r\n", 2U);
    TEST_ASSERT_EQUAL_UINT32(1U, h.engine.stats().overflow);
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[0].result);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.lines.size()));
}

void testTickWrapTimeout()
{
    Harness h;
    setupHarness(h);
    const uint32_t start = 0xFFFFFF00U;
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 600U), start));
    h.engine.poll(start + 599U);
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.done.size()));
    h.engine.poll(start + 600U); // 已回绕过 0
    TEST_ASSERT_EQUAL(AtResult::kTimeout, h.done[0].result);
}

void testSamePrefixUrcNotTakenAsQueryReply()
{
    Harness h;
    setupHarness(h);
    AtCommand cmd = makeCmd(h, "AT+CEREG?");
    cmd.match = &cellular::isRegQueryLine;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    // 同前缀的 URC 形状（单字段）必须走 URC 通道，不能当作查询应答。
    feedStr(h, "+CEREG: 5\r\n+CEREG: 2,1,\"1A2B\"\r\nOK\r\n", 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.urcs.size()));
    TEST_ASSERT_EQUAL_STRING("+CEREG: 5", h.urcs[0].c_str());
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.lines.size()));
    TEST_ASSERT_EQUAL_STRING("+CEREG: 2,1,\"1A2B\"", h.lines[0].c_str());
}

void testDeadlineBoundaryUsesRxTimestamp()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 300U), 0U));
    // 截止时刻及之后到达的字节属于迟到：先超时，不得成功。
    feedStr(h, "OK\r\n", 300U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kTimeout, h.done[0].result);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 300U), 400U));
    feedStr(h, "OK\r\n", 699U);
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[1].result);
}

void testLateTerminatorDoesNotCompleteNextCommand()
{
    Harness h;
    setupHarness(h);
    h.engine.setResyncEnabled(true);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ", 300U), 0U));
    h.engine.poll(300U);
    TEST_ASSERT_EQUAL(AtResult::kTimeout, h.done[0].result);
    // 下一条命令排队后：先等通道静默 kQuietMs，再发重新同步握手，而不是立刻发命令。
    h.tx.clear();
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+GSN", 300U), 310U));
    TEST_ASSERT_EQUAL_STRING("", h.tx.c_str());
    // 旧命令迟到的应答与终结行：丢弃（并刷新静默计时），不归属新命令。
    feedStr(h, "+CSQ: 20,0\r\nOK\r\n", 320U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.lines.size()));
    TEST_ASSERT_TRUE(h.engine.stats().stale >= 1U);
    h.engine.poll(619U);
    TEST_ASSERT_EQUAL_STRING("", h.tx.c_str()); // 距最后一个字节未满 300 ms
    h.engine.poll(620U);
    TEST_ASSERT_EQUAL_STRING("AT+CMEE?\r", h.tx.c_str());
    // 握手完成后才发出新命令，其应答只归新命令。
    feedStr(h, "+CMEE: 2\r\nOK\r\n", 630U);
    TEST_ASSERT_EQUAL_STRING("AT+CMEE?\rAT+GSN\r", h.tx.c_str());
    feedStr(h, "860000000000001\r\nOK\r\n", 640U);
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[1].result);
    TEST_ASSERT_EQUAL_UINT32(1U, h.engine.stats().resyncs);
}

void testCancelInflightNeedsResync()
{
    Harness h;
    setupHarness(h);
    h.engine.setResyncEnabled(true);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ", 300U), 0U));
    h.engine.reset(10U); // 本地取消在途命令
    TEST_ASSERT_TRUE(h.engine.resyncing());
    h.tx.clear();
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 300U), 20U));
    TEST_ASSERT_EQUAL_STRING("", h.tx.c_str());
    h.engine.poll(320U);
    TEST_ASSERT_EQUAL_STRING("AT+CMEE?\r", h.tx.c_str());
}

// I1：探针超时后，迟到的上一轮探针应答不得被下一轮探针（交替为 AT+CSCS?）误认为自己的应答。
void testAlternatingProbeRejectsLateSameShapeReply()
{
    Harness h;
    setupHarness(h);
    h.engine.setResyncEnabled(true);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ", 100U), 0U));
    h.engine.poll(100U);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+GSN", 300U), 110U));
    h.engine.poll(410U);
    TEST_ASSERT_EQUAL_STRING("AT+CMEE?\r", h.tx.c_str() + h.tx.find("AT+CMEE?"));
    h.engine.poll(1410U); // 第一轮探针无应答：超时后重新静默
    h.engine.poll(1710U);
    TEST_ASSERT_TRUE(h.tx.find("AT+CSCS?\r") != std::string::npos);
    // 第一轮探针迟到的应答：属于 +CMEE:，不是本轮 +CSCS:，不得完成握手。
    feedStr(h, "+CMEE: 2\r\nOK\r\n", 1720U);
    TEST_ASSERT_TRUE(h.tx.find("AT+GSN") == std::string::npos);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
}

// I2：命令自带的精确前缀把同前缀 URC 行识别为本命令应答。
void testMatchPrefixOwnsReplyLine()
{
    Harness h;
    setupHarness(h);
    AtCommand cmd = makeCmd(h, "AT+CEREG?");
    cmd.matchPrefix = "+CEREG:";
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    feedStr(h, "+CEREG: 0,1\r\nOK\r\n", 5U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.lines.size()));
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[0].result);
}

// I3：行模式（等待应答）下丢字节：立即以错误结束当前命令，不再等一个孤立的 OK。
void testRxLostInLineModeFailsCommand()
{
    Harness h;
    setupHarness(h);
    h.engine.setResyncEnabled(true);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ", 5000U), 0U));
    feedStr(h, "+CSQ: 2", 10U);
    h.engine.onRxLost(20U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kError, h.done[0].result);
    TEST_ASSERT_TRUE(h.engine.resyncing());
}

void testRawLengthOverRequestIsProtocolError()
{
    Harness h;
    setupHarness(h);
    h.engine.setResyncEnabled(true);
    AtCommand cmd = makeCmd(h, "AT+QIRD=0,1024");
    cmd.rawPrefix = "+QIRD:";
    cmd.rawMax = 1024U;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    feedStr(h, "+QIRD: 1025\r\n", 1U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kError, h.done[0].result);
    TEST_ASSERT_EQUAL_UINT32(1U, h.engine.stats().protocol);
    TEST_ASSERT_TRUE(h.raw.empty());
    TEST_ASSERT_TRUE(h.engine.resyncing());
}

void testRawLengthTrailingJunkRejected()
{
    Harness h;
    setupHarness(h);
    AtCommand cmd = makeCmd(h, "AT+QIRD=0,1024");
    cmd.rawPrefix = "+QIRD:";
    cmd.rawMax = 1024U;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    feedStr(h, "+QIRD: 8abc\r\n", 1U);
    TEST_ASSERT_EQUAL(AtResult::kError, h.done[0].result);
    TEST_ASSERT_TRUE(h.raw.empty());
}

void testRxLostDuringRawAbortsCommand()
{
    Harness h;
    setupHarness(h);
    h.engine.setResyncEnabled(true);
    AtCommand cmd = makeCmd(h, "AT+QIRD=0,16");
    cmd.rawPrefix = "+QIRD:";
    cmd.rawMax = 16U;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    feedStr(h, "+QIRD: 8\r\nab", 1U);
    h.engine.onRxLost(2U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kError, h.done[0].result);
    TEST_ASSERT_TRUE(h.engine.resyncing());
}

void testCmeErrorTextCode()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CPIN?"), 0U));
    feedStr(h, "+CME ERROR: SIM not inserted\r\n", 1U);
    TEST_ASSERT_EQUAL(AtResult::kCmeError, h.done[0].result);
    TEST_ASSERT_EQUAL_INT32(10, h.done[0].errorCode);
}

// r4 R1：静默已满的同一次 feed 若带着旧响应，旧响应必须被丢弃且重新计静默，不得落到刚发出的新命令上。
void testQuietBoundaryFeedDoesNotLeakStaleReply()
{
    Harness h;
    setupHarness(h);
    AtCommand cmd = makeCmd(h, "AT", 300U);
    cmd.quietFirst = true;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    TEST_ASSERT_EQUAL_STRING("", h.tx.c_str());
    // 静默期（300 ms）早已满足，但本次 feed 同时带来旧响应：先丢弃并刷新静默计时。
    feedStr(h, "+CSQ: 20,0\r\nOK\r\n", 400U);
    TEST_ASSERT_EQUAL_STRING("", h.tx.c_str());
    TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(h.done.size()));
    h.engine.poll(699U);
    TEST_ASSERT_EQUAL_STRING("", h.tx.c_str());
    h.engine.poll(700U);
    TEST_ASSERT_EQUAL_STRING("AT\r", h.tx.c_str());
    feedStr(h, "OK\r\n", 710U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[0].result);
}

// r4 R1：同步握手期间丢字节计入无响应阈值，到阈值判通道失效，不无限重进静默。
void testRxLostDuringProbeCountsTowardsDead()
{
    Harness h;
    setupHarness(h);
    h.engine.setResyncEnabled(true);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ", 100U), 0U));
    h.engine.poll(100U); // 第 1 次无响应
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+GSN", 300U), 110U));
    h.engine.poll(410U); // 静默结束，发探针
    TEST_ASSERT_TRUE(h.tx.find("AT+CMEE?\r") != std::string::npos);
    h.engine.onRxLost(420U); // 第 2 次
    TEST_ASSERT_FALSE(h.engine.dead());
    h.engine.poll(720U);
    TEST_ASSERT_TRUE(h.tx.find("AT+CSCS?\r") != std::string::npos);
    h.engine.onRxLost(730U); // 第 3 次 → 失效
    TEST_ASSERT_TRUE(h.engine.dead());
    TEST_ASSERT_FALSE(h.engine.busy());
}

// r4 R2：断电后队列以 kAborted 收尾，重新同步/静默/失效标记全部清除，下次上电从干净状态开始。
void testPowerDownSettlesAndClearsResync()
{
    Harness h;
    setupHarness(h);
    h.engine.setResyncEnabled(true);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ", 100U), 0U));
    h.engine.poll(100U);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+GSN", 300U), 110U));
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+QCCID", 300U), 111U));
    TEST_ASSERT_TRUE(h.engine.resyncing());
    h.engine.powerDown(200U);
    TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kAborted, h.done[1].result);
    TEST_ASSERT_EQUAL(AtResult::kAborted, h.done[2].result);
    TEST_ASSERT_FALSE(h.engine.resyncing());
    TEST_ASSERT_FALSE(h.engine.busy());
    TEST_ASSERT_FALSE(h.engine.dead());
    // 下一条命令直接发出，不再走静默/探针。
    h.tx.clear();
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT", 300U), 300U));
    TEST_ASSERT_EQUAL_STRING("AT\r", h.tx.c_str());
}

// r4 R3：前缀 + 形状同时给出时必须同时满足：异前缀行、单字段 URC 都不是应答。
void testRegReplyNeedsPrefixAndShape()
{
    Harness h;
    setupHarness(h);
    AtCommand cmd = makeCmd(h, "AT+CEREG?");
    cmd.matchPrefix = "+CEREG:";
    cmd.match = &cellular::isRegQueryLine;
    TEST_ASSERT_TRUE(h.engine.submit(cmd, 0U));
    feedStr(h, "+CREG: 2,1\r\n", 1U);        // 异前缀、形状合格：仍是 URC
    feedStr(h, "+CEREG: 5\r\n", 2U);         // 同前缀、单字段：URC
    feedStr(h, "+CEREG: 2,1\r\nOK\r\n", 3U); // 同前缀、两字段：应答
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(h.urcs.size()));
    TEST_ASSERT_EQUAL_STRING("+CREG: 2,1", h.urcs[0].c_str());
    TEST_ASSERT_EQUAL_STRING("+CEREG: 5", h.urcs[1].c_str());
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.lines.size()));
    TEST_ASSERT_EQUAL_STRING("+CEREG: 2,1", h.lines[0].c_str());
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[0].result);
}

// r5 F1：命令完成后同一 RX 块的剩余字节是旧数据，不得完成紧接着发出的下一条命令。
void testSameChunkTailNotDeliveredToNextCommand()
{
    Harness h;
    setupHarness(h);
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CSQ", 1000U), 0U));
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+GSN", 1000U), 0U));
    // 一次 feed：第一条的 OK 之后紧跟旧数据 "\r\nOK\r\n"，它在 AT+GSN 发出之前就已到达。
    feedStr(h, "+CSQ: 20,0\r\nOK\r\nOK\r\n", 10U);
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL_STRING("AT+CSQ\rAT+GSN\r", h.tx.c_str());
    // 第二条只能由发出之后读入的字节完成。
    feedStr(h, "861234\r\nOK\r\n", 20U);
    TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(h.done.size()));
    TEST_ASSERT_EQUAL(AtResult::kOk, h.done[1].result);
    TEST_ASSERT_EQUAL_UINT32(1U, h.engine.stats().stale);
}

// r5 F3：单字段/缺 n 的注册行（如 "+CEREG: ,3"）不是查询应答形状，不得被 isRegQueryLine 接受。
void testRegQueryShapeRequiresNAndStat()
{
    TEST_ASSERT_TRUE(cellular::isRegQueryLine("+CEREG: 2,1"));
    TEST_ASSERT_TRUE(cellular::isRegQueryLine("+CGREG: 0,5"));
    TEST_ASSERT_FALSE(cellular::isRegQueryLine("+CEREG: ,3"));
    TEST_ASSERT_FALSE(cellular::isRegQueryLine("+CEREG: 1"));
    TEST_ASSERT_FALSE(cellular::isRegQueryLine("+CREG: \"1\""));
    cellular::RegStatus reg = cellular::RegStatus::kUnknown;
    TEST_ASSERT_FALSE(cellular::parseReg("+CEREG: ,3", reg));
    TEST_ASSERT_EQUAL(cellular::RegStatus::kUnknown, reg);
}

} // namespace

extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(testOkAndStickyHalfLines);
    RUN_TEST(testEchoDropped);
    RUN_TEST(testUrcMidResponse);
    RUN_TEST(testQueryResponseNotTreatedAsUrc);
    RUN_TEST(testErrorAndCmeError);
    RUN_TEST(testPromptDataPhase);
    RUN_TEST(testFixedLengthRawRead);
    RUN_TEST(testTimeoutAndChannelDead);
    RUN_TEST(testWriteFailureTimesOut);
    RUN_TEST(testOverlongLineDropped);
    RUN_TEST(testTickWrapTimeout);
    RUN_TEST(testSamePrefixUrcNotTakenAsQueryReply);
    RUN_TEST(testDeadlineBoundaryUsesRxTimestamp);
    RUN_TEST(testLateTerminatorDoesNotCompleteNextCommand);
    RUN_TEST(testCancelInflightNeedsResync);
    RUN_TEST(testAlternatingProbeRejectsLateSameShapeReply);
    RUN_TEST(testMatchPrefixOwnsReplyLine);
    RUN_TEST(testRxLostInLineModeFailsCommand);
    RUN_TEST(testRawLengthOverRequestIsProtocolError);
    RUN_TEST(testRawLengthTrailingJunkRejected);
    RUN_TEST(testRxLostDuringRawAbortsCommand);
    RUN_TEST(testCmeErrorTextCode);
    RUN_TEST(testQuietBoundaryFeedDoesNotLeakStaleReply);
    RUN_TEST(testRxLostDuringProbeCountsTowardsDead);
    RUN_TEST(testPowerDownSettlesAndClearsResync);
    RUN_TEST(testRegReplyNeedsPrefixAndShape);
    RUN_TEST(testSameChunkTailNotDeliveredToNextCommand);
    RUN_TEST(testRegQueryShapeRequiresNAndStat);
    return UNITY_END();
}
