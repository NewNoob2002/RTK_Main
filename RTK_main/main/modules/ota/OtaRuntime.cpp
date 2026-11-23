// OTA 运行槽状态、启动确认和 otadata 写入仲裁（ADR-0019）。
// 启动、kLink、httpd、控制台任务调用；共享状态经原子发布，非 ISR。
#include "modules/ota/OtaRuntime.hpp"

#include <atomic>

#include "AppConfig.hpp"
#include "fw/Atomic.hpp"
#include "fw/Log.hpp"
#include "modules/ota/OtaPolicy.hpp"
#include "rtos/EspStatus.hpp"

namespace app::ota {
namespace {

constexpr uint32_t kMsPerSecond = 1000U;
constexpr uint32_t kValidDelayMs = CONFIG_RTK_OTA_VALID_DELAY_S * kMsPerSecond;
static_assert(fw::kAtomicUsable<Operation> && fw::kAtomicUsable<bool> &&
              fw::kAtomicUsable<const char *>);
constinit std::atomic<Operation> s_operation{Operation::kIdle};
constinit std::atomic<const char *> s_last_upload{"none"};
constinit std::atomic<bool> s_confirmation_armed{false};
constinit uint32_t s_boot_ready_ms = 0U; // 启动任务在发布 s_confirmation_armed 前写一次。

} // namespace

std::string_view imageText(std::span<const char> field) noexcept
{
    const std::string_view text{field.data(), field.size()};
    return text.substr(0U, text.find('\0'));
}

esp_ota_img_states_t partitionState(const esp_partition_t *part) noexcept
{
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (part != nullptr && esp_ota_get_state_partition(part, &state) == ESP_OK) {
        return state;
    }
    return ESP_OTA_IMG_UNDEFINED;
}

const char *stateName(esp_ota_img_states_t state) noexcept
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending_verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    default:
        return "undefined";
    }
}

bool beginOperation(Operation op) noexcept
{
    auto expected = Operation::kIdle;
    return s_operation.compare_exchange_strong(expected, op, std::memory_order_acq_rel);
}

void endOperation() noexcept
{
    s_operation.store(Operation::kIdle, std::memory_order_release);
}

bool uploadBusy() noexcept
{
    return s_operation.load(std::memory_order_acquire) == Operation::kUpload;
}

void rememberUpload(const char *result) noexcept
{
    s_last_upload.store(result, std::memory_order_release);
}

const char *lastUpload() noexcept
{
    return s_last_upload.load(std::memory_order_acquire);
}

void bootReady() noexcept
{
    if (partitionState(esp_ota_get_running_partition()) == ESP_OTA_IMG_PENDING_VERIFY) {
        s_boot_ready_ms = rtos::nowMs();
        s_confirmation_armed.store(true, std::memory_order_release);
    }
}

void bootFailed() noexcept
{
    const bool pending =
        partitionState(esp_ota_get_running_partition()) == ESP_OTA_IMG_PENDING_VERIFY;
    if (rollbackAction(pending, BootOutcome::kFailed, 0U, 0U, kValidDelayMs) ==
        RollbackAction::kInvalidate) {
        // API 返回时意味着回滚失败，运行时边界已记录 SDK 错误，继续原有 console-only 路径。
        static_cast<void>(invalidateAndReboot());
    }
}

void pollConfirmation() noexcept
{
    // 必须在 acquire 之后采样，避免 kLink 把发布之前的时间当作毫秒回绕。
    if (!s_confirmation_armed.load(std::memory_order_acquire) ||
        rollbackAction(true, BootOutcome::kReady, s_boot_ready_ms, rtos::nowMs(), kValidDelayMs) !=
            RollbackAction::kConfirm ||
        !beginOperation(Operation::kConfirm)) {
        return;
    }
    // 手动确认可能已解除 pending_verify；不再触发一次冗余的 Flash 操作。
    if (partitionState(esp_ota_get_running_partition()) == ESP_OTA_IMG_PENDING_VERIFY) {
        static_cast<void>(confirm()); // confirm() 记录实际结果。
    }
    s_confirmation_armed.store(false, std::memory_order_release);
    endOperation();
}

fw::Status confirm() noexcept
{
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        FW_LOGI(fw::TraceCat::kSys, "<ota> confirmed running image");
    } else {
        FW_LOGE(fw::TraceCat::kSys, "<ota> confirm running image failed: %s", esp_err_to_name(err));
    }
    return rtos::fromEspErr(err);
}

fw::Status invalidateAndReboot() noexcept
{
    FW_LOGI(fw::TraceCat::kSys, "<ota> invalidate running image and reboot");
    const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    FW_LOGW(fw::TraceCat::kSys, "<ota> rollback failed: %s", esp_err_to_name(err));
    return rtos::fromEspErr(err);
}

} // namespace app::ota

#if defined(ESP_PLATFORM)
namespace app {

std::string_view appVersion() noexcept
{
    // PROJECT_VERSION 由根 CMakeLists.txt 与 PROJECT_VER 同源定义，等于运行镜像描述符中的版本。
    return PROJECT_VERSION;
}

} // namespace app
#endif
