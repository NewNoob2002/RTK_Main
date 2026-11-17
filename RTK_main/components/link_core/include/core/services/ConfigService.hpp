// 配置类指令（PHONE）：0x07 工作模式、0x14 电台、0x15 日志。持有三块配置的唯一副本。
// 行为与 CommandDispatcher 对应分支逐字节一致。以后经 StoreKey 持久化时通过下面的只读视图与 load_*
// 接入（本轮不持久化）。 线程安全：否（链路任务独占）。 冻结接口（ADR-0007 §3.3）。实现方不得修改。
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
    /** 三块配置清零，radio[kRadioModeOffset] = kRadioModeDefault（与旧 dispatcher 相同）。 */
    ConfigService() noexcept;
    ConfigService(const ConfigService &) = delete;
    ConfigService &operator=(const ConfigService &) = delete;
    ConfigService(ConfigService &&) = delete;
    ConfigService &operator=(ConfigService &&) = delete;
    ~ConfigService() = default;

    [[nodiscard]] std::span<const uint8_t, kWorkModeLen> workMode() const noexcept;
    [[nodiscard]] std::span<const uint8_t, kRadioLen> radio() const noexcept;
    [[nodiscard]] std::span<const uint8_t, kLogLen> log() const noexcept;

    [[nodiscard]] static std::span<const Route> routes() noexcept;
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
