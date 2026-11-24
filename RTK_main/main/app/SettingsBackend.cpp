// 设置服务到 DataCenter 与异步总线的适配层。
// 绑定仅启动任务调用。后端操作使用 Slot 和执行器同步机制，非 ISR。
#include "app/SettingsBackend.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "def/GnssSubDef.hpp"
#include "app/PowerRequests.hpp"

#include "AppContext.hpp"

namespace app {
namespace {

using settings::Caller;
using settings::KeyId;
using settings::Owner;
using settings::Value;
using settings::ValueKind;

constexpr uint32_t kMsPerSecond = 1000U;

static_assert(settings::kVerbStorageSaveSn == verb(StorageVerb::kSaveSn));
static_assert(settings::kVerbStorageWhitelistAdd == verb(StorageVerb::kWhitelistAdd));
static_assert(settings::kVerbStorageWhitelistDel == verb(StorageVerb::kWhitelistDel));
static_assert(settings::kVerbStorageSetPairNext == verb(StorageVerb::kSetPairNext));
static_assert(settings::kVerbStorageSetHeight == verb(StorageVerb::kSetHeight));
static_assert(settings::kVerbStorageSetSnPn == verb(StorageVerb::kSetSnPn));
#if RTK_GNSS_ENABLED
static_assert(settings::kVerbGnssSetRate == static_cast<uint8_t>(GnssSubVerb::kApplyRate));
static_assert(settings::kVerbGnssSetOut == static_cast<uint8_t>(GnssSubVerb::kPatchOutput));
static_assert(settings::kVerbGnssSetGgaGate == static_cast<uint8_t>(GnssSubVerb::kApplyGgaGate));
static_assert(settings::kVerbGnssSetPpp == static_cast<uint8_t>(GnssSubVerb::kApplyPpp));
static_assert(settings::kVerbGnssSetSatTrack == static_cast<uint8_t>(GnssSubVerb::kApplySatTrack));
static_assert(settings::kGgaGateFlagEnabled == kGgaGateFlagEnabled);
static_assert(settings::kGgaGateFlagMinSats == kGgaGateFlagMinSats);
static_assert(settings::kGgaGateCmdArgLen == kGgaGateCmdArgLen);
#endif
static_assert(static_cast<uint8_t>(Caller::kConsole) ==
              static_cast<uint8_t>(rtos::Caller::kConsole));
static_assert(static_cast<uint8_t>(Caller::kWeb) == static_cast<uint8_t>(rtos::Caller::kWeb));
static_assert(static_cast<uint8_t>(Caller::kPhone) == static_cast<uint8_t>(rtos::Caller::kPhone));
static_assert(static_cast<uint8_t>(Caller::kInternal) ==
              static_cast<uint8_t>(rtos::Caller::kInternal));
static_assert(static_cast<uint8_t>(Caller::kMqtt) == static_cast<uint8_t>(rtos::Caller::kMqtt));

/** @brief 从后端指针恢复应用上下文。 @param ctx: 绑定时传入的 AppContext 指针。 @retval 应用上下文。 */
AppContext &context(void *ctx) noexcept
{
    return *static_cast<AppContext *>(ctx);
}

/** @brief 查找归属模块的执行器。 @param ctx: 应用上下文。 @param owner: 归属。 @retval 目标执行器或 nullptr。 */
rtos::Executor *executorFor(const AppContext &ctx, Owner owner) noexcept
{
    if (owner == Owner::kStorage) {
        return ctx.linkExecutor;
    }
#if RTK_GNSS_ENABLED
    if (owner == Owner::kGnss) {
        return ctx.ioExecutor;
    }
#endif
    return nullptr;
}

/** @brief 读设置操作结果。 @param ctx: 后端。 @param ticket: 请求凭据。 @param[out] out: 命令结果。 @retval 完成状态。 */
settings::PollState pollSetting(void *ctx, const settings::Ticket &ticket,
                                fw::CommandResult &out) noexcept
{
    auto *executor = executorFor(context(ctx), ticket.owner);
    if (executor == nullptr) {
        return settings::PollState::kInvalid;
    }
    switch (executor->poll({ticket.seq, ticket.slot}, out)) {
    case rtos::PollState::kPending:
        return settings::PollState::kPending;
    case rtos::PollState::kDone:
        return settings::PollState::kDone;
    case rtos::PollState::kCancelled:
        return settings::PollState::kCancelled;
    case rtos::PollState::kInvalid:
    default:
        return settings::PollState::kInvalid;
    }
}

/** @brief 取消设置操作。 @param ctx: 后端。 @param ticket: 请求凭据。 */
void cancelSetting(void *ctx, const settings::Ticket &ticket) noexcept
{
    if (auto *executor = executorFor(context(ctx), ticket.owner); executor != nullptr) {
        executor->cancel({ticket.seq, ticket.slot});
    }
}

/** @brief 读取配置的定位频率。 @param data: 共享主题。 @retval 非零定位频率。 */
uint32_t currentRate(const data::DataCenter &data) noexcept
{
#if RTK_GNSS_ENABLED
    const uint32_t published = data.gnssRateHz.load(std::memory_order_acquire);
    return published == 0U ? CONFIG_RTK_GNSS_RATE_HZ : published;
#else
    static_cast<void>(data);
    return 1U;
#endif
}

/** @brief 读取实际接收机输出周期。 @param data: 共享主题。 @param msg: App 输出索引对应的语句。 @retval 毫秒周期，0 表示关闭。 */
uint32_t currentPeriod(const data::DataCenter &data, gnss::OutputMsg msg) noexcept
{
    // 调用方只传 App 输出集合中的成员，下标可直接用于两个输出表。
    const size_t index = static_cast<size_t>(msg);
    auto config = data::GnssOutConfig::defaults();
#if RTK_GNSS_ENABLED
    const auto status = data.gnssStatus.read();
    if (status.model == gnss::Model::kCm680) {
        if (msg == gnss::OutputMsg::kSatsInfo) {
            constexpr auto kGsvDomain = static_cast<size_t>(settings::OutKeyIndex::kGsv);
            if (data.gnssCaps.read().outPeriods[kGsvDomain].count <= 1U) {
                return 0U;
            }
        }
        return status.rxPeriodMs[index];
    }
    const auto stored = data.gnssOutCfg.read();
    if (stored.isValid()) {
        config = stored;
    }
#endif
    if ((config.mask & (1U << index)) == 0U) {
        return 0U;
    }
    return data::followsFixRate(config, index)
               ? kMsPerSecond / currentRate(data)
               : core::services::GnssService::intervalToPeriodMs(config.intervals[index]);
}

/** @brief 将字节数组编码为大写十六进制字符串。 @param bytes: 输入字节。 @param[out] out: 输出字符缓冲。 @retval 写入字符数。 */
uint8_t formatHexUpper(std::span<const uint8_t> bytes, std::span<char> out) noexcept
{
    constexpr std::string_view kHex = "0123456789ABCDEF";
    constexpr uint8_t kNibbleMask = 0x0FU;
    constexpr unsigned kNibbleShift = 4U;
    if (bytes.size() * 2U > out.size()) {
        return 0U;
    }
    size_t pos = 0U;
    for (const uint8_t byte : bytes) {
        out[pos++] = kHex[static_cast<size_t>((byte >> kNibbleShift) & kNibbleMask)];
        out[pos++] = kHex[static_cast<size_t>(byte & kNibbleMask)];
    }
    // 调用方固定传 8 B 天线高（pos == 16 <= UINT8_MAX），缩窄无损。
    return static_cast<uint8_t>(pos);
}

/**
 * @brief 读取单星座开关设置（gnss.const.gps/bds/glo/gal）当前值。
 * @param data: 共享主题。
 * @param key: 星座设置键编号。
 * @param[out] out: 成功时填充布尔值；失败时不保证内容。
 * @retval true 当前硬件支持该星座并已输出值。
 * @retval false 当前硬件不支持该星座或键号不匹配。
 */
bool getConstSetting(const data::DataCenter &data, KeyId key, Value &out) noexcept
{
    const auto caps = data.gnssCaps.read();
    if (!caps.enabled || !caps.valid) {
        return false;
    }
    const auto cfg = data.gnssSatTrack.read().value_or(core::SatTrackConfig::defaults());
    uint8_t bit = 0U;
    uint8_t flag = 0U;
    switch (key) {
    case KeyId::kGnssConstGps:
        bit = settings::const_bit::kGps;
        flag = cfg.gps;
        break;
    case KeyId::kGnssConstBds:
        bit = settings::const_bit::kBds;
        flag = cfg.bds;
        break;
    case KeyId::kGnssConstGlo:
        bit = settings::const_bit::kGlo;
        flag = cfg.glo;
        break;
    case KeyId::kGnssConstGal:
        bit = settings::const_bit::kGal;
        flag = cfg.gal;
        break;
    default:
        return false;
    }
    if ((caps.constMask & bit) == 0U) {
        return false;
    }
    out = {.kind = ValueKind::kBool, .boolVal = (flag != 0U)};
    return true;
}

/** @brief 读取手机/星座扩展键的当前快照（gnss.ppp/gnss.sat_track/gnss.cutoff/phone.height/phone.sn_pn）。 @param data: 共享主题。 @param key: 键编号。 @param[out] out: 输出值。 @retval 是否命中并成功填充。 */
bool getExtendedSetting(const data::DataCenter &data, KeyId key, Value &out) noexcept
{
    switch (key) {
    case KeyId::kGnssPpp: {
        const auto caps = data.gnssCaps.read();
        out = {.kind = ValueKind::kBool,
               .boolVal =
                   caps.enabled && caps.pppSupported && data.gnssPppEnabled.read().value_or(false)};
        return true;
    }
    case KeyId::kGnssSatTrack: {
        const auto cfg = data.gnssSatTrack.read().value_or(core::SatTrackConfig::defaults());
        out = {.kind = ValueKind::kString};
        const int written =
            std::snprintf(out.text.data(), out.text.size(), "%u,%u,%u,%u,%u,%u",
                          static_cast<unsigned>(cfg.cutoffDeg), static_cast<unsigned>(cfg.gps),
                          static_cast<unsigned>(cfg.glo), static_cast<unsigned>(cfg.bds),
                          static_cast<unsigned>(cfg.gal), static_cast<unsigned>(cfg.sbas));
        if (written <= 0 || static_cast<size_t>(written) >= out.text.size()) {
            return false;
        }
        // 最大输出为 "90,1,1,1,1,1"（12 字节），缩窄到 uint8_t 无损。
        out.textLen = static_cast<uint8_t>(written);
        return true;
    }
    case KeyId::kPhoneHeight: {
        const auto height = data.phoneHeight.read();
        out = {.kind = ValueKind::kString};
        out.textLen = formatHexUpper(height, out.text);
        return out.textLen == core::kAntennaHeightBytes * 2U;
    }
    case KeyId::kPhoneSnPn: {
        const auto sn = data.settingsSn.read();
        const auto pn = data.phonePn.read();
        out = {.kind = ValueKind::kBytes};
        const size_t snLen = std::min(static_cast<size_t>(sn.textLen), core::kPhoneSnBytes);
        if (snLen > 0U) {
            std::memcpy(out.text.data(), sn.text.data(), snLen);
        }
        std::memcpy(std::span{out.text}.subspan(core::kPhoneSnBytes).data(), pn.data(), pn.size());
        // kPhoneSnPnBytes 固定为 32，缩窄到 uint8_t 无损。
        out.textLen = static_cast<uint8_t>(core::kPhoneSnPnBytes);
        return true;
    }
    default:
        break;
    }
    const settings::KeySpec *const spec = settings::keyAt(key);
    if ((spec != nullptr) && (spec->name == "gnss.cutoff")) {
        const auto caps = data.gnssCaps.read();
        if (!caps.enabled || !caps.valid || (caps.constMask == 0U)) {
            return false;
        }
        const auto cfg = data.gnssSatTrack.read().value_or(core::SatTrackConfig::defaults());
        out = {.kind = ValueKind::kInt, .number = static_cast<int32_t>(cfg.cutoffDeg)};
        return true;
    }
    return false;
}

/** @brief 从已发布快照读取当前配置值。 @param ctx: 后端。 @param key: 键编号。 @param[out] out: 值。 @retval 是否提供该值。 */
bool getSetting(void *ctx, KeyId key, Value &out) noexcept
{
    const auto &data = context(ctx).data;
    switch (key) {
    case KeyId::kGnssRate:
        // 配置定位频率最大为 20 Hz。
        out = {.number = static_cast<int32_t>(currentRate(data))};
        return true;
    case KeyId::kGnssGgaGateEnabled:
#if RTK_GNSS_ENABLED
        out = {.kind = ValueKind::kBool, .boolVal = data.ggaGateStatus.read().enabled};
#else
        out = {.kind = ValueKind::kBool};
#endif
        return true;
    case KeyId::kGnssGgaGateMinSats:
#if RTK_GNSS_ENABLED
        out = {.number = data.ggaGateStatus.read().minSats};
#else
        out = {};
#endif
        return true;
    case KeyId::kBtPairNext:
        out = {.kind = ValueKind::kBool, .boolVal = data.storeStatus.read().btPairNext};
        return true;
    case KeyId::kFactorySn:
        out = data.settingsSn.read();
        out.kind = ValueKind::kString;
        return true;
    case KeyId::kGnssPpp:
    case KeyId::kGnssSatTrack:
    case KeyId::kPhoneHeight:
    case KeyId::kPhoneSnPn:
        return getExtendedSetting(data, key, out);
    case KeyId::kGnssConstGps:
    case KeyId::kGnssConstBds:
    case KeyId::kGnssConstGlo:
    case KeyId::kGnssConstGal:
        return getConstSetting(data, key, out);
    default:
        break;
    }

    constexpr std::array kMessages{gnss::OutputMsg::kGga, gnss::OutputMsg::kGsa,
                                   gnss::OutputMsg::kSatsInfo, gnss::OutputMsg::kGst,
                                   gnss::OutputMsg::kRmc};
    if (key < KeyId::kGnssOutGga || key > KeyId::kGnssOutRmc) {
        return getExtendedSetting(data, key, out);
    }
    const size_t index = static_cast<size_t>(key) - static_cast<size_t>(KeyId::kGnssOutGga);
    // 协议间隔最多 128 秒，能无损表示为 int32_t。
    out = {.number = static_cast<int32_t>(currentPeriod(data, kMessages[index]))};
    return true;
}

/** @brief 读取 GNSS 能力快照。 @param ctx: 后端。 @retval 当前能力集合。 */
settings::GnssCaps readCaps(void *ctx) noexcept
{
    return context(ctx).data.gnssCaps.read();
}

/** @brief 查询 GNSS 模块是否可用。 @param ctx: 后端。 @retval GNSS UART 已启动。 */
bool gnssEnabled(void *ctx) noexcept
{
#if RTK_GNSS_ENABLED
    return context(ctx).data.gnssCaps.read().enabled;
#else
    static_cast<void>(ctx);
    return false;
#endif
}

} // namespace

fw::Result postSettingCommand(void *ctx, Owner owner, const fw::Command &cmd, Caller caller,
                              settings::Ticket &out) noexcept
{
    const power::SettingAdmission admission;
    if (!admission.admitted()) {
        return fw::Result::kBusy;
    }
    auto *executor = executorFor(context(ctx), owner);
    if (executor == nullptr) {
        return fw::Result::kNotSupported;
    }
    auto queued = cmd;
    queued.target = id(owner == Owner::kStorage ? ModuleKind::kStorage : ModuleKind::kGnss);
    rtos::Ticket ticket{};
    // 上面的静态断言保证调用方枚举的对应关系。
    const auto result = executor->post(queued, static_cast<rtos::Caller>(caller), ticket);
    if (fw::isOk(result)) {
        out = {ticket.seq, ticket.slot, owner};
    }
    return result;
}

void bindSettings(AppContext &ctx) noexcept
{
    ctx.settings.bindBackend({.ctx = &ctx,
                              .post = &postSettingCommand,
                              .poll = &pollSetting,
                              .cancel = &cancelSetting,
                              .get = &getSetting,
                              .gnssCaps = &readCaps,
                              .gnssEnabled = &gnssEnabled});
}

} // namespace app
