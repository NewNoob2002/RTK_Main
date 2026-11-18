// 存储模块实现：启动加载 SN/白名单与目录中的键表、白名单变化去抖写回、SN 保存命令（只在 kLink 任务或启动前调用）。
// 通用键表的 NVS 读写在 StoreKeys.cpp。
#include "modules/storage/StorageModule.hpp"
#include "AppContext.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>
#include "def/StorageDef.hpp"
#include "fw/Log.hpp"
#include "fw/Tlog.hpp"
#include "nvs.h" // ESP_ERR_NVS_NOT_FOUND：区分“未设置 SN”与真正的读取错误
#include "modules/storage/StoreCatalog.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {
constexpr uint32_t kDebounceMs = 500;
constexpr uint32_t kRetryMaxMs = 30000;
using WhitelistRaw = std::array<std::array<uint8_t, core::kAddrLen>, kMaxAlarms>;
static_assert(sizeof(WhitelistRaw) == kMaxAlarms * NVS_STORE_ADDR_LEN);
} // namespace

fw::Result StorageModule::whitelistLoad(std::span<const uint8_t> in) noexcept
{
    if (in.size() > kMaxAlarms * core::kAddrLen || in.size() % core::kAddrLen != 0U) {
        return fw::Result::kInvalidArg;
    }
    whitelist_.whitelistCount = in.size() / core::kAddrLen;
    for (size_t i = 0; i < whitelist_.whitelistCount; ++i) {
        std::memcpy(whitelist_.whitelist[i].bytes.data(), in.subspan(i * core::kAddrLen).data(),
                    core::kAddrLen);
    }
    return fw::Result::kOk;
}

fw::Result StorageModule::whitelistSave(std::span<uint8_t> out, size_t &len) const noexcept
{
    len = whitelist_.whitelistCount * core::kAddrLen;
    if (out.size() < len) {
        return fw::Result::kNoSpace;
    }
    for (size_t i = 0; i < whitelist_.whitelistCount; ++i) {
        std::memcpy(out.subspan(i * core::kAddrLen).data(), whitelist_.whitelist[i].bytes.data(),
                    core::kAddrLen);
    }
    return fw::Result::kOk;
}

fw::Result StorageModule::snLoad(std::span<const uint8_t> in) noexcept
{
    if (in.empty() || in.size() > NVS_STORE_SN_MAX ||
        std::find_if(in.begin(), in.end(),
                     [](uint8_t c) { return c == 0U || c == '\r' || c == '\n'; }) != in.end()) {
        return fw::Result::kInvalidArg;
    }
    sn_.fill('\0');
    std::memcpy(sn_.data(), in.data(), in.size());
    return fw::Result::kOk;
}

fw::Result StorageModule::snSave(std::span<uint8_t> out, size_t &len) const noexcept
{
    len = std::strlen(sn_.data());
    if (out.size() < len) {
        return fw::Result::kNoSpace;
    }
    std::memcpy(out.data(), sn_.data(), len);
    return fw::Result::kOk;
}

void StorageModule::loadWhitelist() noexcept
{
    WhitelistRaw raw{};
    size_t count = 0;
    // 冻结 C API 使用指向 6 B 行的指针；std::array 布局由 static_assert 保证。
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays): 冻结 C ABI
    auto *rows = reinterpret_cast<uint8_t (*)[NVS_STORE_ADDR_LEN]>(raw.data());
    const esp_err_t error = nvs_store_get_alarms(rows, kMaxAlarms, &count);
    if (error != ESP_OK || count > kMaxAlarms) {
        FW_LOGW(fw::TraceCat::kStore, "load whitelist failed: %s", esp_err_to_name(error));
        return;
    }
    /* 按冻结的逐行字节布局反序列化，失败保持空表。 */
    std::array<uint8_t, sizeof(raw)> bytes{};
    std::memcpy(bytes.data(), raw.data(), count * core::kAddrLen);
    const auto loaded = whitelistLoad(std::span{bytes}.first(count * core::kAddrLen));
    if (!fw::isOk(loaded)) {
        FW_LOGW(fw::TraceCat::kStore, "invalid whitelist; use empty list");
    }
}

