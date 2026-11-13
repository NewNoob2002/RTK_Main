// 异步发送装饰器：把 LinkService 的同步 write 变成“入队即返回”，由独立发送任务调用底层
// Transport::write 并等待 WRITE_EVT。目的：链路任务不再因空口拥塞阻塞最长 200 ms，
// 以免 2 KB 接收缓冲在高速 RTCM 下溢出（J5 报告 §1）。
// 线程模型：write()/disconnect() 只由链路任务调用；pump() 只由发送任务调用；统计为原子量。
// 未 start() 时 write() 直接同步转调底层（主机测试与启动失败的降级路径）。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "core/transport.hpp"
#include "rtos/static_queue.hpp"
#include "rtos/static_task.hpp"
#include "dev_proto_frame.h"

namespace app {

/** 异步发送统计；atomic 字段可跨任务读取。 */
struct AsyncTxStats {
    uint32_t sent = 0;       // 底层 write 成功
    uint32_t failed = 0;     // 底层 write 失败（含会话已关闭）
    uint32_t queue_full = 0; // 入队失败，回复被丢弃
    uint32_t max_depth = 0;  // 观测到的最大排队深度
    uint32_t stack_free = 0; // 发送任务栈剩余字节
};

/** 把 Transport::write 搬进静态发送任务的装饰器；链路任务只入队，发送任务执行写操作。 */
class AsyncTxTransport final : public core::Transport {
public:
    static constexpr size_t kDepth = 8;
    static constexpr size_t kMaxBytes = DEV_PROTO_MAX_FRAME;
    static constexpr size_t kStackBytes = 2048;

    constexpr explicit AsyncTxTransport(core::Transport &inner) noexcept : inner_(inner) {}

    /** 初始化队列并启动发送任务。重复调用返回 kBusy。 */
    [[nodiscard]] fw::Status start(uint32_t priority, BaseType_t core) noexcept;

    /** 已启动：拷贝入队，满则返回 kBusy；未启动：同步转调底层。timeout_ms 用作发送任务里底层 write 的超时。 */
    [[nodiscard]] fw::Status write(core::SessionHandle handle, std::span<const uint8_t> data,
                                   uint32_t timeout_ms) noexcept override;
    /** 直接转调底层；该句柄已排队的回复会在发送时以失败计数。 */
    [[nodiscard]] fw::Status disconnect(core::SessionHandle handle) noexcept override;

    /** 发送任务主体的一步：等待最多 wait_ms 取一条并同步发出。返回是否处理了一条。 */
    bool pump(uint32_t wait_ms) noexcept;

    [[nodiscard]] AsyncTxStats stats() const noexcept;

private:
    struct Item {
        core::SessionHandle handle = 0;
        uint32_t timeout_ms = 0;
        uint16_t size = 0;
        std::array<uint8_t, kMaxBytes> bytes{}; // 队列按值拷贝的定长载荷
    };

    static void task_entry(void *arg) noexcept;

    core::Transport &inner_;
    rtos::StaticQueue<Item, kDepth> queue_{};
    rtos::StaticTask<kStackBytes> task_{};
    Item tx_item_{}; // 只由链路任务（write）使用，避免 ~290 B 压栈
    Item rx_item_{}; // 只由发送任务（pump）使用
    std::atomic<uint32_t> depth_{0};
    std::atomic<uint32_t> sent_{0};
    std::atomic<uint32_t> failed_{0};
    std::atomic<uint32_t> queue_full_{0};
    std::atomic<uint32_t> max_depth_{0};
    bool started_ = false;
};

} // namespace app
