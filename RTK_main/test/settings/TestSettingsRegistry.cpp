// settings 注册表与值校验/周期编码 host 单元测试（ADR-0018 §2.3）。
// 纯主机单线程测试，无 IDF/FreeRTOS 依赖，不分配堆。
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "settings/Registry.hpp"
#include "unity.h"

namespace {

/**
 * @brief  构造一份典型 CM680 10 Hz 下的 GNSS 能力快照
 * @retval 填充好的 GnssCaps（GSA 仅 0/1000，GSV 含 0/1000/5000，其余含 0/100/200/500/1000）
 */
settings::GnssCaps makeCm680Caps10Hz() noexcept
{
    settings::GnssCaps caps{};
    caps.valid = true;
    caps.navRateHz = 10U;
    caps.navRatesHz = {1, 2, 5, 10};
    caps.navRateCount = 4U;

    const settings::PeriodDomain std10Hz{{0, 100, 200, 500, 1000}, 5U, true};
    const settings::PeriodDomain gsaCm680{{0, 1000}, 2U, true};
    const settings::PeriodDomain gsvCm680{{0, 1000, 5000}, 3U, true};
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGga)] = std10Hz;
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGsa)] = gsaCm680;
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGsv)] = gsvCm680;
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGst)] = std10Hz;
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kRmc)] = std10Hz;
    caps.constMask = 0U;
    return caps;
}

/**
 * @brief  查询键的当前取值域并按其校验文本值
 * @param  key:  键名
 * @param  caps: GNSS 能力快照
 * @param  text: 待校验文本
 * @param[out] out: 输出规范值
 * @retval Error 校验结果
 */
settings::Error validateWithCaps(const char *key, const settings::GnssCaps &caps,
                                 std::string_view text, settings::CanonicalValue &out) noexcept
{
    const settings::KeySpec *spec = settings::findKey(key);
    TEST_ASSERT_NOT_NULL(spec);
    settings::Domain dom{};
    spec->domain(caps, true, dom);
    return settings::validateValue(*spec, dom, text, out);
}

/**
 * @brief  校验注册表形状：20 项、键名唯一、KeyId 与下标一致、未知键返回空
 */
void testRegistryShape()
{
    const auto table = settings::keyTable();
    TEST_ASSERT_EQUAL_size_t(20U, table.size());
    for (size_t i = 0U; i < table.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(i), static_cast<uint8_t>(table[i].id));
        TEST_ASSERT_NOT_NULL(table[i].domain);
        const bool constellation = table[i].name.starts_with("gnss.const.");
        TEST_ASSERT_EQUAL(constellation, table[i].encode == nullptr);
        TEST_ASSERT_EQUAL_PTR(&table[i], settings::findKey(table[i].name));
        TEST_ASSERT_EQUAL_PTR(&table[i], settings::keyAt(table[i].id));
        for (size_t j = i + 1U; j < table.size(); ++j) {
            TEST_ASSERT_TRUE(table[i].name != table[j].name);
        }
    }
    TEST_ASSERT_NULL(settings::findKey("gnss.rate2"));
    TEST_ASSERT_NULL(settings::findKey(""));
    TEST_ASSERT_NULL(settings::keyAt(settings::KeyId::kCount));
}

/**
 * @brief  校验枚举、GSV 5000 ms（0.2 Hz）、CM680 GSA 亚秒拒绝与负零拒绝
 */
void testEnumAndDynamicDomains()
{
    const auto caps = makeCm680Caps10Hz();
    settings::CanonicalValue out{};

    for (const char *rate : {"1", "2", "5", "10"}) {
        TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                          static_cast<int>(validateWithCaps("gnss.rate", caps, rate, out)));
        TEST_ASSERT_EQUAL(static_cast<int>(settings::CanonicalKind::kInt),
                          static_cast<int>(out.kind));
    }
    for (const char *bad : {"0", "3", "20", "01", "", " 1", "1 ", "+1", "-1", "-0", "1.0", "20x"}) {
        TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                          static_cast<int>(validateWithCaps("gnss.rate", caps, bad, out)));
    }

    // 10 Hz 下 GGA 允许 0/100/200/500/1000，但 50 ms（20 Hz）域外必须拒绝。
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(validateWithCaps("gnss.out.gga", caps, "100", out)));
    TEST_ASSERT_EQUAL_INT32(100, out.number);
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(validateWithCaps("gnss.out.gga", caps, "50", out)));

    // CM680 GSA 仅支持 0/1000，100/500 必须拒绝。
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(validateWithCaps("gnss.out.gsa", caps, "1000", out)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(validateWithCaps("gnss.out.gsa", caps, "500", out)));

    // CM680 GSV 支持 0/1000/5000（0.2 Hz）。
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(validateWithCaps("gnss.out.gsv", caps, "5000", out)));
    TEST_ASSERT_EQUAL_INT32(5000, out.number);
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(validateWithCaps("gnss.out.gsv", caps, "200", out)));
}

