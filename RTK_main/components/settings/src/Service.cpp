// 统一设置服务实现（ADR-0018 §2.3）：查键→权限→取值域→编码→异步 post，以及快照读与域查询。
// 任意任务可调用（依赖后端锁与执行器 post/poll/cancel 线程安全）；不阻塞、不分配堆，非 ISR。
#include "settings/Service.hpp"

namespace settings {

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
    Domain dom{};
    if (!domain(spec->id, dom) || !dom.supported) {
        return Error::kUnsupported;
    }
    CanonicalValue canonical{};
    const Error valErr = validateValue(*spec, dom, value, canonical);
    if (valErr != Error::kOk) {
        return valErr;
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
