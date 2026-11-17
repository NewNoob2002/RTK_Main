// 模板模块实现：演示状态初始化、非阻塞定时步进与 CommandBus 动词分派。
// init 启动前调用；poll 与 onCommand 仅在 kLink 任务调用；均非阻塞、非 ISR。
#include "modules/_template/TemplateModule.hpp"
#include "AppContext.hpp"

namespace app {
namespace {
constexpr uint32_t kDefaultLimitMs = 1000U; // 默认步进周期（ms）
constexpr size_t kBitsPerByte = 8U;
} // namespace

fw::Result TemplateModule::init() noexcept
{
    if (state_.initialized) {
        return fw::Result::kOk;
    }
    // 示例读取前置 DataCenter 快照；真实模块可据此校验依赖或发布初始状态。
    static_cast<void>(context_.data.systemStatus.read());
    state_ = {.initialized = true, .ticks = 0U, .limitMs = kDefaultLimitMs};
    timer_started_ = false;
    last_tick_ms_ = 0U;
    return fw::Result::kOk;
}

bool TemplateModule::poll(uint32_t nowMs) noexcept
{
    if (!state_.initialized) {
        return false;
    }
    if (!timer_started_ || ((nowMs - last_tick_ms_) >= state_.limitMs)) {
        timer_started_ = true;
        last_tick_ms_ = nowMs;
        ++state_.ticks;
    }
    return false;
}

void TemplateModule::onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
{
    if (!state_.initialized) {
        result.status = fw::Result::kInvalidState;
        return;
    }
    // 每个动词一个具名处理函数，case 内不堆逻辑（CODING-STYLE §6）。
    switch (static_cast<TemplateVerb>(cmd.verb)) {
    case TemplateVerb::kReset:
        result.status = cmd.argLen == 0U ? reset() : fw::Result::kInvalidArg;
        return;
    case TemplateVerb::kSetLimit:
        result.status = setLimit(cmd);
        return;
    }
    result.status = fw::Result::kInvalidArg; // 未知动词，与现有模块一致
}

fw::Result TemplateModule::reset() noexcept
{
    state_.ticks = 0U;
    return fw::Result::kOk;
}

fw::Result TemplateModule::setLimit(const fw::Command &cmd) noexcept
{
    if (cmd.argLen != sizeof(uint32_t)) {
        return fw::Result::kInvalidArg;
    }
    uint32_t limitMs = 0U; // 小端 4 字节
    for (size_t i = 0U; i < sizeof(uint32_t); ++i) {
        limitMs |= static_cast<uint32_t>(cmd.args[i]) << (i * kBitsPerByte);
    }
    if (limitMs == 0U || limitMs > kTemplateMaxLimitMs) {
        return fw::Result::kInvalidArg;
    }
    state_.limitMs = limitMs;
    return fw::Result::kOk;
}

TemplateState TemplateModule::state() const noexcept
{
    return state_;
}
} // namespace app
