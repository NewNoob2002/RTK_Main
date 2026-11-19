// web_proto 键表与错误码 host 测试：每个键的合法/非法值、可选标记、解锁标记、Result 映射。
// 纯 host 测试，无 IDF 依赖；使用 Unity。
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "unity.h"
#include "web_proto/Error.hpp"
#include "web_proto/KeyTable.hpp"

namespace {

web::CanonicalValue validate(const char *key, std::string_view text)
{
    const web::KeySpec *spec = web::findKey(key);
    TEST_ASSERT_NOT_NULL(spec);
    web::CanonicalValue out;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(web::validateValue(*spec, text, out)));
    return out;
}

void assertBadValue(const char *key, std::string_view text)
{
    const web::KeySpec *spec = web::findKey(key);
    TEST_ASSERT_NOT_NULL(spec);
    web::CanonicalValue out;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kBadValue),
                      static_cast<int>(web::validateValue(*spec, text, out)));
}

/**
 * @brief  校验键表总数、名称唯一性与未知键返回空指针
 * @note   单线程主机测试
 */
void testTableShape()
{
    TEST_ASSERT_EQUAL_size_t(16, web::kKeyTable.size());
    for (size_t i = 0; i < web::kKeyTable.size(); ++i) {
        for (size_t j = i + 1; j < web::kKeyTable.size(); ++j) {
            TEST_ASSERT_TRUE(web::kKeyTable[i].name != web::kKeyTable[j].name);
        }
        TEST_ASSERT_NOT_NULL(web::findKey(web::kKeyTable[i].name));
    }
    TEST_ASSERT_NULL(web::findKey("gnss.rate2"));
    TEST_ASSERT_NULL(web::findKey(""));
}

void testKeyMetadata()
{
    const web::KeySpec *rate = web::findKey("gnss.rate");
    TEST_ASSERT_NOT_NULL(rate);
    TEST_ASSERT_EQUAL(static_cast<int>(web::KeyKind::kEnum), static_cast<int>(rate->kind));
    TEST_ASSERT_FALSE(rate->needsUnlock);
    TEST_ASSERT_FALSE(rate->optional);
    TEST_ASSERT_EQUAL_size_t(5, rate->allowed.size());

    for (const char *name :
         {"gnss.const.gps", "gnss.const.bds", "gnss.const.glo", "gnss.const.gal"}) {
        const web::KeySpec *spec = web::findKey(name);
        TEST_ASSERT_NOT_NULL(spec);
        TEST_ASSERT_EQUAL(static_cast<int>(web::KeyKind::kBool), static_cast<int>(spec->kind));
        TEST_ASSERT_TRUE(spec->optional);
    }

    const web::KeySpec *sn = web::findKey("factory.sn");
    TEST_ASSERT_NOT_NULL(sn);
    TEST_ASSERT_EQUAL(static_cast<int>(web::KeyKind::kSn), static_cast<int>(sn->kind));
    TEST_ASSERT_TRUE(sn->needsUnlock);

    const web::KeySpec *pair = web::findKey("bt.pair_next");
    TEST_ASSERT_NOT_NULL(pair);
    TEST_ASSERT_EQUAL(static_cast<int>(web::KeyKind::kBool), static_cast<int>(pair->kind));
}

void testEnumValues()
{
    for (const char *text : {"1", "2", "5", "10", "20"}) {
        const web::CanonicalValue value = validate("gnss.rate", text);
        TEST_ASSERT_EQUAL(static_cast<int>(web::ValueKind::kInt), static_cast<int>(value.kind));
        TEST_ASSERT_EQUAL_INT32(std::atoi(text), value.number);
    }
    for (const char *text : {"0", "50", "100", "200", "500", "1000"}) {
        const web::CanonicalValue value = validate("gnss.out.gga", text);
        TEST_ASSERT_EQUAL_INT32(std::atoi(text), value.number);
    }
    for (const char *text : {"0", "3", "01", "", " 1", "1 ", "+1", "-1", "1.0", "20x"}) {
        assertBadValue("gnss.rate", text);
    }
    assertBadValue("gnss.out.gst", "25");
    assertBadValue("gnss.out.gst", "1001");
}

