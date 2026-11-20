// 网页配置模块跨任务静态 SPSC 帧邮箱与控制事件邮箱（httpd 线程 <-> kLink 任务）。
// 仅单生产者单消费者使用（SPSC 无锁原子下标），不阻塞、不分配堆，非 ISR。
// 堆档位：禁止区；全部存储为静态固定数组，默认零初始化落 .bss。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "fw/Atomic.hpp"
#include "web_proto/Request.hpp"

namespace app::web_cfg {

/** 单条入队结果。 */
enum class MailboxPush : uint8_t {
    kOk = 0,
    kFull,
    kTooLong,
};

/** 单个 WebSocket 文本帧槽位（<= 1024 B，携带会话代号防止 fd 复用串会话）。 */
struct WebFrame {
    int fd = 0;
    uint32_t gen = 0U;
    uint16_t len = 0U;
    std::array<char, web::kMaxFrameBytes> data{};
};

/** 静态定长 SPSC 文本帧邮箱；容量必须为 2 的幂。 */
template <size_t Capacity> class WebFrameMailbox final {
public:
    static_assert(Capacity > 0U && ((Capacity & (Capacity - 1U)) == 0U),
                  "WebFrameMailbox capacity must be a power of 2");
    static_assert(fw::kAtomicUsable<uint32_t>, "SPSC mailbox indices must be lock-free");

    constexpr WebFrameMailbox() noexcept = default;
    ~WebFrameMailbox() = default;
    WebFrameMailbox(const WebFrameMailbox &) = delete;
    WebFrameMailbox &operator=(const WebFrameMailbox &) = delete;

    /**
     * @brief 投递一帧到邮箱尾部（仅生产者线程调用）
     * @param fd: 关联的 WebSocket 套接字描述符
     * @param gen: 会话代号（非 0，防止 fd 复用串会话）
     * @param payload: 待复制的帧字节视图
     * @retval kOk 已入队
     * @retval kTooLong 超过 1024 B 上限
     * @retval kFull 邮箱已满，未写入
     * @note 单生产者无锁调用，不阻塞、不分配堆，非 ISR。
     */
    [[nodiscard]] MailboxPush tryPush(int fd, uint32_t gen, std::span<const char> payload) noexcept
    {
        if (payload.size() > web::kMaxFrameBytes) {
            return MailboxPush::kTooLong;
        }
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if ((head - tail) >= static_cast<uint32_t>(Capacity)) {
            return MailboxPush::kFull;
        }
        WebFrame &slot = slots_[static_cast<size_t>(head & kMask)];
        slot.fd = fd;
        slot.gen = gen;
        // payload.size() 已限制 <= 1024，窄化到 uint16_t 安全。
        slot.len = static_cast<uint16_t>(payload.size());
        if (!payload.empty()) {
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        }
        head_.store(head + 1U, std::memory_order_release);
        return MailboxPush::kOk;
    }

    /**
     * @brief 从邮箱头部弹出一帧（仅消费者线程调用）
     * @param[out] out: 成功时写入帧内容；邮箱为空时保持不变
     * @retval true 成功弹出一帧
     * @retval false 邮箱为空
     * @note 单消费者无锁调用，不阻塞、不分配堆，非 ISR。
     */
    [[nodiscard]] bool tryPop(WebFrame &out) noexcept
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = slots_[static_cast<size_t>(tail & kMask)];
        tail_.store(tail + 1U, std::memory_order_release);
        return true;
    }

    /**
     * @brief 查询邮箱当前是否为空
     * @retval true 当前无待取帧
     * @retval false 至少有一帧待取
     * @note 无锁原子快照，不阻塞。
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    /**
     * @brief 查询邮箱当前积压帧数
     * @retval 已入队未消费的帧数
     * @note 无锁原子快照，不阻塞。
     */
    [[nodiscard]] size_t size() const noexcept
    {
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(head - tail);
    }

