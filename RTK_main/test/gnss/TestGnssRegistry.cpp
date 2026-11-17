// GNSS 驱动注册表 host 测试：型号名/家族映射、family_from_u8/model_from_u8 边界与构建开关下
// enabled_receivers() 的一致性（ADR-0012 §2.2/§2.3）。
#include <cstddef>
#include <span>

#include <cstdint>

#include "gnss/Receiver.hpp"
#include "unity.h"

namespace {

#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
constexpr size_t kExpectedUnicore = 1;
#else
constexpr size_t kExpectedUnicore = 0;
#endif

#if defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
constexpr size_t kExpectedUblox = 1;
#else
constexpr size_t kExpectedUblox = 0;
#endif

#if defined(RTK_GNSS_DRV_COMNAV) && RTK_GNSS_DRV_COMNAV
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
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore),
                      static_cast<int>(gnss::family_of(gnss::Model::kUm980)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore),
                      static_cast<int>(gnss::family_of(gnss::Model::kUm981)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore),
                      static_cast<int>(gnss::family_of(gnss::Model::kUm982)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUblox),
                      static_cast<int>(gnss::family_of(gnss::Model::kZedF9p)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kComnav),
                      static_cast<int>(gnss::family_of(gnss::Model::kK803)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnknown),
                      static_cast<int>(gnss::family_of(gnss::Model::kUnknown)));
}

static void test_enabled_receivers_match_build_flags()
{
    const std::span<const gnss::Receiver *const> receivers = gnss::enabled_receivers();
    TEST_ASSERT_EQUAL_UINT32(
        static_cast<uint32_t>(kExpectedUnicore + kExpectedUblox + kExpectedComnav),
        static_cast<uint32_t>(receivers.size()));

    for (const gnss::Receiver *receiver : receivers) {
        TEST_ASSERT_NOT_NULL(receiver);
        TEST_ASSERT_TRUE(receiver->name().size() > 0U);
        TEST_ASSERT_EQUAL_PTR(receiver, gnss::receiver_for(receiver->family()));
        // 每个驱动都要给出至少一个解码器，否则分流器无法把帧送到它。
        TEST_ASSERT_TRUE(receiver->decoders().size() > 0U);
        for (const demux::Decoder *decoder : receiver->decoders()) {
            TEST_ASSERT_NOT_NULL(decoder);
        }
    }

    // 探测优先级：Unicore → u-blox → ComNav（被关闭的驱动不参与，首位是第一个启用的驱动）。
    if (!receivers.empty()) {
#if defined(RTK_GNSS_DRV_UNICORE) && RTK_GNSS_DRV_UNICORE
        constexpr gnss::Family kExpectedFront = gnss::Family::kUnicore;
#elif defined(RTK_GNSS_DRV_UBLOX) && RTK_GNSS_DRV_UBLOX
        constexpr gnss::Family kExpectedFront = gnss::Family::kUblox;
#else
        constexpr gnss::Family kExpectedFront = gnss::Family::kComnav;
#endif
        TEST_ASSERT_EQUAL(static_cast<int>(kExpectedFront),
                          static_cast<int>(receivers.front()->family()));
    }
    TEST_ASSERT_NULL(gnss::receiver_for(gnss::Family::kUnknown));
}

// family_from_u8/model_from_u8：合法值往返一致，越界回落到 kUnknown（NVS 旧值/未来值）。
static void test_u8_boundaries()
{
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Family::kUnicore),
        static_cast<int>(
            gnss::family_from_u8(static_cast<uint8_t>(gnss::Family::kUnicore)).value()));
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Family::kUblox),
        static_cast<int>(gnss::family_from_u8(static_cast<uint8_t>(gnss::Family::kUblox)).value()));
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Family::kComnav),
        static_cast<int>(
            gnss::family_from_u8(static_cast<uint8_t>(gnss::Family::kComnav)).value()));
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Family::kUnknown),
        static_cast<int>(
            gnss::family_from_u8(static_cast<uint8_t>(gnss::Family::kUnknown)).value()));
    // 越界（旧格式/损坏的 NVS 值）必须显式报告无效，而不是静默当成某个家族。
    TEST_ASSERT_FALSE(gnss::family_from_u8(4U).has_value());
    TEST_ASSERT_FALSE(gnss::family_from_u8(0xFFU).has_value());

    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Model::kUm980),
        static_cast<int>(gnss::model_from_u8(static_cast<uint8_t>(gnss::Model::kUm980)).value()));
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Model::kK803),
        static_cast<int>(gnss::model_from_u8(static_cast<uint8_t>(gnss::Model::kK803)).value()));
    TEST_ASSERT_FALSE(gnss::model_from_u8(6U).has_value());
    TEST_ASSERT_FALSE(gnss::model_from_u8(0xFFU).has_value());
    TEST_ASSERT_FALSE(gnss::model_from_u8(0xFFU).has_value());
}

// family_name 覆盖全部枚举值（main 的追踪文本依赖它，P3a 替换了 main 的 switch）。
static void test_family_names()
{
    TEST_ASSERT_EQUAL_STRING("unknown", gnss::family_name(gnss::Family::kUnknown));
    TEST_ASSERT_EQUAL_STRING("unicore", gnss::family_name(gnss::Family::kUnicore));
    TEST_ASSERT_EQUAL_STRING("ublox", gnss::family_name(gnss::Family::kUblox));
    TEST_ASSERT_EQUAL_STRING("comnav", gnss::family_name(gnss::Family::kComnav));
}

static void test_disabled_family_not_found()
{
    if (kExpectedUnicore == 0) {
        TEST_ASSERT_NULL(gnss::receiver_for(gnss::Family::kUnicore));
    }
    if (kExpectedUblox == 0) {
        TEST_ASSERT_NULL(gnss::receiver_for(gnss::Family::kUblox));
    }
    if (kExpectedComnav == 0) {
        TEST_ASSERT_NULL(gnss::receiver_for(gnss::Family::kComnav));
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_model_names);
    RUN_TEST(test_family_of);
    RUN_TEST(test_enabled_receivers_match_build_flags);
    RUN_TEST(test_u8_boundaries);
    RUN_TEST(test_family_names);
    RUN_TEST(test_disabled_family_not_found);
    return UNITY_END();
}
