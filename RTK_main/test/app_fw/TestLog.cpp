// fw/Log.hpp host 单元测试（ADR-0009 §2.12）：按类别级别过滤（默认 I、与追踪掩码无关）、C 接口、
// 无 sink 时丢失计数、直写 sink 与文本环的通路选择（环优先）、记录头 level 字段、tlog 等价 kInfo、
// 行格式（kInfo 无前缀、其他级别带 "X:"）、级别名解析与截断。
// 全局 sink 只能安装一次，因此用例按顺序执行、相互依赖，顺序见 main()。

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

#include "fw/Lock.hpp"
#include "fw/Log.hpp"
#include "fw/LogC.h"
#include "fw/Result.hpp"
#include "fw/Tlog.hpp"
#include "fw/Trace.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr uint32_t kDirectClockMs = 8712U;
constexpr uint32_t kTraceClockMs = 1234U;
constexpr size_t kRingBytes = fw::kLogHeaderBytes + fw::kLogHexMax;

struct Captured {
    uint32_t calls = 0U;
    fw::LogHeader hdr{};
    std::string text;
};

Captured s_direct{};
std::array<uint8_t, kRingBytes> s_ring_storage{};
fw::LogRing s_ring{};

/**
 * @brief  直写 sink 测试替身：记录最后一条
 * @param  ctx: 指向 Captured
 * @param  hdr: 记录头
 * @param  payload: 文本
 */
void direct_sink(void *ctx, const fw::LogHeader &hdr, std::span<const uint8_t> payload) noexcept
{
    auto *cap = static_cast<Captured *>(ctx);
    ++cap->calls;
    cap->hdr = hdr;
    cap->text.assign(payload.begin(), payload.end());
}

/**
 * @brief  直写通路时钟
 * @retval 固定值 kDirectClockMs
 */
uint32_t direct_clock() noexcept
{
    return kDirectClockMs;
}

/**
 * @brief  trace 时钟（文本环通路使用）
 * @retval 固定值 kTraceClockMs
 */
uint32_t trace_clock() noexcept
{
    return kTraceClockMs;
}

/**
 * @brief  trace 事件 sink 替身（本测试不关心事件）
 * @param  ctx: 未用
 * @param  ev: 未用
 */
void trace_sink(void *ctx, const fw::TraceEvent &ev) noexcept
{
    static_cast<void>(ctx);
    static_cast<void>(ev);
}

/**
 * @brief  格式化一条记录为控制台行，便于断言
 * @param  hdr: 记录头
 * @param  text: 正文
 * @retval 格式化后的行
 */
std::string format(const fw::LogHeader &hdr, std::string_view text)
{
    std::array<char, fw::kLogLineMax> buf{};
    const size_t n = fw::log_format_line(
        buf, hdr,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    return std::string(buf.data(), n);
}

} // namespace

/** 什么都没装：E 级别计入丢失，D 级别（默认 I 级过滤）根本不进入通路、不计数。 */
void test_lost_without_sink(void)
{
    const uint32_t before = fw::log_stats().lost;
    FW_LOGE(fw::TraceCat::kGnss, "boom %d", 1);
    FW_LOGD(fw::TraceCat::kGnss, "quiet");
    TEST_ASSERT_EQUAL_UINT32(before + 1U, fw::log_stats().lost);
}

void test_install_direct_validation(void)
{
    TEST_ASSERT_EQUAL(fw::Result::kInvalidArg,
                      fw::log_install_direct(nullptr, &s_direct, &direct_clock));
    TEST_ASSERT_EQUAL(fw::Result::kInvalidArg,
                      fw::log_install_direct(&direct_sink, &s_direct, nullptr));
    TEST_ASSERT_EQUAL(fw::Result::kOk,
                      fw::log_install_direct(&direct_sink, &s_direct, &direct_clock));
    TEST_ASSERT_EQUAL(fw::Result::kInvalidState,
                      fw::log_install_direct(&direct_sink, &s_direct, &direct_clock));
}

