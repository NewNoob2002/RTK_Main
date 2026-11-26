// AT 引擎黑盒测试（ADR-0021 §2.4）：分帧、终结行、URC、提示符数据、定长读、超时与通道失效。

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
    TEST_ASSERT_TRUE(h.engine.submit(makeCmd(h, "AT+CPIN?"), 0U));
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
    return UNITY_END();
}
