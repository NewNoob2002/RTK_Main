// 存储模块跨模块共享类型与命令动词定义（拆自原 app_data.hpp 与 app_ids.hpp）。
#pragma once

#include <cstdint>

#include "app_config.hpp" // IWYU pragma: keep（保证冻结头中的 ::app 命名空间先于 rtk::app 解析）
#include "esp_err.h"

namespace rtk::app {

/** 存储模块命令动词。 */
enum class StorageVerb : uint8_t { kSaveSn };

// 底层类型均为 uint8_t；这些编号转换不会收窄。线程/ISR 安全：纯函数。
[[nodiscard]] constexpr uint8_t verb(StorageVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/** NVS 写入状态快照（白名单去抖与 SN 错误码）。 */
struct StoreStatus {
    bool whitelist_pending = false;
    esp_err_t whitelist_error = ESP_OK;
    uint32_t failures = 0;
    uint32_t retry_ms = 0;
    esp_err_t sn_error = ESP_OK;
};

} // namespace rtk::app
