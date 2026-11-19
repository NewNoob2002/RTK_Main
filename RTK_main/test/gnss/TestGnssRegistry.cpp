// GNSS 驱动注册表 host 测试：型号名/家族映射、familyFromU8/modelFromU8 边界与构建开关下
// enabledReceivers() 的一致性（ADR-0012 §2.2/§2.3）。
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

static void testModelNames()
{
    TEST_ASSERT_EQUAL_STRING("UM980", gnss::modelName(gnss::Model::kUm980));
    TEST_ASSERT_EQUAL_STRING("UM981", gnss::modelName(gnss::Model::kUm981));
    TEST_ASSERT_EQUAL_STRING("UM982", gnss::modelName(gnss::Model::kUm982));
    TEST_ASSERT_EQUAL_STRING("ZED-F9P", gnss::modelName(gnss::Model::kZedF9p));
    TEST_ASSERT_EQUAL_STRING("K803", gnss::modelName(gnss::Model::kK803));
    TEST_ASSERT_EQUAL_STRING("CM680", gnss::modelName(gnss::Model::kCm680));
    TEST_ASSERT_EQUAL_STRING("unknown", gnss::modelName(gnss::Model::kUnknown));
}

/**
 * @brief  校验型号到家族的映射表（含 CM680 归属 Optisens）
 * @note   单线程主机测试
 */
static void testFamilyOf()
{
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore),
                      static_cast<int>(gnss::familyOf(gnss::Model::kUm980)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore),
                      static_cast<int>(gnss::familyOf(gnss::Model::kUm981)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnicore),
                      static_cast<int>(gnss::familyOf(gnss::Model::kUm982)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kOptisens),
                      static_cast<int>(gnss::familyOf(gnss::Model::kCm680)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUblox),
                      static_cast<int>(gnss::familyOf(gnss::Model::kZedF9p)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kComnav),
                      static_cast<int>(gnss::familyOf(gnss::Model::kK803)));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kUnknown),
                      static_cast<int>(gnss::familyOf(gnss::Model::kUnknown)));
}

/**
 * @brief  校验构建开关与 enabledReceivers()/receiverFor() 的一致性（Optisens 随 Unicore 启用且不增加探测表项）
 * @note   单线程主机测试
 */
static void testEnabledReceiversMatchBuildFlags()
{
    const std::span<const gnss::Receiver *const> receivers = gnss::enabledReceivers();
    TEST_ASSERT_EQUAL_UINT32(
        static_cast<uint32_t>(kExpectedUnicore + kExpectedUblox + kExpectedComnav),
        static_cast<uint32_t>(receivers.size()));

    for (const gnss::Receiver *receiver : receivers) {
        TEST_ASSERT_NOT_NULL(receiver);
        TEST_ASSERT_TRUE(receiver->name().size() > 0U);
        TEST_ASSERT_EQUAL_PTR(receiver, gnss::receiverFor(receiver->family()));
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
    if (kExpectedUnicore != 0U) {
        const gnss::Receiver *optisens = gnss::receiverFor(gnss::Family::kOptisens);
        TEST_ASSERT_NOT_NULL(optisens);
        TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kOptisens),
                          static_cast<int>(optisens->family()));
        TEST_ASSERT_TRUE(optisens->name() == std::string_view{"optisens"});
        TEST_ASSERT_TRUE(optisens->decoders().size() > 0U);
    }
    TEST_ASSERT_NULL(gnss::receiverFor(gnss::Family::kUnknown));
}

/**
 * @brief  校验 familyFromU8/modelFromU8 合法往返与越界边界（NVS 兼容）
 * @note   单线程主机测试
 */
static void testU8Boundaries()
{
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Family::kUnicore),
        static_cast<int>(gnss::familyFromU8(static_cast<uint8_t>(gnss::Family::kUnicore)).value()));
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Family::kUblox),
        static_cast<int>(gnss::familyFromU8(static_cast<uint8_t>(gnss::Family::kUblox)).value()));
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Family::kComnav),
        static_cast<int>(gnss::familyFromU8(static_cast<uint8_t>(gnss::Family::kComnav)).value()));
    TEST_ASSERT_EQUAL_UINT8(4U, static_cast<uint8_t>(gnss::Family::kOptisens));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Family::kOptisens),
                      static_cast<int>(gnss::familyFromU8(4U).value()));
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Family::kUnknown),
        static_cast<int>(gnss::familyFromU8(static_cast<uint8_t>(gnss::Family::kUnknown)).value()));
    // 越界（旧格式/损坏的 NVS 值）必须显式报告无效，而不是静默当成某个家族。
    TEST_ASSERT_FALSE(gnss::familyFromU8(5U).has_value());
    TEST_ASSERT_FALSE(gnss::familyFromU8(0xFFU).has_value());

    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Model::kUm980),
        static_cast<int>(gnss::modelFromU8(static_cast<uint8_t>(gnss::Model::kUm980)).value()));
    TEST_ASSERT_EQUAL(
        static_cast<int>(gnss::Model::kK803),
        static_cast<int>(gnss::modelFromU8(static_cast<uint8_t>(gnss::Model::kK803)).value()));
    TEST_ASSERT_EQUAL_UINT8(6U, static_cast<uint8_t>(gnss::Model::kCm680));
    TEST_ASSERT_EQUAL(static_cast<int>(gnss::Model::kCm680),
                      static_cast<int>(gnss::modelFromU8(6U).value()));
    TEST_ASSERT_FALSE(gnss::modelFromU8(7U).has_value());
    TEST_ASSERT_FALSE(gnss::modelFromU8(0xFFU).has_value());
}

/**
 * @brief  校验 familyName 覆盖全部枚举值（含 kOptisens）
 * @note   单线程主机测试
 */
static void testFamilyNames()
{
    TEST_ASSERT_EQUAL_STRING("unknown", gnss::familyName(gnss::Family::kUnknown));
    TEST_ASSERT_EQUAL_STRING("unicore", gnss::familyName(gnss::Family::kUnicore));
    TEST_ASSERT_EQUAL_STRING("ublox", gnss::familyName(gnss::Family::kUblox));
    TEST_ASSERT_EQUAL_STRING("comnav", gnss::familyName(gnss::Family::kComnav));
    TEST_ASSERT_EQUAL_STRING("optisens", gnss::familyName(gnss::Family::kOptisens));
}

/**
 * @brief  校验未启用驱动的家族经 receiverFor 返回 nullptr
 * @note   单线程主机测试
 */
static void testDisabledFamilyNotFound()
{
    if (kExpectedUnicore == 0) {
        TEST_ASSERT_NULL(gnss::receiverFor(gnss::Family::kUnicore));
        TEST_ASSERT_NULL(gnss::receiverFor(gnss::Family::kOptisens));
    }
    if (kExpectedUblox == 0) {
        TEST_ASSERT_NULL(gnss::receiverFor(gnss::Family::kUblox));
    }
    if (kExpectedComnav == 0) {
        TEST_ASSERT_NULL(gnss::receiverFor(gnss::Family::kComnav));
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testModelNames);
    RUN_TEST(testFamilyOf);
    RUN_TEST(testEnabledReceiversMatchBuildFlags);
    RUN_TEST(testU8Boundaries);
    RUN_TEST(testFamilyNames);
    RUN_TEST(testDisabledFamilyNotFound);
    return UNITY_END();
}
