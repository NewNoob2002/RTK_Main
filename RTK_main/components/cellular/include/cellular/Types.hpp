// 4G 蜂窝链路公共类型（ADR-0021 §2.3/§2.5）：阶段、SIM/注册状态、恢复级别与发布给 DataCenter 的快照。
// 纯 C++20，无 IDF/FreeRTOS 头；类型均可平凡拷贝、不分配堆、不阻塞，线程安全由持有者（kNet）保证。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace cellular {

/** 链路阶段（ADR-0021 §2.5 状态图；kPowerCycle 为 L3 断电保持阶段）。 */
enum class Phase : uint8_t {
    kOff = 0,
    kPowerCycle,
    kPowerOn,
    kSync,
    kSimCheck,
    kRegister,
    kPdpUp,
    kReady,
};

/** SIM 卡状态（来自 AT+CPIN?）。 */
enum class SimStatus : uint8_t {
    kUnknown = 0,
    kReady,
    kAbsent,      // NOT INSERTED / +CME ERROR 10
    kPinRequired, // SIM PIN / SIM PUK 等需要解锁
    kError,       // 其他异常
};

/** 网络注册状态（来自 AT+CEREG?/AT+CREG? 的 <stat>）。 */
enum class RegStatus : uint8_t {
    kUnknown = 0,
    kNotRegistered,
    kSearching,
    kDenied,
    kHome,
    kRoaming,
};

/** 恢复级别：0 无；1 连接级；2 承载级；3 模块级。 */
enum class RecoveryLevel : uint8_t {
    kNone = 0,
    kL1Conn = 1,
    kL2Bearer = 2,
    kL3Module = 3,
};

/** socket 连接槽数量（EC600U connectID 0–11，本固件只用 0–2）。 */
inline constexpr size_t kConnSlots = 3U;
/** IMEI 文本最大长度（15 位 + 结尾 0）。 */
inline constexpr size_t kImeiBytes = 16U;
/** ICCID 文本最大长度（20 位 + 结尾 0）。 */
inline constexpr size_t kIccidBytes = 21U;
/** CSQ 未知/不可测值。 */
inline constexpr uint8_t kCsqUnknown = 99U;

/** 单个 socket 槽的 L1 状态（N1 无使用者时全为 kIdle）。 */
struct ConnSnapshot final {
    uint8_t state = 0U; // LinkFsm::ConnState 数值：0 空闲 1 待开 2 打开中 3 已打开 4 退避 5 关闭中
    uint8_t fails = 0U; // 连续失败次数
    uint32_t backoffRemainingMs = 0U; // 退避/打开等待剩余毫秒
};

/** 发布给 DataCenter 的 4G 状态快照（≤256 B，ADR-0021 §3）。 */
struct LinkSnapshot final {
    Phase phase = Phase::kOff;
    SimStatus sim = SimStatus::kUnknown;
    RegStatus reg = RegStatus::kUnknown;
    RecoveryLevel level = RecoveryLevel::kNone; // 最近一次触发的恢复级别
    bool pdpUp = false;
    bool channelDead = false;
    uint8_t csq = kCsqUnknown;
    uint16_t recoveryCount = 0U;                 // 自上电以来的恢复动作总次数
    uint32_t backoffRemainingMs = 0U;            // 当前退避/断电保持剩余毫秒
    uint32_t baud = 0U;                          // 当前 UART 波特率
    uint16_t droppedRecv = 0U;                   // 无主槽位 recv URC 丢弃次数
    std::array<ConnSnapshot, kConnSlots> conn{}; // 逐槽 L1 快照
    std::array<char, kImeiBytes> imei{};
    std::array<char, kIccidBytes> iccid{};
};

} // namespace cellular
