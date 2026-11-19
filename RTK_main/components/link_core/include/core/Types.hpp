// link_core 基础值类型：会话句柄、容量上限、蓝牙地址、会话角色与并发策略（纯数据，无 IDF 依赖）。
// 线程约束：只含类型与常量定义，无运行期状态；对象的跨任务共享由持有方负责。
// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace core {

/** 传输层会话句柄；由 Transport 分配，只在链路任务内有效。 */
using SessionHandle = uint32_t;

inline constexpr size_t kMaxSessions = 2; // 与 BR/EDR ACL 上限一致（CONFIG_BT_ACL_CONNECTIONS=2）
inline constexpr size_t kMaxAlarms = 4;   // 报警器白名单容量
inline constexpr size_t kAddrLen = 6;

/** 6 字节对端地址；可平凡拷贝，可比较。 */
struct BtAddr {
    std::array<uint8_t, kAddrLen> bytes{};
    friend constexpr bool operator==(const BtAddr &, const BtAddr &) = default;
};

/** 会话角色：未定 / 手机 / 报警器 / 待定（等待 HELLO）。 */
enum class Role : uint8_t {
    kNone = 0, // 句柄不存在 / 被拒绝
    kPhone,
    kAlarm,
    kPending, // 已有手机时新接入的未知设备，等待 HELLO
};

/** 会话并发策略：并发在线 / 手机优先抢占。 */
enum class Policy : uint8_t {
    kConcurrent = 0, // 手机与报警器可同时在线
    kPhonePreempt,   // 手机接入时踢掉报警器；手机在线时拒绝报警器
};

/** 手机端 App 类型：按帧头 sender 字段区分（不改变会话 Role 判定）。 */
enum class AppKind : uint8_t {
    kNav = 0, // sender == 0x00（Nav / Configure App）
    kSurvey,  // 其它非报警器 sender（Survey App）
};

/** @brief 从帧头 sender 字段推导手机 App 类型。
 *  @param sender 请求帧头 sender 字节。
 *  @retval AppKind::kNav sender == 0x00（Nav/Configure App）。
 *  @retval AppKind::kSurvey 其它 sender 字节（Survey App）。
 *  @note 纯函数，线程/ISR 安全；仅在 Role::kPhone 会话中用于区分应答格式。 */
[[nodiscard]] constexpr AppKind appKindFromSender(uint8_t sender) noexcept
{
    return (sender == 0x00U) ? AppKind::kNav : AppKind::kSurvey;
}

} // namespace core
