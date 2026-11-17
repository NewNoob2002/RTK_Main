// GNSS 订阅与最新帧查询服务（PHONE，ADR-0011 §6）：处理 0x04/0x05/0x1F/0x21/0x22/0x23。
// 线程安全：否（仅由链路任务单线程访问；查询与掩码回调须由调用方保证并发安全）。ISR 安全：否。
// 堆：不分配。冻结接口（ADR-0011 §6）。实现方不得修改。
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
/** 未收到 GNSS 状态前的支持位假设：除 HEADINGA(bit 2) 外均可订阅（ADR-0011 §6）。 */
inline constexpr uint32_t kGnssDefaultSupportedMask = 0x3BU;

/** 0x04/0x05/0x1F/0x21/0x22/0x23 单次查询（interval==0）回调结果。 */
struct GnssQueryResult {
    bool ok = false;       // 是否存在可返回的缓存帧
    bool oversize = false; // 缓存存在但长度超过 proto::kMaxPayload（256 B）
    size_t len = 0;        // ok==true 时的有效负载字节数（1..256）
};

using GnssQueryFn = GnssQueryResult (*)(void *ctx, uint16_t msgId, std::span<uint8_t> dst) noexcept;
using GnssMaskChangedFn = void (*)(void *ctx, uint32_t mask) noexcept;

/**
 * GNSS 订阅与查询服务（ADR-0011 §6）：
 * - 路由 0x04(GGA)、0x05(SATSINFOA)、0x1F(HEADINGA)、0x21(GST)、0x22(RMC)、0x23(GSA)，仅允许 PHONE。
 * - interval != 0：按规则换算发送周期并开启对应 type 位订阅（接收机不支持该语句时拒绝 02 02，ADR-0012 §2.5）。
 * - interval == 0：关闭对应 type 位订阅，同时查询最新帧（<=256B 原文回复；无缓存/超长/不支持回 02 02）。
 * - 仅维护会话内订阅态，不写 NVS，断线重连通过 resetSession() 归零。
 */
class GnssService {
public:
    constexpr GnssService() noexcept = default;
    GnssService(const GnssService &) = delete;
    GnssService &operator=(const GnssService &) = delete;
    GnssService(GnssService &&) = delete;
    GnssService &operator=(GnssService &&) = delete;
    ~GnssService() = default;

    /** @brief 设置最新帧查询回调（interval==0 时用来取缓存原文）。
     *  @param fn 回调函数，可为 nullptr（表示未接入，查询统一回失败）。
     *  @param ctx 透传给回调的上下文指针，本服务不解引用。
     *  @note 只保存指针，不调用；回调在 handleFrame 内同步执行，须非阻塞、不重入本服务。 */
    void setQueryHook(GnssQueryFn fn, void *ctx) noexcept;

    /** @brief 设置订阅掩码变化回调（接收机侧开关订阅用）。
     *  @param fn 回调函数，可为 nullptr。
     *  @param ctx 透传给回调的上下文指针，本服务不解引用。
     *  @note 只在掩码实际变化时调用；同步执行，须非阻塞。 */
    void setMaskHook(GnssMaskChangedFn fn, void *ctx) noexcept;

    /**
     * @brief 更新接收机支持的语句位掩码（bit 0..5 对应 0x04/0x05/0x1F/0x21/0x22/0x23，ADR-0012 §2.5）。
     * @param mask 支持位掩码；高于 bit 5 的位会被屏蔽。
     * @note 不影响已建立的订阅；非 ISR。
     */
    void setSupportedMask(uint32_t mask) noexcept;

    /** @brief 当前支持位掩码。
     *  @return 0..0x3F；未调用过 setSupportedMask 时为 kGnssDefaultSupportedMask。 */
    [[nodiscard]] uint32_t supportedMask() const noexcept;

    /** @brief 会话断开/重连时清零全部订阅与周期。
     *  @note 仅在掩码原本非 0 时触发 mask 回调（0 → 0 不回调）；非 ISR、不阻塞。 */
    void resetSession() noexcept;