/** 默认所有类别 I：E/W/I 输出、D 过滤；与追踪掩码无关。 */
void test_default_info_independent_of_trace(void)
{
    fw::trace_set_mask(0U);
    TEST_ASSERT_EQUAL_UINT32(fw::kLogLevelsDefault, fw::log_levels());
    TEST_ASSERT_EQUAL(fw::LogLevel::kInfo, fw::log_cat_level(fw::TraceCat::kSys));
    s_direct = {};
    FW_LOGW(fw::TraceCat::kLink, "peer lost id=%u\n", 3U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_direct.calls);
    TEST_ASSERT_EQUAL(fw::LogLevel::kWarn, s_direct.hdr.level);
    TEST_ASSERT_EQUAL(fw::TraceCat::kLink, s_direct.hdr.cat);
    TEST_ASSERT_EQUAL_UINT32(kDirectClockMs, s_direct.hdr.t_ms);
    TEST_ASSERT_EQUAL_STRING("peer lost id=3", s_direct.text.c_str()); // 结尾换行被削掉
    TEST_ASSERT_EQUAL_UINT16(s_direct.hdr.len, s_direct.hdr.total);
    FW_LOGI(fw::TraceCat::kLink, "info");
    TEST_ASSERT_EQUAL_UINT32(2U, s_direct.calls);
    FW_LOGD(fw::TraceCat::kLink, "debug");
    TEST_ASSERT_EQUAL_UINT32(2U, s_direct.calls);
    TEST_ASSERT_TRUE(fw::log_enabled(fw::LogLevel::kError, fw::TraceCat::kSys));
    TEST_ASSERT_FALSE(fw::log_enabled(fw::LogLevel::kWarn, fw::TraceCat::kCount));
}

/** 按类别设级别：只影响该类别；E 总是记录；整表读写与截断。 */
void test_per_category_levels(void)
{
    s_direct = {};
    fw::log_set_cat_level(fw::TraceCat::kGnss, fw::LogLevel::kDebug);
    fw::log_set_cat_level(fw::TraceCat::kSpp, fw::LogLevel::kError);
    FW_LOGD(fw::TraceCat::kGnss, "detail");
    TEST_ASSERT_EQUAL_UINT32(1U, s_direct.calls);
    TEST_ASSERT_EQUAL(fw::LogLevel::kDebug, s_direct.hdr.level);
    FW_LOGD(fw::TraceCat::kLink, "other cat");
    FW_LOGW(fw::TraceCat::kSpp, "suppressed");
    TEST_ASSERT_EQUAL_UINT32(1U, s_direct.calls);
    FW_LOGE(fw::TraceCat::kSpp, "always");
    TEST_ASSERT_EQUAL_UINT32(2U, s_direct.calls);
    const uint32_t packed = fw::log_levels();
    fw::log_set_levels(fw::log_levels_all(fw::LogLevel::kWarn));
    TEST_ASSERT_EQUAL(fw::LogLevel::kWarn, fw::log_cat_level(fw::TraceCat::kGnss));
    fw::log_set_levels(packed);
    TEST_ASSERT_EQUAL(fw::LogLevel::kDebug, fw::log_cat_level(fw::TraceCat::kGnss));
    TEST_ASSERT_EQUAL(fw::LogLevel::kError, fw::log_cat_level(fw::TraceCat::kSpp));
    TEST_ASSERT_EQUAL(fw::LogLevel::kError, fw::log_cat_level(fw::TraceCat::kCount));
    fw::log_set_cat_level(fw::TraceCat::kCount, fw::LogLevel::kDebug); // 越界忽略
    TEST_ASSERT_EQUAL_UINT32(packed, fw::log_levels());
    fw::log_set_levels(fw::kLogLevelsDefault);
}

/** C 接口与 C++ 宏走同一通路。 */
void test_c_api(void)
{
    s_direct = {};
    FW_CLOGW(FW_LOG_CAT_SPP, "c side %d", 5);
    TEST_ASSERT_EQUAL_UINT32(1U, s_direct.calls);
    TEST_ASSERT_EQUAL(fw::TraceCat::kSpp, s_direct.hdr.cat);
    TEST_ASSERT_EQUAL(fw::LogLevel::kWarn, s_direct.hdr.level);
    TEST_ASSERT_EQUAL_STRING("c side 5", s_direct.text.c_str());
    fw_log_c(FW_LOG_LEVEL_ERROR, FW_LOG_CAT_SYS + 1, "bad cat");
    fw_log_c(FW_LOG_LEVEL_DEBUG + 1, FW_LOG_CAT_SYS, "bad level");
    TEST_ASSERT_EQUAL_UINT32(1U, s_direct.calls);
}

/** 超长文本截断并以 "..." 结尾。 */
void test_truncation(void)
{
    s_direct = {};
    const std::string longtext(fw::kLogTextMax + 40U, 'x');
    FW_LOGE(fw::TraceCat::kSys, "%s", longtext.c_str());
    TEST_ASSERT_EQUAL_size_t(fw::kLogTextMax, s_direct.text.size());
    TEST_ASSERT_EQUAL_STRING("...", s_direct.text.substr(fw::kLogTextMax - 3U).c_str());
}

