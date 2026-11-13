// 存储模块实现：启动加载 SN/白名单与目录中的键表、白名单变化去抖写回、SN 保存命令（只在 kLink 任务或启动前调用）。
// 通用键表的 NVS 读写在 store_keys.cpp。
#include "modules/storage/storage_module.hpp"
#include "app_context.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>
#include "def/storage_def.hpp"
#include "esp_log.h"
#include "fw/tlog.hpp"
#include "modules/storage/store_catalog.hpp"
#include "rtos/esp_status.hpp"

namespace app {
namespace {
constexpr uint32_t kDebounceMs = 500;
constexpr uint32_t kRetryMaxMs = 30000;
using WhitelistRaw = std::array<std::array<uint8_t, core::kAddrLen>, kMaxAlarms>;
static_assert(sizeof(WhitelistRaw) == kMaxAlarms * NVS_STORE_ADDR_LEN);
} // namespace

fw::Result StorageModule::whitelist_load(std::span<const uint8_t> in) noexcept
{
    if (in.size() > kMaxAlarms * core::kAddrLen || in.size() % core::kAddrLen != 0U) {
        return fw::Result::kInvalidArg;
    }
    whitelist_.whitelist_count = in.size() / core::kAddrLen;
    for (size_t i = 0; i < whitelist_.whitelist_count; ++i) {
        std::memcpy(whitelist_.whitelist[i].bytes.data(), in.subspan(i * core::kAddrLen).data(), core::kAddrLen);
    }
    return fw::Result::kOk;
}

fw::Result StorageModule::whitelist_save(std::span<uint8_t> out, size_t &len) const noexcept
{
    len = whitelist_.whitelist_count * core::kAddrLen;
    if (out.size() < len) {
        return fw::Result::kNoSpace;
    }
    for (size_t i = 0; i < whitelist_.whitelist_count; ++i) {
        std::memcpy(out.subspan(i * core::kAddrLen).data(), whitelist_.whitelist[i].bytes.data(), core::kAddrLen);
    }
    return fw::Result::kOk;
}

fw::Result StorageModule::sn_load(std::span<const uint8_t> in) noexcept
{
    if (in.empty() || in.size() > NVS_STORE_SN_MAX ||
        std::find_if(in.begin(), in.end(), [](uint8_t c) { return c == 0U || c == '\r' || c == '\n'; }) != in.end()) {
        return fw::Result::kInvalidArg;
    }
    sn_.fill('\0');
    std::memcpy(sn_.data(), in.data(), in.size());
    return fw::Result::kOk;
}

fw::Result StorageModule::sn_save(std::span<uint8_t> out, size_t &len) const noexcept
{
    len = std::strlen(sn_.data());
    if (out.size() < len) {
        return fw::Result::kNoSpace;
    }
    std::memcpy(out.data(), sn_.data(), len);
    return fw::Result::kOk;
}

void StorageModule::load_whitelist() noexcept
{
    WhitelistRaw raw{};
    size_t count = 0;
    // 冻结 C API 使用指向 6 B 行的指针；std::array 布局由 static_assert 保证。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays)
    auto *rows = reinterpret_cast<uint8_t (*)[NVS_STORE_ADDR_LEN]>(raw.data());
    const esp_err_t error = nvs_store_get_alarms(rows, kMaxAlarms, &count);
    if (error != ESP_OK || count > kMaxAlarms) {
        ESP_LOGW("storage", "load whitelist failed: %s", esp_err_to_name(error));
        return;
    }
    /* 按冻结的逐行字节布局反序列化，失败保持空表。 */
    std::array<uint8_t, sizeof(raw)> bytes{};
    std::memcpy(bytes.data(), raw.data(), count * core::kAddrLen);
    const auto loaded = whitelist_load(std::span{bytes}.first(count * core::kAddrLen));
    if (!fw::is_ok(loaded)) {
        ESP_LOGW("storage", "invalid whitelist; use empty list");
    }
}

void StorageModule::load_sn() noexcept
{
    std::array<char, NVS_STORE_SN_MAX + 1U> sn{};
    const esp_err_t sn_error = nvs_store_get_sn(sn.data(), sn.size());
    if (sn_error != ESP_OK) {
        ESP_LOGI("storage", "sn unavailable: %s; derive from BT MAC", esp_err_to_name(sn_error));
        return;
    }
    /* 验证 SN 后保存到模块，供后续 BT 名称初始化使用。 */
    std::array<uint8_t, NVS_STORE_SN_MAX + 1U> bytes{};
    std::memcpy(bytes.data(), sn.data(), sn.size());
    const auto status = sn_load(std::span{bytes}.first(std::strlen(sn.data())));
    if (!fw::is_ok(status)) {
        ESP_LOGW("storage", "invalid sn; derive from BT MAC");
    }
}

void StorageModule::load_key_tables() noexcept
{
    for (const StoreGroup &group : store_catalog()) {
        // 失败详情已由 load_keys 的 kStore 事件覆盖；启动早期不额外打印。
        if (!fw::is_ok(load_keys(group.keys))) {
            continue;
        }
    }
}

fw::Result StorageModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kOk;
    }
    /* 初始化后端，失败仍按原策略尝试加载并使用缺省值。 */
    const esp_err_t init = nvs_store_init();
    if (init != ESP_OK) {
        ESP_LOGE("storage", "store init failed: %s; use defaults", esp_err_to_name(init));
    }
    load_whitelist();
    load_sn();
    /* ADR-0009 §2.4 / ADR-0010 §2.10：启动时按目录加载所有类别的键表。 */
    load_key_tables();
    /* 保持 LinkStatus、SystemStatus、初始化标记、StoreStatus 的写入顺序。 */
    context_.data.link_status.write(whitelist_);
    SystemStatus system{};
    system.sn = sn_;
    context_.data.system_status.write(system);
    initialized_ = true;
    context_.data.store_status.write(status_);
    return fw::Result::kOk;
}

