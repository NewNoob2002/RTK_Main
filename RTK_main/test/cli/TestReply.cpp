// cli::fail / dispatch / print_usage / print_section / print_kv 的 host 单元测试（ADR-0009 §2.5）。
// 直接编译 components/cli/src/Reply.cpp（只用 C 标准库），通过 tmpfile + dup2 捕获 stdout。
// 单线程执行，非 ISR 相关。

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <unistd.h>

#include "cli/Console.hpp"
#include "ReplyState.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr size_t kCaptureMax = 1024U;
constexpr int kFailCode = 7;
constexpr size_t kScratchLen = 32U;

char s_capture[kCaptureMax] = {};
FILE *s_tmp = nullptr;
int s_saved_stdout = -1;
bool s_capture_ready = false;

// 各动词的调用记录；测试是单线程的，用文件作用域静态变量即可。
int s_handler_calls = 0;
int s_last_argc = -1;
char s_last_verb[kScratchLen] = {};
char s_last_arg[kScratchLen] = {};

/**
 * 把 stdout 重定向到临时文件。必须在断言前检查 s_capture_ready，
 * 否则测试会在错误的文件描述符上比较（那比失败更难查）。
 */
void capture_begin() noexcept
{
    s_capture[0] = '\0';
    s_handler_calls = 0;
    s_last_argc = -1;
    s_last_verb[0] = '\0';
    s_last_arg[0] = '\0';
    s_saved_stdout = dup(STDOUT_FILENO);
    s_tmp = tmpfile();
    s_capture_ready = false;
    if ((s_saved_stdout < 0) || (s_tmp == nullptr)) {
        return;
    }
    if (fflush(stdout) != 0) {
        return;
    }
    s_capture_ready = dup2(fileno(s_tmp), STDOUT_FILENO) >= 0;
}

/** 恢复 stdout 并把临时文件内容读进 s_capture（截断到 kCaptureMax-1）。 */
void capture_end() noexcept
{
    fflush(stdout);
    if (s_saved_stdout >= 0) {
        (void)dup2(s_saved_stdout, STDOUT_FILENO);
        (void)close(s_saved_stdout);
        s_saved_stdout = -1;
    }
    if (s_tmp == nullptr) {
        return;
    }
    (void)fseeko(s_tmp, 0, SEEK_SET);
    const size_t read_bytes = fread(s_capture, 1U, kCaptureMax - 1U, s_tmp);
    s_capture[read_bytes] = '\0';
    (void)fclose(s_tmp);
    s_tmp = nullptr;
}

void record_call(int argc, char **argv) noexcept
{
    ++s_handler_calls;
    s_last_argc = argc;
    if ((argc > 0) && (argv != nullptr) && (argv[0] != nullptr)) {
        (void)std::snprintf(s_last_verb, kScratchLen, "%s", argv[0]);
    }
    if ((argc > 1) && (argv != nullptr) && (argv[1] != nullptr)) {
        (void)std::snprintf(s_last_arg, kScratchLen, "%s", argv[1]);
    }
}

int verb_ok(void *ctx, int argc, char **argv) noexcept
{
    record_call(argc, argv);
    // 顺便验证 ctx 传到了 handler（值 0x5A 表示调用方传入的标记）。
    if ((ctx == nullptr) || (*static_cast<const int *>(ctx) != 0x5A)) {
        return kFailCode;
    }
    return 0;
}

int verb_fail_with_reason(void * /*ctx*/, int argc, char **argv) noexcept
{
    record_call(argc, argv);
    return cli::fail("bad %s", "state");
}

int verb_fail_without_reason(void * /*ctx*/, int argc, char **argv) noexcept
{
    record_call(argc, argv);
    return kFailCode;
}

constexpr std::array<cli::Verb, 3> kVerbs{{
    {"list", nullptr, "list sessions", verb_ok},
    {"set", "<state>", "set alarm state", verb_fail_with_reason},
    {"crash", "<code>", "fail without reason", verb_fail_without_reason},
}};

int s_ctx_marker = 0x5A;

void test_fail_prints_err_line()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    cli::detail::reply_reset();
    const int rc = cli::fail("boom %d", 5);
    TEST_ASSERT_TRUE(cli::detail::reply_failed());
    capture_end();
    TEST_ASSERT_EQUAL_INT(cli::kCmdFail, rc);
    TEST_ASSERT_EQUAL_STRING("ERR boom 5\n", s_capture);
    cli::detail::reply_reset();
    TEST_ASSERT_FALSE(cli::detail::reply_failed());
}

void test_dispatch_known_verb()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    char arg0[] = "alarm";
    char arg1[] = "list";
    char arg2[] = "extra";
    char *argv[] = {arg0, arg1, arg2};
    const int rc = cli::dispatch(kVerbs, &s_ctx_marker, 3, argv, nullptr);
    capture_end();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, s_handler_calls);
    // handler 看到的是去掉组名后的参数：argv[0] = 动词。
    TEST_ASSERT_EQUAL_INT(2, s_last_argc);
    TEST_ASSERT_EQUAL_STRING("list", s_last_verb);
    TEST_ASSERT_EQUAL_STRING("extra", s_last_arg);
    TEST_ASSERT_EQUAL_STRING("", s_capture);
}

