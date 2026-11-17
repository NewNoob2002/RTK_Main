// fw::tlog 在 FW_TRACE_ENABLED=0 下的编译路径（ADR-0009 §2.6）：tlog/tlog_hex/log_hex_on 退化为空内联，
// 但不允许出现“未使用变量/参数”之类的告警，且 log_install 的校验语义保持不变。

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/Result.hpp"
#include "fw/Tlog.hpp"
#include "fw/Trace.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr size_t kRingBytes = fw::kLogHeaderBytes + fw::kLogTextMax;
constexpr uint32_t kMarker = 7U;
uint32_t s_clock_value = 0U;

uint32_t test_clock() noexcept
{
    s_clock_value += 1U;
    return s_clock_value;
}

void trace_sink(void * /*ctx*/, const fw::TraceEvent & /*ev*/) noexcept {}

void log_sink(void * /*ctx*/, const fw::LogHeader & /*hdr*/,
              std::span<const uint8_t> /*payload*/) noexcept
{}

void test_tlog_is_noop_when_disabled()
{
    std::array<uint8_t, kRingBytes> storage{};
    fw::LogRing ring{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(ring.init(storage, fw::kNoLock)));

    fw::trace_set_mask(fw::kTraceAllMask);
    fw::log_set_hex_mask(fw::kTraceAllMask);
    // 十六进制通通关闭：即使两级掩码都置位也不认为打开。
    TEST_ASSERT_FALSE(fw::log_hex_on(fw::TraceCat::kGnss));

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    fw::tlog(fw::TraceCat::kGnss, "must not reach the ring: %u", kMarker);
    const std::array<uint8_t, 4U> bytes{0U, 1U, 2U, 3U};
    fw::tlog_hex(fw::TraceCat::kGnss, bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, ring.stats().pushed);
    TEST_ASSERT_EQUAL_UINT32(fw::kTraceAllMask, fw::log_hex_mask());
}

void test_log_install_still_validated_when_disabled()
{
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidArg),
                            static_cast<uint8_t>(fw::log_install(nullptr, nullptr)));
    // 未安装 trace 时没有时钟源。
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(fw::log_install(&log_sink, nullptr)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(fw::Result::kOk),
        static_cast<uint8_t>(fw::trace_install(&trace_sink, nullptr, &test_clock)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kOk),
                            static_cast<uint8_t>(fw::log_install(&log_sink, nullptr)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(fw::Result::kInvalidState),
                            static_cast<uint8_t>(fw::log_install(&log_sink, nullptr)));
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tlog_is_noop_when_disabled);
    RUN_TEST(test_log_install_still_validated_when_disabled);
    return UNITY_END();
}