void testBoolValues()
{
    TEST_ASSERT_EQUAL_INT32(0, validate("bt.pair_next", "0").number);
    TEST_ASSERT_EQUAL_INT32(1, validate("bt.pair_next", "1").number);
    TEST_ASSERT_EQUAL_INT32(1, validate("gnss.const.bds", "1").number);
    assertBadValue("bt.pair_next", "2");
    assertBadValue("bt.pair_next", "true");
    assertBadValue("bt.pair_next", "");
}

void testMacValues()
{
    const web::CanonicalValue upper = validate("bt.wl.add", "AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL(static_cast<int>(web::ValueKind::kBytes), static_cast<int>(upper.kind));
    TEST_ASSERT_EQUAL_UINT8(6, upper.size);
    const std::array<uint8_t, 6> expected{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(expected[i], upper.bytes[i]);
    }
    const web::CanonicalValue lower = validate("bt.wl.del", "aa:bb:cc:dd:ee:ff");
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(expected[i], lower.bytes[i]);
    }
    for (const char *text : {"", "AA:BB:CC:DD:EE", "AA:BB:CC:DD:EE:FF:00", "AA-BB-CC-DD-EE-FF",
                             "AA:BB:CC:DD:EE:FG", "GG:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:F "}) {
        assertBadValue("bt.wl.add", text);
    }
}

void testSnValues()
{
    const web::CanonicalValue first = validate("factory.sn", "A");
    TEST_ASSERT_EQUAL_UINT8(1, first.size);
    TEST_ASSERT_EQUAL_UINT8('A', first.bytes[0]);

    const web::CanonicalValue full = validate("factory.sn", "0123456789ABCDEF");
    TEST_ASSERT_EQUAL_UINT8(16, full.size);
    TEST_ASSERT_EQUAL_UINT8('F', full.bytes[15]);

    const web::CanonicalValue mixed = validate("factory.sn", "p2-a9");
    TEST_ASSERT_EQUAL_UINT8(5, mixed.size);

    for (const char *text :
         {"", "0123456789ABCDEFG", "P2 A0001", "P2A0001!", "P2/A0001", "P2A0001\n"}) {
        assertBadValue("factory.sn", text);
    }
}

/**
 * @brief  校验全表每个键各有一条合法与一条非法样例
 * @note   单线程主机测试
 */
void testEveryKeyAcceptsAndRejects()
{
    // 每个键各一条合法、一条非法值：防止个别表项接错验证器时测试仍然通过。
    struct KeyCase final {
        const char *key;
        const char *good;
        const char *bad;
    };
    constexpr std::array<KeyCase, 16> cases{{
        {"gnss.rate", "20", "3"},
        {"gnss.out.gga", "1000", "25"},
        {"gnss.out.gsa", "500", "101"},
        {"gnss.out.gsv", "200", "1001"},
        {"gnss.out.gst", "100", "1"},
        {"gnss.out.rmc", "50", "1000.0"},
        {"gnss.gga_gate.enabled", "1", "true"},
        {"gnss.gga_gate.min_sats", "8", "41"},
        {"gnss.const.gps", "1", "2"},
        {"gnss.const.bds", "0", "true"},
        {"gnss.const.glo", "1", ""},
        {"gnss.const.gal", "0", "2"},
        {"bt.wl.add", "00:11:22:33:44:55", "00:11:22:33:44"},
        {"bt.wl.del", "AA:BB:CC:DD:EE:FF", "AA:BB:CC:DD:EE:GG"},
        {"bt.pair_next", "1", "2"},
        {"factory.sn", "P2A0001", "P2 A0001"},
    }};
    TEST_ASSERT_EQUAL_size_t(web::kKeyTable.size(), cases.size());
    for (const KeyCase &entry : cases) {
        const web::KeySpec *spec = web::findKey(entry.key);
        TEST_ASSERT_NOT_NULL(spec);
        web::CanonicalValue value;
        TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                          static_cast<int>(web::validateValue(*spec, entry.good, value)));
        const bool integerKind = spec->kind == web::KeyKind::kEnum ||
                                 spec->kind == web::KeyKind::kBool ||
                                 spec->kind == web::KeyKind::kIntRange;
        TEST_ASSERT_EQUAL(
            static_cast<int>(integerKind ? web::ValueKind::kInt : web::ValueKind::kBytes),
            static_cast<int>(value.kind));
        TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kBadValue),
                          static_cast<int>(web::validateValue(*spec, entry.bad, value)));
    }
}