/** 装上 trace 时钟与文本环后，日志改走环（时间戳来自 trace 时钟），直写 sink 不再收到。 */
void test_ring_takes_precedence(void)
{
    TEST_ASSERT_EQUAL(fw::Result::kOk, fw::trace_install(&trace_sink, nullptr, &trace_clock));
    TEST_ASSERT_EQUAL(fw::Result::kOk, s_ring.init(s_ring_storage, fw::LockOps{}));
    TEST_ASSERT_EQUAL(fw::Result::kOk, fw::log_install(&fw::LogRing::sink, &s_ring));
    s_direct = {};
    FW_LOGW(fw::TraceCat::kStore, "save failed key=%s", "sn");
    fw::tlog(fw::TraceCat::kGnss, "legacy %d", 7); // 默认 I 级
    TEST_ASSERT_EQUAL_UINT32(0U, s_direct.calls);

    std::array<uint8_t, fw::kLogTextMax> out{};
    fw::LogHeader hdr{};
    size_t copied = 0U;
    TEST_ASSERT_TRUE(s_ring.pop(hdr, out, copied));
    TEST_ASSERT_EQUAL(fw::LogLevel::kWarn, hdr.level);
    TEST_ASSERT_EQUAL(fw::TraceCat::kStore, hdr.cat);
    TEST_ASSERT_EQUAL_UINT32(kTraceClockMs, hdr.t_ms);
    TEST_ASSERT_EQUAL_STRING("save failed key=sn",
                             std::string(out.begin(), out.begin() + copied).c_str());
    TEST_ASSERT_TRUE(s_ring.pop(hdr, out, copied));
    TEST_ASSERT_EQUAL(fw::LogLevel::kInfo, hdr.level); // tlog 等价 kInfo
    TEST_ASSERT_EQUAL_STRING("legacy 7", std::string(out.begin(), out.begin() + copied).c_str());
    TEST_ASSERT_FALSE(s_ring.pop(hdr, out, copied));
}

void test_format_line(void)
{
    fw::LogHeader hdr{};
    hdr.t_ms = kDirectClockMs;
    hdr.cat = fw::TraceCat::kGnss;
    hdr.level = fw::LogLevel::kInfo;
    TEST_ASSERT_EQUAL_STRING("[   8.712] [GNSS] probe ok\r\n", format(hdr, "probe ok").c_str());
    hdr.level = fw::LogLevel::kWarn;
    hdr.cat = fw::TraceCat::kSys;
    TEST_ASSERT_EQUAL_STRING("[   8.712] [SYS] W: low heap\r\n", format(hdr, "low heap").c_str());
    // 缓冲不足：截断正文，仍以 CRLF 结尾。
    std::array<char, 16U> small{};
    const std::string_view text = "0123456789abcdef";
    const size_t n = fw::log_format_line(
        small, hdr,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    TEST_ASSERT_EQUAL_size_t(small.size(), n);
    TEST_ASSERT_EQUAL_CHAR('\r', small[n - 2U]);
    TEST_ASSERT_EQUAL_CHAR('\n', small[n - 1U]);
}

void test_parse_level_and_names(void)
{
    fw::LogLevel lv = fw::LogLevel::kInfo;
    TEST_ASSERT_TRUE(fw::log_parse_level("W", lv));
    TEST_ASSERT_EQUAL(fw::LogLevel::kWarn, lv);
    TEST_ASSERT_TRUE(fw::log_parse_level("Debug", lv));
    TEST_ASSERT_EQUAL(fw::LogLevel::kDebug, lv);
    TEST_ASSERT_FALSE(fw::log_parse_level("verbose", lv));
    TEST_ASSERT_FALSE(fw::log_parse_level("", lv));
    TEST_ASSERT_EQUAL(fw::LogLevel::kDebug, lv); // 失败不修改
    TEST_ASSERT_EQUAL_CHAR('E', fw::log_level_char(fw::LogLevel::kError));
    TEST_ASSERT_EQUAL_STRING("SYS", std::string(fw::log_cat_name(fw::TraceCat::kSys)).c_str());
    TEST_ASSERT_EQUAL_STRING("?", std::string(fw::log_cat_name(fw::TraceCat::kCount)).c_str());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lost_without_sink);
    RUN_TEST(test_install_direct_validation);
    RUN_TEST(test_default_info_independent_of_trace);
    RUN_TEST(test_per_category_levels);
    RUN_TEST(test_c_api);
    RUN_TEST(test_truncation);
    RUN_TEST(test_ring_takes_precedence);
    RUN_TEST(test_format_line);
    RUN_TEST(test_parse_level_and_names);
    return UNITY_END();
}
