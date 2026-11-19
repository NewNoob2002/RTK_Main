// GGA 质量门控（Q1）纯主机测试：质量 0/卫星不足/空字段阻断、连续 3 条达标放行、开关关闭全部放行、边界校验。
// 纯 host 测试，无 IDF 依赖；使用 Unity。
#include <cstdint>
#include <span>
#include <string_view>

#include "GgaQualityGate.hpp"
#include "unity.h"

namespace {

constexpr std::string_view kGoodGga =
    "$GNGGA,080000.00,3112.34567,N,12123.45678,E,4,12,0.8,15.2,M,-5.0,M,1.0,0001*7A\r\n";
constexpr std::string_view kGoodGgaLeadingZero =
    "$GPGGA,080001.00,3112.34567,N,12123.45678,E,1,08,1.2,15.2,M,-5.0,M,,*5E\r\n";
constexpr std::string_view kQualityZeroGga =
    "$GNGGA,080002.00,3112.34567,N,12123.45678,E,0,15,0.8,15.2,M,-5.0,M,,*70\r\n";
constexpr std::string_view kLowSatsGga =
    "$GNGGA,080003.00,3112.34567,N,12123.45678,E,4,07,1.0,15.2,M,-5.0,M,,*71\r\n";
constexpr std::string_view kEmptyQualityGga =
    "$GNGGA,080004.00,3112.34567,N,12123.45678,E,,12,1.0,15.2,M,-5.0,M,,*72\r\n";
constexpr std::string_view kEmptySatsGga =
    "$GNGGA,080005.00,3112.34567,N,12123.45678,E,4,,1.0,15.2,M,-5.0,M,,*73\r\n";
constexpr std::string_view kMalformedSatsGga =
    "$GNGGA,080006.00,3112.34567,N,12123.45678,E,4,1A,1.0,15.2,M,-5.0,M,,*74\r\n";
constexpr std::string_view kShortGga = "$GNGGA,080007.00,3112.34567,N*00\r\n";
constexpr std::string_view kNonGga =
    "$GNRMC,080008.00,A,3112.34567,N,12123.45678,E,0.0,0.0,191126,,,A*00\r\n";

/**
 * @brief  将字符串视图转为只读字节切片
 * @param  text: ASCII NMEA 语句文本
 * @retval 指向同一内存的 uint8_t span
 * @note   仅单线程测试辅助
 */
std::span<const uint8_t> bytesOf(std::string_view text) noexcept
{
    // NMEA 文本按字节视角传入解析器。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
}

/**
 * @brief  校验默认配置（关闭、min_sats=8）下所有 GGA 全部放行且不计拦截
 * @note   单线程主机测试
 */
void testDisabledPassesAll()
{
    gnss::GgaQualityGate gate{};
    TEST_ASSERT_TRUE(gate.setConfig(gnss::GgaGateConfig::defaults()));
    TEST_ASSERT_FALSE(gate.config().enabled);
    TEST_ASSERT_EQUAL_UINT8(8U, gate.config().minSats);
    TEST_ASSERT_TRUE(gate.ggaAllowed());

    TEST_ASSERT_TRUE(gate.evaluate(bytesOf(kQualityZeroGga)));
    TEST_ASSERT_TRUE(gate.evaluate(bytesOf(kLowSatsGga)));
    TEST_ASSERT_TRUE(gate.evaluate(bytesOf(kEmptyQualityGga)));
    TEST_ASSERT_TRUE(gate.evaluate(bytesOf(kEmptySatsGga)));
    TEST_ASSERT_TRUE(gate.ggaAllowed());

    const auto st = gate.stats();
    TEST_ASSERT_FALSE(st.enabled);
    TEST_ASSERT_FALSE(st.blocked);
    TEST_ASSERT_EQUAL_UINT32(0U, st.ggaBlocked);
    TEST_ASSERT_EQUAL_UINT32(0U, st.gateTrans);
}

/**
 * @brief  校验质量 0、卫星不足、空字段、解析失败与非 GGA 均立即触发阻断
 * @note   单线程主机测试
 */
void testBadCasesBlockImmediately()
{
    const std::string_view badCases[] = {
        kQualityZeroGga,   kLowSatsGga, kEmptyQualityGga, kEmptySatsGga,
        kMalformedSatsGga, kShortGga,   kNonGga,
    };
    for (const std::string_view bad : badCases) {
        gnss::GgaQualityGate gate{};
        TEST_ASSERT_TRUE(gate.setConfig({.enabled = true, .minSats = 8U}));
        TEST_ASSERT_TRUE(gate.evaluate(bytesOf(kGoodGga)));
        TEST_ASSERT_TRUE(gate.ggaAllowed());

        TEST_ASSERT_FALSE(gate.evaluate(bytesOf(bad)));
        TEST_ASSERT_FALSE(gate.ggaAllowed());
        const auto st = gate.stats();
        TEST_ASSERT_TRUE(st.blocked);
        TEST_ASSERT_EQUAL_UINT32(1U, st.ggaBlocked);
        TEST_ASSERT_EQUAL_UINT32(1U, st.gateTrans);
    }
}

/**
 * @brief  校验迟滞策略：阻断后需连续 3 条达标 GGA 才放行，中途出现 1 条坏帧须重新计数
 * @note   单线程主机测试
 */
void testHysteresisReleaseAfterThreeGood()
{
    gnss::GgaQualityGate gate{};
    TEST_ASSERT_TRUE(gate.setConfig({.enabled = true, .minSats = 8U}));

    // 1 条不达标即阻断
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kQualityZeroGga)));
    TEST_ASSERT_FALSE(gate.ggaAllowed());

    // 连续 2 条达标后出现 1 条卫星不足 → 计数归零，继续阻断
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kGoodGga)));
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kGoodGgaLeadingZero)));
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kLowSatsGga)));
    TEST_ASSERT_FALSE(gate.ggaAllowed());

    // 连续 3 条达标：前 2 条仍阻断，第 3 条放行
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kGoodGga)));
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kGoodGgaLeadingZero)));
    TEST_ASSERT_FALSE(gate.ggaAllowed());
    TEST_ASSERT_TRUE(gate.evaluate(bytesOf(kGoodGga)));
    TEST_ASSERT_TRUE(gate.ggaAllowed());

    const auto st = gate.stats();
    TEST_ASSERT_FALSE(st.blocked);
    TEST_ASSERT_EQUAL_UINT32(6U, st.ggaBlocked);
    TEST_ASSERT_EQUAL_UINT32(2U, st.gateTrans);

    // 放行态再收 1 条不达标即再次阻断
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kEmptySatsGga)));
    TEST_ASSERT_FALSE(gate.ggaAllowed());
    TEST_ASSERT_EQUAL_UINT32(7U, gate.stats().ggaBlocked);
    TEST_ASSERT_EQUAL_UINT32(3U, gate.stats().gateTrans);
}