    /**
     * @brief 清空读写下标（仅启动前或双方停止后调用）
     * @note 不阻塞，非线程安全（仅静默态重置）。
     */
    void clear() noexcept
    {
        head_.store(0U, std::memory_order_relaxed);
        tail_.store(0U, std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kMask = static_cast<uint32_t>(Capacity - 1U);

    std::array<WebFrame, Capacity> slots_{};
    std::atomic<uint32_t> head_{0U};
    std::atomic<uint32_t> tail_{0U};
};

/** 外部存储版 SPSC 文本帧邮箱（由 BootArena 在 kWifiConfig 模式外供槽位，零蓝牙模式 DRAM 占用）。 */
class WebFrameMailboxView final {
public:
    static_assert(fw::kAtomicUsable<uint32_t>, "SPSC mailbox indices must be lock-free");

    constexpr WebFrameMailboxView() noexcept = default;
    ~WebFrameMailboxView() = default;
    WebFrameMailboxView(const WebFrameMailboxView &) = delete;
    WebFrameMailboxView &operator=(const WebFrameMailboxView &) = delete;

    /**
     * @brief 绑定外供帧槽位数组（容量必须为 2 的幂）
     * @param slots: 启动期分配的帧数组，生命周期覆盖整个运行期
     * @retval true 绑定成功
     * @retval false 数组为空或长度非 2 的幂
     * @note 仅启动前单线程调用，不阻塞。
     */
    [[nodiscard]] bool bind(std::span<WebFrame> slots) noexcept
    {
        if (slots.empty() || ((slots.size() & (slots.size() - 1U)) != 0U)) {
            return false;
        }
        slots_ = slots;
        // slots.size() 为 2 的幂且在固件中 <= 16，转 uint32_t 无损。
        mask_ = static_cast<uint32_t>(slots.size() - 1U);
        clear();
        return true;
    }

    /**
     * @brief 解除外供槽位引用（仅启动回滚、消费者未启动时调用）
     * @note 不阻塞，非线程安全。
     */
    void unbind() noexcept
    {
        slots_ = {};
        mask_ = 0U;
        clear();
    }

    /**
     * @brief 投递一帧到邮箱尾部（仅生产者线程调用）
     * @param fd: 关联的 WebSocket 套接字描述符
     * @param gen: 会话代号（非 0，防止 fd 复用串会话）
     * @param payload: 待复制的帧字节视图
     * @retval kOk 已入队
     * @retval kTooLong 超过 1024 B 上限
     * @retval kFull 邮箱未绑定或已满，未写入
     * @note 单生产者无锁调用，不阻塞、不分配堆，非 ISR。
     */
    [[nodiscard]] MailboxPush tryPush(int fd, uint32_t gen, std::span<const char> payload) noexcept
    {
        if (payload.size() > web::kMaxFrameBytes) {
            return MailboxPush::kTooLong;
        }
        if (slots_.empty()) {
            return MailboxPush::kFull;
        }
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if ((head - tail) >= static_cast<uint32_t>(slots_.size())) {
            return MailboxPush::kFull;
        }
        WebFrame &slot = slots_[static_cast<size_t>(head & mask_)];
        slot.fd = fd;
        slot.gen = gen;
        // payload.size() 已限制 <= 1024，窄化到 uint16_t 安全。
        slot.len = static_cast<uint16_t>(payload.size());
        if (!payload.empty()) {
            std::memcpy(slot.data.data(), payload.data(), payload.size());
        }
        head_.store(head + 1U, std::memory_order_release);
        return MailboxPush::kOk;
    }

    /**
     * @brief 从邮箱头部弹出一帧（仅消费者线程调用）
     * @param[out] out: 成功时写入帧内容；邮箱为空时保持不变
     * @retval true 成功弹出一帧
     * @retval false 邮箱未绑定或为空
     * @note 单消费者无锁调用，不阻塞、不分配堆，非 ISR。
     */
    [[nodiscard]] bool tryPop(WebFrame &out) noexcept
    {
        if (slots_.empty()) {
            return false;
        }
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = slots_[static_cast<size_t>(tail & mask_)];
        tail_.store(tail + 1U, std::memory_order_release);
        return true;
    }

    /**
     * @brief 查询邮箱当前是否为空
     * @retval true 当前无待取帧
     * @retval false 至少有一帧待取
     * @note 无锁原子快照，不阻塞。
     */
    [[nodiscard]] bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    /**
     * @brief 查询邮箱当前积压帧数
     * @retval 已入队未消费的帧数
     * @note 无锁原子快照，不阻塞。
     */
    [[nodiscard]] size_t size() const noexcept
    {
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(head - tail);
    }

    /**
     * @brief 查询邮箱总容量（槽位数）
     * @retval 已绑定槽位数；未绑定为 0
     * @note 只读查询，不阻塞。
     */
    [[nodiscard]] size_t capacity() const noexcept
    {
        return slots_.size();
    }

    /**
     * @brief 查询邮箱当前剩余空闲槽位数
     * @retval 可继续写入的槽位数
     * @note 无锁原子快照，不阻塞。
     */
    [[nodiscard]] size_t freeSlots() const noexcept
    {
        const size_t used = size();
        return (used < slots_.size()) ? (slots_.size() - used) : 0U;
    }

    /**
     * @brief 清空读写下标（仅启动前或双方停止后调用）
     * @note 不阻塞，非线程安全（仅静默态重置）。
     */
    void clear() noexcept
    {
        head_.store(0U, std::memory_order_relaxed);
        tail_.store(0U, std::memory_order_relaxed);
    }

private:
    std::span<WebFrame> slots_{};
    uint32_t mask_ = 0U;
    std::atomic<uint32_t> head_{0U};
    std::atomic<uint32_t> tail_{0U};
};

/** WebSocket 连接生命周期控制事件（httpd -> kLink，携带会话代号）。 */
struct WebCtrlEvent {
    enum class Kind : uint8_t {
        kNone = 0,
        kConnect,
        kDisconnect,
    };

    Kind kind = Kind::kNone;
    int fd = 0;
    uint32_t gen = 0U;
};

/** 静态定长 SPSC 控制事件邮箱；容量必须为 2 的幂。 */
template <size_t Capacity> class WebCtrlMailbox final {
public:
    static_assert(Capacity > 0U && ((Capacity & (Capacity - 1U)) == 0U),
                  "WebCtrlMailbox capacity must be a power of 2");
    static_assert(fw::kAtomicUsable<uint32_t>, "SPSC ctrl mailbox indices must be lock-free");

    constexpr WebCtrlMailbox() noexcept = default;
    ~WebCtrlMailbox() = default;
    WebCtrlMailbox(const WebCtrlMailbox &) = delete;
    WebCtrlMailbox &operator=(const WebCtrlMailbox &) = delete;

    /**
     * @brief 查询控制邮箱当前剩余空闲槽位数（用于升级前为断开事件预留空间）
     * @retval 剩余槽位数
     * @note 单生产者或只读快照，不阻塞。
     */
    [[nodiscard]] size_t freeSlots() const noexcept
    {
        const uint32_t head = head_.load(std::memory_order_acquire);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        const size_t used = static_cast<size_t>(head - tail);
        return (used < Capacity) ? (Capacity - used) : 0U;
    }

    /**
     * @brief 投递一条连接/断开控制事件（仅生产者调用）
     * @param event: 待入队的控制事件
     * @retval true 成功入队
     * @retval false 邮箱已满
     * @note 单生产者无锁调用，不阻塞、不分配堆，非 ISR。
     */
    [[nodiscard]] bool tryPush(const WebCtrlEvent &event) noexcept
    {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        if ((head - tail) >= static_cast<uint32_t>(Capacity)) {
            return false;
        }
        slots_[static_cast<size_t>(head & kMask)] = event;
        head_.store(head + 1U, std::memory_order_release);
        return true;
    }

    /**
     * @brief 弹出一条控制事件（仅消费者调用）
     * @param[out] out: 成功时写入事件；为空时保持不变
     * @retval true 成功弹出
     * @retval false 邮箱为空
     * @note 单消费者无锁调用，不阻塞、不分配堆，非 ISR。
     */
    [[nodiscard]] bool tryPop(WebCtrlEvent &out) noexcept
    {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = slots_[static_cast<size_t>(tail & kMask)];
        tail_.store(tail + 1U, std::memory_order_release);
        return true;
    }

    /**
     * @brief 清空读写下标（仅启动前或静默态调用）
     * @note 不阻塞，非线程安全。
     */
    void clear() noexcept
    {
        head_.store(0U, std::memory_order_relaxed);
        tail_.store(0U, std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kMask = static_cast<uint32_t>(Capacity - 1U);

    std::array<WebCtrlEvent, Capacity> slots_{};
    std::atomic<uint32_t> head_{0U};
    std::atomic<uint32_t> tail_{0U};
};

/**
 * @brief 轻量扫描 JSON 请求帧中的整数 id（1..65535），供邮箱满时在 httpd 线程回 busy
 * @param frame: 原始文本帧字节视图
 * @retval 1..65535 扫描到的有效请求 ID
 * @retval 0 未找到或格式不合法
 * @note 纯只读扫描，无大缓冲、无堆、不访问模块状态，线程安全。
 */
[[nodiscard]] inline uint16_t extractRequestId(std::span<const char> frame) noexcept
{
    constexpr std::string_view kIdKey = "\"id\"";
    constexpr uint32_t kDecimalBase = 10U;
    constexpr uint32_t kMaxId = 65535U;
    const std::string_view text{frame.data(), frame.size()};
    const size_t pos = text.find(kIdKey);
    if (pos == std::string_view::npos) {
        return 0U;
    }
    size_t idx = pos + kIdKey.size();
    while ((idx < text.size()) && ((text[idx] == ' ') || (text[idx] == '\t') ||
                                   (text[idx] == '\r') || (text[idx] == '\n'))) {
        ++idx;
    }
    if ((idx >= text.size()) || (text[idx] != ':')) {
        return 0U;
    }
    ++idx;
    while ((idx < text.size()) && ((text[idx] == ' ') || (text[idx] == '\t') ||
                                   (text[idx] == '\r') || (text[idx] == '\n'))) {
        ++idx;
    }
    if ((idx >= text.size()) || (text[idx] < '1') || (text[idx] > '9')) {
        return 0U;
    }
    uint32_t value = 0U;
    while ((idx < text.size()) && (text[idx] >= '0') && (text[idx] <= '9')) {
        value = (value * kDecimalBase) + static_cast<uint32_t>(text[idx] - '0');
        if (value > kMaxId) {
            return 0U;
        }
        ++idx;
    }
    // value 已限制在 1..65535，窄化到 uint16_t 安全。
    return static_cast<uint16_t>(value);
}

} // namespace app::web_cfg
