// 冻结接口（ADR-0002）。实现方不得修改。
// ESP-IDF Bluedroid 经典蓝牙 SPP 多会话服务端（仅 ESP32：SOC_BT_CLASSIC_SUPPORTED）。
// 线程模型：
// - Bluedroid 回调（BTC 任务，C 函数指针、无上下文）经 noexcept 静态蹦床转到唯一实例；回调中只做：
//   拷贝数据进该会话 StreamBuffer、更新拥塞标志、向事件队列非阻塞投递。不解析协议、不阻塞。
// - poll_event()/read()/write()/disconnect() 只允许由同一个链路任务调用（单一所有者）。
// IDF v6.1 已核实事实（reference/c-v1/docs/reports/T04.md）：esp_spp_write 深拷贝数据；
// SRV_OPEN 后服务端继续监听，最多 ESP_SPP_MAX_SESSION(=2) 个会话；对端地址在 srv_open.rem_bda。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "rtk/core/transport.hpp"
#include "rtk/core/types.hpp"
#include "rtk/status.hpp"

namespace rtk::bt {

inline constexpr size_t kRxBufferSize = 2048; // 每会话接收 StreamBuffer
inline constexpr size_t kEventQueueDepth = 16;
inline constexpr size_t kNameMax = 31;                 // 设备名最大字节数（不含结尾 0）
inline constexpr std::string_view kLegacyPin = "1234"; // 与 5.4 注释一致的旧设备 PIN

enum class EventType : uint8_t {
    kOpen = 0, // 新会话建立，addr 有效
    kClose,    // 会话关闭
    kData,     // 该会话 StreamBuffer 有新数据
    kRxDrop,   // 回调中缓冲满，丢弃 dropped 字节
};

struct Event {
    EventType type = EventType::kClose;
    core::SessionHandle handle = 0;
    core::BtAddr addr{};
    uint32_t dropped = 0;
};

class SppServer final : public core::Transport {
public:
    /**
     * 唯一实例。Bluedroid 回调无上下文指针，只能经它转发。实现为
     * `static constinit SppServer s_server;` 函数局部静态：常量初始化，不生成 __cxa_guard（CPP-009）。
     * 驱动状态放在 .cpp 的文件作用域静态存储中（s_ 前缀），不在本类中暴露。
     */
    [[nodiscard]] static SppServer &instance() noexcept;

    /**
     * 初始化控制器（BR/EDR only，释放 BLE 内存）、Bluedroid、GAP（SSP，IO 能力 NONE，
     * CFM_REQ 自动确认，PIN_REQ 回 kLegacyPin）、SPP（ESP_SPP_MODE_CB），在 SPP INIT 事件中启动服务端
     * （ESP_SPP_SEC_NONE，与 5.4 一致）、设置设备名、可连接+可发现。
     * name 为空、超过 kNameMax 或含 '\n'/'\r' → kInvalidArg；重复调用 → kBusy；
     * 任一步失败：按逆序释放已初始化部分并返回映射后的错误。
     */
    [[nodiscard]] Status init(std::string_view name) noexcept;

    /** 等待事件，timeout_ms=0 不等待。有事件返回 true。 */
    [[nodiscard]] bool poll_event(Event &event, uint32_t timeout_ms) noexcept;

    /** 从会话 StreamBuffer 非阻塞读取，返回字节数；未知句柄返回 0。 */
    [[nodiscard]] size_t read(core::SessionHandle handle, std::span<uint8_t> out) noexcept;

    /** 拥塞时等待 CONG 解除，最多 timeout_ms，超时 kTimeout；未知句柄 kNotFound。 */
    [[nodiscard]] Status write(core::SessionHandle handle, std::span<const uint8_t> data,
                               uint32_t timeout_ms) noexcept override;

    [[nodiscard]] Status disconnect(core::SessionHandle handle) noexcept override;

    [[nodiscard]] Status local_address(core::BtAddr &addr) const noexcept;

    /** 事件队列满被丢弃的事件数（诊断）。 */
    [[nodiscard]] uint32_t dropped_events() const noexcept;

private:
    constexpr SppServer() noexcept = default;
};

} // namespace rtk::bt