/**
 * @brief  校验布尔、闭区间整数、MAC、SN 及失败不污染输出缓冲契约
 */
void testBoolRangeMacSnAndFailurePreservation()
{
    const auto caps = makeCm680Caps10Hz();
    settings::CanonicalValue out{};

    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(validateWithCaps("gnss.gga_gate.enabled", caps, "1", out)));
    TEST_ASSERT_EQUAL_INT32(1, out.number);
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kBadValue),
        static_cast<int>(validateWithCaps("gnss.gga_gate.enabled", caps, "true", out)));

    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(validateWithCaps("gnss.gga_gate.min_sats", caps, "0", out)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(validateWithCaps("gnss.gga_gate.min_sats", caps, "40", out)));
    for (const char *bad : {"-1", "-0", "41", "08", "", " 8", "8.0"}) {
        TEST_ASSERT_EQUAL(
            static_cast<int>(settings::Error::kBadValue),
            static_cast<int>(validateWithCaps("gnss.gga_gate.min_sats", caps, bad, out)));
    }

    // MAC 合法与非法不污染旧值。
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(validateWithCaps("bt.wl.add", caps, "AA:BB:CC:DD:EE:FF", out)));
    TEST_ASSERT_EQUAL_UINT8(6U, out.size);
    const settings::CanonicalValue prevMac = out;
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kBadValue),
        static_cast<int>(validateWithCaps("bt.wl.add", caps, "11:22:33:44:55:F ", out)));
    TEST_ASSERT_EQUAL_UINT8(prevMac.size, out.size);
    TEST_ASSERT_EQUAL_UINT8(0xAAU, out.bytes[0]);

    // SN 合法与非法不污染旧值。
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(validateWithCaps("factory.sn", caps, "P2A0001-X", out)));
    TEST_ASSERT_EQUAL_UINT8(9U, out.size);
    const settings::CanonicalValue prevSn = out;
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(validateWithCaps("factory.sn", caps, "BAD SN!", out)));
    TEST_ASSERT_EQUAL_UINT8(prevSn.size, out.size);
    TEST_ASSERT_EQUAL_UINT8('P', out.bytes[0]);
}

/**
 * @brief  校验周期毫秒到协议有符号 int8_t 间隔的编码（含 5000 ms -> -5 与 1000 ms -> -1）
 */
void testPeriodMsToInterval()
{
    int8_t iv = 0;
    TEST_ASSERT_TRUE(settings::periodMsToInterval(0, iv));
    TEST_ASSERT_EQUAL_INT8(0, iv);
    TEST_ASSERT_TRUE(settings::periodMsToInterval(50, iv));
    TEST_ASSERT_EQUAL_INT8(20, iv);
    TEST_ASSERT_TRUE(settings::periodMsToInterval(100, iv));
    TEST_ASSERT_EQUAL_INT8(10, iv);
    TEST_ASSERT_TRUE(settings::periodMsToInterval(200, iv));
    TEST_ASSERT_EQUAL_INT8(5, iv);
    TEST_ASSERT_TRUE(settings::periodMsToInterval(500, iv));
    TEST_ASSERT_EQUAL_INT8(2, iv);
    TEST_ASSERT_TRUE(settings::periodMsToInterval(1000, iv));
    TEST_ASSERT_EQUAL_INT8(-1, iv);
    TEST_ASSERT_TRUE(settings::periodMsToInterval(5000, iv));
    TEST_ASSERT_EQUAL_INT8(-5, iv);
    TEST_ASSERT_FALSE(settings::periodMsToInterval(-1000, iv));
    TEST_ASSERT_FALSE(settings::periodMsToInterval(150, iv));
    TEST_ASSERT_FALSE(settings::periodMsToInterval(1500, iv));
    TEST_ASSERT_FALSE(settings::periodMsToInterval(128000, iv));
}

