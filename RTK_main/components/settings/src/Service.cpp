// 统一设置服务实现（ADR-0018 §2.3）：查键→权限→取值域→编码→异步 post，以及快照读与域查询。
// 任意任务可调用（依赖后端锁与执行器 post/poll/cancel 线程安全）；不阻塞、不分配堆，非 ISR。
#include "settings/Service.hpp"

#include <array>

namespace settings {
namespace {

/** @brief 严格解析非负十进制整数（不允许符号、前导零或空白）。 @param text: 文本输入。 @param[out] out: 输出值。 @retval 是否合法。 */
bool parseStrictUnsigned(std::string_view text, int32_t &out) noexcept
{
    constexpr uint32_t kDecimalBase = 10U;
    constexpr uint32_t kMaxPeriodMs = 127000U;
    if (text.empty() || ((text.size() > 1U) && (text[0] == '0'))) {
        return false;
    }
    uint32_t acc = 0U;
    for (const char ch : text) {
        if ((ch < '0') || (ch > '9')) {
            return false;
        }
        acc = (acc * kDecimalBase) + static_cast<uint32_t>(ch - '0');
        if (acc > kMaxPeriodMs) {
            return false;
        }
    }
    // 已限制 acc <= 127000，转有符号 int32_t 无损。
    out = static_cast<int32_t>(acc);
    return true;
}

/** @brief GNSS 尚未探测完成时，按静态合法范围校验 rate 或单条输出周期（SV3 P2-1/P2-2）。 @param spec: 键定义。 @param value: 文本值。 @param[out] out: 规范值。 @retval kOk 通过；kBadValue 非法；kUnsupported 非此类键。 */
Error validateGnssPreProbe(const KeySpec &spec, std::string_view value,
                           CanonicalValue &out) noexcept
{
    if (spec.id == KeyId::kGnssRate) {
        constexpr std::array<int32_t, 5U> kStaticRates{1, 2, 5, 10, 20};
        Domain fallback{.kind = KeyKind::kEnum, .supported = true};
        for (size_t i = 0U; i < kStaticRates.size(); ++i) {
            fallback.enumValues[i] = kStaticRates[i];
        }
        fallback.enumCount = static_cast<uint8_t>(kStaticRates.size());
        return validateValue(spec, fallback, value, out);
    }
    if ((spec.id >= KeyId::kGnssOutGga) && (spec.id <= KeyId::kGnssOutRmc)) {
        int32_t periodMs = 0;
        int8_t iv = 0;
        if (!parseStrictUnsigned(value, periodMs) || !periodMsToInterval(periodMs, iv)) {
            return Error::kBadValue;
        }
        out.kind = CanonicalKind::kInt;
        out.number = periodMs;
        return Error::kOk;
    }
    return Error::kUnsupported;
}

} // namespace

/** @brief 读取接收机能力。 @retval 后端快照或空能力。 @note 任意任务，非 ISR。 */
GnssCaps Service::readGnssCaps() const noexcept
{
    if (backend_.gnssCaps == nullptr) {
        return GnssCaps{};
    }
    return backend_.gnssCaps(backend_.ctx);
}

/** @brief 查询 GNSS 是否可用。 @retval 是否启用。 @note 任意任务，非 ISR。 */
bool Service::isGnssEnabled() const noexcept
{
    return (backend_.gnssEnabled != nullptr) && backend_.gnssEnabled(backend_.ctx);
}

Error Service::set(std::string_view key, std::string_view value, Caller caller, Ticket &out,
                   bool factoryUnlocked) noexcept
{
    out = Ticket{};
    const KeySpec *const spec = findKey(key);
    if (spec == nullptr) {
        return Error::kUnknownKey;
    }
    const uint8_t bit = callerBit(caller);
    if ((bit == 0U) || ((spec->access & bit) == 0U)) {
        return Error::kLocked;
    }
    if (((spec->access & access::kNeedsUnlock) != 0U) && !factoryUnlocked) {
        return Error::kLocked;
    }
    const GnssCaps caps = readGnssCaps();
    const bool gnssEnabled = isGnssEnabled();
    Domain dom{};
    if (spec->domain == nullptr) {
        return Error::kUnsupported;
    }
    spec->domain(caps, gnssEnabled, dom);
    CanonicalValue canonical{};
    if (dom.supported) {
        const Error valErr = validateValue(*spec, dom, value, canonical);
        if (valErr != Error::kOk) {
            return valErr;
        }
    } else if (gnssEnabled && !caps.valid) {
        // 接收机尚未探测完成前，gnss.rate / gnss.out.* 仅按静态合法范围放行，与旧版预存 NVS 行为一致。
        const Error preErr = validateGnssPreProbe(*spec, value, canonical);
        if (preErr != Error::kOk) {
            return preErr;
        }
    } else {
        return Error::kUnsupported;
    }
    if (spec->encode == nullptr) {
        return Error::kUnsupported;
    }
    fw::Command cmd{};
    if (!spec->encode(canonical, cmd)) {
        return Error::kFailed;
    }
    if (backend_.post == nullptr) {
        return Error::kFailed;
    }
    return fromResult(backend_.post(backend_.ctx, spec->owner, cmd, caller, out));
}

PollState Service::poll(const Ticket &t, fw::CommandResult &out) const noexcept
{
    if (backend_.poll == nullptr || t.seq == 0U || t.slot == kInvalidSlot) {
        return PollState::kInvalid;
    }
    return backend_.poll(backend_.ctx, t, out);
}

void Service::cancel(const Ticket &t) const noexcept
{
    if (backend_.cancel == nullptr || t.seq == 0U || t.slot == kInvalidSlot) {
        return;
    }
    backend_.cancel(backend_.ctx, t);
}

bool Service::get(std::string_view key, Value &out) const noexcept
{
    const KeySpec *const spec = findKey(key);
    if (spec == nullptr) {
        return false;
    }
    return get(spec->id, out);
}

/** @brief 读指定键的值。 @param id: 注册编号。 @param[out] out: 值。 @retval 是否成功。 @note 任意任务，非 ISR。 */
bool Service::get(KeyId id, Value &out) const noexcept
{
    const KeySpec *const spec = keyAt(id);
    if (spec == nullptr || !spec->readable || backend_.get == nullptr) {
        return false;
    }
    return backend_.get(backend_.ctx, id, out);
}

bool Service::domain(std::string_view key, Domain &out) const noexcept
{
    const KeySpec *const spec = findKey(key);
    if (spec == nullptr) {
        out = Domain{};
        return false;
    }
    return domain(spec->id, out);
}

/** @brief 查询指定键的取值域。 @param id: 注册编号。 @param[out] out: 取值域。 @retval 是否存在定义。 @note 任意任务，非 ISR。 */
bool Service::domain(KeyId id, Domain &out) const noexcept
{
    const KeySpec *const spec = keyAt(id);
    if (spec == nullptr || spec->domain == nullptr) {
        out = Domain{};
        return false;
    }
    spec->domain(readGnssCaps(), isGnssEnabled(), out);
    return true;
}

} // namespace settings
