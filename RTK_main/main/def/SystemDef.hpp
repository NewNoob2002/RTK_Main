// 系统模块跨模块共享类型与命令动词定义（拆自原 AppData.hpp 与 app_ids.hpp）。
// 只含类型与枚举定义，无运行期状态；由系统模块与 status/system 命令共用，线程/ISR 安全。
#pragma once

#include <array>
#include <cstdint>

#include "AppConfig.hpp"
#include "NvsStore.h"
#include "rtos/ExecTrace.hpp"

namespace app {

/** kExec 追踪事件码：事件由 rtos 执行器发出，码值定义在 rtos/ExecTrace.hpp（组件依赖方向）。 */
using ExecTrace = rtos::ExecTrace;

// 码值是对外可观测契约：两边不一致会让控制台文本表解释错位，故编译期锁定。
static_assert(static_cast<uint8_t>(ExecTrace::kRoundOverrun) == 0U,
              "kExec 码值与 rtos/ExecTrace.hpp 不一致");
static_assert(static_cast<uint8_t>(ExecTrace::kCallDrop) == 1U,
              "kExec 码值与 rtos/ExecTrace.hpp 不一致");

/** 系统模块命令动词。 */
enum class SystemVerb : uint8_t { kPublish };

/**
 * @brief 读取命令动词的数值编号
 * @param value: 待转换或写入的值
 * @retval 与枚举底层值相同的 uint8_t 动词
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t verb(SystemVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/** 系统状态快照（蓝牙就绪、链路运行、SN/设备名、堆用量）。 */
struct SystemStatus {
    bool btReady = false;
    bool linkRunning = false;
    std::array<char, NVS_STORE_SN_MAX + 1U> sn{};
    std::array<char, kDeviceNameBuf> deviceName{};
    uint32_t freeHeap = 0;
    uint32_t minFreeHeap = 0;
};
/** 系统工作模式；本轮只有 kNormal。 */
enum class Mode : uint8_t { kNormal };
/** 工作模式快照（含生效时刻）。 */
struct SystemMode {
    Mode mode = Mode::kNormal;
    uint32_t sinceMs = 0;
};

} // namespace app
