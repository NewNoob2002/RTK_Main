// GNSS App 优先级记录环（P1 Rev B）：满环按 GSV → GSA → GST → RMC → 其它 → GGA 顺序淘汰。
// 线程安全：通过注入的 fw::LockOps 保护；单写者（kIo）+ 单读者（kLink）；非 ISR 安全，不分配堆。
// 存储由 BootBuffers 外供（4096 B），零初值以保证 AppContext 落 .bss。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "fw/FanoutRing.hpp"
#include "fw/Lock.hpp"
#include "fw/Result.hpp"

namespace app::data {

/**
 * 固定容量、单写单读、按类型优先级淘汰的 GNSS App 记录环。
 * - 接口与 fw::FanoutRing typed 模式对齐（initTyped/attach/detach/setMask/write/peek/commit/cancel）；
 * - 满环时优先淘汰 GSV → GSA → GST → RMC → 其它非 GGA，GGA 最后淘汰，且低优先级新帧不会挤掉已入环的高优先级帧；
 * - 统计 ggaDropCount() 供 status gnss / gnss status 展示。
 */
class GnssAppRing {
public:
    constexpr GnssAppRing() noexcept = default;
    GnssAppRing(const GnssAppRing &) = delete;
    GnssAppRing &operator=(const GnssAppRing &) = delete;
    GnssAppRing(GnssAppRing &&) = delete;
    GnssAppRing &operator=(GnssAppRing &&) = delete;
    ~GnssAppRing() = default;

    /**
     * @brief 绑定外供存储与元数据锁，初始化为 typed 优先级环。
     * @param storage: 外供存储，长度须 >= maxRecord + kFanoutTypedHeaderBytes。
     * @param maxRecord: 单条负载上限（1..65535）。
     * @param lock: 元数据锁；lock/unlock 须同为空或同为非空。
     * @retval fw::Result::kOk 初始化成功。
     * @retval fw::Result::kInvalidArg 参数越界、缓冲过小或锁表不成对。
     * @retval fw::Result::kInvalidState 已初始化过。
     * @note 仅启动前单线程调用，非 ISR，不分配堆。
     */
    [[nodiscard]] fw::Result initTyped(std::span<uint8_t> storage, size_t maxRecord,
                                       const fw::LockOps &lock) noexcept;

    /**
     * @brief 登记唯一读者（id 固定为 0）并设置初始订阅位图。
     * @param mask: 订阅位图（bit n 对应 type n）；0 表示暂不接收。
     * @param[out] readerId: 成功时写入 0；失败不修改。
     * @retval fw::Result::kOk 登记成功。
     * @retval fw::Result::kNoSpace 已有读者占用。
     * @retval fw::Result::kInvalidState 尚未初始化或代次耗尽。
     * @note 任务上下文，持注入锁，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result attach(fw::FanoutMask mask, uint8_t &readerId) noexcept;

    /**
     * @brief 注销读者并清空未读记录。
     * @param readerId: 读者 id（须为 0）。
     * @retval fw::Result::kOk 注销成功。
     * @retval fw::Result::kInvalidArg id 越界或未 attach。
     * @retval fw::Result::kBusy 读者正持有租约。
     * @retval fw::Result::kInvalidState 尚未初始化。
     * @note 任务上下文，持注入锁，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result detach(uint8_t readerId) noexcept;

    /**
     * @brief 更新读者订阅位图并跳过旧记录（与 FanoutRing::setMask 语义一致）。
     * @param readerId: 读者 id（须为 0）。
     * @param mask: 新订阅位图。
     * @retval fw::Result::kOk 修改成功。
     * @retval fw::Result::kInvalidArg id 越界或未 attach。
     * @retval fw::Result::kBusy 读者正持有租约。
     * @retval fw::Result::kInvalidState 尚未初始化。
     * @note 读者任务调用，持注入锁，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result setMask(uint8_t readerId, fw::FanoutMask mask) noexcept;

    /**
     * @brief 写入一条带类型的记录；空间不足时按 GSV→GSA→GST→RMC→其它→GGA 淘汰。
     * @param type: 语句类型下标（0..31，与 gnss::OutputMsg 一致）。
     * @param payload: 负载字节（长度 1..maxRecord，不得与环存储重叠）。
     * @retval fw::Result::kOk 写入成功，或无订阅者（仅累加 noSubscriber）。
     * @retval fw::Result::kInvalidArg type 越界或长度非法（累加 rejected/oversizeDrop）。
     * @retval fw::Result::kNoSpace 环内被更高优先级记录占满，本次低优先级帧已丢弃。
     * @retval fw::Result::kBusy 读侧租约阻塞且无法腾出足够空间（累加 pinnedDrop）。
     * @retval fw::Result::kInvalidState 尚未初始化。
     * @note 仅写者任务（kIo）调用，持注入锁，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result write(uint8_t type, std::span<const uint8_t> payload) noexcept;

    /**
     * @brief 查看下一条匹配订阅位图的记录并 pin 住（零拷贝，单段连续）。
     * @param readerId: 读者 id（须为 0）。
     * @param[out] view: 成功时写入负载视图（second 恒为空）；失败清空。
     * @retval fw::Result::kOk 已 pin，须 commit 或 cancel 释放。
     * @retval fw::Result::kNotFound 暂无可读记录。
     * @retval fw::Result::kBusy 读者已持有租约。
     * @retval fw::Result::kInvalidArg id 越界或未 attach。
     * @retval fw::Result::kInvalidState 尚未初始化或 serial 耗尽。
     * @note 仅读者任务（kLink）调用，持注入锁，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result peek(uint8_t readerId, fw::FanoutView &view) noexcept;

    /**
     * @brief 提交租约并消费当前记录。
     * @param lease: peek 返回的租约。
     * @retval fw::Result::kOk 提交成功。
     * @retval fw::Result::kInvalidArg 租约无效或已过期。
     * @retval fw::Result::kInvalidState 尚未初始化。
     * @note 仅读者任务（kLink）调用，持注入锁，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result commit(const fw::FanoutLease &lease) noexcept;

    /**
     * @brief 回滚租约并保留当前记录供下次重试。
     * @param lease: peek 返回的租约。
     * @retval fw::Result::kOk 回滚成功。
     * @retval fw::Result::kInvalidArg 租约无效或已过期。
     * @retval fw::Result::kInvalidState 尚未初始化。
     * @note 仅读者任务（kLink）调用，持注入锁，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Result cancel(const fw::FanoutLease &lease) noexcept;

    /**
     * @brief 读取读者统计快照。
     * @param readerId: 读者 id（0）。
     * @retval 累计统计；非法 id 或未 attach 返回全零。
     * @note 任意任务，持注入锁，非 ISR。
     */
    [[nodiscard]] fw::FanoutTypedReaderStats typedReaderStats(uint8_t readerId) const noexcept;

