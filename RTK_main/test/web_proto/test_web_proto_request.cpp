// web_proto 请求解析 host 测试：协议 §6 示例、拒绝原因、id 边界、帧长边界、转义与视图归属。
// 纯 host 测试，无 IDF 依赖；使用 Unity。
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "unity.h"
#include "web_proto/Request.hpp"

namespace {

web::ErrorCode parse(std::string_view frame, web::Request &request)
{
    return web::parseRequest(std::span<const char>(frame.data(), frame.size()), request);
}

void assertError(std::string_view frame, web::ErrorCode expected)
{
    web::Request request;
    TEST_ASSERT_EQUAL(static_cast<int>(expected), static_cast<int>(parse(frame, request)));
}

void testParseSetProtocolExample()
{
    web::Request request;
    const std::string_view frame = R"({"t":"set","id":7,"key":"gnss.rate","value":"10"})";
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(frame, request)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::RequestType::kSet), static_cast<int>(request.type));
    TEST_ASSERT_EQUAL_UINT16(7, request.id);
    TEST_ASSERT_EQUAL_STRING_LEN("gnss.rate", request.key.data(), request.key.size());
    TEST_ASSERT_EQUAL_STRING_LEN("10", request.value.data(), request.value.size());
    TEST_ASSERT_EQUAL_size_t(0, request.name.size());
    TEST_ASSERT_EQUAL_size_t(0, request.token.size());
}

void testParseCmdWithUnknownFieldsAndWhitespace()
{
    web::Request request;
    const std::string_view frame =
        " {\n \"id\" : 8 ,\t\"t\" : \"cmd\" , \"name\" : \"save\" , \"future\" : 12 } ";
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(frame, request)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::RequestType::kCmd), static_cast<int>(request.type));
    TEST_ASSERT_EQUAL_UINT16(8, request.id);
    TEST_ASSERT_EQUAL_STRING_LEN("save", request.name.data(), request.name.size());
}

void testParseUnlockEscapedToken()
{
    web::Request request;
    const std::string_view frame = R"({"t":"unlock","id":9,"token":"a\"b\\c"})";
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(frame, request)));
    TEST_ASSERT_EQUAL(static_cast<int>(web::RequestType::kUnlock), static_cast<int>(request.type));
    TEST_ASSERT_EQUAL_UINT16(9, request.id);
    TEST_ASSERT_EQUAL_STRING_LEN("a\"b\\c", request.token.data(), request.token.size());
    TEST_ASSERT_EQUAL_size_t(5, request.token.size());
}

void testRejectNonObject()
{
    for (const std::string_view frame :
         {std::string_view{}, std::string_view{"   "}, std::string_view{"[]"},
          std::string_view{"\"x\""}, std::string_view{"null"}, std::string_view{"12"}}) {
        assertError(frame, web::ErrorCode::kBadJson);
    }
}

void testFrameLengthBoundary()
{
    const std::string_view body = R"({"t":"cmd","id":1,"name":"save"})";
    std::string exactly(body);
    exactly.append(web::kMaxFrameBytes - exactly.size(), ' ');
    TEST_ASSERT_EQUAL_size_t(web::kMaxFrameBytes, exactly.size());
    web::Request request;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(exactly, request)));

    std::string tooLong = exactly;
    tooLong.push_back(' ');
    assertError(tooLong, web::ErrorCode::kTooLong);
}

void testTooLongClearsRequest()
{
    // 复用同一个 Request：超长失败也必须清空上一条请求的 id 与视图。
    web::Request request;
    TEST_ASSERT_EQUAL(
        static_cast<int>(web::ErrorCode::kNone),
        static_cast<int>(parse(R"({"t":"set","id":3,"key":"gnss.rate","value":"10"})", request)));
    TEST_ASSERT_EQUAL_UINT16(3, request.id);
    std::string tooLong(web::kMaxFrameBytes + 1, ' ');
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kTooLong),
                      static_cast<int>(parse(tooLong, request)));
    TEST_ASSERT_EQUAL_UINT16(0, request.id);
    TEST_ASSERT_EQUAL_size_t(0, request.key.size());
    TEST_ASSERT_EQUAL_size_t(0, request.value.size());
}

