// GNSS 控制台维护命令与自动保存的合并标志。
// 仅由控制台打包命令，保存标志由 kIo 单任务拥有。
#pragma once

#include <cstdint>

namespace app {

enum class GnssMaintenanceVerb : uint8_t { kSave = 6, kFactoryReset, kBaud };

/** kIo 在接收机忙时合并多次保存请求。 */
class GnssSaveQueue {
public:
    /** @brief 登记一次保存。 @note 仅 kIo。 */
    void request() noexcept
    {
        pending_ = true;
    }

    /** @brief 取走可发送的保存。 @param ready: 控制器可受理保存。 @retval true 应发送一次。 */
    [[nodiscard]] bool take(bool ready) noexcept
    {
        if (!ready || !pending_) {
            return false;
        }
        pending_ = false;
        return true;
    }

    /** @brief 清除尚未发送的保存。 @note 仅 kIo。 */
    void clear() noexcept
    {
        pending_ = false;
    }

private:
    bool pending_ = false;
};

} // namespace app
