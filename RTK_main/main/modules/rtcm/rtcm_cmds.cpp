// RTCM 模块控制台命令表：rtcm。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "support/console_support.hpp"
#include "modules/rtcm/rtcm_module.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>

namespace rtk::app {

namespace {

// NOLINTNEXTLINE(misc-const-correctness)
int cmd_rtcm(void * /*ctx*/, int argc, char **argv) noexcept
{
    // argc 由 esp_console 统计且 >= 0，转换为 size_t 无损。
    const std::span<char *> args(argv, static_cast<size_t>(argc));
    if ((args.size() != 2U) || (strcmp(args[1], "reset") != 0)) {
        printf("usage: rtcm reset\n");
        return 1;
    }
    fw::Command command{};
    command.target = id(ModuleKind::kRtcm);
    command.verb = verb(RtcmVerb::kReset);
    return submit(command, "rtcm reset") ? 0 : 1;
}

constexpr std::array<rtk::console::Cmd, 1> kRtcmCmds{{
    {"rtcm", "rtcm reset: clear RTCM and demux statistics", nullptr, cmd_rtcm, nullptr},
}};

} // namespace

std::span<const rtk::console::Cmd> rtcm_cmds() noexcept
{
    return kRtcmCmds;
}

} // namespace rtk::app
