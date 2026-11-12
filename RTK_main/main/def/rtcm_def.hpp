// RTCM 模块跨模块共享类型与命令动词定义（拆自原 app_data.hpp 与 app_ids.hpp）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "app_config.hpp"

namespace app {

/** RTCM 模块命令动词。 */
enum class RtcmVerb : uint8_t { kReset };

// 底层类型均为 uint8_t；这些编号转换不会收窄。线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t verb(RtcmVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/** 分流器统计快照，只保留要展示的三项计数。 */
struct DemuxStats {
    uint32_t unknown_bytes = 0;
    uint32_t product_rejects = 0;
    uint32_t rtcm_rejects = 0;
};

// 仅 kLink 内部传递完整 RTCM 批次。view 指向 bt_link 的静态存储，
// 在 rtcm.poll 消费后、下一轮 wait 前才复用；其他任务只读取 kRtcmStats。
// 每会话遗留数据最多 1029 B，新输入每轮最多 256 B，故 2×1029+256 B 足够。
inline constexpr size_t kRtcmBatchBytes = kMaxSessions * 1029U + 256U;
/** 一轮交付给 RTCM 模块的批次视图；wire 指向 bt_link 的静态存储，仅 kLink 内部使用。 */
struct RtcmBatch {
    std::span<const uint8_t> wire{};
    uint32_t rejected = 0;
};

} // namespace app
