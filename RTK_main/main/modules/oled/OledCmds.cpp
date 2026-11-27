// OLED 状态控制台命令（DEV 档，遵循 ADR-0016 / OL2）：显示在位、页面、帧率、I2C/帧耗时与脏页统计。
// 仅在 console REPL 任务调用，不阻塞 I2C，不分配堆，非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/oled/OledCmds.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AppContext.hpp"
#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "def/OledDef.hpp"
#include "fw/Status.hpp"
#include "modules/oled/OledUi.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {

constexpr uint16_t kFpsDivisor = 10U;

/**
 * @brief 打印同一统计区间的帧率、唤醒率与单核 CPU 估算。
 * @param total: 本区间计数。
 * @param elapsedMs: 区间时长。
 * @note 仅 console REPL 任务调用。
 */
void printPerf(const oled::PerfTotals &total, uint32_t elapsedMs) noexcept
{
    const uint32_t fps = total.fpsX10(elapsedMs);
    const uint32_t wakes = total.wakeupsX10(elapsedMs);
    const uint32_t cpu = total.cpuX10(elapsedMs);
    cli::printKv("fps_avg", "%" PRIu32 ".%" PRIu32, fps / kFpsDivisor, fps % kFpsDivisor);
    cli::printKv("wakeups/s", "%" PRIu32 ".%" PRIu32, wakes / kFpsDivisor, wakes % kFpsDivisor);
    cli::printKv("cpu_pct", "%" PRIu32 ".%" PRIu32, cpu / kFpsDivisor, cpu % kFpsDivisor);
}

/**
 * @brief 输出 OLED 在位状态、当前页、帧率、每帧 I2C/总耗时、脏页数与错误计数
 * @param argc: 参数个数（含动词）
 * @retval 0 已输出状态
 * @retval 非零 参数错误
 * @note 仅 console REPL 任务调用，非 ISR。
 */
int cmdOledStatus(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: oled status");
    }
    const auto &app = appContext();
    const OledStatus st = app.data.oledStatus.read();
    cli::printSection("oled");
    cli::printKv("enabled", "%s", st.enabled ? "yes" : "no");
    cli::printKv("present", "%s", st.present ? "yes" : "no");
    cli::printKv("powered_off", "%s", st.poweredOff ? "yes" : "no");
    cli::printKv("init", "%s (%s, attempts=%" PRIu32 ")", oledInitStageName(st.initStage),
                 fw::toString(st.initStatus), st.initAttempts);
    cli::printKv("page", "%s", oledPageName(st.page));
    const auto perf = app.oled.recentPerf();
    const uint32_t fps = perf.fpsX10(oled::kPerfWindowMs);
    cli::printKv("fps", "%" PRIu32 ".%" PRIu32, fps / kFpsDivisor, fps % kFpsDivisor);
    printPerf(perf, oled::kPerfWindowMs);
    cli::printKv("frame_interval_ms", "last=%" PRIu32 " max=%" PRIu32, st.lastIntervalMs,
                 st.maxIntervalMs);
    cli::printKv("bytes/frame", "%u", static_cast<unsigned>(st.lastTxBytes));
    cli::printKv("render_us", "last=%" PRIu32 " max=%" PRIu32, st.lastRenderUs, st.maxRenderUs);
    cli::printKv("frames", "flushed=%" PRIu32 " skipped=%" PRIu32, st.framesFlushed,
                 st.framesSkipped);
    cli::printKv("dirty_pages", "last=%u total=%" PRIu32, static_cast<unsigned>(st.lastDirtyPages),
                 st.totalDirtyPages);
    cli::printKv("i2c_us", "last=%" PRIu32 " max=%" PRIu32, st.lastI2cUs, st.maxI2cUs);
    cli::printKv("frame_us", "last=%" PRIu32 " max=%" PRIu32, st.lastFrameUs, st.maxFrameUs);
    cli::printKv("i2c_errors", "%" PRIu32, st.i2cErrors);
    return 0;
}

/**
 * @brief 执行一轮自动转场 bench，等待 OLED 发布统计后输出。
 * @param argc: 含动词的参数个数。
 * @param argv: 参数数组。
 * @retval 控制台命令退出码。
 * @note 仅 console REPL 任务调用，等待时阻塞让出 CPU。
 */
