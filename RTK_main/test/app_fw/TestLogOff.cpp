// FW_TRACE_ENABLED=0 + FW_LOG_MAX_LEVEL=2 时的 fw/Log.hpp（ADR-0009 §2.12）：追踪关闭不影响日志，I 级经直写 sink
// 输出（追踪关闭的固件没有文本环）；高于编译期上限的 FW_LOGD 连同参数求值一起编译掉。

#include <cstdint>
#include <span>
#include <string>

#include "fw/Log.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

uint32_t s_calls = 0U;
std::string s_last;

/**
 * @brief  直写 sink 替身
 * @param  ctx: 未用
 * @param  hdr: 未用
 * @param  payload: 文本
 */
void sink(void *ctx, const fw::LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    static_cast<void>(ctx);
    static_cast<void>(hdr);
    ++s_calls;
    s_last.assign(payload.begin(), payload.end());
}

/**
 * @brief  固定时钟
 * @retval 0
 */
uint32_t clock0() noexcept
{
    return 0U;
}

/**
 * @brief  有副作用的参数，用来证明被编译掉的调用不求值
 * @param  counter: 被递增的计数
 * @retval 递增后的值
 */
int bump(int &counter) noexcept
{
    return ++counter;
}

} // namespace

/** 追踪关闭：日志不受影响，默认 I 级经直写 sink 输出；高于编译期上限的 D 级（本构建上限 I）不求值。 */
void test_off_logs_still_work(void)
{
    static_assert(fw::kLogMaxLevel == fw::LogLevel::kInfo, "this build sets FW_LOG_MAX_LEVEL=2");
    TEST_ASSERT_EQUAL(fw::Result::kOk, fw::log_install_direct(&sink, nullptr, &clock0));
    int evaluated = 0;
    FW_LOGD(fw::TraceCat::kGnss, "x=%d", bump(evaluated));
    TEST_ASSERT_EQUAL_INT(0, evaluated);
    TEST_ASSERT_EQUAL_UINT32(0U, s_calls);
    FW_LOGI(fw::TraceCat::kGnss, "x=%d", bump(evaluated));
    TEST_ASSERT_EQUAL_UINT32(1U, s_calls);
    TEST_ASSERT_EQUAL_STRING("x=1", s_last.c_str());
    fw::log_set_cat_level(fw::TraceCat::kGnss, fw::LogLevel::kDebug);
    TEST_ASSERT_EQUAL(fw::LogLevel::kInfo,
                      fw::log_cat_level(fw::TraceCat::kGnss)); // 运行期也被上限截住
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_off_logs_still_work);
    return UNITY_END();
}