void StorageModule::mark_whitelist_dirty(std::span<const core::BtAddr> list, uint32_t now_ms) noexcept
{
    /* 新变化只重启去抖，不缩短已有失败的退避窗口。 */
    whitelist_.whitelist_count = std::min(list.size(), kMaxAlarms);
    std::copy_n(list.begin(), whitelist_.whitelist_count, whitelist_.whitelist.begin());
    pending_ = true;
    changed_ms_ = now_ms;
    status_.whitelist_pending = true;
    trace_store(StoreTrace::kDebounce, kDebounceMs, whitelist_.whitelist_count);
    context_.data.store_status.write(status_);
}

bool StorageModule::poll(uint32_t /*now_ms*/) noexcept
{
    const uint32_t now = rtos::now_ms();
    if (!pending_ || now - changed_ms_ < kDebounceMs || (attempted_ && now - attempt_ms_ < retry_ms_)) {
        return false;
    }
    /* 完成去抖和退避后，同步序列化并写入白名单。 */
    WhitelistRaw raw{};
    size_t len = 0;
    std::array<uint8_t, kMaxAlarms * core::kAddrLen> out{};
    const auto serialized = whitelist_save(out, len);
    if (!fw::is_ok(serialized)) {
        fw::tlog(fw::TraceCat::kStore, "whitelist not saved, serialization failed (%s)", fw::to_string(serialized));
        return false;
    }
    std::memcpy(raw.data(), out.data(), len);
    // 冻结 C API 的连续 6 B 行布局与 raw 相同。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays)
    const esp_err_t error =
        nvs_store_set_alarms(reinterpret_cast<const uint8_t (*)[NVS_STORE_ADDR_LEN]>(raw.data()), len / core::kAddrLen);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays)
    /* 使用写入完成时间计算重试，保持失败倍增和状态发布顺序。 */
    attempt_ms_ = rtos::now_ms();
    status_.whitelist_error = error;
    if (error == ESP_OK) {
        pending_ = false;
        attempted_ = false;
        retry_ms_ = kRetryInitialMs;
        status_.retry_ms = 0;
        trace_store(StoreTrace::kSave, static_cast<uint32_t>(fw::Result::kOk),
                    static_cast<uint32_t>(len / core::kAddrLen));
    } else {
        retry_ms_ = attempted_ ? std::min(retry_ms_ * 2U, kRetryMaxMs) : kRetryInitialMs;
        attempted_ = true;
        ++status_.failures;
        status_.retry_ms = retry_ms_;
        trace_store(StoreTrace::kSave, static_cast<uint32_t>(fw::Result::kIoError), static_cast<uint32_t>(error));
        trace_store(StoreTrace::kRetry, retry_ms_, status_.failures);
        fw::tlog(fw::TraceCat::kStore, "whitelist save failed (%s), retry in %u ms", esp_err_to_name(error), static_cast<unsigned>(retry_ms_));
    }
    status_.whitelist_pending = pending_;
    context_.data.store_status.write(status_);
    return false;
}

esp_err_t StorageModule::flush_whitelist(uint32_t now_ms) noexcept
{
    /* 显式命令跳过去抖和退避，并返回实际后端结果。 */
    changed_ms_ = now_ms - kDebounceMs;
    attempted_ = false;
    static_cast<void>(poll(now_ms));
    return status_.whitelist_error;
}

void StorageModule::save_sn(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const auto previous = sn_;
    result.status = sn_load(std::span{cmd.args}.first(cmd.arg_len));
    if (!fw::is_ok(result.status)) {
        return;
    }
    /* 先验证并编码，失败时还原模块内的原 SN。 */
    std::array<uint8_t, NVS_STORE_SN_MAX + 1U> encoded{};
    size_t len = 0;
    result.status = sn_save(encoded, len);
    if (!fw::is_ok(result.status)) {
        sn_ = previous;
        return;
    }
    /* 同步写入后端，回执和 StoreStatus 保留相同错误码。 */
    std::array<char, NVS_STORE_SN_MAX + 1U> sn{};
    std::memcpy(sn.data(), encoded.data(), len);
    const esp_err_t error = nvs_store_set_sn(sn.data());
    status_.sn_error = error;
    result.detail = error;
    result.status = error == ESP_OK ? fw::Result::kOk : fw::Result::kIoError;
    if (error != ESP_OK) {
        sn_ = previous;
        fw::tlog(fw::TraceCat::kStore, "serial number save failed (%s), previous value kept", esp_err_to_name(error));
    }
    context_.data.store_status.write(status_);
}

void StorageModule::on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (cmd.verb != verb(StorageVerb::kSaveSn) || cmd.arg_len > cmd.args.size()) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    /* 校验信封后交给 SN 保存流程。 */
    save_sn(cmd, result);
}
} // namespace app
