// 存储模块实现：启动加载 SN/白名单与目录中的键表、白名单变化去抖写回、SN 保存命令（只在 kLink 任务或启动前调用）。
// 通用键表的 NVS 读写在 StoreKeys.cpp。
#include "modules/storage/StorageModule.hpp"
#include "AppContext.hpp"
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>
#include <string_view>
#include "def/StorageDef.hpp"
#include "fw/Log.hpp"
#include "fw/Tlog.hpp"
#include "modules/storage/StoreCatalog.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "modules/storage/StoreWriteGate.hpp"
#include "rtos/EspStatus.hpp"
#include "sdkconfig.h"

extern "C" {
#include "nvs.h"
}

namespace app {
namespace {
constexpr uint32_t kDebounceMs = 500;
constexpr uint32_t kRetryMaxMs = 30000;
using WhitelistRaw = std::array<std::array<uint8_t, core::kAddrLen>, kMaxAlarms>;
static_assert(sizeof(WhitelistRaw) == kMaxAlarms * NVS_STORE_ADDR_LEN);

#if defined(CONFIG_RTK_BOOT_VERBOSE) && CONFIG_RTK_BOOT_VERBOSE
constexpr bool kBootVerbose = true;
#else
constexpr bool kBootVerbose = false;
#endif

/**
 * @brief 显示两个分区的 NVS 条目用量。
 * @note 仅启动任务，可等待 NVS 和日志输出，非 ISR。
 */
void logNvsUsage() noexcept
{
    nvs_stats_t user{};
    nvs_stats_t factory{};
    const auto userErr = nvs_get_stats(nullptr, &user);
    const auto factoryErr = nvs_get_stats(NVS_STORE_FACTORY_PARTITION, &factory);
    if (userErr == ESP_OK && factoryErr == ESP_OK) {
        FW_LOGI(fw::TraceCat::kStore, "NVS used %zu of %zu entries (nvs), %zu of %zu (nvs_fac)",
                user.used_entries, user.total_entries, factory.used_entries, factory.total_entries);
    } else if (userErr == ESP_OK) {
        FW_LOGI(fw::TraceCat::kStore, "NVS used %zu of %zu entries (nvs), unavailable (nvs_fac)",
                user.used_entries, user.total_entries);
    } else if (factoryErr == ESP_OK) {
        FW_LOGI(fw::TraceCat::kStore, "NVS unavailable (nvs), used %zu of %zu entries (nvs_fac)",
                factory.used_entries, factory.total_entries);
    } else {
        FW_LOGI(fw::TraceCat::kStore, "NVS usage unavailable (nvs and nvs_fac)");
    }
}

/**
 * @brief 读取目录中有显示名的缓存值。
 * @param group: 目录组。
 * @param name: 要显示的键名。
 * @retval 静态显示名，缺失、不可读或没有名称时为 nullptr。
 * @note 仅启动任务，可阻塞，非 ISR。
 */
const char *cachedName(const StoreGroup &group, std::string_view name) noexcept
{
    for (const fw::StoreKey &key : group.keys) {
        if (key.key == name) {
            uint32_t value = 0U;
            if (store::readValue(key, value) == ESP_OK) {
                return group.valueName(key.key, value);
            }
            break;
        }
    }
    return nullptr;
}

/**
 * @brief 输出目录组的加载摘要，GNSS 缓存附家族和型号名。
 * @param group: 目录组。
 * @param counts: 该组加载结果。
 * @note 仅启动任务，可阻塞，非 ISR。
 */
void logLoadSummary(const StoreGroup &group, const StoreLoadCounts &counts) noexcept
{
    // 组名来自目录的短常量，转换为 printf 精度无损。
    if (group.valueName != nullptr && counts.saved != 0U) {
        const char *family = cachedName(group, "family");
        const char *model = cachedName(group, "model");
        if (family != nullptr || model != nullptr) {
            FW_LOGI(fw::TraceCat::kStore, "%.*s: %" PRIu32 " saved, %" PRIu32 " default (%s, %s)",
                    static_cast<int>(group.name.size()), group.name.data(), counts.saved,
                    counts.defaults, family != nullptr ? family : "-",
                    model != nullptr ? model : "-");
            return;
        }
    }
    FW_LOGI(fw::TraceCat::kStore, "%.*s: %" PRIu32 " saved, %" PRIu32 " default",
            static_cast<int>(group.name.size()), group.name.data(), counts.saved, counts.defaults);
}
/**
 * @brief 在内存白名单中执行单条添加或删除（不含 NVS 写入）
 * @param list: 待修改的白名单状态
 * @param addr: 目标 6 字节 MAC
 * @param add: true 添加，false 删除
 * @retval kOk 列表已更新
 * @retval kInvalidState 添加时地址已存在
 * @retval kNoSpace 添加时列表已满
 * @retval kNotFound 删除时地址不存在
 * @note 纯内存操作，不阻塞，非 ISR。
 */
fw::Result mutateWhitelist(LinkStatus &list, const core::BtAddr &addr, bool add) noexcept
{
    const auto entries = std::span{list.whitelist}.first(list.whitelistCount);
    const auto found = std::ranges::find(entries, addr);
    if (add) {
        if (found != entries.end()) {
            return fw::Result::kInvalidState;
        }
        if (list.whitelistCount >= kMaxAlarms) {
            return fw::Result::kNoSpace;
        }
        list.whitelist[list.whitelistCount++] = addr;
        return fw::Result::kOk;
    }
    if (found == entries.end()) {
        return fw::Result::kNotFound;
    }
    // found 落在 [begin, end) 内，索引差值非负且小于 kMaxAlarms。
    const auto removeIdx = static_cast<size_t>(found - entries.begin());
    for (size_t i = removeIdx + 1U; i < list.whitelistCount; ++i) {
        list.whitelist[i - 1U] = list.whitelist[i];
    }
    --list.whitelistCount;
    list.whitelist[list.whitelistCount] = core::BtAddr{};
    return fw::Result::kOk;
}

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
        const auto failure = (error != ESP_OK) ? error : ESP_ERR_INVALID_SIZE;
        FW_LOGW(fw::TraceCat::kStore, "Whitelist unavailable: %s, using empty list",
                esp_err_to_name(failure));
        return;
    }
    /* 按冻结的逐行字节布局反序列化，失败保持空表。 */
    std::array<uint8_t, sizeof(raw)> bytes{};
    std::memcpy(bytes.data(), raw.data(), count * core::kAddrLen);
    const auto loaded = whitelistLoad(std::span{bytes}.first(count * core::kAddrLen));
    if (!fw::isOk(loaded)) {
        FW_LOGW(fw::TraceCat::kStore, "Whitelist invalid, using empty list");
    } else {
        FW_LOGI(fw::TraceCat::kStore, "Whitelist loaded, %zu entries", count);
    }
}

