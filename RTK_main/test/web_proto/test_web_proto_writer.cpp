// web_proto JSON 输出器 host 测试：对象/嵌套/字符串数组/int32/bool/转义/缓冲不足/非法顺序。
// 纯 host 测试，无 IDF 依赖；使用 Unity。
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "unity.h"
#include "web_proto/Writer.hpp"

namespace {

std::string render(web::JsonWriter &writer)
{
    std::array<char, web::kMaxJsonBytes> out{};
    const size_t length = writer.finish(std::span<char>(out.data(), out.size()));
    if (length == 0) {
        return std::string();
    }
    return std::string(out.data(), length);
}

void testFlatObject()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("t", "ack"));
    TEST_ASSERT_TRUE(writer.writeInt("id", 7));
    TEST_ASSERT_TRUE(writer.writeBool("ok", true));
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(R"({"t":"ack","id":7,"ok":true})", render(writer).c_str());
}

void testNestedObject()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("t", "status"));
    TEST_ASSERT_TRUE(writer.beginNested("gnss"));
    TEST_ASSERT_TRUE(writer.writeInt("sats", 12));
    TEST_ASSERT_TRUE(writer.writeBool("fix", false));
    TEST_ASSERT_TRUE(writer.endNested());
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(R"({"t":"status","gnss":{"sats":12,"fix":false}})",
                             render(writer).c_str());
}

void testStringArray()
{
    const std::array<std::string_view, 2> macs{"AA:BB:CC:DD:EE:FF", "11:22:33:44:55:66"};
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.beginNested("bt"));
    TEST_ASSERT_TRUE(writer.writeStringArray("whitelist", macs));
    TEST_ASSERT_TRUE(writer.endNested());
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(R"({"bt":{"whitelist":["AA:BB:CC:DD:EE:FF","11:22:33:44:55:66"]}})",
                             render(writer).c_str());
}

void testEmptyStringArray()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.beginNested("bt"));
    TEST_ASSERT_TRUE(writer.writeStringArray("whitelist", std::span<const std::string_view>()));
    TEST_ASSERT_TRUE(writer.endNested());
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(R"({"bt":{"whitelist":[]}})", render(writer).c_str());
}

void testEscaping()
{
    const std::string value = "q\"b\\c\n\t";
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("s", value));
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(R"({"s":"q\"b\\c\n\t"})", render(writer).c_str());
}

void testEscapingOtherControls()
{
    const std::string value(1, '\x01');
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("s", value));
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(R"({"s":"\u0001"})", render(writer).c_str());
}

void testIntBoundaries()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeInt("min", -2147483647 - 1));
    TEST_ASSERT_TRUE(writer.writeInt("max", 2147483647));
    TEST_ASSERT_TRUE(writer.writeInt("zero", 0));
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(R"({"min":-2147483648,"max":2147483647,"zero":0})",
                             render(writer).c_str());
}

void testSmallBufferLeavesOutputUntouched()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("t", "ack"));
    TEST_ASSERT_TRUE(writer.endObject());

    std::array<char, 8> small{};
    small.fill('X');
    TEST_ASSERT_EQUAL_size_t(0, writer.finish(std::span<char>(small.data(), small.size())));
    for (const char c : small) {
        TEST_ASSERT_EQUAL_CHAR('X', c);
    }

    std::array<char, 11> exact{};
    TEST_ASSERT_EQUAL_size_t(11, writer.finish(std::span<char>(exact.data(), exact.size())));
    TEST_ASSERT_EQUAL_STRING_LEN(R"({"t":"ack"})", exact.data(), exact.size());
}

void testFinishRequiresClosedObject()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("t", "ack"));
    TEST_ASSERT_EQUAL_size_t(0, render(writer).size());

    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_size_t(11, render(writer).size());
}

void testInvalidSequences()
{
    web::JsonWriter noObject;
    TEST_ASSERT_FALSE(noObject.writeString("t", "ack"));

    web::JsonWriter deep;
    TEST_ASSERT_TRUE(deep.beginObject());
    TEST_ASSERT_TRUE(deep.beginNested("a"));
    TEST_ASSERT_FALSE(deep.beginNested("b"));

    web::JsonWriter wrongEnd;
    TEST_ASSERT_TRUE(wrongEnd.beginObject());
    TEST_ASSERT_TRUE(wrongEnd.beginNested("a"));
    TEST_ASSERT_FALSE(wrongEnd.endObject());

    web::JsonWriter closed;
    TEST_ASSERT_TRUE(closed.beginObject());
    TEST_ASSERT_TRUE(closed.beginNested("a"));
    TEST_ASSERT_TRUE(closed.endNested());
    TEST_ASSERT_TRUE(closed.endObject());
    TEST_ASSERT_FALSE(closed.beginObject());
}