    /** @brief 当前活跃订阅位掩码。
     *  @return bit 0..5 对应六条语句；断开态为 0。 */
    [[nodiscard]] uint32_t activeMask() const noexcept;

    /** @brief 指定 type 当前生效的发送周期。
     *  @param typeIdx type 索引（0..5）。
     *  @return 周期（ms）；未订阅或索引越界返回 0。 */
    [[nodiscard]] uint32_t periodMs(size_t typeIdx) const noexcept;

    /** @brief 因缓存超过 256 B 被拒绝的查询次数。
     *  @return 单调递增的累计值，供诊断读取。 */
    [[nodiscard]] uint32_t queryOversizeCount() const noexcept;

    /** @brief 协议 interval（有符号字节）换算发送周期。
     *  @param interval 协议第 19 字节的解释结果。
     *  @return 0 → 0；<0 → |v| 秒（|v|*1000 ms）；>0 → max(50, 1000/v) ms。
     *  @note 取负前先扩到 int32_t，避免 -128 溢位。 */
    [[nodiscard]] static uint32_t intervalToPeriodMs(int8_t interval) noexcept;

    /** @brief 协议 msgId 映射到 type 索引。
     *  @param msgId 协议命令 ID。
     *  @return 0..5；不是本服务的六条语句时为 std::nullopt。 */
    [[nodiscard]] static std::optional<size_t> typeIndexOf(uint16_t msgId) noexcept;

    /** @brief type 索引映射到协议 msgId。
     *  @param typeIdx type 索引（0..5）。
     *  @return 对应 msgId；越界返回 0。 */
    [[nodiscard]] static uint16_t msgIdAt(size_t typeIdx) noexcept;

    /** @brief type 索引对应的默认周期。
     *  @param typeIdx type 索引（0..5）。
     *  @return 默认周期（ms：1000/5000/1000/1000/1000/3000）；越界返回 0。 */
    [[nodiscard]] static uint32_t defaultPeriodMs(size_t typeIdx) noexcept;

    /** @brief 本服务负责的六条语句静态路由表（仅 PHONE）。
     *  @return 指向静态数组的视图，内容只读且与实例无关；不得修改。 */
    [[nodiscard]] static std::span<const Route> routes() noexcept;

    /** @brief 把静态路由表与 this 绑定，供 CommandRouter::init 展开。
     *  @return RouteTable{this, routes()}；本对象须存活到路由器不再使用该表为止。
     *  @note 不分配、无副作用；启动期调用。 */
    [[nodiscard]] RouteTable table() noexcept
    {
        return {this, routes()};
    }

private:
    /** @brief 处理本服务的六条语句（0x04/0x05/0x1F/0x21/0x22/0x23）。
     *  @param frame 已通过解析的请求帧；type 不是 QUERY/SET 时回失败结果码。
     *  @param[out] reply 应答缓冲；先写结果码，失败后不再改写内容。
     *  @retval DispatchResult::kReply 已组帧（成功/失败结果码或缓存原文）。
     *  @retval DispatchResult::kNoReply 组帧失败，或 msgId 不属于本服务。
     *  @note interval==0 表示关闭订阅并查询缓存：缓存缺失、无查询回调、超 256 B 或语句
     *        不受支持都回失败（超长另计 query_oversize_）；interval!=0 时若语句不受支持回失败，
     *        否则写入周期、置活跃位，并仅在掩码变化时同步调用 mask 回调。
     *  @note 非 ISR；回调在调用栈内执行，不阻塞、不分配。 */
    DispatchResult handleFrame(const proto::FrameView &frame, Reply &reply) noexcept;

    GnssQueryFn query_fn_ = nullptr;
    void *query_ctx_ = nullptr;
    GnssMaskChangedFn mask_fn_ = nullptr;
    void *mask_ctx_ = nullptr;
    uint32_t active_mask_ = kGnssDefaultMask;
    std::array<uint32_t, kGnssMsgCount> periods_ms_{};
    uint32_t query_oversize_ = 0;
    uint32_t supported_mask_ = kGnssDefaultSupportedMask;
};

} // namespace core::services