void StorageModule::loadSn() noexcept
{
    std::array<char, NVS_STORE_SN_MAX + 1U> sn{};
    status_.snError = nvs_store_get_sn(sn.data(), sn.size());
    if (status_.snError == ESP_OK) {
        std::array<uint8_t, NVS_STORE_SN_MAX + 1U> bytes{};
        std::memcpy(bytes.data(), sn.data(), sn.size());
        if (!fw::isOk(snLoad(std::span{bytes}.first(std::strlen(sn.data()))))) {
            status_.snError = ESP_ERR_INVALID_ARG;
        }
    }
    if (status_.snError == ESP_OK) {
        FW_LOGI(fw::TraceCat::kStore, "SN loaded: %s", sn_.data());
    } else if (status_.snError == ESP_ERR_NVS_NOT_FOUND) {
        FW_LOGI(fw::TraceCat::kStore, "SN not set, name derived from BT MAC");
    } else {
        FW_LOGW(fw::TraceCat::kStore, "SN unavailable: %s, name derived from BT MAC",
                esp_err_to_name(status_.snError));
    }
}

void StorageModule::loadKeyTables() noexcept
{
    const auto &boot = ctx().bootConfig;
    for (const StoreGroup &group : storeCatalog()) {
        const StoreLoadCounts counts = loadBootGroup(group);
        logLoadSummary(group, counts);
        if (!kBootVerbose && counts.failed == 0U) {
            continue;
        }
        for (const fw::StoreKey &key : group.keys) {
            std::array<char, store::kKeyLineBytes> line{};
            store::formatKey(line, key, group.valueName, boot.safeDefaults || !boot.userNvsReady);
            FW_LOGI(fw::TraceCat::kStore, "%s", line.data());
        }
    }
}

