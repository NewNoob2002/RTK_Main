// 主机测试 ADR-0015 的窗口、安全输出表及电源保持判定。
// 单线程测试进程，无平台硬件、非 ISR。
#include <array>
#include <cstdio>
#include <cstdlib>

#include "board/BoardProfile.hpp"

namespace {

using namespace app::board;

int s_failures = 0;

/**
 * @brief 累计并打印失败表达式
 * @param ok: 检查结果
 * @param expression: 失败时打印的文字
 * @note 单线程测试，非 ISR。
 */
void check(bool ok, const char *expression) noexcept
{
    if (!ok) {
        std::printf("FAIL %s\n", expression);
        ++s_failures;
    }
}

#define CHECK(condition) check((condition), #condition)

/** @brief 验证配置表的区间及安全输出约束；单线程测试，非 ISR。 */
void checkProfiles() noexcept
{
    const auto all = profiles();
    CHECK(all.size() == 2U);
    for (const Profile &profile : all) {
        CHECK(byId(profile.id) == &profile);
        CHECK(profile.gnss.uart == CONFIG_RTK_GNSS_UART_NUM);
        if (profile.id == BoardId::kE1DevKit) {
            CHECK(profile.idMinMv == 0U && profile.idMaxMv == 0U);
            CHECK(profile.safeOutputs.empty());
        } else {
            CHECK(profile.idMinMv >= 150U && profile.idMaxMv <= 2450U);
            CHECK(profile.idMinMv <= profile.idMaxMv);
            for (const Profile &other : all) {
                if (&profile != &other && other.id != BoardId::kE1DevKit) {
                    CHECK(profile.idMaxMv + 100U <= other.idMinMv ||
                          other.idMaxMv + 100U <= profile.idMinMv);
                }
            }
        }
        CHECK(profile.safeOutputs.size() <= 8U);
        for (const SafeOutput &output : profile.safeOutputs) {
            CHECK(output.gpio >= 0 && output.gpio < 34);
            CHECK(output.gpio != profile.gnss.rxGpio && output.gpio != profile.gnss.txGpio &&
                  output.gpio != profile.gnss.powerGpio);
            for (const SafeOutput &other : profile.safeOutputs) {
                CHECK(&other == &output || other.gpio != output.gpio);
            }
        }
    }
    CHECK(byId(BoardId::kUnknown) == nullptr);
    const Profile *p2 = byId(BoardId::kP2V141);
    CHECK(p2 != nullptr);
    if (p2 == nullptr) {
        return;
    }
    CHECK(p2->gnss.receiverPort == 1U && p2->gnss.powerGpio == -1);
    constexpr std::array kOutputs{SafeOutput{13, true}, SafeOutput{15, false},
                                  SafeOutput{23, false}, SafeOutput{26, false},
                                  SafeOutput{4, false}};
    CHECK(p2->safeOutputs.size() == kOutputs.size());
    for (const SafeOutput &expected : kOutputs) {
        bool found = false;
        for (const SafeOutput &actual : p2->safeOutputs) {
            found = found || (actual.gpio == expected.gpio && actual.high == expected.high);
        }
        CHECK(found);
    }
    CHECK(matchById(1499U) == nullptr);
    CHECK(matchById(1500U) == p2);
    CHECK(matchById(1650U) == p2);
    CHECK(matchById(1800U) == p2);
    CHECK(matchById(1801U) == nullptr);
    CHECK(matchById(0U) == nullptr);
    CHECK(matchById(3300U) == nullptr);
}

/** @brief 验证连续三次、抖动复位和截止边界；单线程测试，非 ISR。 */
void checkPowerHold() noexcept
{
    const Profile *p2 = byId(BoardId::kP2V141);
    if (p2 == nullptr) {
        return;
    }
    const PowerHold &hold = p2->powerHold;
    CHECK(hold.kind == HoldKind::kKeyEmulate && hold.keyGpio == 4 && hold.senseGpio == 25);
    CHECK(hold.senseOnMv == 1600U && hold.holdTimeoutMs == 5000U);

    uint8_t streak = 0U;
    CHECK(checkHold(hold, 1600U, 10U, streak) == HoldDecision::kContinue);
    CHECK(checkHold(hold, 1630U, 20U, streak) == HoldDecision::kContinue);
    CHECK(checkHold(hold, 1650U, 30U, streak) == HoldDecision::kHeld);

    streak = 0U;
    CHECK(checkHold(hold, 1600U, 10U, streak) == HoldDecision::kContinue);
    CHECK(checkHold(hold, 1630U, 20U, streak) == HoldDecision::kContinue);
    CHECK(checkHold(hold, 1599U, 30U, streak) == HoldDecision::kContinue && streak == 0U);
    CHECK(checkHold(hold, 1650U, 40U, streak) == HoldDecision::kContinue);
    CHECK(checkHold(hold, 1630U, 50U, streak) == HoldDecision::kContinue);
    CHECK(checkHold(hold, 1600U, 60U, streak) == HoldDecision::kHeld);

    streak = 0U;
    CHECK(checkHold(hold, 1600U, 4980U, streak) == HoldDecision::kContinue);
    CHECK(checkHold(hold, 1600U, 4990U, streak) == HoldDecision::kContinue);
    CHECK(checkHold(hold, 1600U, 5000U, streak) == HoldDecision::kTimeout);
    CHECK(checkHold(hold, 1600U, 4999U, streak) == HoldDecision::kHeld);

    streak = 0U;
    for (uint32_t ms = 10U; ms < 5000U; ms += 10U) {
        CHECK(checkHold(hold, 1599U, ms, streak) == HoldDecision::kContinue);
    }
    CHECK(checkHold(hold, 1599U, 5000U, streak) == HoldDecision::kTimeout);
}

} // namespace

/**
 * @brief 运行配置表和保持判定用例
 * @retval EXIT_SUCCESS 用例全通过
 * @retval EXIT_FAILURE 存在失败用例
 * @note 单线程主机测试，非 ISR。
 */
int main()
{
    checkProfiles();
    checkPowerHold();
    return s_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
