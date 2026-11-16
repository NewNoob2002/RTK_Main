// GNSS 订阅与最新帧查询服务（PHONE，ADR-0011）：处理 0x04/0x05/0x1F/0x21/0x22/0x23。
// 线程安全：否（仅由链路任务单线程访问；查询与掩码回调须由调用方保证并发安全）。ISR 安全：否。
// 冻结接口（ADR-0011 / K4）。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/CommandRouter.hpp"
#include "core/Reply.hpp"

namespace core::services {

inline constexpr size_t kGnssMsgCount = 6;
inline constexpr uint32_t kGnssDefaultMask = 0;

/** 0x04/0x05/0x1F/0x21/0x22/0x23 单次查询（interval==0）回调结果。 */
struct GnssQueryResult {
    bool ok = false;       // 是否存在可返回的缓存帧
    bool oversize = false; // 缓存存在但长度超过 proto::kMaxPayload（256 B）
    size_t len = 0;        // ok==true 时的有效负载字节数（1..256）
};

using GnssQueryFn = GnssQueryResult (*)(void *ctx, uint16_t msg_id, std::span<uint8_t> dst) noexcept;
using GnssMaskChangedFn = void (*)(void *ctx, uint32_t mask) noexcept;

/**
 * GNSS 订阅与查询服务（ADR-0011 §6）：
 * - 路由 0x04(GGA)、0x05(SATSINFOA)、0x1F(HEADINGA)、0x21(GST)、0x22(RMC)、0x23(GSA)，仅允许 PHONE。
 * - interval != 0：按规则换算发送周期并开启对应 type 位订阅（0x1F 无航向能力时拒绝 02 02）。
 * - interval == 0：关闭对应 type 位订阅，同时查询最新帧（<=256B 原文回复；无缓存/超长/不支持回 02 02）。
 * - 仅维护会话内订阅态，不写 NVS，断线重连通过 reset_session() 归零。
 */
class GnssService {
public:
    constexpr GnssService() noexcept = default;
    GnssService(const GnssService &) = delete;
    GnssService &operator=(const GnssService &) = delete;
    GnssService(GnssService &&) = delete;
    GnssService &operator=(GnssService &&) = delete;
    ~GnssService() = default;

    /** 设置最新帧查询回调；线程安全：否（仅链路任务调用）。 */
    void set_query_hook(GnssQueryFn fn, void *ctx) noexcept;

    /** 设置订阅掩码变化回调；线程安全：否（仅链路任务调用）。 */
    void set_mask_hook(GnssMaskChangedFn fn, void *ctx) noexcept;

    /** 更新接收机是否支持 HEADINGA（0x1F）；线程安全：否（仅链路任务调用）。 */
    void set_heading_supported(bool supported) noexcept;

    /** 查询当前是否支持 HEADINGA（0x1F）；线程安全：否（仅链路任务调用）。 */
    [[nodiscard]] bool heading_supported() const noexcept;

    /** 会话断开/重连时清零全部订阅掩码与周期，并在掩码变化时触发回调；线程安全：否。 */
    void reset_session() noexcept;

    /** 当前活跃订阅位掩码（bit 0..5 对应 0x04/0x05/0x1F/0x21/0x22/0x23）；线程安全：否。 */
    [[nodiscard]] uint32_t active_mask() const noexcept;

    /** 查询指定 type 索引（0..5）当前生效周期（ms，未订阅为 0）；线程安全：否。 */
    [[nodiscard]] uint32_t period_ms(size_t type_idx) const noexcept;

    /** 查询因缓存超 256B 被拒绝的次数；线程安全：否。 */
    [[nodiscard]] uint32_t query_oversize_count() const noexcept;

    /** 协议 interval（有符号字节）换算周期（ms）：0->0，<0->|v|*1000，>0->max(50, 1000/v)。 */
    [[nodiscard]] static uint32_t interval_to_period_ms(int8_t interval) noexcept;

    /** 协议 msg_id 映射到 type 索引（0..5）。 */
    [[nodiscard]] static std::optional<size_t> type_index_of(uint16_t msg_id) noexcept;

    /** type 索引（0..5）映射到协议 msg_id。 */
    [[nodiscard]] static uint16_t msg_id_at(size_t type_idx) noexcept;

    /** type 索引（0..5）对应的默认周期（ms：1000/5000/1000/1000/1000/3000）。 */
    [[nodiscard]] static uint32_t default_period_ms(size_t type_idx) noexcept;

    /** 返回本服务的路由表；线程安全：否。 */
    [[nodiscard]] static std::span<const Route> routes() noexcept;

    /** 绑定本实例与静态路由表；线程安全：否。 */
    [[nodiscard]] RouteTable table() noexcept
    {
        return {this, routes()};
    }

private:
    DispatchResult handle_frame(const proto::FrameView &frame, Reply &reply) noexcept;

    GnssQueryFn query_fn_ = nullptr;
    void *query_ctx_ = nullptr;
    GnssMaskChangedFn mask_fn_ = nullptr;
    void *mask_ctx_ = nullptr;
    uint32_t active_mask_ = kGnssDefaultMask;
    std::array<uint32_t, kGnssMsgCount> periods_ms_{};
    uint32_t query_oversize_ = 0;
    bool heading_supported_ = false;
};

} // namespace core::services
