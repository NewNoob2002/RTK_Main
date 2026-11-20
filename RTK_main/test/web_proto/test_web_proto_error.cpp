// 验证 WebSocket 错误映射与协议字符串。仅 host 单线程测试。
#include "unity.h"
#include "web_proto/Error.hpp"

namespace {

/** @brief 验证模块结果到协议错误的映射。 */
void testResultMapping()
{
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(web::fromResult(fw::Result::kOk)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kBadValue),
                      static_cast<int>(web::fromResult(fw::Result::kInvalidArg)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kBusy),
                      static_cast<int>(web::fromResult(fw::Result::kBusy)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kUnsupported),
                      static_cast<int>(web::fromResult(fw::Result::kNotSupported)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kTimeout),
                      static_cast<int>(web::fromResult(fw::Result::kTimeout)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kFailed),
                      static_cast<int>(web::fromResult(fw::Result::kNoSpace)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kFailed),
                      static_cast<int>(web::fromResult(fw::Result::kNotFound)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kFailed),
                      static_cast<int>(web::fromResult(fw::Result::kIoError)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kFailed),
                      static_cast<int>(web::fromResult(fw::Result::kInvalidState)));
}

/** @brief 验证协议错误字符串。 */
void testErrorStrings()
{
    TEST_ASSERT_EQUAL_STRING("bad_json", web::toString(web::ErrorCode::kBadJson));
    TEST_ASSERT_EQUAL_STRING("too_long", web::toString(web::ErrorCode::kTooLong));
    TEST_ASSERT_EQUAL_STRING("unknown_key", web::toString(web::ErrorCode::kUnknownKey));
    TEST_ASSERT_EQUAL_STRING("bad_value", web::toString(web::ErrorCode::kBadValue));
    TEST_ASSERT_EQUAL_STRING("locked", web::toString(web::ErrorCode::kLocked));
    TEST_ASSERT_EQUAL_STRING("busy", web::toString(web::ErrorCode::kBusy));
    TEST_ASSERT_EQUAL_STRING("unsupported", web::toString(web::ErrorCode::kUnsupported));
    TEST_ASSERT_EQUAL_STRING("failed", web::toString(web::ErrorCode::kFailed));
    TEST_ASSERT_EQUAL_STRING("timeout", web::toString(web::ErrorCode::kTimeout));
    TEST_ASSERT_EQUAL_STRING("ok", web::toString(web::ErrorCode::kNone));
}

} // namespace

/** @brief Unity 前置钩子。 */
void setUp() {}

/** @brief Unity 后置钩子。 */
void tearDown() {}

/** @brief 运行错误协议回归。 @retval Unity 测试结果。 */
int main()
{
    UNITY_BEGIN();
    RUN_TEST(testResultMapping);
    RUN_TEST(testErrorStrings);
    return UNITY_END();
}
