// Generated-by: pi + codex/gpt-6.1-sol
// 人工负责人：UNKNOWN（待人工审查签字）
#include "storage_module.hpp"
#include <algorithm>
#include <cstring>
#include "esp_log.h"
#include "rtk/os/esp_status.hpp"

namespace rtk::app {
constinit StorageModule s_storage_module{};
namespace {
constexpr uint32_t kDebounceMs = 500;
constexpr uint32_t kRetryInitialMs = 1000;
constexpr uint32_t kRetryMaxMs = 30000;
using WhitelistRaw = std::array<std::array<uint8_t, core::kAddrLen>, kMaxAlarms>;
static_assert(sizeof(WhitelistRaw) == kMaxAlarms * RTK_STORE_ADDR_LEN);

::app::Result whitelist_load(void *obj, std::span<const uint8_t> in) noexcept
{
    // StoreKey 的 obj 固定绑定到 StorageModule。
    auto &module = *static_cast<StorageModule *>(obj);
    if (in.size() > kMaxAlarms * core::kAddrLen || in.size() % core::kAddrLen != 0U) {
        return ::app::Result::kInvalidArg;
    }
    module.whitelist.whitelist_count = in.size() / core::kAddrLen;
    for (size_t i = 0; i < module.whitelist.whitelist_count; ++i) {
        std::memcpy(module.whitelist.whitelist[i].bytes.data(), in.subspan(i * core::kAddrLen).data(), core::kAddrLen);
    }
    return ::app::Result::kOk;
}
::app::Result whitelist_save(const void *obj, std::span<uint8_t> out, size_t &len) noexcept
{
    // StoreKey 的 obj 固定绑定到 StorageModule。
    const auto &module = *static_cast<const StorageModule *>(obj);
    len = module.whitelist.whitelist_count * core::kAddrLen;
    if (out.size() < len) {
        return ::app::Result::kNoSpace;
    }
    for (size_t i = 0; i < module.whitelist.whitelist_count; ++i) {
        std::memcpy(out.subspan(i * core::kAddrLen).data(), module.whitelist.whitelist[i].bytes.data(), core::kAddrLen);
    }
    return ::app::Result::kOk;
}
::app::Result sn_load(void *obj, std::span<const uint8_t> in) noexcept
{
    if (in.empty() || in.size() > RTK_STORE_SN_MAX ||
        std::find_if(in.begin(), in.end(), [](uint8_t c) { return c == 0U || c == '\r' || c == '\n'; }) != in.end()) {
        return ::app::Result::kInvalidArg;
    }
    // StoreKey 的 obj 固定绑定到 StorageModule。
    auto &module = *static_cast<StorageModule *>(obj);
    module.sn.fill('\0');
    std::memcpy(module.sn.data(), in.data(), in.size());
    return ::app::Result::kOk;
}
::app::Result sn_save(const void *obj, std::span<uint8_t> out, size_t &len) noexcept
{
    // StoreKey 的 obj 固定绑定到 StorageModule，sn 始终有结尾 0。
    const auto &module = *static_cast<const StorageModule *>(obj);
    len = std::strlen(module.sn.data());
    if (out.size() < len) {
        return ::app::Result::kNoSpace;
    }
    std::memcpy(out.data(), module.sn.data(), len);
    return ::app::Result::kOk;
}
constexpr std::array<::app::StoreKey, 2> kKeys{{
    {"rtk",
     "alarm_wl",
     ::app::StoreType::kBlob,
     kMaxAlarms * core::kAddrLen,
     ::app::store_flag::kConsole | ::app::store_flag::kExport,
     0,
     0,
     ::app::kAllProducts,
     "报警器白名单",
     {&s_storage_module, whitelist_load, whitelist_save}},
    {"rtk",
     "sn",
     ::app::StoreType::kStr,
     RTK_STORE_SN_MAX,
     ::app::store_flag::kConsole | ::app::store_flag::kExport | ::app::store_flag::kRebootRequired,
     0,
     0,
     ::app::kAllProducts,
     "设备 SN（重启生效）",
     {&s_storage_module, sn_load, sn_save}},
}};
static_assert(::app::valid(kKeys[0]) && ::app::valid(kKeys[1]) && ::app::unique_keys(kKeys));
} // namespace

::app::Result storage_init(void *self) noexcept
{
    // ModuleOps 绑定保证 self 为 StorageModule，且只在启动前调用。
    auto &module = *static_cast<StorageModule *>(self);
    if (module.initialized) {
        return ::app::Result::kOk;
    }
    const esp_err_t init = rtk_store_init();
    if (init != ESP_OK) {
        ESP_LOGE("storage", "store init failed: %s; use defaults", esp_err_to_name(init));
    }
    WhitelistRaw raw{};
    size_t count = 0;
    // 冻结 C API 使用指向 6 B 行的指针；std::array 布局由 static_assert 保证。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays)
    auto *rows = reinterpret_cast<uint8_t (*)[RTK_STORE_ADDR_LEN]>(raw.data());
    const esp_err_t error = rtk_store_get_alarms(rows, kMaxAlarms, &count);
    if (error == ESP_OK && count <= kMaxAlarms) {
        std::array<uint8_t, sizeof(raw)> bytes{};
        std::memcpy(bytes.data(), raw.data(), count * core::kAddrLen);
        const auto loaded = kKeys[0].binding.load(kKeys[0].binding.obj, std::span{bytes}.first(count * core::kAddrLen));
        if (!::app::is_ok(loaded)) {
            ESP_LOGW("storage", "invalid whitelist; use empty list");
        }
    } else {
        ESP_LOGW("storage", "load whitelist failed: %s", esp_err_to_name(error));
    }
    std::array<char, RTK_STORE_SN_MAX + 1U> sn{};
    const esp_err_t sn_error = rtk_store_get_sn(sn.data(), sn.size());
    if (sn_error == ESP_OK) {
        std::array<uint8_t, RTK_STORE_SN_MAX + 1U> bytes{};
        std::memcpy(bytes.data(), sn.data(), sn.size());
        const auto status = kKeys[1].binding.load(kKeys[1].binding.obj, std::span{bytes}.first(std::strlen(sn.data())));
        if (!::app::is_ok(status)) {
            ESP_LOGW("storage", "invalid sn; derive from BT MAC");
        }
    } else {
        ESP_LOGI("storage", "sn unavailable: %s; derive from BT MAC", esp_err_to_name(sn_error));
    }
    data::kLinkStatus.write(module.whitelist);
    SystemStatus system{};
    system.sn = module.sn;
    data::kSystemStatus.write(system);
    module.initialized = true;
    data::kStoreStatus.write(module.status);
    return ::app::Result::kOk;
}

void mark_whitelist_dirty(std::span<const core::BtAddr> list, uint32_t now_ms) noexcept
{
    auto &module = s_storage_module;
    module.whitelist.whitelist_count = std::min(list.size(), kMaxAlarms);
    std::copy_n(list.begin(), module.whitelist.whitelist_count, module.whitelist.whitelist.begin());
    module.pending = true;
    module.changed_ms = now_ms;
    // 新变化只重启去抖，不缩短已有失败的退避窗口。
    module.status.whitelist_pending = true;
    data::kStoreStatus.write(module.status);
}

bool storage_poll(void *self, uint32_t /*round_start*/) noexcept
{
    // 绑定类型安全，所有读写仅在 kLink。
    auto &module = *static_cast<StorageModule *>(self);
    const uint32_t now = os::now_ms();
    if (!module.pending || now - module.changed_ms < kDebounceMs ||
        (module.attempted && now - module.attempt_ms < module.retry_ms)) {
        return false;
    }
    WhitelistRaw raw{};
    size_t len = 0;
    std::array<uint8_t, kMaxAlarms * core::kAddrLen> out{};
    const auto serialized = kKeys[0].binding.save(kKeys[0].binding.obj, out, len);
    if (!::app::is_ok(serialized)) {
        ESP_LOGE("storage", "serialize whitelist failed");
        return false;
    }
    std::memcpy(raw.data(), out.data(), len);
    // 冻结 C API 的连续 6 B 行布局与 raw 相同。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays)
    const esp_err_t error =
        rtk_store_set_alarms(reinterpret_cast<const uint8_t (*)[RTK_STORE_ADDR_LEN]>(raw.data()), len / core::kAddrLen);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays)
    module.attempt_ms = os::now_ms();
    module.status.whitelist_error = error;
    if (error == ESP_OK) {
        module.pending = false;
        module.attempted = false;
        module.retry_ms = kRetryInitialMs;
        module.status.retry_ms = 0;
    } else {
        module.retry_ms = module.attempted ? std::min(module.retry_ms * 2U, kRetryMaxMs) : kRetryInitialMs;
        module.attempted = true;
        ++module.status.failures;
        module.status.retry_ms = module.retry_ms;
        ESP_LOGE("storage", "persist whitelist failed: %s; retry pending", esp_err_to_name(error));
    }
    module.status.whitelist_pending = module.pending;
    data::kStoreStatus.write(module.status);
    return false;
}

