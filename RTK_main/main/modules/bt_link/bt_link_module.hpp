// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include <span>

#include "data/app_data.hpp"
#include "rtk/console/console.hpp"
#include "rtk/status.hpp"
namespace rtk::app {
struct BtLinkModule {
    uint32_t last_tick_ms = 0;
    uint32_t reset_version = 0;
    uint32_t round_start_ms = 0;
};
extern BtLinkModule s_bt_link_module;
// ModuleOps 均由 kLink 调用，init 仅启动前；均非 ISR。
[[nodiscard]] ::app::Result bt_link_init(void *self) noexcept;
[[nodiscard]] bool bt_link_poll(void *self, uint32_t now_ms) noexcept;
void bt_link_command(void *self, const ::app::Command &cmd, ::app::CommandResult &result) noexcept;
// 仅执行器任务调用；先处理生命周期事件，再进入命令/RX 阶段。
void bt_link_wait(void *ctx, uint32_t max_ms) noexcept;
// 启动异步发送任务；须在链路执行器 start 之前由 app_main 调用一次。失败时回复退回同步发送。
[[nodiscard]] rtk::Status bt_link_start_tx() noexcept;
[[nodiscard]] std::span<const rtk::console::Cmd> link_cmds() noexcept;
inline constexpr ::app::ModuleOps kBtLinkOps{bt_link_init, nullptr, bt_link_poll, bt_link_command};
} // namespace rtk::app