void testIdBoundaries()
{
    web::Request request;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(R"({"t":"cmd","id":1,"name":"save"})", request)));
    TEST_ASSERT_EQUAL_UINT16(1, request.id);
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(R"({"t":"cmd","id":65535,"name":"save"})", request)));
    TEST_ASSERT_EQUAL_UINT16(65535, request.id);
    assertError(R"({"t":"cmd","id":0,"name":"save"})", web::ErrorCode::kBadValue);
    assertError(R"({"t":"cmd","id":65536,"name":"save"})", web::ErrorCode::kBadValue);
    assertError(R"({"t":"cmd","id":-1,"name":"save"})", web::ErrorCode::kBadValue);
    assertError(R"({"t":"cmd","id":999999999,"name":"save"})", web::ErrorCode::kBadValue);
    assertError(R"({"t":"cmd","id":"1","name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1.5,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1e3,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":01,"name":"save"})", web::ErrorCode::kBadJson);
}

void testIdSyntaxBeforeRange()
{
    // 先判数字语法是否完整（bad_json），再按 1..65535 分类（bad_value）；
    // `-01` 与 `01` 属同一类语法错误，`-0` 语法完整但越界。
    assertError(R"({"t":"cmd","id":-01,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":-00,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1x,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":-0,"name":"save"})", web::ErrorCode::kBadValue);
    assertError(R"({"t":"cmd","id":-65535,"name":"save"})", web::ErrorCode::kBadValue);
}

void testUnknownIntegerRange()
{
    // 协议 §1 把整数限制在 int32，未知字段同样校验。
    web::Request request;
    TEST_ASSERT_EQUAL(
        static_cast<int>(web::ErrorCode::kNone),
        static_cast<int>(parse(R"({"t":"cmd","id":1,"name":"save","x":2147483647})", request)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(web::ErrorCode::kNone),
        static_cast<int>(parse(R"({"t":"cmd","id":1,"name":"save","x":-2147483648})", request)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(web::ErrorCode::kNone),
        static_cast<int>(parse(R"({"t":"cmd","id":1,"name":"save","x":0})", request)));
    assertError(R"({"t":"cmd","id":1,"name":"save","x":2147483648})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save","x":-2147483649})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save","x":99999999999999999999})",
                web::ErrorCode::kBadJson);
}

void testRejectDuplicateKeys()
{
    assertError(R"({"t":"cmd","id":1,"id":2,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","t":"set","id":1,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save","x":1,"x":2})", web::ErrorCode::kBadJson);
}

void testUnknownScalarCountIsFrameBounded()
{
    web::Request request;
    // 第 12、13 个成员都必须解析成功：未知键按协议 §1 忽略，数量不设固定上限。
    for (int extra = 9; extra <= 10; ++extra) {
        std::string frame = R"({"t":"cmd","id":1,"name":"save")";
        for (int i = 0; i < extra; ++i) {
            frame += ",\"x" + std::to_string(i) + "\":0";
        }
        frame += '}';
        TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                          static_cast<int>(parse(frame, request)));
    }

    // 把帧填到接近 1024 B 的成员数量也必须通过（重复检查容量由帧长给出上界）。
    std::string dense = R"({"t":"cmd","id":1,"name":"save")";
    for (int i = 0; dense.size() + 12 <= web::kMaxFrameBytes - 1; ++i) {
        dense += ",\"x" + std::to_string(i) + "\":0";
    }
    dense += '}';
    TEST_ASSERT_TRUE(dense.size() <= web::kMaxFrameBytes);
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(dense, request)));

    // 未知键的重复仍拒绝。
    assertError(R"({"t":"cmd","id":1,"name":"save","y":1,"y":1})", web::ErrorCode::kBadJson);
}

void testRejectMissingFields()
{
    assertError(R"({})", web::ErrorCode::kBadJson);
    assertError(R"({"id":1})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"set","id":1})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"set","id":1,"key":"gnss.rate"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"set","id":1,"value":"10"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"unlock","id":1})", web::ErrorCode::kBadJson);
}

