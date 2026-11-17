// SPP 会话角色与策略管理：准入规则 1–4、超时、配对窗口与白名单；纯逻辑，不调用传输层。
// 线程安全：否，链路任务独占调用、非 ISR；时间由参数传入，比较用无符号差值以容忍 32 位回绕。
// 堆：不分配（全为定长数组）。冻结接口（ADR-0002）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/Types.hpp"

namespace core {

inline constexpr uint32_t kAlarmRxTimeoutMs = 3000; // ALARM 会话无有效帧即断开（> 阈值才断开）
inline constexpr uint32_t kPendingTimeoutMs = 5000; // PENDING 等待 HELLO 的时限（> 阈值才断开）
inline constexpr uint32_t kPairWindowMs = 60000;    // 配对窗口（>= 阈值关闭）
inline constexpr uint16_t kHelloMsgId = 0x0041;
inline constexpr uint8_t kHelloSender = 0x03;

/** 单条白名单编辑结果。 */
enum class WhitelistEdit : uint8_t {
    kDone = 0,  // 已加入 / 已移除
    kUnchanged, // add：地址已存在；remove：地址不在白名单
    kFull,      // add：白名单已满（kMaxAlarms）
};

/** 一次状态转移要求传输层执行的动作（待断开句柄，按产生顺序排列）；调用方逐条执行，本类不持有传输层。 */
struct Actions {
    std::array<SessionHandle, kMaxSessions + 1> handles{};
    uint8_t count = 0;

    /** @brief 清空待断开句柄列表。
     *  @note 每个修改型调用（onOpen/onFrame/tick）开始时由实现清空，不分配、不阻塞。 */
    void clear() noexcept
    {
        count = 0;
    }
    /** @brief 待断开句柄的只读视图。
     *  @return 指向 handles 前 count 条的视图；无动作时为空视图。 */
    [[nodiscard]] std::span<const SessionHandle> view() const noexcept
    {
        return {handles.data(), count};
    }
};

/** 会话角色、策略、配对窗口与白名单的状态机（纯逻辑）；仅链路任务使用，时间由参数传入。 */
class LinkManager {
public:
    constexpr LinkManager() noexcept = default;

    /** @brief 清空会话表与白名单，按 policy 载入 alarms；启动期一次性调用（参数与约束见实现处文档）。 */
    void init(Policy policy, std::span<const BtAddr> alarms) noexcept;

    /**
     * @brief 连接建立：按准入规则决定角色，并把要求断开的句柄写进 act。
     * @param handle 新连接的会话句柄。
     * @param addr 对端蓝牙地址。
     * @param nowMs 当前时间（ms）。
     * @param[out] act 先清空，再写入需要调用方断开的句柄：拒绝时是本 handle，手机优先策略抢占时是被踢的 ALARM。
     * @retval Role::kNone 拒绝该连接（act 中含 handle，调用方负责断开）。
     * @retval Role::kAlarm 白名单内的报警器会话。
     * @retval Role::kPhone 非白名单且当前无手机，作为手机接入。
     * @retval Role::kPending 已有手机且配对窗口打开，等待 HELLO。
     * @note 规则（按顺序）：
     * 1. handle 已存在：拒绝。
     * 2. 会话表已满：拒绝。
     * 3. 地址在白名单：kPhonePreempt 且已有 PHONE 则拒绝；否则 ALARM。
     * 4. 地址不在白名单：
     *    - 无 PHONE：PHONE；kPhonePreempt 时把所有 ALARM 句柄放入 act 并立即从表中移除；
     *    - 已有 PHONE 且配对窗口打开：PENDING；
     *    - 否则拒绝。
     * 新会话的 since/last_rx 都置为 nowMs；不分配、不阻塞。
     */
    [[nodiscard]] Role onOpen(SessionHandle handle, const BtAddr &addr, uint32_t nowMs,
                              Actions &act) noexcept;

    /** @brief 会话关闭：释放槽位并记录追踪；未知句柄忽略。
     *  @param handle 已关闭的会话句柄。
     *  @note 不调用传输层、不分配、不阻塞。 */
    void onClose(SessionHandle handle) noexcept;

