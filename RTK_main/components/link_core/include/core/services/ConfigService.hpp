// 配置类指令（PHONE）：0x07 工作模式、0x14 电台、0x15 日志，持有三块配置的唯一副本（本轮不持久化）。
// 线程安全：否（链路任务独占）；非 ISR、不阻塞。堆：不分配（三块配置为定长成员）。
// 冻结接口（ADR-0007 §3.3）。实现方不得修改。
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"

namespace core::services {

/** 配置类指令服务（0x07/0x14/0x15）：持有配置的唯一副本；仅链路任务调用。 */
class ConfigService {
public:
    /** @brief 三块配置清零，且 radio[kRadioModeOffset] 置为 kRadioModeDefault。
     *  @note 启动期调用；不分配、不阻塞、非 ISR 安全。 */
    ConfigService() noexcept;
    ConfigService(const ConfigService &) = delete;
    ConfigService &operator=(const ConfigService &) = delete;
    ConfigService(ConfigService &&) = delete;
    ConfigService &operator=(ConfigService &&) = delete;
    ~ConfigService() = default;

    /** @brief 工作模式配置的只读视图（0x07）。
     *  @return 指向内部定长数组的 span；本对象存活期间有效，调用方只能读。 */
    [[nodiscard]] std::span<const uint8_t, kWorkModeLen> workMode() const noexcept;
    /** @brief 电台配置的只读视图（0x14），首字节为电台模式。
     *  @return 指向内部定长数组的 span；本对象存活期间有效，调用方只能读。 */
    [[nodiscard]] std::span<const uint8_t, kRadioLen> radio() const noexcept;
    /** @brief 日志配置的只读视图（0x15）。
     *  @return 指向内部定长数组的 span；本对象存活期间有效，调用方只能读。 */
    [[nodiscard]] std::span<const uint8_t, kLogLen> log() const noexcept;

    /** @brief 本服务负责的 0x07/0x14/0x15 静态路由表。
     *  @return 指向静态数组的视图，内容只读且与实例无关；不得修改。 */
    [[nodiscard]] static std::span<const Route> routes() noexcept;
    /** @brief 把静态路由表与 this 绑定，供 CommandRouter::init 展开。
     *  @return RouteTable{this, routes()}；本对象须存活到路由器不再使用该表为止。
     *  @note 不分配、无副作用；启动期调用。 */
    [[nodiscard]] RouteTable table() noexcept
    {
        return RouteTable{this, routes()};
    }

private:
    std::array<uint8_t, kWorkModeLen> work_mode_{};
    std::array<uint8_t, kRadioLen> radio_{};
    std::array<uint8_t, kLogLen> log_{};
};

} // namespace core::services