void testRejectUnknownType()
{
    assertError(R"({"t":"save","id":1,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":1,"id":1,"name":"save"})", web::ErrorCode::kBadJson);
}

void testRejectNestingAndNonScalar()
{
    assertError(R"({"t":"cmd","id":1,"name":{"x":1}})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":["save"]})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save","x":{"y":1}})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save","x":[1]})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save","x":null})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save","x":1.5})", web::ErrorCode::kBadJson);
}

void testMemberNameCharset()
{
    // 成员名按协议 §1：小写 ASCII 字母、数字、`.`、`_`，不允许空名、大写、转义与空白。
    for (const char *frame :
         {R"({"T":"cmd","id":1,"name":"save"})", R"({"":1,"t":"cmd","id":1,"name":"save"})",
          R"({"Key":1,"t":"cmd","id":1,"name":"save"})",
          R"({"ke\/y":1,"t":"cmd","id":1,"name":"save"})",
          R"({"k\n":1,"t":"cmd","id":1,"name":"save"})",
          R"({"k y":1,"t":"cmd","id":1,"name":"save"})",
          R"({"k-y":1,"t":"cmd","id":1,"name":"save"})",
          R"({"k\ty":1,"t":"cmd","id":1,"name":"save"})"}) {
        assertError(frame, web::ErrorCode::kBadJson);
    }
    // 合法字符集（含数字、点、下划线）的未知键照常忽略。
    web::Request request;
    TEST_ASSERT_EQUAL(
        static_cast<int>(web::ErrorCode::kNone),
        static_cast<int>(parse(R"({"t":"cmd","id":1,"name":"save","a.b_c9":1})", request)));
    // 同一成员名出现两次（其中一个带转义拼写会被字符集拒绝）由原文比较兜住。
    assertError(R"({"t":"cmd","id":1,"name":"save","a.b":1,"a.b":2})", web::ErrorCode::kBadJson);
}

void testUtf8Validation()
{
    web::Request request;
    // 合法多字节 UTF-8（"中"）出现在字符串值中：接受。
    TEST_ASSERT_EQUAL(
        static_cast<int>(web::ErrorCode::kNone),
        static_cast<int>(
            parse("{\"t\":\"cmd\",\"id\":1,\"name\":\"save\",\"x\":\"\xE4\xB8\xAD\"}", request)));
    // 非法编码一律 bad_json：非续字节、孤立续字节、截断、代理区、超范围、过长编码。
    for (const char *frame :
         {"{\"t\":\"cmd\",\"id\":1,\"name\":\"save\",\"x\":\"\xC3\x28\"}",
          "{\"t\":\"cmd\",\"id\":1,\"name\":\"save\",\"x\":\"\x80\"}",
          "{\"t\":\"cmd\",\"id\":1,\"name\":\"save\",\"x\":\"\xE4\xB8\"}",
          "{\"t\":\"cmd\",\"id\":1,\"name\":\"save\",\"x\":\"\xED\xA0\x80\"}",
          "{\"t\":\"cmd\",\"id\":1,\"name\":\"save\",\"x\":\"\xF4\x90\x80\x80\"}",
          "{\"t\":\"cmd\",\"id\":1,\"name\":\"save\",\"x\":\"\xC0\xAF\"}"}) {
        assertError(frame, web::ErrorCode::kBadJson);
    }
}

void testMixedRawAndEscapedViews()
{
    // Architect 决定：未转义字段借用输入帧，含转义字段借用 Request 内部缓冲。
    std::string frame = R"({"t":"set","id":4,"key":"gnss.rate","value":"a\"b"})";
    const auto base = reinterpret_cast<uintptr_t>(frame.data());
    const auto end = base + frame.size();
    web::Request request;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(frame, request)));
    TEST_ASSERT_EQUAL_size_t(9, request.key.size());
    TEST_ASSERT_EQUAL_size_t(3, request.value.size());
    // 原始字段落在输入帧内；转义字段必须落在帧外（内部缓冲）。
    const auto keyBegin = reinterpret_cast<uintptr_t>(request.key.data());
    const auto valueBegin = reinterpret_cast<uintptr_t>(request.value.data());
    TEST_ASSERT_TRUE(keyBegin >= base);
    TEST_ASSERT_TRUE(keyBegin + request.key.size() <= end);
    TEST_ASSERT_TRUE(valueBegin < base || valueBegin >= end);

    // 覆盖输入帧后转义字段仍读出原值，证明它不借用帧。
    const std::string expected("a\"b");
    frame.assign(frame.size(), '#');
    TEST_ASSERT_EQUAL_STRING_LEN(expected.c_str(), request.value.data(), request.value.size());
}