/**
 * @brief  校验 SV3 新增手机/GNSS 键（gnss.ppp/gnss.sat_track/phone.height/phone.sn_pn）校验与域约束
 */
void testPhoneAndSatTrackKeys()
{
    auto caps = makeCm680Caps10Hz();
    caps.pppSupported = false;
    caps.constMask = settings::const_bit::kGps | settings::const_bit::kBds |
                     settings::const_bit::kGlo | settings::const_bit::kGal |
                     settings::const_bit::kSbas;
    settings::CanonicalValue out{};

    // CM680 不支持 PPP，domain.supported == false；UM98x 开启后合法 0/1。
    {
        const auto *spec = settings::findKey("gnss.ppp");
        TEST_ASSERT_NOT_NULL(spec);
        settings::Domain dom{};
        spec->domain(caps, true, dom);
        TEST_ASSERT_FALSE(dom.supported);
        caps.pppSupported = true;
        spec->domain(caps, true, dom);
        TEST_ASSERT_TRUE(dom.supported);
        TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                          static_cast<int>(settings::validateValue(*spec, dom, "1", out)));
        TEST_ASSERT_EQUAL_INT32(1, out.number);
        TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                          static_cast<int>(settings::validateValue(*spec, dom, "2", out)));
    }

    // gnss.sat_track：截止角 0..90，且仅允许 dom.mask 支持的星座置 1。
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(validateWithCaps("gnss.sat_track", caps, "15,1,0,1,1,1", out)));
    TEST_ASSERT_EQUAL_UINT8(6U, out.size);
    TEST_ASSERT_EQUAL_UINT8(15U, out.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, out.bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(0U, out.bytes[2]);
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kBadValue),
        static_cast<int>(validateWithCaps("gnss.sat_track", caps, "91,1,1,1,1,1", out)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kBadValue),
        static_cast<int>(validateWithCaps("gnss.sat_track", caps, "10,1,1,1,1", out)));
    caps.constMask = settings::const_bit::kGps | settings::const_bit::kBds;
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kBadValue),
        static_cast<int>(validateWithCaps("gnss.sat_track", caps, "10,1,1,1,0,0", out)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(validateWithCaps("gnss.sat_track", caps, "10,1,0,1,0,0", out)));

    // phone.height：16 hex chars -> 8 B 小端双精度，范围 [0.0, 10.0]。
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(validateWithCaps("phone.height", caps, "000000000000F83F", out)));
    TEST_ASSERT_EQUAL_UINT8(8U, out.size);
    TEST_ASSERT_EQUAL_UINT8(0x3FU, out.bytes[7]);
    // 11.0 m / -1.0 m / NaN / 长度不对 必须拒绝。
    for (const char *bad : {"0000000000002640", "000000000000F0BF", "000000000000F87F", "0000"}) {
        TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                          static_cast<int>(validateWithCaps("phone.height", caps, bad, out)));
    }

    // phone.sn_pn：64 hex chars -> 32 B（[0..15] 非空 SN + [16..31] PN，禁止 \r\n）。
    const char *goodSnPn = "534E3132000000000000000000000000"
                           "504E3031000000000000000000000000";
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(validateWithCaps("phone.sn_pn", caps, goodSnPn, out)));
    TEST_ASSERT_EQUAL_UINT8(32U, out.size);
    TEST_ASSERT_EQUAL_UINT8('S', out.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8('P', out.bytes[16]);
    const char *emptySn = "00000000000000000000000000000000"
                          "504E3031000000000000000000000000";
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(validateWithCaps("phone.sn_pn", caps, emptySn, out)));
}

} // namespace

/** @brief Unity 每用例前置钩子。 */
void setUp() {}

/** @brief Unity 每用例后置钩子。 */
void tearDown() {}

/**
 * @brief  运行全部 settings 注册表单元测试
 * @retval 0 全部通过
 */
int main()
{
    UNITY_BEGIN();
    RUN_TEST(testRegistryShape);
    RUN_TEST(testEnumAndDynamicDomains);
    RUN_TEST(testBoolRangeMacSnAndFailurePreservation);
    RUN_TEST(testPeriodMsToInterval);
    RUN_TEST(testPhoneAndSatTrackKeys);
    return UNITY_END();
}
