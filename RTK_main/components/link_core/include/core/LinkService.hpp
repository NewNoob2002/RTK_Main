// 链路服务：把传输事件 → 每会话解析器 → LinkManager → CommandRouter → 传输写回串起来（纯逻辑，经 Transport 访问硬件）。
// 线程安全：否，所有方法只由同一个链路任务调用（单一所有者）；非 ISR、不阻塞。
// 堆：不分配（槽位与应答缓冲均为成员）。冻结接口（ADR-0002；ADR-0007 §3.5 起分发走 CommandRouter）。实现方不得修改。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/LinkManager.hpp"
#include "core/Reply.hpp"
#include "core/services/AlarmService.hpp"
#include "core/Transport.hpp"
#include "core/Types.hpp"
#include "proto/AlarmCodec.hpp"
#include "proto/Frame.hpp"

namespace core {

inline constexpr uint32_t kWriteTimeoutMs = 200;
inline constexpr size_t kMaxDeferredReplies = 4;

/** 链路服务累计统计；只由链路任务写入。 */
struct LinkStats {
    uint32_t framesRx = 0;
    uint32_t repliesTx = 0;
    uint32_t writeErrors = 0;
    uint32_t pushesTx = 0;
    uint32_t disconnects = 0; // 本服务主动请求的断开次数
    uint32_t rxDropped = 0;   // 由 onRxDropped() 累计
    uint32_t dispatchPauses = 0;
    uint32_t dispatchCancelled = 0;
};

/** 传输事件 → 解析 → LinkManager → 路由 → 写回的编排者；仅链路任务使用，非 ISR。 */
class LinkService {
public:
    /**
     * @brief 绑定协作对象；不注册回调、不分配。
     * @param transport 会话传输实现，生命周期须覆盖本对象。
     * @param link 会话角色与白名单状态机，生命周期须覆盖本对象。
     * @param router 已 init 的指令路由器，生命周期须覆盖本对象。
     * @param alarm 报警状态组帧者，tick() 中用它组 0x40 推送；生命周期须覆盖本对象。
     * @note 启动期装配，单线程调用；四个引用都必须活到本对象析构。
     */
    LinkService(Transport &transport, LinkManager &link, CommandRouter &router,
                const services::AlarmService &alarm) noexcept;

    // 持有引用与会话状态：不可拷贝、不可移动（C.21 rule of five）
    LinkService(const LinkService &) = delete;
    LinkService &operator=(const LinkService &) = delete;
    LinkService(LinkService &&) = delete;
    LinkService &operator=(LinkService &&) = delete;
    ~LinkService() = default;

    /**
     * @brief 新连接：交 LinkManager 判角色，执行其要求的断开，并为接纳的会话分配全新解析器槽。
     * @param handle 新连接的会话句柄。
     * @param addr 对端蓝牙地址。
     * @param nowMs 当前时间（ms）。
     * @note 对 Actions 中每个句柄调用 transport.disconnect() 并释放其解析器槽；
     *       角色非 kNone 时分配一个 reset 过的槽（错误计数从 0 开始，旧计数已累加到累计值）。
     *       槽位耗尽时主动断开（与 LinkManager 表容量一致，理论上不可达）。非 ISR、不阻塞。
     */
    void onOpen(SessionHandle handle, const BtAddr &addr, uint32_t nowMs) noexcept;

    /** @brief 连接关闭：LinkManager::onClose 并释放解析器槽（错误计数转存到累计值）。
     *  @param handle 已关闭的会话句柄；槽位不存在时只通知 LinkManager。
     *  @note 对未知/重复句柄幂等；非 ISR、不阻塞。 */
    void onClose(SessionHandle handle) noexcept;

