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

#include "core/command_router.hpp"
#include "core/link_manager.hpp"
#include "core/reply.hpp"
#include "core/services/alarm_service.hpp"
#include "core/transport.hpp"
#include "core/types.hpp"
#include "proto/alarm_codec.hpp"
#include "proto/frame.hpp"

namespace core {

inline constexpr uint32_t kWriteTimeoutMs = 200;

/** 链路服务累计统计；只由链路任务写入。 */
struct LinkStats {
    uint32_t frames_rx = 0;
    uint32_t replies_tx = 0;
    uint32_t write_errors = 0;
    uint32_t pushes_tx = 0;
    uint32_t disconnects = 0; // 本服务主动请求的断开次数
    uint32_t rx_dropped = 0;  // 由 on_rx_dropped() 累计
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
    void on_open(SessionHandle handle, const BtAddr &addr, uint32_t now_ms) noexcept;

    /** 连接关闭：LinkManager::on_close 并释放解析器槽。 */
    void on_close(SessionHandle handle) noexcept;

    /**
     * 收到数据：逐字节喂给该句柄的解析器。对每个完整帧：
     * 1. LinkManager::on_frame，执行断开动作；若当前句柄因此被断开，丢弃本次剩余字节并返回。
     * 2. 角色非 kNone 时 CommandRouter::handle（结果与旧 CommandDispatcher::handle 逐字节一致）；kReply 时
     * transport.write(handle, bytes, kWriteTimeoutMs)， 失败计入 write_errors。
     * 3. reply.whitelist_changed 或角色由非 ALARM 变为 ALARM 时置白名单脏标志。
     * 4. reply.heartbeat 有值时记录为 last_heartbeat()。
     * 未分配解析器的句柄：数据丢弃。
     */
    void on_data(SessionHandle handle, std::span<const uint8_t> data, uint32_t now_ms) noexcept;

    /** 传输层报告接收缓冲溢出丢弃的字节数。 */
    void on_rx_dropped(uint32_t count) noexcept;

    /**
     * 周期调用（建议 100 ms）：LinkManager::tick 并执行断开动作；
     * 距上次推送 >= kPushPeriodMs（首次调用视为已到期）时，对每个 ALARM 句柄发送 0x40
     * 推送（AlarmService::build_status_push 组帧），seq 每轮 +1。
     */
    void tick(uint32_t now_ms) noexcept;

    /** 读取并清除白名单脏标志；为 true 时调用方应持久化 LinkManager::whitelist()。 */
    [[nodiscard]] bool consume_whitelist_dirty() noexcept;

    [[nodiscard]] std::optional<proto::alarm::Heartbeat> last_heartbeat() const noexcept;
    [[nodiscard]] const LinkStats &stats() const noexcept;
    /** 所有会话解析器的 CRC / 长度错误累计。 */
    [[nodiscard]] uint32_t parser_crc_errors() const noexcept;
    [[nodiscard]] uint32_t parser_length_errors() const noexcept;

private:
    struct Slot {
        bool in_use = false;
        SessionHandle handle = 0;
        proto::FrameParser parser{};
    };

    void apply(const Actions &act) noexcept;
    Slot *find_slot(SessionHandle handle) noexcept;

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
