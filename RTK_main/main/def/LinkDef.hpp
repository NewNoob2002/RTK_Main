// 链路模块跨模块共享类型与命令动词定义（拆自原 AppData.hpp 与 app_ids.hpp）。
// 只含类型与枚举定义，无运行期状态；由链路模块与其命令共用，线程/ISR 安全。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "AppConfig.hpp"
#include "core/LinkService.hpp"

namespace app {

/** 链路模块命令动词（由 fw::Command 的 arg 携带）。 */
/** kWhitelistAdd/kWhitelistDel 的 args 为 6 字节 MAC（core::kAddrLen）。 */
enum class LinkVerb : uint8_t {
    kAlarmSet,
    kAlarmClear,
    kPair,
    kWhitelistClear,
    kWhitelistAdd,
    kWhitelistDel,
    kPairClose
};

/**
 * @brief 读取命令动词的数值编号
 * @param value: 待转换或写入的值
 * @retval 与枚举底层值相同的 uint8_t 动词
 * @note 纯函数，线程/ISR 安全。
 */
[[nodiscard]] constexpr uint8_t verb(LinkVerb value) noexcept
{
    return static_cast<uint8_t>(value);
}

/** 单个会话的快照，供 status 展示；纯数据。 */
struct SessionSnapshot {
    bool used = false;
    core::SessionHandle handle = 0;
    core::Role role = core::Role::kNone;
    core::BtAddr addr{};
};
/** 链路策略、配对状态与白名单的快照；经 DataCenter 共享。 */
struct LinkStatus {
    core::Policy policy = policyFromConfig();
    bool pairing = false;
    size_t whitelistCount = 0;
    std::array<core::BtAddr, kMaxAlarms> whitelist{};
    uint32_t snapshotMs = 0;
};
/** 当前会话表的快照。 */
struct Sessions {
    size_t count = 0;
    std::array<SessionSnapshot, kMaxSessions> entries{};
};
/** 报警状态快照（测试覆盖优先）。 */
struct AlarmState {
    uint8_t state = 0;
    uint8_t level = 0;
    bool override = false;
};
/** 最近一次收到的心跳；present=false 表示尚无。 */
struct Heartbeat {
    bool present = false;
    proto::alarm::Heartbeat value{};
};
/** 链路服务统计 + 驱动异步字节发送统计；纯数据快照。 */
struct LinkStats : core::LinkStats {
    uint32_t parserCrcErrors = 0;
    uint32_t parserLengthErrors = 0;
    uint32_t txAccepted = 0;  // 字节，接纳到驱动环
    uint32_t txCompleted = 0; // 字节，真实 WRITE_EVT 成功
    uint32_t txCancelled = 0; // 字节，关闭/取消
    uint32_t txErrors = 0;
    uint32_t txCong = 0;
    uint32_t txHighWater = 0; // 字节
    uint32_t txPaused = 0;
    uint32_t txAnonWriteFail = 0;
    uint32_t txStrayWrite = 0;
    uint32_t txInflight = 0;
    uint32_t evtDropped = 0;
    uint32_t rxWakeCalls = 0;
    std::array<uint32_t, kMaxSessions> rxUsed{};
    std::array<uint32_t, kMaxSessions> rxHighWater{};
    std::array<uint32_t, kMaxSessions> rxDroppedBytes{};
    std::array<uint32_t, kMaxSessions> rxEvents{};
    std::array<uint32_t, kMaxSessions> rxDropEvents{};
};

} // namespace app
