// 通用持久化键的 NVS 读写（ADR-0010 §2.10）：按 fw::StoreKey 的类型读写一个整数值。
// 线程：NVS API 自带锁，可在任何任务调用（非 ISR）；StorageModule::load_keys/save_keys 与 config 命令共用。
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "fw/store_key.hpp"

namespace app::store {

/** 键的值宽度（字节）：u8/u16/u32 → 1/2/4；本轮不支持的类型返回 0。 */
[[nodiscard]] size_t value_width(fw::StoreType type) noexcept;

/** 读一个整数键。命名空间或键不存在 → ESP_ERR_NVS_NOT_FOUND；类型不支持/名字非法 → ESP_ERR_INVALID_ARG。 */
[[nodiscard]] esp_err_t read_value(const fw::StoreKey &key, uint32_t &value) noexcept;

/** 写一个整数键并提交；value 超出该键宽度的高位被忽略。错误同 read_value，另含后端写错误。 */
[[nodiscard]] esp_err_t write_value(const fw::StoreKey &key, uint32_t value) noexcept;

} // namespace app::store
