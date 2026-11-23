// OTA 槽查询、启动回滚和 otadata 操作仲裁（ADR-0019）。
// 启动、kLink、httpd、控制台任务调用，非 ISR。
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "esp_ota_ops.h"
#include "fw/Status.hpp"

namespace app::ota {

enum class Operation : uint8_t { kIdle, kUpload, kConfirm, kRollback };

/** @brief 取定长 app 描述符字段。 @param field: 原始字段。 @retval 截至首个 NUL 的视图。 @note 不阻塞。 */
[[nodiscard]] std::string_view imageText(std::span<const char> field) noexcept;

/** @brief 读取槽状态。 @param part: 槽指针，可空。 @retval 槽状态，读不到时为 undefined。 @note 可访问 Flash。 */
[[nodiscard]] esp_ota_img_states_t partitionState(const esp_partition_t *part) noexcept;

/** @brief 取得契约状态名。 @param state: IDF OTA 槽状态。 @retval 静态的小写名称。 @note 不阻塞。 */
[[nodiscard]] const char *stateName(esp_ota_img_states_t state) noexcept;

/** @brief 尝试独占 OTA 状态修改。 @param op: 非空闲操作。 @retval true 已取得。 @retval false 其他操作占用。 @note 不阻塞。 */
[[nodiscard]] bool beginOperation(Operation op) noexcept;

/** @brief 释放 OTA 状态修改权。 @note 仅当前所有者调用，不阻塞。 */
void endOperation() noexcept;

/** @brief 查询是否正在上传或已成功待重启。 @retval true 上传门关闭。 @retval false 无上传。 @note 不阻塞。 */
[[nodiscard]] bool uploadBusy() noexcept;

/** @brief 保存最近上传结果。 @param result: 生命周期覆盖本次启动的静态字符串。 @note httpd 调用，不阻塞。 */
void rememberUpload(const char *result) noexcept;

/** @brief 查询最近上传结果。 @retval 静态结果字符串，无上传时为 none。 @note 不阻塞。 */
[[nodiscard]] const char *lastUpload() noexcept;

/** @brief 启动成功后发布待确认状态及计时起点。 @note 仅启动任务调用，可访问 Flash。 */
void bootReady() noexcept;

/** @brief 对启动失败的待确认镜像尝试立即回滚。 @note 仅启动任务调用，可重启。 */
void bootFailed() noexcept;

/** @brief 检查确认延时，达标时执行一次确认。 @note 仅 kLink 调用，确认时访问 Flash。 */
void pollConfirmation() noexcept;

/** @brief 标记当前运行镜像有效。 @retval kOk 确认成功。 @retval 其他 IDF 失败映射。 @note 调用者须独占 OTA 操作，可访问 Flash。 */
[[nodiscard]] fw::Status confirm() noexcept;

/** @brief 标记当前运行镜像无效并重启。 @retval 失败时返回 IDF 状态映射；成功会重启。 @note 调用者须独占 OTA 操作或尚未启动业务。 */
[[nodiscard]] fw::Status invalidateAndReboot() noexcept;

} // namespace app::ota