    /**
     * @brief 读取写者统计快照。
     * @retval 累计统计；未初始化返回全零。
     * @note 任意任务，持注入锁，非 ISR。
     */
    [[nodiscard]] fw::FanoutTypedWriterStats typedWriterStats() const noexcept;

    /**
     * @brief 读取因满环或 pin 阻塞导致的 GGA 丢弃累计次数。
     * @retval 单调递增的 GGA 丢弃计数。
     * @note 任意任务，持注入锁，非 ISR。
     */
    [[nodiscard]] uint32_t ggaDropCount() const noexcept;

private:
    struct Reader {
        bool attached = false;
        fw::FanoutReaderStats stats{};
        fw::FanoutMask mask = 0U;
        uint32_t generation = 0U;
        uint32_t serial = 0U;
        uint32_t filtered = 0U;
        uint32_t pinnedDrop = 0U;
        bool pinned = false;
    };

    /**
     * @brief 读取偏移 pos 处记录的负载长度（调用方持锁且确保 pos 合法）。
     * @param pos: 记录起始偏移。
     * @retval 负载字节数。
     */
    [[nodiscard]] uint16_t recordLen(size_t pos) const noexcept;

    /**
     * @brief 读取偏移 pos 处记录的类型字节（调用方持锁且确保 pos 合法）。
     * @param pos: 记录起始偏移。
     * @retval 记录类型下标。
     */
    [[nodiscard]] uint8_t recordType(size_t pos) const noexcept;

    /**
     * @brief 返回偏移 pos 处记录的总字节数（3 B 头 + 负载）。
     * @param pos: 记录起始偏移。
     * @retval 记录总长度。
     */
    [[nodiscard]] size_t recordTotal(size_t pos) const noexcept;

    /**
     * @brief 把未读区间 [read_pos_, write_pos_) 搬移至缓冲起点 0（调用方持锁且未 pin）。
     * @note 仅在 read_pos_ > 0 且 !reader_.pinned 时调用。
     */
    void compactToFront() noexcept;

    /**
     * @brief 按优先级为即将写入的记录腾出至少 need 字节尾部空间（调用方持锁）。
     * @param incomingType: 新记录类型。
     * @param need: 所需总字节数（含 3 B 头）。
     * @retval fw::Result::kOk 已腾出足够尾部空间。
     * @retval fw::Result::kNoSpace 环被更高优先级记录占满，新帧已计入丢弃。
     * @retval fw::Result::kBusy 读侧 pin 阻塞，新帧已计入 pinnedDrop。
     */
    [[nodiscard]] fw::Result makeSpaceFor(uint8_t incomingType, size_t need) noexcept;

    /**
     * @brief 淘汰偏移 victimPos 处的单条记录并左移后续记录（调用方持锁）。
     * @param victimPos: 待淘汰记录起始偏移（须在 pin 区之后）。
     */
    void evictAt(size_t victimPos) noexcept;

    /**
     * @brief 在 write_pos_ 处写入单条记录并更新写统计与水位（调用方持锁且空间足够）。
     * @param type: 语句类型下标。
     * @param payload: 负载字节视图。
     */
    void appendRecord(uint8_t type, std::span<const uint8_t> payload) noexcept;

    /**
     * @brief 完成一次租约提交或回滚（内部共用校验逻辑）。
     * @param lease: 待处理的租约。
     * @param consume: true 消费并前移读指针，false 仅解除 pin。
     * @retval fw::Result::kOk 成功。
     * @retval fw::Result::kInvalidArg 租约无效或已过期。
     * @retval fw::Result::kInvalidState 尚未初始化。
     */
    [[nodiscard]] fw::Result finishLease(const fw::FanoutLease &lease, bool consume) noexcept;

    fw::LockOps lock_{};
    uint8_t *buf_ = nullptr;
    size_t cap_ = 0U;
    size_t max_record_ = 0U;
    size_t read_pos_ = 0U;
    size_t write_pos_ = 0U;
    Reader reader_{};
    fw::FanoutWriterStats wstats_{};
    fw::FanoutTypedWriterStats typed_wstats_{};
    uint32_t gga_drop_ = 0U;
    bool inited_ = false;
};

} // namespace app::data