    /**
     * @brief 喂入原始字节：逐字节解析，成帧后先取 reply credit 再走 LinkManager/路由器并把应答写回。
     * @param handle 来源会话句柄；未知或已断开时全部按契约丢弃。
     * @param data 本次读到的字节。
     * @param nowMs 当前时间（ms）。
     * @return 已消费字节数；调用者必须保留未消费尾部。会话在本次调用中被断开时返回 data.size()；
     *         有待定帧（paused）时返回 0。
     * @note 只有容量已固定才推进角色/路由；credit 暂不可用（满）时保留 parser 中的完整帧、置 paused 并
     *       由上层停止本会话 RX，TX_SPACE 后 resume，CLOSE 取消且不执行尾帧。无 reply 的分支取消 credit，
     *       commit 不重新争抢；旧同步 host Transport 保留 write 兼容行为。只允许链路任务、非 ISR。
     */
    size_t onData(SessionHandle handle, std::span<const uint8_t> data, uint32_t nowMs) noexcept;
    /** @brief 重查传输容量后尝试一次待定帧的派发。
     *  @param handle 目标会话句柄。
     *  @param nowMs 当前时间（ms），作为 LinkManager::onFrame 的时间基准。
     *  @note 只在 replyReady() 为 true 且确有 pending 帧时动作；不重跑已执行过的请求。链路任务/非 ISR/不等待。 */
    void resume(SessionHandle handle, uint32_t nowMs) noexcept;
    /** @brief 该会话是否有等容量的待定帧（上层应停止其 RX）。
     *  @param handle 目标会话句柄。
     *  @return true 表示 onData 会返回 0，不应以积压为由忙轮询；false 可继续喂数据。 */
    [[nodiscard]] bool paused(SessionHandle handle) const noexcept;

    /** @brief 累计传输层接收缓冲溢出丢弃的字节数。
     *  @param count 本次丢弃字节数；直接加到 stats().rxDropped。 */
    void onRxDropped(uint32_t count) noexcept;

    /**
     * @brief 周期调用：执行 LinkManager 的断开动作、轮询已就绪的延迟应答，并按周期向所有 ALARM 推送 0x40 状态帧。
     * @param nowMs 当前时间（ms）。
     * @note 建议每 100 ms 调用一次（LinkManager 与推送周期都按此推进）；首次调用视为推送到期；
     *       推送 seq 每轮 +1，单帧失败只记 writeErrors 并继续下一个会话。非 ISR、不阻塞。 */
    void tick(uint32_t nowMs) noexcept;

    /**
     * @brief 轮询所有挂起的延迟应答；就绪时使用原会话预留额度发送，未填充帧时释放额度。
     * @param nowMs 当前时间（ms）。
     * @note 仅链路任务调用，非 ISR、不阻塞；tick() 内也会自动调用。 */
    void pollDeferred(uint32_t nowMs) noexcept;

    /** @brief 读取并清除白名单脏标志。
     *  @return true 表示自上次调用以来白名单或角色变化，调用方应持久化 LinkManager::whitelist()。 */
    [[nodiscard]] bool consumeWhitelistDirty() noexcept;

    /** @brief 最近一次收到的心跳（0x41）。
     *  @return 心跳副本；从未收到时有值。 */
    [[nodiscard]] std::optional<proto::alarm::Heartbeat> lastHeartbeat() const noexcept;
    /** @brief 累计统计。
     *  @return 统计结构的只读引用；调用方不得保存指针跨任务使用。 */
    [[nodiscard]] const LinkStats &stats() const noexcept;
    /** @brief 所有会话解析器的 CRC 错误累计（含已释放槽位的部分）。
     *  @return 单调递增的累计值。 */
    [[nodiscard]] uint32_t parserCrcErrors() const noexcept;
    /** @brief 所有会话解析器的长度错误累计（含已释放槽位的部分）。
     *  @return 单调递增的累计值。 */
    [[nodiscard]] uint32_t parserLengthErrors() const noexcept;

private:
    struct Slot {
        bool inUse = false;
        SessionHandle handle = 0;
        proto::FrameParser parser{};
        proto::FrameView frame{}; // payload 指向 parser，pending 期间不再 feed
        bool pending = false;
    };

