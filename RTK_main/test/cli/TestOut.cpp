// cli 统一输出出口（components/cli/src/Out.cpp，ADR-0009 §2.8）的 host 单元测试：
// 行尾展开、跨调用的 "\r\n" 状态、整行一次写出、超长回落 stdout、日志钩子重入拒绝。
// 单线程执行；输出端是记录到静态缓冲的假 sink。

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include "cli/Out.hpp"
#include "unity.h"

namespace {

std::string s_sunk;     // sink 收到的全部字节
int s_sink_calls = 0;   // sink 调用次数
int s_reentry_rc = 0;   // 重入测试中嵌套 outLogVprintf 的返回值
bool s_reenter = false; // 置位时 sink 内部再打一次“日志”

int logCall(const char *fmt, ...) noexcept
{
    va_list args;
    va_start(args, fmt);
    const int rc = cli::outLogVprintf(fmt, args);
    va_end(args);
    return rc;
}

void fakeSink(const char *text, size_t len) noexcept
{
    ++s_sink_calls;
    s_sunk.append(text, len);
    if (s_reenter) {
        s_reenter = false;
        s_reentry_rc = logCall("nested %d\n", 1);
    }
}

} // namespace

void setUp(void)
{
    s_sunk.clear();
    s_sink_calls = 0;
    s_reentry_rc = 0;
    s_reenter = false;
    cli::setOutSink(fakeSink, true);
}

void tearDown(void)
{
    cli::setOutSink(nullptr, false);
}

static void testCrlfExpandedOncePerLine(void)
{
    cli::outPrintf("  %-4s %d\n", "ab", 7);
    TEST_ASSERT_EQUAL_STRING("  ab   7\r\n", s_sunk.c_str());
    TEST_ASSERT_EQUAL_INT(1, s_sink_calls);
}

static void testExistingCrlfNotDoubled(void)
{
    static constexpr char kLine[] = "[1.000] evt\r\n";
    cli::outWrite(kLine, sizeof(kLine) - 1U);
    TEST_ASSERT_EQUAL_STRING("[1.000] evt\r\n", s_sunk.c_str());
}

static void testCrLfSplitAcrossCalls(void)
{
    cli::outWrite("a\r", 2U);
    cli::outWrite("\nb\n", 3U);
    TEST_ASSERT_EQUAL_STRING("a\r\nb\r\n", s_sunk.c_str());
}

static void testNoCrlfModePassthrough(void)
{
    cli::setOutSink(fakeSink, false);
    cli::outPrintf("x\ny\n");
    TEST_ASSERT_EQUAL_STRING("x\ny\n", s_sunk.c_str());
}

static void testLongNewlineRunChunkedWithoutLoss(void)
{
    // 全是 '\n' 时展开后长度翻倍，超过展开缓冲需要分段；内容不能丢。
    std::string text(cli::kOutLineMax - 1U, '\n');
    cli::outWrite(text.data(), text.size());
    TEST_ASSERT_EQUAL_size_t(text.size() * 2U, s_sunk.size());
    TEST_ASSERT_GREATER_THAN_INT(1, s_sink_calls);
    for (size_t i = 0; i < s_sunk.size(); i += 2U) {
        TEST_ASSERT_EQUAL_CHAR('\r', s_sunk[i]);
        TEST_ASSERT_EQUAL_CHAR('\n', s_sunk[i + 1U]);
    }
}

static void testOverlongFormatFallsBackToStdout(void)
{
    // 超过 kOutLineMax 的一条输出改走 stdout（不截断），sink 不应收到半截内容。
    const std::string big(cli::kOutLineMax + 10U, 'z');
    cli::outPrintf("%s\n", big.c_str());
    TEST_ASSERT_EQUAL_INT(0, s_sink_calls);
}

static void testLogReentryRejected(void)
{
    s_reenter = true;
    const int rc = logCall("outer\n");
    TEST_ASSERT_EQUAL_INT(6, rc);
    TEST_ASSERT_EQUAL_INT(-1, s_reentry_rc); // 同任务重入不碰缓冲
    TEST_ASSERT_EQUAL_STRING("outer\r\n", s_sunk.c_str());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(testCrlfExpandedOncePerLine);
    RUN_TEST(testExistingCrlfNotDoubled);
    RUN_TEST(testCrLfSplitAcrossCalls);
    RUN_TEST(testNoCrlfModePassthrough);
    RUN_TEST(testLongNewlineRunChunkedWithoutLoss);
    RUN_TEST(testOverlongFormatFallsBackToStdout);
    RUN_TEST(testLogReentryRejected);
    return UNITY_END();
}
