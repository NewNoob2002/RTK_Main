// 报警状态源：测试覆盖优先，其次围栏结果（phone_protocol.md §5）。
// 只被链路任务调用（类本身无锁）；纯内存读写，不阻塞，非 ISR 安全。

#include "core/AlarmSource.hpp"

namespace core {

namespace {

using proto::alarm::kLevelMax;
using proto::alarm::kStateAlert;
using proto::alarm::kStateNoFix;
using proto::alarm::kStateNormal;

// 围栏报警（尚无覆盖时）的默认等级，与 §5 一致。
constexpr uint8_t kFenceAlertLevel = 2U;

bool isValidState(uint8_t state) noexcept
{
    return (state == kStateNormal) || (state == kStateAlert) || (state == kStateNoFix);
}

} // namespace

bool AlarmSource::setOverride(uint8_t state, uint8_t level) noexcept
{
    if ((!isValidState(state)) || (level > kLevelMax)) {
        return false;
    }
    override_ = true;
    override_state_ = state;
    override_level_ = level;
    return true;
}

void AlarmSource::clearOverride() noexcept
{
    override_ = false;
}

void AlarmSource::setFence(uint8_t state) noexcept
{
    fence_state_ = state;
}

uint8_t AlarmSource::state() const noexcept
{
    return override_ ? override_state_ : fence_state_;
}

uint8_t AlarmSource::level() const noexcept
{
    if (override_) {
        return override_level_;
    }
    return (fence_state_ == kStateAlert) ? kFenceAlertLevel : 0U;
}

bool AlarmSource::isOverride() const noexcept
{
    return override_;
}

} // namespace core
