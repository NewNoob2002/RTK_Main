// 原子量在当前目标上的可用性判断，供 CPP-008 的 static_assert 使用（B7，esp32c3 回归修复）。
// 纯编译期常量，无状态、无分配；任意上下文可用。
#pragma once

#include <atomic>

namespace fw {

/**
 * 目标是否有原生原子指令。RISC-V 无 A 扩展（esp32c3 为 RV32IMC）时为 false：
 * 此时 IDF 用关中断模拟 __atomic_*，不是无锁实现，但单核下对任务和 ISR 都安全（RV-001）。
 */
inline constexpr bool kNativeAtomics =
#if defined(__riscv) && !defined(__riscv_atomic)
    false;
#else
    true;
#endif

/**
 * std::atomic<T> 在当前目标上是否可用于实时路径：有原生原子指令时必须无锁
 * （否则退回 libatomic 的锁实现），无原生原子指令时接受 IDF 的关中断模拟。
 */
template <typename T> inline constexpr bool kAtomicUsable = !kNativeAtomics || std::atomic<T>::is_always_lock_free;

} // namespace fw
