// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#pragma once
#include <span>

#include "data/app_data.hpp"
#include "app/store_key.hpp"
#include "rtk/console/console.hpp"
#include "rtk_store.h"

namespace rtk::app {
struct AppContext;
struct ModuleTestAccess;
class StorageModule final {
public:
    constexpr explicit StorageModule(AppContext &context) noexcept : context_(context) {}
    ~StorageModule() = default;
    StorageModule(const StorageModule &) = delete;
    StorageModule &operator=(const StorageModule &) = delete;
    StorageModule(StorageModule &&) = delete;
    StorageModule &operator=(StorageModule &&) = delete;
    /** 启动前单线程，非 ISR；同步读取 NVS，幂等且加载失败时使用缺省值。 */
    [[nodiscard]] ::app::Result init() noexcept;
    /** 仅 kLink，非 ISR；可同步写 NVS，返回 false（重试由时钟驱动）。 */
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;
    /** 仅 kLink，非 ISR；同步保存 SN，结果写入 result。 */
    void on_command(const ::app::Command &cmd, ::app::CommandResult &result) noexcept;
    /** 仅 kLink，非 ISR、非阻塞；登记白名单变化并重启去抖。 */
    void mark_whitelist_dirty(std::span<const core::BtAddr> list, uint32_t now_ms) noexcept;
    /** 仅 kLink，非 ISR；跳过去抖同步写 NVS，返回后端错误。 */
    [[nodiscard]] esp_err_t flush_whitelist(uint32_t now_ms) noexcept;

private:
    friend struct ModuleTestAccess;
    void load_whitelist() noexcept;
    void load_sn() noexcept;
    [[nodiscard]] ::app::Result whitelist_load(std::span<const uint8_t> in) noexcept;
    [[nodiscard]] ::app::Result whitelist_save(std::span<uint8_t> out, size_t &len) const noexcept;
    [[nodiscard]] ::app::Result sn_load(std::span<const uint8_t> in) noexcept;
    [[nodiscard]] ::app::Result sn_save(std::span<uint8_t> out, size_t &len) const noexcept;
    void save_sn(const ::app::Command &cmd, ::app::CommandResult &result) noexcept;

    static constexpr uint32_t kRetryInitialMs = 1000;
    AppContext &context_;
    bool initialized_ = false;
    bool pending_ = false;
    bool attempted_ = false;
    uint32_t changed_ms_ = 0;
    uint32_t attempt_ms_ = 0;
    uint32_t retry_ms_ = kRetryInitialMs;
    StoreStatus status_{};
    LinkStatus whitelist_{};
    std::array<char, RTK_STORE_SN_MAX + 1U> sn_{};
};
[[nodiscard]] std::span<const rtk::console::Cmd> storage_cmds() noexcept;
} // namespace rtk::app