StoreLoadCounts StorageModule::loadBootGroup(const StoreGroup &group) noexcept
{
    const auto &boot = ctx().bootConfig;
    if (boot.safeDefaults || !boot.userNvsReady) {
        return {.defaults = static_cast<uint32_t>(group.keys.size())}; // 固件目录只含少量键。
    }
    if (group.name == "debug") {
        return boot.debugCounts;
    }
    if (group.name == "gnss.config" && !group.keys.empty()) {
        // baud 是目录末项，已由 earlyConfig 读取。
        const auto later = loadKeys(group.keys.first(group.keys.size() - 1U));
        return {later.saved + boot.baudCounts.saved, later.defaults + boot.baudCounts.defaults,
                later.failed + boot.baudCounts.failed};
    }
    return loadKeys(group.keys);
}

/**
 * @brief  加载 SN、白名单与后期模块键，沿用早期 NVS 初始化结果
 * @retval fw::Result::kOk 总是（加载失败使用缺省值）
 * @note   启动前单线程，非 ISR；同步读 NVS，幂等。
 */
fw::Result StorageModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kOk;
    }
    retry_ms_ = kRetryInitialMs;
    logNvsUsage();
    loadSn();
    if (ctx().bootConfig.userNvsReady) {
        loadWhitelist();
    }
    loadKeyTables();
    /* 保持 LinkStatus、SystemStatus、初始化标记、StoreStatus 的写入顺序。 */
    ctx().data.linkStatus.write(whitelist_);
    SystemStatus system{};
    system.sn = sn_;
    ctx().data.systemStatus.write(system);
    publishSettingsSn();
    initialized_ = true;
    status_.btPairNext = bt_pair_next_;
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

/**
 * @brief 推进用户设置的去抖与写入重试。
 * @retval false 等待下一轮时钟推进。
 * @note 仅 kLink，写 NVS 可阻塞，非 ISR。
 */