int cmdOledBench(void * /*ctx*/, int argc, char **argv) noexcept
{
    uint32_t seconds = 0U;
    if (argc != 2) {
        return cli::fail("usage: oled bench <seconds 1..60>");
    }
    const std::span<char *const> args{argv, 2U};
    if (!cli::parseU32(args.back(), seconds) || seconds == 0U ||
        seconds > OledModule::kMaxBenchSeconds) {
        return cli::fail("usage: oled bench <seconds 1..60>");
    }
    auto &oledModule = appContext().oled;
    const auto status = oledModule.requestBench(seconds);
    if (!fw::isOk(status)) {
        return cli::fail("%s", fw::toString(status));
    }
    oled::BenchStats result{};
    constexpr uint32_t kPollMs = 20U;
    constexpr uint32_t kFinishGraceMs = 2000U;
    constexpr uint32_t kMsPerSec = 1000U;
    const uint32_t deadlineMs = rtos::nowMs() + seconds * kMsPerSec + kFinishGraceMs;
    while (!oledModule.benchResult(result)) {
        if (oled::remainingMs(rtos::nowMs(), deadlineMs) == 0U) {
            return cli::fail("OLED bench timed out");
        }
        vTaskDelay(std::max<TickType_t>(1U, pdMS_TO_TICKS(kPollMs)));
    }
    cli::printSection("oled bench");
    cli::printKv("elapsed_ms", "%" PRIu32, result.elapsedMs);
    cli::printKv("frames", "%" PRIu32, result.totals.frames);
    printPerf(result.totals, result.elapsedMs);
    cli::printKv("frame_interval_ms", "last=%" PRIu32 " max=%" PRIu32, result.lastIntervalMs,
                 result.maxIntervalMs);
    cli::printKv("bytes/frame", "%" PRIu32,
                 result.totals.frames != 0U ? result.bytes / result.totals.frames : 0U);
    cli::printKv("render_us", "last=%" PRIu32 " max=%" PRIu32, result.lastRenderUs,
                 result.maxRenderUs);
    cli::printKv("i2c_errors", "%" PRIu32, result.i2cErrors);
    return result.interrupted ? cli::fail("OLED bench interrupted by power transition") : 0;
}

/**
 * @brief 发布 OL9 测试告警，验证全屏显示与按键确认。
 * @param argc: 参数个数（含动词）。
 * @param argv: 可选的测试告警类型。
 * @retval 0 事件已发布；非零为参数错误或 OLED 不在位。
 * @note 仅 DEV console REPL 调用，是测试告警批次的唯一写者。
 */
int cmdOledAlarm(void * /*ctx*/, int argc, char **argv) noexcept
{
    constexpr const char *kUsage = "usage: oled alarm [enter|leave|precision|lowbat]";
    if (argc < 1 || argc > 2) {
        return cli::fail("%s", kUsage);
    }
    // argc 已限定为 1 或 2。
    const std::span<char *const> args{argv, static_cast<size_t>(argc)};
    auto sample = oled::makeTestAlarm(args.size() == 1U ? "enter" : args.back());
    if (sample.pending == 0U) {
        return cli::fail("%s", kUsage);
    }
    auto &data = appContext().data;
    if (!data.oledStatus.read().present) {
        return cli::fail("OLED not present");
    }
    sample.sequence = data.oledAlarm.read().sequence + 1U;
    data.oledAlarm.write(sample);
    data.uiEvents.notifyWake();
    return 0;
}

constexpr std::array<cli::Verb, 3U> kOledVerbs{{
    {"status", nullptr, "show OLED state, fps, frame/I2C timings, dirty pages and errors",
     &cmdOledStatus},
    {"bench", "<seconds>", "run repeated OLED page transitions", &cmdOledBench},
    {"alarm", "[enter|leave|precision|lowbat]", "display an OLED test alarm", &cmdOledAlarm},
}};

/**
 * @brief 分派 oled 子命令，缺省执行 oled status
 * @param ctx: 控制台上下文
 * @param argc: 参数个数
 * @param argv: 参数数组
 * @retval 0 子命令成功
 * @retval 非零 参数错误
 * @note 仅 console REPL 任务调用，非 ISR。
 */
int cmdOled(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kOledVerbs, ctx, argc, argv, "status");
}

constexpr std::array<cli::Cmd, 1U> kOledCmds{{
    {"oled", "oled [status|bench <seconds>|alarm [type]]: OLED diagnostics",
     "[status|bench <seconds>|alarm [type]]", &cmdOled, nullptr},
}};

} // namespace

std::span<const cli::Cmd> oledCmds() noexcept
{
    return kOledCmds;
}

std::span<const cli::Verb> oledVerbs() noexcept
{
    return kOledVerbs;
}

} // namespace app