void StorageModule::loadSn() noexcept
{
    std::array<char, NVS_STORE_SN_MAX + 1U> sn{};
    const esp_err_t snError = nvs_store_get_sn(sn.data(), sn.size());
    if (snError != ESP_OK) {
        if (snError == ESP_ERR_NVS_NOT_FOUND) {
            // 未写入 SN 是出厂前的正常状态，不当作错误呈现。
            FW_LOGI(fw::TraceCat::kStore, "no SN set; name derived from BT MAC");
        } else {
            FW_LOGW(fw::TraceCat::kStore, "SN read failed: %s; name derived from BT MAC",
                    esp_err_to_name(snError));
        }
        return;
    }
    /* 验证 SN 后保存到模块，供后续 BT 名称初始化使用。 */
    std::array<uint8_t, NVS_STORE_SN_MAX + 1U> bytes{};
    std::memcpy(bytes.data(), sn.data(), sn.size());
    const auto status = snLoad(std::span{bytes}.first(std::strlen(sn.data())));
    if (!fw::isOk(status)) {
        FW_LOGW(fw::TraceCat::kStore, "invalid sn; derive from BT MAC");
    }
}

void StorageModule::loadKeyTables() noexcept
{
    for (const StoreGroup &group : storeCatalog()) {
        // 失败详情已由 loadKeys 的 kStore 事件覆盖；启动早期不额外打印。
        if (!fw::isOk(loadKeys(group.keys))) {
            continue;
        }
    }
}

/**
 * @brief  初始化 NVS 并加载 SN、白名单与各模块键表
 * @retval fw::Result::kOk 总是（加载失败使用缺省值）
 * @note   启动前单线程，非 ISR；同步读 NVS，幂等。
 */
fw::Result StorageModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kOk;
    }
    retry_ms_ = kRetryInitialMs;
    /* 初始化后端，失败仍按原策略尝试加载并使用缺省值。 */
    const esp_err_t init = nvs_store_init();
    if (init != ESP_OK) {
        FW_LOGE(fw::TraceCat::kStore, "store init failed: %s; use defaults", esp_err_to_name(init));
    }
    loadWhitelist();
    loadSn();
    /* ADR-0009 §2.4 / ADR-0010 §2.10：启动时按目录加载所有类别的键表。 */
    loadKeyTables();
    /* 保持 LinkStatus、SystemStatus、初始化标记、StoreStatus 的写入顺序。 */
    ctx().data.linkStatus.write(whitelist_);
    SystemStatus system{};
    system.sn = sn_;
    ctx().data.systemStatus.write(system);
    initialized_ = true;
    ctx().data.storeStatus.write(status_);
    return fw::Result::kOk;
}

void StorageModule::markWhitelistDirty(std::span<const core::BtAddr> list, uint32_t nowMs) noexcept
{
    /* 新变化只重启去抖，不缩短已有失败的退避窗口。 */
    whitelist_.whitelistCount = std::min(list.size(), kMaxAlarms);
    std::copy_n(list.begin(), whitelist_.whitelistCount, whitelist_.whitelist.begin());
    pending_ = true;
    changed_ms_ = nowMs;
    status_.whitelistPending = true;
    traceStore(StoreTrace::kDebounce, kDebounceMs, whitelist_.whitelistCount);
    ctx().data.storeStatus.write(status_);
}

