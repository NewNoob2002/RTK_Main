// OTA 通用结果与进度快照，由当前升级入口发布给 OLED。
// 纯值类型，无 IDF 依赖，不分配堆。
#pragma once

#include <cstdint>

namespace app::ota {

enum class ResultCode : uint8_t {
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

enum class Source : uint8_t { kNone, kWeb, kCellular };

struct Progress {
    static constexpr uint32_t kFailureVisibleMs = 3000U;

    uint32_t receivedBytes = 0U;
    uint32_t totalBytes = 0U;
    uint32_t changedMs = 0U;
    OtaPhase phase = OtaPhase::kIdle;
    ResultCode code = ResultCode::kOk;
    Source source = Source::kNone;

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

} // namespace app::ota

namespace app::web_ota {
using UploadCode = ota::ResultCode;
} // namespace app::web_ota
