// 4G 运行态块（ADR-0021 §2.1 / N1 裁决 #11）：执行器、控制存储与 NetModule 运行态合为一块，
// 仅在“板型有 4G、net.enable=1、非配网”时由启动期 boot_alloc 一次性分配；其余情况零静态 RAM。
#pragma once

#include <cstddef>
#include <bit>
#include <cstdint>
#include <span>
#include <new>

#include "AppContext.hpp"
#include "modules/net/NetModule.hpp"

namespace app {

/** kNet 执行器命令队列深度：该执行器只挂 NetModule 一个模块，控制台命令串行投递，4 足够。 */
inline constexpr size_t kNetQueueDepth = 4U;

/** 启动期 boot_alloc 外供的 4G 运行态块；构造见 constructNetBlock。 */
struct NetBlock {
    rtos::Executor executor{};
    ExecutorControlStorage<kNetQueueDepth> storage{};
    NetModule::Runtime runtime{};
};

/**
 * @brief 在外供内存上构造 NetBlock
 * @param mem: 启动期分配的内存，须不小于 sizeof(NetBlock) 且按 alignof(NetBlock) 对齐
 * @retval 构造好的块；内存不足或未对齐返回 nullptr
 * @note 仅启动任务单线程调用；不分配堆，非 ISR。
 */
[[nodiscard]] inline NetBlock *constructNetBlock(std::span<uint8_t> mem) noexcept
{
    if (mem.size() < sizeof(NetBlock) ||
        (std::bit_cast<std::uintptr_t>(mem.data()) % alignof(NetBlock)) != 0U) {
        return nullptr;
    }
    return ::new (static_cast<void *>(mem.data())) NetBlock{};
}

} // namespace app
