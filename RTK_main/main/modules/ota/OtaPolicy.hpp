// OTA 启动回滚判定（ADR-0019 §2.5），不依赖 IDF。
// 纯函数，任意任务或 ISR 可调用。
#pragma once

#include <cstdint>

namespace app::ota {

enum class BootOutcome : uint8_t { kFailed, kReady };
enum class RollbackAction : uint8_t { kNone, kConfirm, kInvalidate };

/**
 * @brief 判定运行镜像的回滚动作
 * @param pendingVerify: 运行槽是否处于 pending_verify，其他状态不需处理
 * @param outcome: 启动是否正常完成
 * @param readyMs: 完成启动的单调时钟（ms）
 * @param nowMs: 当前单调时钟（ms）
 * @param delayMs: 确认延时（ms），小于时钟周期
 * @retval kNone 非待确认镜像或尚未达到延时
 * @retval kConfirm 待确认镜像正常运行达到延时
 * @retval kInvalidate 待确认镜像启动失败
 * @note 使用无符号差值，允许单调时钟在延时窗口内回绕。
 */
[[nodiscard]] constexpr RollbackAction rollbackAction(bool pendingVerify, BootOutcome outcome,
                                                      uint32_t readyMs, uint32_t nowMs,
                                                      uint32_t delayMs) noexcept
{
    if (!pendingVerify) {
        return RollbackAction::kNone;
    }
    if (outcome == BootOutcome::kFailed) {
        return RollbackAction::kInvalidate;
    }
    return nowMs - readyMs >= delayMs ? RollbackAction::kConfirm : RollbackAction::kNone;
}

/**
 * @brief 判定另一应用槽的镜像版本是否可信、可对外报告
 * @param stateDefined: 槽状态查询成功且非 ESP_OTA_IMG_UNDEFINED
 * @param uploadDirty: 本次启动期间是否已向该槽开始写入且尚未成功提交
 * @retval true 可读取并对外报告该槽版本
 * @retval false 须抑制版本显示（HTTP 返回 null，控制台显示 "-"）
 * @note 纯函数，任意任务或 ISR 可调用。
 */
[[nodiscard]] constexpr bool otherSlotVersionReadable(bool stateDefined, bool uploadDirty) noexcept
{
    return stateDefined && !uploadDirty;
}

} // namespace app::ota
