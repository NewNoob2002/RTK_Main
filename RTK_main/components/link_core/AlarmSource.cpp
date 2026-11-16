// 报警状态源：测试覆盖优先，其次围栏结果（phone_protocol.md §5）。

#include "core/AlarmSource.hpp"

namespace core {

namespace {

using proto::alarm::kLevelMax;
using proto::alarm::kStateAlert;
using proto::alarm::kStateNoFix;
using proto::alarm::kStateNormal;

// 围栏报警（尚无覆盖时）的默认等级，与 §5 一致。
constexpr uint8_t kFenceAlertLevel = 2U;

bool is_valid_state(uint8_t state) noexcept
{
    return (state == kStateNormal) || (state == kStateAlert) || (state == kStateNoFix);
}

} // namespace

bool AlarmSource::set_override(uint8_t state, uint8_t level) noexcept
{
    if ((!is_valid_state(state)) || (level > kLevelMax)) {
        return false;
    }
    override_ = true;
    override_state_ = state;
    override_level_ = level;
    return true;
}

void AlarmSource::clear_override() noexcept
{
    override_ = false;
}

void AlarmSource::set_fence(uint8_t state) noexcept
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

bool AlarmSource::is_override() const noexcept
{
    return override_;
}

} // namespace core