bool StorageModule::poll(uint32_t /*nowMs*/) noexcept
{
    const uint32_t now = rtos::nowMs();
    pollPhoneBlobs(now);
    if (!pending_ || now - changed_ms_ < kDebounceMs ||
        (attempted_ && now - attempt_ms_ < retry_ms_)) {
        return false;
    }
    const store::WriteGuard guard;
    if (!guard.active()) {
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

fw::Result StorageModule::loadPhoneHeight(void *, std::span<const uint8_t> in) noexcept
{
    if (in.size() != core::AntennaHeight{}.size() ||
        !core::validAntennaHeight(in.first<core::kAntennaHeightBytes>())) {
        return fw::Result::kInvalidArg;
    }
    auto &storage = appContext().storage;
    std::copy(in.begin(), in.end(), storage.phone_height_.begin());
    storage.phone_height_state_.known = true;
    return fw::Result::kOk;
}

fw::Result StorageModule::loadPhonePpp(void *, std::span<const uint8_t> in) noexcept
{
    if (in.size() != 1U || in.front() > 1U) {
        return fw::Result::kInvalidArg;
    }
    appContext().data.phonePppEnabled.write(in.front() != 0U);
    return fw::Result::kOk;
}

fw::Result StorageModule::loadPhoneSatCfg(void *, std::span<const uint8_t> in) noexcept
{
    const auto decoded = core::decodeSatTrackConfig(in);
    if (!decoded.has_value()) {
        return fw::Result::kInvalidArg;
    }
    appContext().data.phoneSatConfig.write(decoded);
    return fw::Result::kOk;
}

fw::Result StorageModule::loadPhonePn(void *, std::span<const uint8_t> in) noexcept
{
    if (in.size() != core::kPhonePnBytes) {
        return fw::Result::kInvalidArg;
    }
    auto &storage = appContext().storage;
    std::copy(in.begin(), in.end(), storage.phone_pn_.begin());
    storage.phone_pn_state_.known = true;
    return fw::Result::kOk;
}

fw::Result StorageModule::loadPhoneNtrip(void *, std::span<const uint8_t> in) noexcept
{
    if (in.size() != core::kPhoneNtripBytes) {
        return fw::Result::kInvalidArg;
    }
    auto &storage = appContext().storage;
    std::copy(in.begin(), in.end(), storage.phone_ntrip_.begin());
    storage.phone_ntrip_state_.known = true;
    return fw::Result::kOk;
}

fw::Result StorageModule::loadPhoneJsonSrv(void *, std::span<const uint8_t> in) noexcept
{
    if (in.size() != core::kPhoneJsonServerBytes) {
        return fw::Result::kInvalidArg;
    }
    auto &storage = appContext().storage;
    std::copy(in.begin(), in.end(), storage.phone_json_srv_.begin());
    storage.phone_json_srv_state_.known = true;
    return fw::Result::kOk;
}

fw::Result StorageModule::loadPhoneTb(void *, std::span<const uint8_t> in) noexcept
{
    if (in.size() != core::kPhoneThingsboardBytes) {
        return fw::Result::kInvalidArg;
    }
    auto &storage = appContext().storage;
    std::copy(in.begin(), in.end(), storage.phone_tb_.begin());
    storage.phone_tb_state_.known = true;
    return fw::Result::kOk;
}

fw::Result StorageModule::loadBtPairNext(void * /*obj*/, std::span<const uint8_t> in) noexcept
{
    if (in.size() != 1U || in.front() > 1U) {
        return fw::Result::kInvalidArg;
    }
    appContext().storage.bt_pair_next_ = (in.front() != 0U);
    return fw::Result::kOk;
}

std::string_view StorageModule::sn() const noexcept
{
    return std::string_view{sn_.data(), std::strlen(sn_.data())};
}

bool StorageModule::consumeBtPairNext() noexcept
{
    if (!bt_pair_next_ || (context_ == nullptr)) {
        return false;
    }
    const auto &boot = ctx().bootConfig;
    if (!boot.userNvsReady || boot.safeDefaults) {
        FW_LOGW(fw::TraceCat::kStore, "bt.pair_next clear skipped: user NVS unavailable");
        return false;
    }
    // 先确认 NVS 清零持久化，再清 RAM 标志并允许开窗，防止掉电/写失败导致跨重启重复开窗。
    const esp_err_t error = store::writeValue(store::kBtPairNextKey, 0U);
    if (error != ESP_OK) {
        FW_LOGW(fw::TraceCat::kStore, "bt.pair_next clear failed (%s); pairing window deferred",
                esp_err_to_name(error));
        return false;
    }
    bt_pair_next_ = false;
    status_.btPairNext = false;
    ctx().data.storeStatus.write(status_);
    return true;
}

bool StorageModule::setPhoneBlob(std::span<uint8_t> dst, DebouncedState &state,
                                 std::span<const uint8_t> value, uint32_t nowMs) noexcept
{
    const auto &boot = ctx().bootConfig;
    const store::WriteGuard guard;
    if (!boot.userNvsReady || boot.safeDefaults || !guard.active() ||
        (dst.size() != value.size())) {
        return false;
    }
    if (!state.known || !std::equal(dst.begin(), dst.end(), value.begin())) {
        state.known = true;
        std::copy(value.begin(), value.end(), dst.begin());
        state.pending = true;
        state.changedMs = nowMs;
        traceStore(StoreTrace::kDebounce, kDebounceMs);
    }
    return true;
}

void StorageModule::pollPhoneBlob(const fw::StoreKey &key, std::span<const uint8_t> data,
                                  DebouncedState &state, uint32_t nowMs, const char *label) noexcept
{
    if (!state.pending || (nowMs - state.changedMs < kDebounceMs) ||
        (nowMs - state.attemptMs < state.retryMs)) {
        return;
    }
    const auto error = store::writeBlob(key, data);
    if (error == ESP_OK) {
        state.pending = false;
        state.retryMs = 0U;
        traceStore(StoreTrace::kSave, static_cast<uint32_t>(fw::Result::kOk), 1U);
    } else {
        state.attemptMs = rtos::nowMs();
        state.retryMs =
            state.retryMs == 0U ? kRetryInitialMs : std::min(state.retryMs * 2U, kRetryMaxMs);
        FW_LOGW(fw::TraceCat::kStore, "%s save failed (%s)", label, esp_err_to_name(error));
    }
}

void StorageModule::pollPhoneBlobs(uint32_t nowMs) noexcept
{
    pollPhoneBlob(store::kPhoneHeightKey, phone_height_, phone_height_state_, nowMs,
                  "antenna height");
    pollPhoneBlob(store::kPhonePnKey, phone_pn_, phone_pn_state_, nowMs, "pn");
    pollPhoneBlob(store::kPhoneNtripKey, phone_ntrip_, phone_ntrip_state_, nowMs, "ntrip");
    pollPhoneBlob(store::kPhoneJsonSrvKey, phone_json_srv_, phone_json_srv_state_, nowMs,
                  "json server");
    pollPhoneBlob(store::kPhoneTbKey, phone_tb_, phone_tb_state_, nowMs, "thingsboard");
}

bool StorageModule::setPhoneHeight(const core::AntennaHeight &height, uint32_t nowMs) noexcept
{
    return setPhoneBlob(phone_height_, phone_height_state_, height, nowMs);
}

core::PhoneSnPn StorageModule::phoneSnPn() const noexcept
{
    core::PhoneSnPn out{};
    const size_t snLen = std::min(std::strlen(sn_.data()), core::kPhoneSnBytes);
    std::memcpy(out.data(), sn_.data(), snLen);
    std::copy(phone_pn_.begin(), phone_pn_.end(),
              std::span<uint8_t>{out}.subspan(core::kPhoneSnBytes).begin());
    return out;
}

bool StorageModule::setPhoneSnPn(const core::PhoneSnPn &snPn, uint32_t nowMs) noexcept
{
    const auto &boot = ctx().bootConfig;
    if (!boot.userNvsReady || boot.safeDefaults) {
        return false;
    }
    const auto snField = std::span<const uint8_t>{snPn}.first(core::kPhoneSnBytes);
    const auto pnField = std::span<const uint8_t>{snPn}.subspan(core::kPhoneSnBytes);
    size_t snLen = 0U;
    while ((snLen < snField.size()) && (snField[snLen] != 0U)) {
        ++snLen;
    }
    for (const uint8_t ch : pnField) {
        if (ch == 0U) {
            break;
        }
        if ((ch == '\r') || (ch == '\n')) {
            return false;
        }
    }
    fw::Command cmd{};
    cmd.target = id(ModuleKind::kStorage);
    cmd.verb = verb(StorageVerb::kSaveSn);
    // snLen <= core::kPhoneSnBytes(16) <= fw::kCommandArgBytes(32)，缩窄安全。
    cmd.argLen = static_cast<uint8_t>(snLen);
    std::copy_n(snField.begin(), snLen, cmd.args.begin());
    fw::CommandResult result{};
    saveSn(cmd, result);
    if (!fw::isOk(result.status)) {
        return false;
    }
    return setPhoneBlob(phone_pn_, phone_pn_state_, pnField, nowMs);
}

bool StorageModule::setPhoneNtrip(const core::PhoneNtripConfig &cfg, uint32_t nowMs) noexcept
{
    return setPhoneBlob(phone_ntrip_, phone_ntrip_state_, cfg, nowMs);
}

bool StorageModule::setPhoneJsonServer(const core::PhoneJsonServerConfig &cfg,
                                       uint32_t nowMs) noexcept
{
    return setPhoneBlob(phone_json_srv_, phone_json_srv_state_, cfg, nowMs);
}

bool StorageModule::setPhoneThingsboard(const core::PhoneThingsboardConfig &cfg,
                                        uint32_t nowMs) noexcept
{
    return setPhoneBlob(phone_tb_, phone_tb_state_, cfg, nowMs);
}

esp_err_t StorageModule::flushWhitelist(uint32_t nowMs) noexcept
{
    /* 显式命令跳过去抖和退避，并返回实际后端结果。 */
    changed_ms_ = nowMs - kDebounceMs;
    attempted_ = false;
    static_cast<void>(poll(nowMs));
    return status_.whitelistError;
}

void StorageModule::publishSettingsSn() noexcept
{
    settings::Value value{.kind = settings::ValueKind::kString};
    const auto serial = sn();
    const size_t len = std::min(serial.size(), value.text.size());
    std::copy_n(serial.begin(), len, value.text.begin());
    // SN 有效字符数最多 16，能够无损转换。
    value.textLen = static_cast<uint8_t>(len);
    ctx().data.settingsSn.write(value);
}

void StorageModule::saveSn(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    const store::WriteGuard guard;
    if (!guard.active()) {
        result.status = fw::Result::kBusy;
        return;
    }
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
    if (error == ESP_OK) {
        publishSettingsSn();
        ctx().data.configRev.fetch_add(1U, std::memory_order_release);
    } else {
        sn_ = previous;
        FW_LOGE(fw::TraceCat::kStore, "serial number save failed (%s), previous value kept",
                esp_err_to_name(error));
    }
    ctx().data.storeStatus.write(status_);
}

esp_err_t StorageModule::writeWhitelistNow() const noexcept
{
    WhitelistRaw raw{};
    size_t len = 0U;
    std::array<uint8_t, kMaxAlarms * core::kAddrLen> out{};
    const auto serialized = whitelistSave(out, len);
    if (!fw::isOk(serialized)) {
        FW_LOGE(fw::TraceCat::kStore, "whitelist not saved, serialization failed (%s)",
                fw::toString(serialized));
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(raw.data(), out.data(), len);
    // 冻结 C API 的连续 6 B 行布局与 raw 相同。
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays): 冻结 C ABI
    return nvs_store_set_alarms(reinterpret_cast<const uint8_t (*)[NVS_STORE_ADDR_LEN]>(raw.data()),
                                len / core::kAddrLen);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-avoid-c-arrays)
}

void StorageModule::editWhitelistCmd(const fw::Command &cmd, bool add,
                                     fw::CommandResult &result) noexcept
{
    if (cmd.argLen != core::kAddrLen) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    const store::WriteGuard guard;
    if (!guard.active()) {
        result.status = fw::Result::kBusy;
        return;
    }
    const auto &boot = ctx().bootConfig;
    if (!boot.userNvsReady || boot.safeDefaults) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    core::BtAddr addr{};
    std::copy_n(cmd.args.begin(), core::kAddrLen, addr.bytes.begin());
    const LinkStatus previous = whitelist_;
    result.status = mutateWhitelist(whitelist_, addr, add);
    if (!fw::isOk(result.status)) {
        return;
    }
    const esp_err_t error = writeWhitelistNow();
    status_.whitelistError = error;
    result.detail = error;
    if (error != ESP_OK) {
        whitelist_ = previous;
        result.status = fw::Result::kIoError;
        FW_LOGE(fw::TraceCat::kStore, "whitelist command save failed (%s), previous list kept",
                esp_err_to_name(error));
        ctx().data.storeStatus.write(status_);
        return;
    }
    pending_ = false;
    attempted_ = false;
    retry_ms_ = kRetryInitialMs;
    status_.whitelistPending = false;
    status_.retryMs = 0U;
    ctx().data.linkStatus.write(whitelist_);
    ctx().data.storeStatus.write(status_);
    result.status = fw::Result::kOk;
}

void StorageModule::setPairNextCmd(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if ((cmd.argLen != 1U) || (cmd.args[0] > 1U)) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    const store::WriteGuard guard;
    if (!guard.active()) {
        result.status = fw::Result::kBusy;
        return;
    }
    const auto &boot = ctx().bootConfig;
    if (!boot.userNvsReady || boot.safeDefaults) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    const uint32_t value = cmd.args[0];
    const esp_err_t error = store::writeValue(store::kBtPairNextKey, value);
    result.detail = error;
    if (error != ESP_OK) {
        result.status = fw::Result::kIoError;
        FW_LOGE(fw::TraceCat::kStore, "bt.pair_next save failed (%s)", esp_err_to_name(error));
        return;
    }
    bt_pair_next_ = (value != 0U);
    status_.btPairNext = bt_pair_next_;
    ctx().data.storeStatus.write(status_);
    result.status = fw::Result::kOk;
}

static_assert(verb(StorageVerb::kSaveSn) != StorageModule::kEraseUser &&
                  verb(StorageVerb::kSaveSn) != StorageModule::kEraseFactory &&
                  verb(StorageVerb::kWhitelistAdd) != StorageModule::kEraseUser &&
                  verb(StorageVerb::kWhitelistAdd) != StorageModule::kEraseFactory &&
                  verb(StorageVerb::kWhitelistDel) != StorageModule::kEraseUser &&
                  verb(StorageVerb::kWhitelistDel) != StorageModule::kEraseFactory &&
                  verb(StorageVerb::kSetPairNext) != StorageModule::kEraseUser &&
                  verb(StorageVerb::kSetPairNext) != StorageModule::kEraseFactory,
              "StorageVerb values must not collide with kEraseUser/kEraseFactory");

void StorageModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (cmd.argLen > cmd.args.size()) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    if (cmd.verb == kEraseUser || cmd.verb == kEraseFactory) {
        eraseForReset(cmd, result);
        return;
    }
    // verb 来源为 uint8_t，与 StorageVerb 底层类型相同；未知动词由 default 拒绝。
    switch (static_cast<StorageVerb>(cmd.verb)) {
    case StorageVerb::kSaveSn:
        saveSn(cmd, result);
        return;
    case StorageVerb::kWhitelistAdd:
        editWhitelistCmd(cmd, true, result);
        break;
    case StorageVerb::kWhitelistDel:
        editWhitelistCmd(cmd, false, result);
        break;
    case StorageVerb::kSetPairNext:
        setPairNextCmd(cmd, result);
        break;
    default:
        result.status = fw::Result::kInvalidArg;
        break;
    }
    if (fw::isOk(result.status)) {
        ctx().data.configRev.fetch_add(1U, std::memory_order_release);
    }
}

void StorageModule::eraseForReset(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (cmd.argLen != 0U) {
        result.status = fw::Result::kInvalidArg;
        return;
    }
    if (!store::closeWritesForErase()) {
        result.status = fw::Result::kBusy;
        return;
    }
    // IDF 擦除时关闭该分区的所有句柄并卸载分区，Bluedroid 无法在重启前再写回绑定。
    esp_err_t error = nvs_store_erase_user();
    if (error == ESP_OK && cmd.verb == kEraseFactory) {
        error = nvs_store_erase_factory();
    }
    result.detail = error;
    result.status = error == ESP_OK ? fw::Result::kOk : fw::Result::kIoError;
    if (error != ESP_OK) {
        FW_LOGE(fw::TraceCat::kStore, "Factory reset erase failed: %s", esp_err_to_name(error));
        store::reopenWrites();
    }
}
} // namespace app
