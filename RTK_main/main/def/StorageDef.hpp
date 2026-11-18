// 存储模块跨模块共享类型与命令动词定义（拆自原 AppData.hpp 与 app_ids.hpp）。
// 只含类型与枚举定义，无运行期状态；由存储模块与 config 命令共用，线程/ISR 安全。
#pragma once

#include <cstdint>

#include "AppConfig.hpp" // IWYU pragma: keep（保留模块共享配置入口）
#include "esp_err.h"
#include "fw/Trace.hpp"

namespace app {

/** 存储模块命令动词。 */
enum class StorageVerb : uint8_t { kSaveSn };

/** kStore 追踪事件码（ADR-0009）；结果是 fw::Result 的数值，控制台按表解释。 */
enum class StoreTrace : uint8_t {
    kLoad = 0,     // a=fw::Result 数值，b=成功加载的键数
    kSave = 1,     // a=fw::Result 数值，b=成功写入的键数
    kDebounce = 2, // a=去抖窗口 ms，b=0（登记新的待写请求）
    kRetry = 3,    // a=下一次重试间隔 ms，b=累计失败次数
};

/**
 * @brief 记录一条持久化子事件
 * @param code: 子事件码
 * @param a: 事件第一个数值字段
 * @param b: 事件第二个数值字段
 * @note 启动任务或 kLink 任务调用，线程安全、不分配、不格式化；非 ISR。
 * @note 事件码是 uint8_t 负载；storage 与蓝牙会话无关，session 恒为 0。线程安全：只写 trace 环。
 */
inline void traceStore(StoreTrace code, uint32_t a = 0, uint32_t b = 0) noexcept
{
    fw::trace(fw::TraceCat::kStore, static_cast<uint8_t>(code), 0, a, b);
}

/**
 * @brief 读取命令动词的数值编号
 * @param value: 待转换或写入的值
 * @retval 与枚举底层值相同的 uint8_t 动词
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t verb(StorageVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/** NVS 写入状态快照（白名单去抖与 SN 错误码）。 */
struct StoreStatus {
    bool whitelistPending = false;
    esp_err_t whitelistError = ESP_OK;
    uint32_t failures = 0;
    uint32_t retryMs = 0;
    esp_err_t snError = ESP_OK;
};

} // namespace app