/**
 * @brief  校验配置范围（0..40）以及运行中关闭开关立即解除阻断
 * @note   单线程主机测试
 */
void testConfigBoundsAndImmediateDisable()
{
    gnss::GgaQualityGate gate{};
    TEST_ASSERT_TRUE(gate.setConfig({.enabled = true, .minSats = 0U}));
    TEST_ASSERT_TRUE(gate.evaluate(bytesOf(kLowSatsGga)));      // quality=4, sats=7 >= 0 达标
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kQualityZeroGga))); // quality=0 仍阻断

    // 越界 min_sats=41 必须拒绝且保留原配置与阻断态
    TEST_ASSERT_FALSE(gate.setConfig({.enabled = false, .minSats = 41U}));
    TEST_ASSERT_TRUE(gate.config().enabled);
    TEST_ASSERT_EQUAL_UINT8(0U, gate.config().minSats);
    TEST_ASSERT_FALSE(gate.ggaAllowed());

    // min_sats=40 合法
    TEST_ASSERT_TRUE(gate.setConfig({.enabled = true, .minSats = 40U}));
    TEST_ASSERT_EQUAL_UINT8(40U, gate.config().minSats);
    TEST_ASSERT_FALSE(gate.evaluate(bytesOf(kGoodGga))); // sats=12 < 40 仍阻断

    // 关闭开关立即生效并解除阻断（计 1 次状态切换）
    const uint32_t transBefore = gate.stats().gateTrans;
    TEST_ASSERT_TRUE(gate.setConfig({.enabled = false, .minSats = 8U}));
    TEST_ASSERT_TRUE(gate.ggaAllowed());
    TEST_ASSERT_FALSE(gate.stats().blocked);
    TEST_ASSERT_EQUAL_UINT32(transBefore + 1U, gate.stats().gateTrans);
    TEST_ASSERT_TRUE(gate.evaluate(bytesOf(kQualityZeroGga)));
}

} // namespace

void setUp() {}
void tearDown() {}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(testDisabledPassesAll);
    RUN_TEST(testBadCasesBlockImmediately);
    RUN_TEST(testHysteresisReleaseAfterThreeGood);
    RUN_TEST(testConfigBoundsAndImmediateDisable);
    return UNITY_END();
}