void testCapacityOverflow()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    const std::string big(web::kMaxJsonBytes, 'a');
    TEST_ASSERT_FALSE(writer.writeString("s", big));
    TEST_ASSERT_EQUAL_size_t(0, render(writer).size());
}

void testExactCapacityWithEscaping()
{
    // 帧长恰好 1024 B（含转义与末尾右花括号）必须成功；多 1 字节必须整体失败。
    // 结构：{ "s" : " <pad+换行> " } = 1 + 3 + 1 + 2 + (pad + 2) + 1 = pad + 10。
    constexpr size_t kOverhead = 10;
    const std::string base(web::kMaxJsonBytes - kOverhead, 'a');
    std::string value = base;
    value += '\n';

    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("s", value));
    TEST_ASSERT_TRUE(writer.endObject());

    // 调用方缓冲在帧尾放哨兵，检查不会越界写入。
    std::array<char, web::kMaxJsonBytes + 2> out{};
    out.fill('S');
    const size_t length = writer.finish(std::span<char>(out.data(), web::kMaxJsonBytes));
    TEST_ASSERT_EQUAL_size_t(web::kMaxJsonBytes, length);
    TEST_ASSERT_EQUAL_CHAR('{', out.front());
    TEST_ASSERT_EQUAL_CHAR('}', out[web::kMaxJsonBytes - 1]);
    TEST_ASSERT_EQUAL_CHAR('S', out[web::kMaxJsonBytes]);
    TEST_ASSERT_EQUAL_CHAR('S', out[web::kMaxJsonBytes + 1]);

    web::JsonWriter overflow;
    TEST_ASSERT_TRUE(overflow.beginObject());
    const std::string oneMore = value + 'a';
    // 该帧完整长度为 1025 B：字符串本身恰好写满 1024 B 暂存，末尾右花括号无处可放。
    TEST_ASSERT_TRUE(overflow.writeString("s", oneMore));
    TEST_ASSERT_FALSE(overflow.endObject());
    std::array<char, web::kMaxJsonBytes + 2> untouched{};
    untouched.fill('S');
    TEST_ASSERT_EQUAL_size_t(0,
                             overflow.finish(std::span<char>(untouched.data(), untouched.size())));
    TEST_ASSERT_EQUAL_CHAR('S', untouched.front());
    TEST_ASSERT_EQUAL_CHAR('S', untouched[untouched.size() - 1]);
}

void testClearReuse()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("t", "ack"));
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_size_t(11, render(writer).size());

    writer.clear();
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("t", "bye"));
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(R"({"t":"bye"})", render(writer).c_str());
}

/**
 * @brief  校验 config 帧输出嵌套对象 gga_gate:{enabled,min_sats} 的 JSON 格式
 * @note   单线程主机测试
 */
void testConfigGgaGateNestedObject()
{
    web::JsonWriter writer;
    TEST_ASSERT_TRUE(writer.beginObject());
    TEST_ASSERT_TRUE(writer.writeString("t", "config"));
    TEST_ASSERT_TRUE(writer.writeInt("gnss.rate", 10));
    TEST_ASSERT_TRUE(writer.beginNested("gga_gate"));
    TEST_ASSERT_TRUE(writer.writeBool("enabled", true));
    TEST_ASSERT_TRUE(writer.writeInt("min_sats", 8));
    TEST_ASSERT_TRUE(writer.endNested());
    TEST_ASSERT_TRUE(writer.endObject());
    TEST_ASSERT_EQUAL_STRING(
        R"({"t":"config","gnss.rate":10,"gga_gate":{"enabled":true,"min_sats":8}})",
        render(writer).c_str());
}

} // namespace

void setUp() {}
void tearDown() {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(testFlatObject);
    RUN_TEST(testNestedObject);
    RUN_TEST(testStringArray);
    RUN_TEST(testEmptyStringArray);
    RUN_TEST(testEscaping);
    RUN_TEST(testEscapingOtherControls);
    RUN_TEST(testIntBoundaries);
    RUN_TEST(testSmallBufferLeavesOutputUntouched);
    RUN_TEST(testFinishRequiresClosedObject);
    RUN_TEST(testInvalidSequences);
    RUN_TEST(testCapacityOverflow);
    RUN_TEST(testExactCapacityWithEscaping);
    RUN_TEST(testClearReuse);
    RUN_TEST(testConfigGgaGateNestedObject);
    return UNITY_END();
}
