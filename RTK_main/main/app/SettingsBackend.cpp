// 设置服务到 DataCenter 与异步总线的适配层。
// 绑定仅启动任务调用。后端操作使用 Slot 和执行器同步机制，非 ISR。
#include "app/SettingsBackend.hpp"
#include <algorithm>

#include "def/GnssSubDef.hpp"

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
#if RTK_GNSS_ENABLED
static_assert(settings::kVerbGnssSetRate == static_cast<uint8_t>(GnssSubVerb::kApplyRate));
static_assert(settings::kVerbGnssSetOut == static_cast<uint8_t>(GnssSubVerb::kPatchOutput));
static_assert(settings::kVerbGnssSetGgaGate == static_cast<uint8_t>(GnssSubVerb::kApplyGgaGate));
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
    default:
        break;
    }

    constexpr std::array kMessages{gnss::OutputMsg::kGga, gnss::OutputMsg::kGsa,
                                   gnss::OutputMsg::kSatsInfo, gnss::OutputMsg::kGst,
                                   gnss::OutputMsg::kRmc};
    if (key < KeyId::kGnssOutGga || key > KeyId::kGnssOutRmc) {
        return false;
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