esp_err_t flush_whitelist(uint32_t now_ms) noexcept
{
    auto &module = s_storage_module;
    module.changed_ms = now_ms - kDebounceMs;
    module.attempted = false;
    // poll 的返回值只表示积压；显式命令读取后端错误码作为回执。
    static_cast<void>(storage_poll(&module, now_ms));
    return module.status.whitelist_error;
}

void storage_command(void *self, const ::app::Command &cmd, ::app::CommandResult &result) noexcept
{
    // 绑定类型安全；只由执行器调用，不用于 ISR。
    auto &module = *static_cast<StorageModule *>(self);
    if (cmd.verb != verb(StorageVerb::kSaveSn) || cmd.arg_len > cmd.args.size()) {
        result.status = ::app::Result::kInvalidArg;
        return;
    }
    const auto previous = module.sn;
    result.status = kKeys[1].binding.load(kKeys[1].binding.obj, std::span{cmd.args}.first(cmd.arg_len));
    if (!::app::is_ok(result.status)) {
        return;
    }
    std::array<uint8_t, RTK_STORE_SN_MAX + 1U> encoded{};
    size_t len = 0;
    result.status = kKeys[1].binding.save(kKeys[1].binding.obj, encoded, len);
    if (!::app::is_ok(result.status)) {
        module.sn = previous;
        return;
    }
    std::array<char, RTK_STORE_SN_MAX + 1U> sn{};
    std::memcpy(sn.data(), encoded.data(), len);
    const esp_err_t error = rtk_store_set_sn(sn.data());
    module.status.sn_error = error;
    result.detail = error;
    result.status = error == ESP_OK ? ::app::Result::kOk : ::app::Result::kIoError;
    if (error != ESP_OK) {
        module.sn = previous;
        ESP_LOGE("storage", "save sn failed: %s", esp_err_to_name(error));
    }
    data::kStoreStatus.write(module.status);
}
} // namespace rtk::app