    /**
     * @brief 某会话收到一帧 CRC 正确的产品帧：刷新 last_rx，并按 HELLO/角色规则转移状态。
     * @param handle 来源会话句柄。
     * @param sender 帧头 sender 字段。
     * @param msgId 产品帧 msgId。
     * @param nowMs 当前时间（ms）。
     * @param[out] act 先清空，再写入需要调用方断开的句柄。
     * @retval Role::kNone 会话未知，或本次已决定断开（act 中给出句柄）。
     * @retval Role::kAlarm 报警器角色，放行该帧；HELLO 可把 PENDING 提升为 ALARM。
     * @retval Role::kPhone 手机角色，放行该帧。
     * @note 判定：
     * - msgId == kHelloMsgId 且 sender == kHelloSender：
     *   ALARM 不变；PHONE/PENDING 且配对窗口打开：改为 ALARM，地址加入白名单（已存在不重复），
     *   白名单已满则断开；PHONE/PENDING 且窗口未打开：断开（未授权报警器）。
     * - 其他帧来自 PENDING：断开（不允许第二部手机）。
     * - 断开时句柄放入 act、会话立即移除；未知句柄返回 kNone 且 act 为空。
     */
    [[nodiscard]] Role onFrame(SessionHandle handle, uint8_t sender, uint16_t msgId, uint32_t nowMs,
                               Actions &act) noexcept;

    /**
     * @brief 周期超时检查：报警器静默、待定会话超时、配对窗口到期。
     * @param nowMs 当前时间（ms）。
     * @param[out] act 先清空，再写入需要调用方断开的句柄。
     * @note 建议每 100 ms 调用一次；ALARM 超过 kAlarmRxTimeoutMs、PENDING 超过 kPendingTimeoutMs 断开，
     * 配对窗口达到 kPairWindowMs 关闭；PHONE 不因接收超时断开。被断开的会话立即移除。
     */
    void tick(uint32_t nowMs, Actions &act) noexcept;

    /** @brief 查会话当前角色。
     *  @param handle 会话句柄。
     *  @return 角色；未知句柄返回 Role::kNone。 */
    [[nodiscard]] Role role(SessionHandle handle) const noexcept;
    /** @brief 收集指定角色的全部在线句柄。
     *  @param role 目标角色。
     *  @param[out] out 写入缓冲；容量不足时只写前 out.size() 条，其余不写。
     *  @return 实际写入数量（≤ out.size()）。 */
    [[nodiscard]] size_t handles(Role role, std::span<SessionHandle> out) const noexcept;
    /** @brief 是否存在该角色的在线会话。
     *  @param role 目标角色。
     *  @return true 至少一个；false 没有。 */
    [[nodiscard]] bool hasRole(Role role) const noexcept;

    /** @brief 打开配对窗口（重复调用重新计时）。
     *  @param nowMs 当前时间（ms）。
     *  @note 窗口在 tick() 中按 kPairWindowMs 关闭；不分配、不阻塞。 */
    void pairOpen(uint32_t nowMs) noexcept;
    /** @brief 配对窗口是否仍打开。
     *  @return true 打开中（尚未被 tick 关闭）。 */
    [[nodiscard]] bool isPairing() const noexcept;

    /** @brief 清空内存中的白名单。
     *  @note 不影响已连接会话的角色；持久化由 app 层负责，本方法只改内存。 */
    void whitelistClear() noexcept;
    /** @brief 手动加入一条报警器地址（追加到末尾）。
     *  @param addr 待加入地址。
     *  @retval WhitelistEdit::kDone 已加入。
     *  @retval WhitelistEdit::kUnchanged 地址已存在，未改动。
     *  @retval WhitelistEdit::kFull 白名单已满（kMaxAlarms），未加入。
     *  @note 不影响已连接会话的角色（断开后重连按新白名单判定）。 */
    [[nodiscard]] WhitelistEdit whitelistAdd(const BtAddr &addr) noexcept;
    /** @brief 移除一条地址，其后条目前移以保持持久化顺序稳定。
     *  @param addr 待移除地址。
     *  @retval WhitelistEdit::kDone 已移除。
     *  @retval WhitelistEdit::kUnchanged 地址不在白名单。
     *  @note 不影响已连接会话的角色（断开后重连按新白名单判定）。 */
    [[nodiscard]] WhitelistEdit whitelistRemove(const BtAddr &addr) noexcept;
    /** @brief 当前白名单只读视图。
     *  @return 指向内部数组前 alarm_count_ 条的视图；空表时长度为 0。 */
    [[nodiscard]] std::span<const BtAddr> whitelist() const noexcept;

    /** @brief 当前准入策略。
     *  @return init() 时设置的策略值。 */
    [[nodiscard]] Policy policy() const noexcept;

private:
    struct Session {
        bool inUse = false;
        SessionHandle handle = 0;
        Role role = Role::kNone;
        BtAddr addr{};
        uint32_t sinceMs = 0;
        uint32_t lastRxMs = 0;
    };

    std::array<Session, kMaxSessions> sessions_{};
    std::array<BtAddr, kMaxAlarms> alarms_{};
    size_t alarm_count_ = 0;
    Policy policy_ = Policy::kConcurrent;
    bool pairing_ = false;
    uint32_t pair_start_ms_ = 0;
};

} // namespace core