/**
 * @brief  校验 GGA 质量门控键的合法边界（0..40）与越界/非法输入拒绝
 * @note   单线程主机测试
 */
void testGgaGateKeys()
{
    TEST_ASSERT_EQUAL_INT32(0, validate("gnss.gga_gate.enabled", "0").number);
    TEST_ASSERT_EQUAL_INT32(1, validate("gnss.gga_gate.enabled", "1").number);
    assertBadValue("gnss.gga_gate.enabled", "2");
    assertBadValue("gnss.gga_gate.enabled", "on");

    TEST_ASSERT_EQUAL_INT32(0, validate("gnss.gga_gate.min_sats", "0").number);
    TEST_ASSERT_EQUAL_INT32(8, validate("gnss.gga_gate.min_sats", "8").number);
    TEST_ASSERT_EQUAL_INT32(40, validate("gnss.gga_gate.min_sats", "40").number);
    for (const char *bad : {"-1", "-0", "41", "100", "08", "", " 8", "8 ", "8.0", "abc"}) {
        assertBadValue("gnss.gga_gate.min_sats", bad);
    }
}

void testFailureKeepsPreviousValue()
{
    // 公开契约：失败时 out 保持原内容；末尾非法字符不得留下半份前缀。
    const web::KeySpec *mac = web::findKey("bt.wl.add");
    TEST_ASSERT_NOT_NULL(mac);
    web::CanonicalValue value;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(web::validateValue(*mac, "AA:BB:CC:DD:EE:FF", value)));
    const web::CanonicalValue previous = value;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kBadValue),
                      static_cast<int>(web::validateValue(*mac, "AA:BB:CC:DD:EE:F ", value)));
    TEST_ASSERT_EQUAL(static_cast<int>(previous.kind), static_cast<int>(value.kind));
    TEST_ASSERT_EQUAL_INT32(previous.number, value.number);
    TEST_ASSERT_EQUAL_UINT8(previous.size, value.size);
    for (size_t i = 0; i < previous.bytes.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(previous.bytes[i], value.bytes[i]);
    }

    const web::KeySpec *sn = web::findKey("factory.sn");
    TEST_ASSERT_NOT_NULL(sn);
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(web::validateValue(*sn, "0123456789ABCDEF", value)));
    const web::CanonicalValue filled = value;
    TEST_ASSERT_EQUAL_UINT8(16, filled.size);
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kBadValue),
                      static_cast<int>(web::validateValue(*sn, "0123456789ABCDE!", value)));
    TEST_ASSERT_EQUAL_UINT8(filled.size, value.size);
    for (size_t i = 0; i < filled.bytes.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(filled.bytes[i], value.bytes[i]);
    }
}

void testNegativeZeroRejected()
{
    // "-0" 是合法 JSON 数字但不是协议 §4 列举的取值，按 mock 的语义拒绝。
    for (const char *text : {"-0", "-00"}) {
        assertBadValue("gnss.out.gga", text);
        assertBadValue("gnss.out.gst", text);
        assertBadValue("gnss.rate", text);
    }
}

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

void setUp() {}
void tearDown() {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(testTableShape);
    RUN_TEST(testKeyMetadata);
    RUN_TEST(testEnumValues);
    RUN_TEST(testBoolValues);
    RUN_TEST(testMacValues);
    RUN_TEST(testSnValues);
    RUN_TEST(testEveryKeyAcceptsAndRejects);
    RUN_TEST(testGgaGateKeys);
    RUN_TEST(testFailureKeepsPreviousValue);
    RUN_TEST(testNegativeZeroRejected);
    RUN_TEST(testResultMapping);
    RUN_TEST(testErrorStrings);
    return UNITY_END();
}
