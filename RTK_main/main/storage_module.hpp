// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include "app_data.hpp"
#include "app/store_key.hpp"

namespace rtk::app {
inline constexpr uint32_t kStoreRetryInitialMs = 1000;
struct StorageModule {
    bool initialized = false;
    bool pending = false;
    bool attempted = false;
    uint32_t changed_ms = 0;
    uint32_t attempt_ms = 0;
    uint32_t retry_ms = kStoreRetryInitialMs;
    StoreStatus status{};
    LinkStatus whitelist{};
    std::array<char, RTK_STORE_SN_MAX + 1U> sn{};
};
extern StorageModule s_storage_module;
// 启动前单线程读取 NVS；重复调用保持已加载状态。非 ISR。
[[nodiscard]] ::app::Result storage_init(void *self) noexcept;
// 仅 kLink 调用。bt_link → storage 的单向通知：复制最新白名单并启动 500 ms 去抖；无反向调用。
void mark_whitelist_dirty(std::span<const core::BtAddr> list, uint32_t now_ms) noexcept;
// 仅 kLink 的 bt_link 单向调用；显式 wl clear 保留基线同步落盘/错误回执，后台变更仍去抖。
[[nodiscard]] esp_err_t flush_whitelist(uint32_t now_ms) noexcept;
// 仅 kLink 调用，非 ISR。
[[nodiscard]] bool storage_poll(void *self, uint32_t now_ms) noexcept;
void storage_command(void *self, const ::app::Command &cmd, ::app::CommandResult &result) noexcept;
inline constexpr ::app::ModuleOps kStorageOps{storage_init, nullptr, storage_poll, storage_command};
} // namespace rtk::app
