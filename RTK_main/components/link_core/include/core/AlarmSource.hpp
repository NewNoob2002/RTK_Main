// 报警状态源：测试覆盖优先，其次围栏结果（围栏模块预留）。线程安全：否（链路任务独占）。
// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <cstdint>

#include "proto/AlarmCodec.hpp"

namespace core {

/** 报警状态源：测试覆盖优先、其次取围栏结果；无锁，仅链路任务使用。 */
class AlarmSource {
public:
    constexpr AlarmSource() noexcept = default;

    /**
     * 设置测试覆盖。state 必须是 kStateNormal/kStateAlert/kStateNoFix，level ≤ kLevelMax，
     * 否则返回 false 且状态不变。
     */
    [[nodiscard]] bool set_override(uint8_t state, uint8_t level) noexcept;
    void clear_override() noexcept;

    /** 围栏模块（预留）写入结果；接受任意值。 */
    void set_fence(uint8_t state) noexcept;

    /** 覆盖生效取覆盖值，否则取围栏值（初始 kStateUnknown）。 */
    [[nodiscard]] uint8_t state() const noexcept;
    /** 覆盖生效取覆盖等级；否则围栏为 kStateAlert 时为 2，其他为 0。 */
    [[nodiscard]] uint8_t level() const noexcept;
    [[nodiscard]] bool is_override() const noexcept;

private:
    bool override_ = false;
    uint8_t override_state_ = proto::alarm::kStateNormal;
    uint8_t override_level_ = 0;
    uint8_t fence_state_ = proto::alarm::kStateUnknown;
};

} // namespace core
