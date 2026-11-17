// 报警状态源：测试覆盖优先，其次取围栏结果（围栏模块预留）；纯内存读写，不含持久化。
// 线程安全：否，仅链路任务调用；非 ISR、不阻塞、不分配堆。
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
     * @brief 设置测试覆盖；生效后 state()/level() 优先取覆盖值。
     * @param state 报警状态，必须是 kStateNormal/kStateAlert/kStateNoFix。
     * @param level 报警等级，须 ≤ kLevelMax。
     * @retval true 已设置，覆盖生效。
     * @retval false 参数非法，状态保持不变。
     * @note 非 ISR、不阻塞、不分配。
     */
    [[nodiscard]] bool setOverride(uint8_t state, uint8_t level) noexcept;
    /** @brief 清除测试覆盖，状态回落到围栏结果。
     *  @note 幂等：无覆盖时调用无副作用；非 ISR、不阻塞。 */
    void clearOverride() noexcept;

    /** @brief 写入围栏模块的报警结果（无覆盖时立即生效）。
     *  @param state 报警状态；协议侧透传，接受任意 uint8_t，不校验。
     *  @note 围栏模块尚未接入，本方法为后续实现保留；非 ISR、不阻塞。 */
    void setFence(uint8_t state) noexcept;

    /** @brief 当前生效的报警状态。
     *  @return 覆盖生效时取覆盖值，否则取围栏值（初始 kStateUnknown）。 */
    [[nodiscard]] uint8_t state() const noexcept;
    /** @brief 当前生效的报警等级。
     *  @return 覆盖生效时取覆盖等级；否则围栏为 kStateAlert 时为 2，其他为 0。 */
    [[nodiscard]] uint8_t level() const noexcept;
    /** @brief 测试覆盖当前是否生效。
     *  @return true 生效中；false 使用围栏值。 */
    [[nodiscard]] bool isOverride() const noexcept;

private:
    bool override_ = false;
    uint8_t override_state_ = proto::alarm::kStateNormal;
    uint8_t override_level_ = 0;
    uint8_t fence_state_ = proto::alarm::kStateUnknown;
};

} // namespace core
