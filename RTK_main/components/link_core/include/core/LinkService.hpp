// 链路服务：把传输事件 → 每会话解析器 → LinkManager → CommandRouter → 传输写回串起来。
// 纯逻辑（经 Transport 接口访问硬件），可在 host 上用假传输测试。
// 线程安全：否。所有方法只允许由同一个链路任务调用（单一所有者）。
// 冻结接口（ADR-0002；ADR-0007 J2b 修订：分发改为 CommandRouter，推送改由 AlarmService 组帧）。实现方不得修改。
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
    /** router 必须已 init；alarm 用于 tick() 中组 0x40 推送帧。三个引用的生命周期须覆盖本对象。 */
    LinkService(Transport &transport, LinkManager &link, CommandRouter &router,
                const services::AlarmService &alarm) noexcept;

    // 持有引用与会话状态：不可拷贝、不可移动（C.21 rule of five）
    LinkService(const LinkService &) = delete;
    LinkService &operator=(const LinkService &) = delete;
    LinkService(LinkService &&) = delete;
    LinkService &operator=(LinkService &&) = delete;
    ~LinkService() = default;

    /**
     * 新连接：交给 LinkManager；对 Actions 中每个句柄调用 transport.disconnect() 并释放其解析器；
     * 角色非 kNone 时为该句柄分配一个全新（reset 过的）解析器槽。
     */
    void onOpen(SessionHandle handle, const BtAddr &addr, uint32_t nowMs) noexcept;

    /** 连接关闭：LinkManager::onClose 并释放解析器槽。 */
    void onClose(SessionHandle handle) noexcept;

    /** 只允许链路任务、非 ISR。返回已消费字节；调用者必须保留未消费尾部。
     * 完整请求先取得280B reply credit，再执行 LinkManager/Router，满则保留 parser 中的完整帧。
     * paused 为 true 时不能继续 feed/read 本会话；TX_SPACE 后 resume，CLOSE 取消且不执行尾帧。
     * 无 reply 的路由取消 credit；commit 不重新争抢。旧同步 host Transport 保留 write 兼容行为。
     */
    size_t onData(SessionHandle handle, std::span<const uint8_t> data, uint32_t nowMs) noexcept;
    /** 链路任务/非 ISR/不等待；重查 level 后尝试一次 pending dispatch，不重跑已执行请求。 */
    void resume(SessionHandle handle, uint32_t nowMs) noexcept;
    /** 链路任务/非 ISR；true 时上层停止本会话 RX，不应以积压为由忙轮询。 */
    [[nodiscard]] bool paused(SessionHandle handle) const noexcept;

    /** 传输层报告接收缓冲溢出丢弃的字节数。 */
    void onRxDropped(uint32_t count) noexcept;

    /**
     * 周期调用（建议 100 ms）：LinkManager::tick 并执行断开动作；
     * 距上次推送 >= kPushPeriodMs（首次调用视为已到期）时，对每个 ALARM 句柄发送 0x40
     * 推送（AlarmService::buildStatusPush 组帧），seq 每轮 +1。
     */
    void tick(uint32_t nowMs) noexcept;

    /** 读取并清除白名单脏标志；为 true 时调用方应持久化 LinkManager::whitelist()。 */
    [[nodiscard]] bool consumeWhitelistDirty() noexcept;

    [[nodiscard]] std::optional<proto::alarm::Heartbeat> lastHeartbeat() const noexcept;
    [[nodiscard]] const LinkStats &stats() const noexcept;
    /** 所有会话解析器的 CRC / 长度错误累计。 */
    [[nodiscard]] uint32_t parserCrcErrors() const noexcept;
    [[nodiscard]] uint32_t parserLengthErrors() const noexcept;

private:
    struct Slot {
        bool inUse = false;
        SessionHandle handle = 0;
        proto::FrameParser parser{};
        proto::FrameView frame{}; // payload 指向 parser，pending 期间不再 feed
        bool pending = false;
    };

    void apply(const Actions &act) noexcept;
    Slot *findSlot(SessionHandle handle) noexcept;
    bool dispatchPending(Slot &slot, uint32_t nowMs) noexcept;
    void sendReply(SessionHandle handle, const ReplyCredit &credit, uint16_t msgId) noexcept;

    Transport &transport_;
    LinkManager &link_;
    CommandRouter &router_;
    const services::AlarmService &alarm_;
    std::array<Slot, kMaxSessions> slots_{};
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
