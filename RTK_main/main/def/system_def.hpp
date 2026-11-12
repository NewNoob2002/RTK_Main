// 系统模块跨模块共享类型与命令动词定义（拆自原 app_data.hpp 与 app_ids.hpp）。
#pragma once

#include <array>
#include <cstdint>

#include "app_config.hpp"
#include "rtk_store.h"

namespace rtk::app {

enum class SystemVerb : uint8_t { kPublish };

// 底层类型均为 uint8_t；这些编号转换不会收窄。线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t verb(SystemVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

struct SystemStatus {
    bool bt_ready = false;
    bool link_running = false;
    std::array<char, RTK_STORE_SN_MAX + 1U> sn{};
    std::array<char, kDeviceNameBuf> device_name{};
    uint32_t free_heap = 0;
    uint32_t min_free_heap = 0;
};
enum class Mode : uint8_t { kNormal };
struct SystemMode {
    Mode mode = Mode::kNormal;
    uint32_t since_ms = 0;
};

} // namespace rtk::app
