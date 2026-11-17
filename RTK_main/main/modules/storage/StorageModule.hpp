// 存储模块（kLink 执行器）：SN 与报警器白名单的 NVS 读写、去抖重试、目录键表加载与 `config` 查看命令。
// 由 kLink 执行器任务调用；NVS 读写可阻塞，非 ISR 安全。
#pragma once
#include <span>

#include "data/AppData.hpp"
#include "fw/StoreKey.hpp"
#include "cli/Console.hpp"
#include "NvsStore.h"

namespace app {
struct AppContext;
struct ModuleTestAccess;
/** 存储模块（kLink 执行器）：SN 与白名单的 NVS 持久化与重试。 */
class StorageModule final {
public:
    constexpr StorageModule() noexcept = default;
    constexpr explicit StorageModule(AppContext &context) noexcept : context_(&context) {}
    void bind_context(AppContext &context) noexcept
    {
        context_ = &context;
    }
    ~StorageModule() = default;
    StorageModule(const StorageModule &) = delete;
    StorageModule &operator=(const StorageModule &) = delete;
    StorageModule(StorageModule &&) = delete;
    StorageModule &operator=(StorageModule &&) = delete;
    /** 启动前单线程，非 ISR；同步读取 NVS，幂等且加载失败时使用缺省值。 */
    [[nodiscard]] fw::Result init() noexcept;
    /**
     * @brief  处理挂起的白名单保存（去抖与失败重试）
     * @param  now_ms: 当前时间（ms）
     * @retval false 总是（重试由时钟驱动，不需要立即再轮询）
     * @note   仅 kLink，非 ISR；可同步写 NVS；失败记 W/E 级日志。
     */
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;
    /** 仅 kLink，非 ISR；同步保存 SN，结果写入 result。 */
    void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept;
    /** 仅 kLink，非 ISR、非阻塞；登记白名单变化并重启去抖。 */
    void mark_whitelist_dirty(std::span<const core::BtAddr> list, uint32_t now_ms) noexcept;
    /** 仅 kLink，非 ISR；跳过去抖同步写 NVS，返回后端错误。 */
    [[nodiscard]] esp_err_t flush_whitelist(uint32_t now_ms) noexcept;

    /** 按 fw::StoreKey 表读 NVS（启动前或仅 kLink，非 ISR）；缺键返回 kNotFound。 */
    [[nodiscard]] fw::Result load_keys(std::span<const fw::StoreKey> keys) noexcept;
    /** 按 fw::StoreKey 表同步写 NVS（仅 kLink，非 ISR）；任一键失败即返回该错误。 */
    [[nodiscard]] fw::Result save_keys(std::span<const fw::StoreKey> keys) noexcept;

private:
    friend struct ModuleTestAccess;
    void load_whitelist() noexcept;
    void load_sn() noexcept;
    void load_key_tables() noexcept;
    [[nodiscard]] fw::Result whitelist_load(std::span<const uint8_t> in) noexcept;
    [[nodiscard]] fw::Result whitelist_save(std::span<uint8_t> out, size_t &len) const noexcept;
    [[nodiscard]] fw::Result sn_load(std::span<const uint8_t> in) noexcept;
    [[nodiscard]] fw::Result sn_save(std::span<uint8_t> out, size_t &len) const noexcept;
    /**
     * @brief  执行 SN 写入命令：校验后写 NVS，失败保留旧值
     * @param  cmd: 命令（载荷为 SN 文本）
     * @param[out] result: 执行结果
     */
    void save_sn(const fw::Command &cmd, fw::CommandResult &result) noexcept;

    [[nodiscard]] AppContext &ctx() noexcept
    {
        return *context_;
    }

    static constexpr uint32_t kRetryInitialMs = 1000;
    AppContext *context_ = nullptr;
    bool initialized_ = false;
    bool pending_ = false;
    bool attempted_ = false;
    uint32_t changed_ms_ = 0;
    uint32_t attempt_ms_ = 0;
    uint32_t retry_ms_ = 0U;
    StoreStatus status_{};
    LinkStatus whitelist_{};
    std::array<char, NVS_STORE_SN_MAX + 1U> sn_{};
};
/** `config` 只读查看命令表；静态存储，进程期有效。 */
[[nodiscard]] std::span<const cli::Cmd> config_cmds() noexcept;
} // namespace app
