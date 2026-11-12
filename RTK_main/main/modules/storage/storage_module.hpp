// 存储模块（kLink 执行器）：SN 与报警器白名单的 NVS 读写、去抖重试与 `sn` 控制台命令。
#pragma once
#include <span>

#include "data/app_data.hpp"
#include "fw/store_key.hpp"
#include "cli/console.hpp"
#include "rtk_store.h"

namespace app {
struct AppContext;
struct ModuleTestAccess;
/** 存储模块（kLink 执行器）：SN 与白名单的 NVS 持久化与重试。 */
class StorageModule final {
public:
    constexpr explicit StorageModule(AppContext &context) noexcept : context_(context) {}
    ~StorageModule() = default;
    StorageModule(const StorageModule &) = delete;
    StorageModule &operator=(const StorageModule &) = delete;
    StorageModule(StorageModule &&) = delete;
    StorageModule &operator=(StorageModule &&) = delete;
    /** 启动前单线程，非 ISR；同步读取 NVS，幂等且加载失败时使用缺省值。 */
    [[nodiscard]] fw::Result init() noexcept;
    /** 仅 kLink，非 ISR；可同步写 NVS，返回 false（重试由时钟驱动）。 */
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;
    /** 仅 kLink，非 ISR；同步保存 SN，结果写入 result。 */
    void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /** 仅 kLink，非 ISR、非阻塞；登记白名单变化并重启去抖。 */
    void mark_whitelist_dirty(std::span<const core::BtAddr> list, uint32_t now_ms) noexcept;
    /** 仅 kLink，非 ISR；跳过去抖同步写 NVS，返回后端错误。 */
    [[nodiscard]] esp_err_t flush_whitelist(uint32_t now_ms) noexcept;

private:
    friend struct ModuleTestAccess;
    void load_whitelist() noexcept;
    void load_sn() noexcept;
    [[nodiscard]] fw::Result whitelist_load(std::span<const uint8_t> in) noexcept;
    [[nodiscard]] fw::Result whitelist_save(std::span<uint8_t> out, size_t &len) const noexcept;
    [[nodiscard]] fw::Result sn_load(std::span<const uint8_t> in) noexcept;
    [[nodiscard]] fw::Result sn_save(std::span<uint8_t> out, size_t &len) const noexcept;
    void save_sn(const fw::Command &cmd, fw::CommandResult &result) noexcept;

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
/** 存储模块的控制台命令表；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> storage_cmds() noexcept;
} // namespace app
