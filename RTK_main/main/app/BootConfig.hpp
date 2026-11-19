// 启动早期配置与失败计数，启动任务写入，放行后供模块和控制台读取。
#pragma once

#include <cstdint>

namespace app {

struct AppContext;

/** 启动加载结果，零初始化便于 AppContext 放入 .bss。 */
struct StoreLoadCounts {
    uint32_t saved = 0U;
    uint32_t defaults = 0U;
    uint32_t failed = 0U;
};

/** 早期配置快照，启动结束后只读。 */
struct BootConfig {
    uint32_t traceMask = 0U;
    uint32_t hexMask = 0U;
    uint32_t logLevels = 0U;
    uint32_t gnssBaud = 0U; // 0 代表未配置用户波特率，启动后切到 RTK_GNSS_TARGET_BAUD。
    StoreLoadCounts debugCounts{};
    StoreLoadCounts baudCounts{};
    uint16_t reportSeconds = 0U;
    uint8_t previousFailures = 0U;
    bool safeDefaults = false;
    bool userNvsReady = false;
};

/** @brief 初始化 NVS、记录本次启动并应用早期配置。 @param app: 当前应用上下文。 @note 仅启动任务，可阻塞，非 ISR。 */
void loadEarlyConfig(AppContext &app) noexcept;

/** @brief 记录消费者已放行，清除连续失败次数。 @param config: 当前启动快照。 @note 仅启动任务，可阻塞，非 ISR。 */
void clearBootFailures(const BootConfig &config) noexcept;

} // namespace app
