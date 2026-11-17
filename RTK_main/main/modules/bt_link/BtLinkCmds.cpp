// 链路模块控制台命令表（ADR-0009 §2.5/§2.10）：只读的 bt / alarm 状态。
// 线程：仅在 console REPL 任务调用，非 ISR；只读 DataCenter。改状态的动作见 system/FactoryCmds.cpp。

#include "modules/bt_link/BtLinkModule.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "AppContext.hpp"
#include "cli/Out.hpp"
#include "support/ConsoleSupport.hpp"

namespace app {

namespace {

int cmd_bt(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: bt");
    }
    const auto &data = app_context().data;
    const auto sessions = data.sessions.read();
    const auto link = data.link_status.read();
    const auto sys = data.system_status.read();

    cli::printSection("bt");
    cli::printKv("stack", "%s", sys.bt_ready ? "ready" : "down");
    cli::printKv("link", "%s", sys.link_running ? "running" : "stopped");
    cli::printKv("name", "%s", sys.device_name.data());
    // 会话数上限为 kMaxSessions，转换为 unsigned 无损。
    cli::printKv("sessions", "%u / %u", static_cast<unsigned>(sessions.count),
                 static_cast<unsigned>(kMaxSessions));
    std::array<char, kMacStrBuf> mac_buf{};
    for (size_t idx = 0U; idx < sessions.count; ++idx) {
        const SessionSnapshot &session = sessions.entries[idx];
        format_mac(session.addr, mac_buf);
        cli::printKv("session", "%-6s %s (handle 0x%08" PRIx32 ")", role_name(session.role),
                     mac_buf.data(), session.handle);
    }
    if (link.pairing) {
        // 配对窗口只由 factory pair 或手机 0x44 打开；这里只提示状态。
        cli::printKv("pairing", "open");
    }
    return 0;
}

int cmd_alarm(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: alarm");
    }
    const auto &data = app_context().data;
    const auto alarm = data.alarm_state.read();
    const auto heartbeat = data.heartbeat.read();

    cli::printSection("alarm");
    // 报警状态/级别均为 uint8_t，转换为 unsigned 无损。
    cli::printKv("state", "%u", static_cast<unsigned>(alarm.state));
    cli::printKv("level", "%u", static_cast<unsigned>(alarm.level));
    cli::printKv("override", "%s", alarm.override ? "yes" : "no");
    if (heartbeat.present) {
        cli::printKv("heartbeat", "seq %u, battery %u%%, state %u",
                     static_cast<unsigned>(heartbeat.value.seq),
                     static_cast<unsigned>(heartbeat.value.batteryPct),
                     static_cast<unsigned>(heartbeat.value.localState));
    } else {
        cli::printKv("heartbeat", "none");
    }
    return 0;
}

// 只保留只读状态命令（ADR-0009 §2.10）；改链路状态的动作都在隐藏的 factory 命令集里。
constexpr std::array<cli::Cmd, 2U> kLinkCmds{{
    {"bt", "bt: BT stack state and current sessions", nullptr, cmd_bt, nullptr, false},
    {"alarm", "alarm: alarm state and latest heartbeat", nullptr, cmd_alarm, nullptr, false},
}};

} // namespace

std::span<const cli::Cmd> link_cmds() noexcept
{
    return kLinkCmds;
}

} // namespace app
