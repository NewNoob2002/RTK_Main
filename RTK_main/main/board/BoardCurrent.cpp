// 从组合根读取启动期发布的板级配置。
// 识别完成后供各模块/控制台调用，非 ISR，不阻塞。
#include "board/BoardProfile.hpp"

#include "AppContext.hpp"

namespace app::board {

const Profile &current() noexcept
{
    constexpr static Profile kUnknown{};
    // 识别失败后 console-only 仍可执行诊断命令，此时返回无有效引脚的 unknown，不断言（否则 abort 重启循环）。
    const Profile *profile = appContext().boardProfile;
    return profile != nullptr ? *profile : kUnknown;
}

#if RTK_GNSS_ENABLED
uint8_t gnssReceiverPort() noexcept
{
    const uint8_t port = current().gnss.receiverPort;
    // Kconfig 限制端口号为 1..3；转为 uint8_t 无损。
    return port != 0U ? port : static_cast<uint8_t>(CONFIG_RTK_GNSS_PORT);
}
#endif

} // namespace app::board
