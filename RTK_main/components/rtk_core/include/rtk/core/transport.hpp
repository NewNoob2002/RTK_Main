// 会话传输抽象：SPP 驱动实现它；将来 BLE/UART/ESP-NOW 通道可复用 LinkService。
// 运行期选择的边界，按 CPP-011 使用受保护非虚析构 + final 实现。
// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <cstdint>
#include <span>

#include "rtk/core/types.hpp"
#include "rtk/status.hpp"

namespace rtk::core {

/** 逻辑会话的字节传输抽象；线程约束由实现方声明，调用方是链路任务。 */
class Transport {
public:
    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;

    /** 发送；拥塞时最多等待 timeout_ms。线程安全：只允许链路任务调用。 */
    [[nodiscard]] virtual Status write(SessionHandle handle, std::span<const uint8_t> data,
                                       uint32_t timeout_ms) noexcept = 0;

    /** 请求断开；结果以关闭事件异步返回。线程安全：只允许链路任务调用。 */
    [[nodiscard]] virtual Status disconnect(SessionHandle handle) noexcept = 0;

protected:
    Transport() = default;
    ~Transport() = default;
};

} // namespace rtk::core
