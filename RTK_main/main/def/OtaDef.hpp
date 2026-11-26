// Web OTA 结果以及由 httpd 发布、OLED 只读的进度快照。
// 纯值类型，无 IDF 依赖，不分配堆。
#pragma once

#include <cstdint>

namespace app::web_ota {

enum class UploadCode : uint8_t {
    kOk,
    kBusy,
    kState,
    kLength,
    kTooLarge,
    kBadImage,
    kValidate,
    kTimeout,
    kFlash,
    kAborted,
};

enum class OtaPhase : uint8_t { kIdle, kReceiving, kVerifying, kSuccess, kFailed };

struct OtaProgress {
    static constexpr uint32_t kFailureVisibleMs = 3000U;

    uint32_t receivedBytes = 0U;
    uint32_t totalBytes = 0U;
    uint32_t changedMs = 0U;
    OtaPhase phase = OtaPhase::kIdle;
    UploadCode code = UploadCode::kOk;

    /** @brief 计算已收比例。 @retval 0–100，总长度未知时为 0。 @note 纯函数，可跨任务调用。 */
    [[nodiscard]] constexpr uint32_t percent() const noexcept
    {
        constexpr uint32_t kFullPercent = 100U;
        if (totalBytes == 0U || receivedBytes >= totalBytes) {
            return totalBytes == 0U ? 0U : kFullPercent;
        }
        // receivedBytes < totalBytes，比例小于 100，可收窄。
        return static_cast<uint32_t>(receivedBytes * uint64_t{kFullPercent} / totalBytes);
    }
};

} // namespace app::web_ota
