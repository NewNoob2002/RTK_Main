// OLED 状态控制台命令（DEV 档，遵循 ADR-0016 / OL2）：显示在位、页面、帧率、I2C/帧耗时与脏页统计。
// 仅在 console REPL 任务调用，不阻塞 I2C，不分配堆，非 ISR。
// 堆档位：禁止区；本文件不分配堆。
#include "modules/oled/OledCmds.hpp"

#include <array>
#include <cinttypes>

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "def/OledDef.hpp"
#include "fw/Status.hpp"

namespace app {
namespace {

constexpr uint16_t kFpsDivisor = 10U;

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
    const OledStatus st = appContext().data.oledStatus.read();
    cli::printSection("oled");
    cli::printKv("enabled", "%s", st.enabled ? "yes" : "no");
    cli::printKv("present", "%s", st.present ? "yes" : "no");
    cli::printKv("powered_off", "%s", st.poweredOff ? "yes" : "no");
    cli::printKv("init", "%s (%s, attempts=%" PRIu32 ")", oledInitStageName(st.initStage),
                 fw::toString(st.initStatus), st.initAttempts);
    cli::printKv("page", "%s", oledPageName(st.page));
    cli::printKv("fps", "%u.%u", static_cast<unsigned>(st.fpsX10 / kFpsDivisor),
                 static_cast<unsigned>(st.fpsX10 % kFpsDivisor));
    cli::printKv("frames", "flushed=%" PRIu32 " skipped=%" PRIu32, st.framesFlushed,
                 st.framesSkipped);
    cli::printKv("dirty_pages", "last=%u total=%" PRIu32, static_cast<unsigned>(st.lastDirtyPages),
                 st.totalDirtyPages);
    cli::printKv("i2c_us", "last=%" PRIu32 " max=%" PRIu32, st.lastI2cUs, st.maxI2cUs);
    cli::printKv("frame_us", "last=%" PRIu32 " max=%" PRIu32, st.lastFrameUs, st.maxFrameUs);
    cli::printKv("i2c_errors", "%" PRIu32, st.i2cErrors);
    return 0;
}

constexpr std::array<cli::Verb, 1U> kOledVerbs{{
    {"status", nullptr, "show OLED state, fps, frame/I2C timings, dirty pages and errors",
     &cmdOledStatus},
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
    {"oled", "oled [status]: show OLED display state, fps and timing counters", "[status]",
     &cmdOled, nullptr},
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
