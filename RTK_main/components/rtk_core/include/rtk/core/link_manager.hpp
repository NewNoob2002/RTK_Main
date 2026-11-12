// SPP 会话角色与策略管理（纯逻辑，无 IDF 依赖）。不调用传输层，只通过 Actions 返回要断开的句柄。
// 时间以 now_ms 传入，所有时间比较用无符号差值以容忍 32 位回绕。线程安全：否（链路任务独占）。
// 冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rtk/core/types.hpp"

namespace rtk::core {

inline constexpr uint32_t kAlarmRxTimeoutMs = 3000; // ALARM 会话无有效帧即断开（> 阈值才断开）
inline constexpr uint32_t kPendingTimeoutMs = 5000; // PENDING 等待 HELLO 的时限（> 阈值才断开）
inline constexpr uint32_t kPairWindowMs = 60000;    // 配对窗口（>= 阈值关闭）
inline constexpr uint16_t kHelloMsgId = 0x0041;
inline constexpr uint8_t kHelloSender = 0x03;

// 一次调用要求调用方断开的句柄（按产生顺序）。每个修改型调用开始时先清空。
/** 一次状态转移要求传输层执行的动作（待断开句柄）；由调用方逐条执行，自身不持有传输层。 */
struct Actions {
    std::array<SessionHandle, kMaxSessions + 1> handles{};
    uint8_t count = 0;

    void clear() noexcept
    {
        count = 0;
    }
    [[nodiscard]] std::span<const SessionHandle> view() const noexcept
    {
        return {handles.data(), count};
    }
};

/** 会话角色、策略、配对窗口与白名单的状态机（纯逻辑）；仅链路任务使用，时间由参数传入。 */
class LinkManager {
public:
    constexpr LinkManager() noexcept = default;

    /** 清空会话表与白名单后载入 alarms（超出 kMaxAlarms 的部分忽略）。 */
    void init(Policy policy, std::span<const BtAddr> alarms) noexcept;

    /**
     * 新连接。返回分配的角色；kNone 表示拒绝，且 act 中包含该 handle。规则（按顺序）：
     * 1. handle 已存在：拒绝。
     * 2. 会话表已满：拒绝。
     * 3. 地址在白名单：kPhonePreempt 且已有 PHONE 则拒绝；否则 ALARM。
     * 4. 地址不在白名单：
     *    - 无 PHONE：PHONE；kPhonePreempt 时把所有 ALARM 句柄放入 act 并立即从表中移除；
     *    - 已有 PHONE 且配对窗口打开：PENDING；
     *    - 否则拒绝。
     * 新会话的 since/last_rx 都置为 now_ms。
     */
    [[nodiscard]] Role on_open(SessionHandle handle, const BtAddr &addr, uint32_t now_ms, Actions &act) noexcept;

    /** 连接关闭（任何一方发起）。未知句柄忽略。 */
    void on_close(SessionHandle handle) noexcept;

    /**
     * 某会话收到一帧 CRC 正确的帧。返回处理后的角色。
     * - 刷新 last_rx。
     * - msg_id == kHelloMsgId 且 sender == kHelloSender：
     *   ALARM 不变；PHONE/PENDING 且配对窗口打开：改为 ALARM，地址加入白名单（已存在不重复），
     *   白名单已满则断开；PHONE/PENDING 且窗口未打开：断开（未授权报警器）。
     * - 其他帧来自 PENDING：断开（不允许第二部手机）。
     * - 断开时：句柄放入 act，会话立即移除，返回 kNone。未知句柄返回 kNone 且 act 为空。
     */
    [[nodiscard]] Role on_frame(SessionHandle handle, uint8_t sender, uint16_t msg_id, uint32_t now_ms,
                                Actions &act) noexcept;

    /**
     * 周期调用（建议 100 ms）。ALARM 会话 now-last_rx > kAlarmRxTimeoutMs 断开；
     * PENDING 会话 now-since > kPendingTimeoutMs 断开；配对窗口 now-start >= kPairWindowMs 关闭。
     * 被断开的会话立即移除。
     */
    void tick(uint32_t now_ms, Actions &act) noexcept;

    [[nodiscard]] Role role(SessionHandle handle) const noexcept;
    /** 把指定角色的句柄写入 out，返回写入数量（≤ out.size()）。 */
    [[nodiscard]] size_t handles(Role role, std::span<SessionHandle> out) const noexcept;
    [[nodiscard]] bool has_role(Role role) const noexcept;

    void pair_open(uint32_t now_ms) noexcept; // 重复调用重新计时
    [[nodiscard]] bool is_pairing() const noexcept;

    void whitelist_clear() noexcept; // 不影响已连接会话的角色
    [[nodiscard]] std::span<const BtAddr> whitelist() const noexcept;

    [[nodiscard]] Policy policy() const noexcept;

private:
    struct Session {
        bool in_use = false;
        SessionHandle handle = 0;
        Role role = Role::kNone;
        BtAddr addr{};
        uint32_t since_ms = 0;
        uint32_t last_rx_ms = 0;
    };

    std::array<Session, kMaxSessions> sessions_{};
    std::array<BtAddr, kMaxAlarms> alarms_{};
    size_t alarm_count_ = 0;
    Policy policy_ = Policy::kConcurrent;
    bool pairing_ = false;
    uint32_t pair_start_ms_ = 0;
};

} // namespace rtk::core