bool StorageModule::poll(uint32_t /*nowMs*/) noexcept
{
    const uint32_t now = rtos::nowMs();
    if (!pending_ || now - changed_ms_ < kDebounceMs ||
        (attempted_ && now - attempt_ms_ < retry_ms_)) {
        return false;
    }
    /* 完成去抖和退避后，同步序列化并写入白名单。 */
    WhitelistRaw raw{};
    size_t len = 0;
    std::array<uint8_t, kMaxAlarms * core::kAddrLen> out{};
    const auto serialized = whitelistSave(out, len);
    if (!fw::isOk(serialized)) {
        FW_LOGE(fw::TraceCat::kStore, "whitelist not saved, serialization failed (%s)",
                fw::toString(serialized));
        return false;
    }
    std::memcpy(raw.data(), out.data(), len);
    // 冻结 C API 的连续 6 B 行布局与 raw 相同。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays): 冻结 C ABI
    const esp_err_t error = nvs_store_set_alarms(
        reinterpret_cast<const uint8_t (*)[NVS_STORE_ADDR_LEN]>(raw.data()), len / core::kAddrLen);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays)
    /* 使用写入完成时间计算重试，保持失败倍增和状态发布顺序。 */
    attempt_ms_ = rtos::nowMs();
    status_.whitelistError = error;
    if (error == ESP_OK) {
        pending_ = false;
        attempted_ = false;
        retry_ms_ = kRetryInitialMs;
        status_.retryMs = 0;
        traceStore(StoreTrace::kSave, static_cast<uint32_t>(fw::Result::kOk),
                   static_cast<uint32_t>(len / core::kAddrLen));
    } else {
        retry_ms_ = attempted_ ? std::min(retry_ms_ * 2U, kRetryMaxMs) : kRetryInitialMs;
        attempted_ = true;
        ++status_.failures;
        status_.retryMs = retry_ms_;
        traceStore(StoreTrace::kSave, static_cast<uint32_t>(fw::Result::kIoError),
                   static_cast<uint32_t>(error));
        traceStore(StoreTrace::kRetry, retry_ms_, status_.failures);
        FW_LOGW(fw::TraceCat::kStore, "whitelist save failed (%s), retry in %u ms",
                esp_err_to_name(error), static_cast<unsigned>(retry_ms_));
    }
    status_.whitelistPending = pending_;
    ctx().data.storeStatus.write(status_);
    return false;
}

esp_err_t StorageModule::flushWhitelist(uint32_t nowMs) noexcept
{
    /* 显式命令跳过去抖和退避，并返回实际后端结果。 */
    changed_ms_ = nowMs - kDebounceMs;
    attempted_ = false;
    static_cast<void>(poll(nowMs));
    return status_.whitelistError;
}

void StorageModule::saveSn(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const auto previous = sn_;
    result.status = snLoad(std::span{cmd.args}.first(cmd.argLen));
    if (!fw::isOk(result.status)) {
        return;
    }
    /* 先验证并编码，失败时还原模块内的原 SN。 */
    std::array<uint8_t, NVS_STORE_SN_MAX + 1U> encoded{};
    size_t len = 0;
    result.status = snSave(encoded, len);
    if (!fw::isOk(result.status)) {
        sn_ = previous;
        return;
    }
    /* 同步写入后端，回执和 StoreStatus 保留相同错误码。 */
    std::array<char, NVS_STORE_SN_MAX + 1U> sn{};
    std::memcpy(sn.data(), encoded.data(), len);
    const esp_err_t error = nvs_store_set_sn(sn.data());
    status_.snError = error;
    result.detail = error;
    result.status = error == ESP_OK ? fw::Result::kOk : fw::Result::kIoError;
    if (error != ESP_OK) {
        sn_ = previous;
        FW_LOGE(fw::TraceCat::kStore, "serial number save failed (%s), previous value kept",
                esp_err_to_name(error));
    }
    ctx().data.storeStatus.write(status_);
}

void StorageModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (cmd.verb != verb(StorageVerb::kSaveSn) || cmd.argLen > cmd.args.size()) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    /* 校验信封后交给 SN 保存流程。 */
    saveSn(cmd, result);
}
} // namespace app
