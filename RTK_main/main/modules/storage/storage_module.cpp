// 存储模块实现：启动加载 SN/白名单、白名单变化去抖写回、SN 保存命令（只在 kLink 任务或启动前调用）。
#include "modules/storage/storage_module.hpp"
#include "app_context.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>
#include "def/storage_def.hpp"
#include "esp_log.h"
#include "fw/trace.hpp" // FW_TRACE_ENABLED
#if FW_TRACE_ENABLED
#include "modules/debug/debug_keys.hpp"
#endif
#if RTK_GNSS_ENABLED
#include "modules/gnss/gnss_keys.hpp"
#endif
/* 通用键表要读写整数键；nvs_store.h（冻结）没有对应 API，按任务卡直接使用 nvs.h。
 * 替身头不带 C++ 链接守卫，因此这里显式声明 C 链接；IDF 的 nvs.h 自带 extern "C"，嵌套无害。 */
extern "C" {
#include "nvs.h"
}
#include "rtos/esp_status.hpp"

namespace app {
namespace {
constexpr uint32_t kDebounceMs = 500;
constexpr uint32_t kRetryMaxMs = 30000;
using WhitelistRaw = std::array<std::array<uint8_t, core::kAddrLen>, kMaxAlarms>;
static_assert(sizeof(WhitelistRaw) == kMaxAlarms * NVS_STORE_ADDR_LEN);

constexpr size_t kMaxKeyWidth = 4U;   // 支持的最高键宽（u32）
constexpr uint32_t kByteMask = 0xFFU; // 按字节拆整数时使用的掩码

/* ADR-0009 §2.4：通用键表的登记处。新模块把 kKeys 加到这里即被启动流程加载。 */
// 按 FW_TRACE_ENABLED 与 RTK_GNSS_ENABLED 条件登记键表。
#if FW_TRACE_ENABLED && RTK_GNSS_ENABLED
constexpr std::array<std::span<const fw::StoreKey>, 2U> kKeyTables{{
    std::span<const fw::StoreKey>(debug::kKeys),
    std::span<const fw::StoreKey>(gnss_store::kKeys),
}};
#elif FW_TRACE_ENABLED
constexpr std::array<std::span<const fw::StoreKey>, 1U> kKeyTables{{
    std::span<const fw::StoreKey>(debug::kKeys),
}};
#elif RTK_GNSS_ENABLED
constexpr std::array<std::span<const fw::StoreKey>, 1U> kKeyTables{{
    std::span<const fw::StoreKey>(gnss_store::kKeys),
}};
#else
constexpr std::array<std::span<const fw::StoreKey>, 0U> kKeyTables{};
#endif

/* 通用键表只支持整数类型；其它类型宽度为 0，调用方据此拒绝该键。 */
size_t key_width(fw::StoreType type) noexcept
{
    switch (type) {
    case fw::StoreType::kU8:
        return 1U;
    case fw::StoreType::kU16:
        return 2U;
    case fw::StoreType::kU32:
        return 4U;
    default:
        return 0U;
    }
}

/* StoreKey 的 ns/key 只是视图，NVS 需要 0 结尾的名字；超长直接拒绝。 */
bool copy_key_name(std::string_view in, std::array<char, fw::kStoreNameMax + 1U> &out) noexcept
{
    if (in.size() > fw::kStoreNameMax) {
        return false;
    }
    std::memcpy(out.data(), in.data(), in.size());
    return true;
}
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

fw::Result StorageModule::load_keys(std::span<const fw::StoreKey> keys) noexcept
{
    fw::Result overall = fw::Result::kOk;
    uint32_t loaded = 0U;
    for (const fw::StoreKey &key : keys) {
        const size_t width = key_width(key.type);
        if (!fw::valid(key) || (key.binding.load == nullptr) || (width == 0U)) {
            overall = fw::Result::kInvalidArg;
            continue;
        }
        std::array<char, fw::kStoreNameMax + 1U> ns{};
        std::array<char, fw::kStoreNameMax + 1U> name{};
        if (!copy_key_name(key.ns, ns) || !copy_key_name(key.key, name)) {
            overall = fw::Result::kInvalidArg;
            continue;
        }
        nvs_handle_t handle = 0;
        if (nvs_open(ns.data(), NVS_READONLY, &handle) != ESP_OK) {
            // 命名空间还不存在：视为“未保存过”，保持模块缺省值。
            overall = fw::Result::kNotFound;
            continue;
        }
        uint32_t value = 0U;
        esp_err_t error = ESP_OK;
        if (key.type == fw::StoreType::kU32) {
            error = nvs_get_u32(handle, name.data(), &value);
        } else if (key.type == fw::StoreType::kU16) {
            uint16_t narrow = 0U;
            error = nvs_get_u16(handle, name.data(), &narrow);
            value = narrow;
        } else {
            uint8_t narrow = 0U;
            error = nvs_get_u8(handle, name.data(), &narrow);
            value = narrow;
        }
        nvs_close(handle);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            overall = fw::Result::kNotFound;
            continue;
        }
        if (error != ESP_OK) {
            overall = fw::Result::kIoError;
            continue;
        }
        /* 绑定函数按小端字节序列解析：与固件内其它持久化编码保持一致。 */
        std::array<uint8_t, kMaxKeyWidth> bytes{};
        for (size_t index = 0U; index < width; ++index) {
            // value 为 32 位、width ≤ 4，移位与掩码都不会越界。
            bytes[index] = static_cast<uint8_t>((value >> (8U * index)) & kByteMask);
        }
        const fw::Result applied = key.binding.load(key.binding.obj, std::span<const uint8_t>{bytes}.first(width));
        if (!fw::is_ok(applied)) {
            overall = applied;
            continue;
        }
        ++loaded;
    }
    trace_store(StoreTrace::kLoad, static_cast<uint32_t>(overall), loaded);
    return overall;
}

fw::Result StorageModule::save_keys(std::span<const fw::StoreKey> keys) noexcept
{
    fw::Result overall = fw::Result::kOk;
    uint32_t saved = 0U;
    for (const fw::StoreKey &key : keys) {
        const size_t width = key_width(key.type);
        if (!fw::valid(key) || (key.binding.save == nullptr) || (width == 0U)) {
            overall = fw::Result::kInvalidArg;
            continue;
        }
        std::array<char, fw::kStoreNameMax + 1U> ns{};
        std::array<char, fw::kStoreNameMax + 1U> name{};
        if (!copy_key_name(key.ns, ns) || !copy_key_name(key.key, name)) {
            overall = fw::Result::kInvalidArg;
            continue;
        }
        std::array<uint8_t, kMaxKeyWidth> bytes{};
        size_t len = 0U;
        const fw::Result encoded = key.binding.save(key.binding.obj, std::span<uint8_t>{bytes}, len);
        if (!fw::is_ok(encoded) || (len != width)) {
            overall = fw::is_ok(encoded) ? fw::Result::kInvalidArg : encoded;
            continue;
        }
        uint32_t value = 0U;
        for (size_t index = 0U; index < len; ++index) {
            // len ≤ 4，移位结果落在 32 位内。
            value |= static_cast<uint32_t>(bytes[index]) << (8U * index);
        }
        nvs_handle_t handle = 0;
        esp_err_t error = nvs_open(ns.data(), NVS_READWRITE, &handle);
        if (error == ESP_OK) {
            if (key.type == fw::StoreType::kU32) {
                error = nvs_set_u32(handle, name.data(), value);
            } else if (key.type == fw::StoreType::kU16) {
                // width == 2 时 value 只用了低 16 位，窄化无损。
                error = nvs_set_u16(handle, name.data(), static_cast<uint16_t>(value));
            } else {
                // width == 1 时 value 只用了低 8 位，窄化无损。
                error = nvs_set_u8(handle, name.data(), static_cast<uint8_t>(value));
            }
            if (error == ESP_OK) {
                error = nvs_commit(handle);
            }
            nvs_close(handle);
        }
        if (error != ESP_OK) {
            overall = fw::Result::kIoError;
            continue;
        }
        ++saved;
    }
    trace_store(StoreTrace::kSave, static_cast<uint32_t>(overall), saved);
    return overall;
}

std::span<const std::span<const fw::StoreKey>> StorageModule::key_tables() noexcept
{
    return {kKeyTables};
}

void StorageModule::load_key_tables() noexcept
{
    for (const std::span<const fw::StoreKey> table : key_tables()) {
        // 失败详情已由 load_keys 的 kStore 事件覆盖；启动早期不额外打印。
        if (!fw::is_ok(load_keys(table))) {
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
    /* ADR-0009 §2.4：启动时加载所有登记的键表（当前为 debug 的掩码与报告周期）。 */
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
        ESP_LOGE("storage", "serialize whitelist failed");
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
        ESP_LOGE("storage", "persist whitelist failed: %s; retry pending", esp_err_to_name(error));
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
        ESP_LOGE("storage", "save sn failed: %s", esp_err_to_name(error));
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