void testReparseInvalidatesEscapedViews()
{
    web::Request request;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(R"({"t":"unlock","id":1,"token":"a\"b"})", request)));
    const std::string_view first = request.token;
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kNone),
                      static_cast<int>(parse(R"({"t":"unlock","id":2,"token":"c\"d"})", request)));
    TEST_ASSERT_EQUAL_UINT16(2, request.id);
    TEST_ASSERT_EQUAL_STRING_LEN("c\"d", request.token.data(), request.token.size());
    // 转义视图共用同一块内部缓冲：再次解析会覆盖旧视图的内容。
    TEST_ASSERT_EQUAL_PTR(request.token.data(), first.data());
}

void testRejectBadJsonSyntax()
{
    assertError(R"({"t":"cmd","id":1,"name":"save",})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save"}x)", web::ErrorCode::kBadJson);
    assertError(R"({t:"cmd","id":1,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd" "id":1,"name":"save"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"sa\u0076e"})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save)", web::ErrorCode::kBadJson);
    assertError("{\"t\":\"cmd\",\"id\":1,\"name\":\"sa\nve\"}", web::ErrorCode::kBadJson);
    assertError(R"({"t":"set","id":1,"key":"gnss.rate","value":10})", web::ErrorCode::kBadJson);
    assertError(R"({"t":"cmd","id":1,"name":"save","x":trueX})", web::ErrorCode::kBadJson);
}

void testFailedParseClearsOutput()
{
    web::Request request;
    TEST_ASSERT_EQUAL(
        static_cast<int>(web::ErrorCode::kNone),
        static_cast<int>(parse(R"({"t":"set","id":3,"key":"gnss.rate","value":"10"})", request)));
    TEST_ASSERT_EQUAL_size_t(9, request.key.size());
    TEST_ASSERT_EQUAL(static_cast<int>(web::ErrorCode::kBadJson),
                      static_cast<int>(parse(R"({"t":"set","id":3,"key":})", request)));
    TEST_ASSERT_EQUAL_UINT16(0, request.id);
    TEST_ASSERT_EQUAL_size_t(0, request.key.size());
    TEST_ASSERT_EQUAL_size_t(0, request.value.size());
}

} // namespace

void setUp() {}
void tearDown() {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(testParseSetProtocolExample);
    RUN_TEST(testParseCmdWithUnknownFieldsAndWhitespace);
    RUN_TEST(testParseUnlockEscapedToken);
    RUN_TEST(testRejectNonObject);
    RUN_TEST(testFrameLengthBoundary);
    RUN_TEST(testTooLongClearsRequest);
    RUN_TEST(testIdBoundaries);
    RUN_TEST(testIdSyntaxBeforeRange);
    RUN_TEST(testUnknownIntegerRange);
    RUN_TEST(testRejectDuplicateKeys);
    RUN_TEST(testUnknownScalarCountIsFrameBounded);
    RUN_TEST(testRejectMissingFields);
    RUN_TEST(testRejectUnknownType);
    RUN_TEST(testRejectNestingAndNonScalar);
    RUN_TEST(testMemberNameCharset);
    RUN_TEST(testUtf8Validation);
    RUN_TEST(testMixedRawAndEscapedViews);
    RUN_TEST(testReparseInvalidatesEscapedViews);
    RUN_TEST(testRejectBadJsonSyntax);
    RUN_TEST(testFailedParseClearsOutput);
    return UNITY_END();
}