void test_dispatch_unknown_verb_prints_usage_and_fails()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    cli::detail::reply_reset();
    char arg0[] = "alarm";
    char arg1[] = "bogus";
    char *argv[] = {arg0, arg1};
    const int rc = cli::dispatch(kVerbs, &s_ctx_marker, 2, argv, nullptr);
    capture_end();
    TEST_ASSERT_EQUAL_INT(cli::kCmdFail, rc);
    TEST_ASSERT_EQUAL_INT(0, s_handler_calls);
    TEST_ASSERT_NOT_NULL(std::strstr(s_capture, "usage: alarm <verb> [args]\n"));
    TEST_ASSERT_NOT_NULL(std::strstr(s_capture, "  list       -              list sessions\n"));
    TEST_ASSERT_NOT_NULL(std::strstr(s_capture, "ERR usage\n"));
}

void test_dispatch_missing_verb_without_default()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    cli::detail::reply_reset();
    char arg0[] = "alarm";
    char *argv[] = {arg0};
    const int rc = cli::dispatch(kVerbs, &s_ctx_marker, 1, argv, nullptr);
    capture_end();
    TEST_ASSERT_EQUAL_INT(cli::kCmdFail, rc);
    TEST_ASSERT_EQUAL_INT(0, s_handler_calls);
    TEST_ASSERT_NOT_NULL(std::strstr(s_capture, "usage: alarm <verb> [args]\n"));
    TEST_ASSERT_NOT_NULL(std::strstr(s_capture, "ERR usage\n"));
}

void test_dispatch_missing_verb_with_default()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    char arg0[] = "alarm";
    char *argv[] = {arg0};
    const int rc = cli::dispatch(kVerbs, &s_ctx_marker, 1, argv, "list");
    capture_end();
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(1, s_handler_calls);
    TEST_ASSERT_EQUAL_INT(1, s_last_argc);
    TEST_ASSERT_EQUAL_STRING("list", s_last_verb);
    TEST_ASSERT_EQUAL_STRING("", s_capture);
}

void test_dispatch_default_verb_not_found()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    cli::detail::reply_reset();
    char arg0[] = "alarm";
    char *argv[] = {arg0};
    const int rc = cli::dispatch(kVerbs, &s_ctx_marker, 1, argv, "nope");
    capture_end();
    TEST_ASSERT_EQUAL_INT(cli::kCmdFail, rc);
    TEST_ASSERT_EQUAL_INT(0, s_handler_calls);
    TEST_ASSERT_NOT_NULL(std::strstr(s_capture, "usage: alarm <verb> [args]\n"));
}

void test_dispatch_null_argv()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    cli::detail::reply_reset();
    const int rc = cli::dispatch(kVerbs, &s_ctx_marker, 0, nullptr, nullptr);
    capture_end();
    TEST_ASSERT_EQUAL_INT(cli::kCmdFail, rc);
    TEST_ASSERT_NOT_NULL(std::strstr(s_capture, "usage:  <verb> [args]\n"));
    TEST_ASSERT_NOT_NULL(std::strstr(s_capture, "ERR usage\n"));
}

void test_dispatch_propagates_handler_failure()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    cli::detail::reply_reset();
    char arg0[] = "alarm";
    char arg1[] = "set";
    char *argv[] = {arg0, arg1};
    const int rc = cli::dispatch(kVerbs, &s_ctx_marker, 2, argv, nullptr);
    const bool failed = cli::detail::reply_failed();
    capture_end();
    TEST_ASSERT_EQUAL_INT(cli::kCmdFail, rc);
    TEST_ASSERT_TRUE(failed);
    TEST_ASSERT_EQUAL_STRING("ERR bad state\n", s_capture);
}

void test_print_usage_format()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    cli::print_usage("alarm", kVerbs);
    capture_end();
    TEST_ASSERT_EQUAL_STRING("usage: alarm <verb> [args]\n"
                             "  list       -              list sessions\n"
                             "  set        <state>        set alarm state\n"
                             "  crash      <code>         fail without reason\n",
                             s_capture);
}

void test_print_section_and_kv_alignment()
{
    capture_begin();
    TEST_ASSERT_TRUE(s_capture_ready);
    cli::print_section("link");
    cli::print_kv("sn", "%s", "ABC123");
    cli::print_kv("a_very_long_key", "%u", 42U);
    capture_end();
    TEST_ASSERT_EQUAL_STRING("[link]\n"
                             "  sn             ABC123\n"
                             "  a_very_long_key 42\n",
                             s_capture);
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fail_prints_err_line);
    RUN_TEST(test_dispatch_known_verb);
    RUN_TEST(test_dispatch_unknown_verb_prints_usage_and_fails);
    RUN_TEST(test_dispatch_missing_verb_without_default);
    RUN_TEST(test_dispatch_missing_verb_with_default);
    RUN_TEST(test_dispatch_default_verb_not_found);
    RUN_TEST(test_dispatch_null_argv);
    RUN_TEST(test_dispatch_propagates_handler_failure);
    RUN_TEST(test_print_usage_format);
    RUN_TEST(test_print_section_and_kv_alignment);
    return UNITY_END();
}
