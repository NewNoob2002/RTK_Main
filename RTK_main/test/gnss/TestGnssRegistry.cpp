// GNSS 方言注册表 host 测试：型号名/家族映射与构建开关下 enabled_dialects() 的一致性（ADR-0010 §3）。
#include <cstddef>
#include <span>

#include "gnss/Dialect.hpp"
#include "unity.h"

namespace {

#if defined(RTK_GNSS_DIALECT_UNICORE) && RTK_GNSS_DIALECT_UNICORE
constexpr size_t kExpectedUnicore = 1;
#else
constexpr size_t kExpectedUnicore = 0;
#endif

#if defined(RTK_GNSS_DIALECT_UBLOX) && RTK_GNSS_DIALECT_UBLOX
constexpr size_t kExpectedUblox = 1;
#else
constexpr size_t kExpectedUblox = 0;
#endif

#if defined(RTK_GNSS_DIALECT_COMNAV) && RTK_GNSS_DIALECT_COMNAV
constexpr size_t kExpectedComnav = 1;
#else
constexpr size_t kExpectedComnav = 0;
#endif

} // namespace

void setUp() {}
void tearDown() {}

static void test_model_names()
{
    TEST_ASSERT_EQUAL_STRING("UM980", gnss::model_name(gnss::Model::kUm980));
    TEST_ASSERT_EQUAL_STRING("UM981", gnss::model_name(gnss::Model::kUm981));
    TEST_ASSERT_EQUAL_STRING("UM982", gnss::model_name(gnss::Model::kUm982));
    TEST_ASSERT_EQUAL_STRING("ZED-F9P", gnss::model_name(gnss::Model::kZedF9p));
    TEST_ASSERT_EQUAL_STRING("K803", gnss::model_name(gnss::Model::kK803));
    TEST_ASSERT_EQUAL_STRING("unknown", gnss::model_name(gnss::Model::kUnknown));
}

static void test_family_of()
{
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore), static_cast<int>(gnss::family_of(gnss::Model::kUm980)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore), static_cast<int>(gnss::family_of(gnss::Model::kUm981)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore), static_cast<int>(gnss::family_of(gnss::Model::kUm982)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUblox), static_cast<int>(gnss::family_of(gnss::Model::kZedF9p)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kComnav), static_cast<int>(gnss::family_of(gnss::Model::kK803)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnknown),
                      static_cast<int>(gnss::family_of(gnss::Model::kUnknown)));
}

static void test_enabled_dialects_match_build_flags()
{
    const std::span<const gnss::Dialect *const> dialects = gnss::enabled_dialects();
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kExpectedUnicore + kExpectedUblox + kExpectedComnav),
                             static_cast<uint32_t>(dialects.size()));

    for (const gnss::Dialect *dialect : dialects) {
        TEST_ASSERT_NOT_NULL(dialect);
        TEST_ASSERT_NOT_NULL(dialect->name);
        TEST_ASSERT_NOT_NULL(dialect->build_probe);
        TEST_ASSERT_NOT_NULL(dialect->match_probe);
        TEST_ASSERT_NOT_NULL(dialect->rover_step);
        TEST_ASSERT_NOT_NULL(dialect->match_reply);
        TEST_ASSERT_NOT_NULL(dialect->caps);
        TEST_ASSERT_EQUAL_PTR(dialect, gnss::dialect_for(dialect->family));
    }

    // 探测优先级：Unicore → u-blox → ComNav（被关闭的方言不参与）。
    if (!dialects.empty()) {
        TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore), static_cast<int>(dialects.front()->family));
    }
    TEST_ASSERT_NULL(gnss::dialect_for(gnss::Family::kUnknown));
}

static void test_disabled_family_not_found()
{
    if (kExpectedUnicore == 0) {
        TEST_ASSERT_NULL(gnss::dialect_for(gnss::Family::kUnicore));
    }
    if (kExpectedUblox == 0) {
        TEST_ASSERT_NULL(gnss::dialect_for(gnss::Family::kUblox));
    }
    if (kExpectedComnav == 0) {
        TEST_ASSERT_NULL(gnss::dialect_for(gnss::Family::kComnav));
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_model_names);
    RUN_TEST(test_family_of);
    RUN_TEST(test_enabled_dialects_match_build_flags);
    RUN_TEST(test_disabled_family_not_found);
    return UNITY_END();
}
