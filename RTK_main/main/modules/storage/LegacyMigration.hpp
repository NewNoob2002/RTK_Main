// ADR-0022 旧配置的一次性启动迁移。
// 仅启动任务调用，可阻塞，非 ISR。
#pragma once

#include "NvsStore.h"

namespace app::store {

/**
 * @brief 在两个 NVS 实例就绪后迁移第一阶段字段。
 * @param withScratch: 已确认源长度后借用启动缓冲的提供者。
 * @note 在读取用户配置和启动运行期任务前调用；失败仅记录日志，下次启动重试。
 */
void migrateLegacyEeprom(nvs_store_boot_scratch_fn withScratch) noexcept;

} // namespace app::store