    struct DeferredEntry {
        bool active = false;
        SessionHandle handle = 0;
        uint16_t msgId = 0;
        ReplyCredit credit{};
        bool hasCredit = false;
        DeferredHook hook{};
    };

    /** @brief 取消指定会话全部挂起的延迟应答，释放 credit 并通知服务。
     *  @param handle 正在断开或关闭的会话句柄。
     *  @note 仅链路任务调用，非 ISR、不阻塞。 */
    void cancelDeferred(SessionHandle handle) noexcept;

    /** @brief 登记一条延迟应答；槽满或回调为空时释放 credit 并通知取消。
     *  @param handle 来源会话句柄。
     *  @param msgId 请求 msg_id。
     *  @param credit 已预留的应答额度。
     *  @param hook 服务提供的轮询/取消回调。
     *  @note 仅链路任务调用，非 ISR、不阻塞。 */
    void registerDeferred(SessionHandle handle, uint16_t msgId, const ReplyCredit &credit,
                          const DeferredHook &hook) noexcept;

    /** @brief 执行 LinkManager 要求断开的所有句柄：断开传输、退役其解析器错误计数并清 pending。
     *  @param act LinkManager 最近一次调用填写的断开动作。
     *  @note 不保存 act（指向 LinkManager 内部缓冲），也不回滚本地状态；随后的 CLOSE 事件由 onClose 收尾，
     *        因此 onClose 必须幂等。只改内存，不等待写完成；非 ISR。 */
    void apply(const Actions &act) noexcept;
    /** @brief 按句柄查解析器槽。
     *  @param handle 会话句柄。
     *  @return 槽位指针（指向 slots_ 内部，生命周期到下一次 onClose）；未分配过则 nullptr。 */
    Slot *findSlot(SessionHandle handle) noexcept;
    /**
     * @brief 尝试派发槽内唯一的待定帧：先取 reply credit，再走 LinkManager + 路由器并写回应答。
     * @param slot 目标槽位（必须 pending）。
     * @param nowMs 当前时间（ms），作为 onFrame 的时间基准。
     * @retval true 已处理完（含"出错已断开"），调用方可继续喂数据。
     * @retval false 容量仍不可用（kTimeout/kNoSpace/kBusy），保持 pending，等待下次 resume。
     * @note 进入时清除 pending，保证同一请求不会被重跑；credit 取到后若无需应答则取消。
     */
    bool dispatchPending(Slot &slot, uint32_t nowMs) noexcept;
    /** @brief 把路由器填好的 reply_ 经 transport 写回。
     *  @param handle 目标会话句柄。
     *  @param credit 该会话的应答额度。
     *  @param msgId 请求的 msg_id，用于组装应答头。
     *  @note 无应答时释放 credit 并返回；写失败只记 writeErrors，不断开（由链路层错误策略处理）。
     *        credit 一律 commit，不重新争抢。非 ISR、不等待对方接收。 */
    void sendReply(SessionHandle handle, const ReplyCredit &credit, uint16_t msgId) noexcept;

    Transport &transport_;
    LinkManager &link_;
    CommandRouter &router_;
    const services::AlarmService &alarm_;
    std::array<Slot, kMaxSessions> slots_{};
    std::array<DeferredEntry, kMaxDeferredReplies> deferred_{};
    Reply reply_{}; // 复用的应答缓冲（避免占用链路任务栈）
    LinkStats stats_{};
    std::optional<proto::alarm::Heartbeat> last_hb_{};
    bool whitelist_dirty_ = false;
    bool pushed_once_ = false;
    uint32_t last_push_ms_ = 0;
    uint16_t push_seq_ = 0;
    uint32_t retired_crc_errors_ = 0; // 已释放槽位的错误计数，保证累计值单调
    uint32_t retired_length_errors_ = 0;
};

} // namespace core
