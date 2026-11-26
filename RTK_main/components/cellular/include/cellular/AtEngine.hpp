// AT 命令引擎（ADR-0021 §2.4）：单在途命令队列、行分帧、OK/ERROR/+CME ERROR 终结、URC 分发、
// '>' 提示符发送数据、定长原始数据读取、逐命令超时与连续无响应判通道失效。
// 纯 C++20，无 IDF/FreeRTOS 头；非线程安全，只能由 kNet 任务（单线程）调用；不阻塞、不分配堆，所有超时用 tick 比较。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace cellular {

/** 命令最终结果。 */
enum class AtResult : uint8_t {
    kOk = 0,
    kError,    // ERROR / SEND FAIL
    kCmeError, // +CME ERROR: <code> 或 +CMS ERROR
    kTimeout,  // 超时无终结行
    kAborted,  // 通道失效/复位被取消，不计入无响应
};

/** 命令完成通知内容。 */
struct AtResponse final {
    AtResult result = AtResult::kAborted;
    int32_t errorCode = 0; // kCmeError 时为错误码，否则 0
    uint32_t nowMs = 0U;   // 完成时刻（引擎收到的 tick），供上层无需另存时间
};

/** 中间响应行回调（不含 CR/LF，不含终结行与 URC）。 */
using AtLineFn = void (*)(void *ctx, std::string_view line) noexcept;
/** 定长原始数据回调（rawPrefix 命中后紧随的 N 字节，可能分多块到达）。 */
using AtRawFn = void (*)(void *ctx, std::span<const uint8_t> chunk) noexcept;
/** 命令完成回调；回调内允许再次提交命令。 */
using AtDoneFn = void (*)(void *ctx, const AtResponse &rsp) noexcept;
/** URC 回调：整行文本（不含 CR/LF）。 */
using AtUrcFn = void (*)(void *ctx, std::string_view line) noexcept;
/** 串口写出钩子：写入成功返回 true。 */
using AtWriteFn = bool (*)(void *ctx, std::span<const uint8_t> bytes) noexcept;

/** 待提交的 AT 命令描述；text 被复制进队列槽，payload 指向的内存须保持有效直到完成回调。 */
/** AT 命令缺省超时与通道失效缺省阈值。 */
inline constexpr uint32_t kDefaultTimeoutMs = 1000U;
inline constexpr uint8_t kDefaultDeadThreshold = 3U;

/** 待提交的 AT 命令描述（见上）。 */
struct AtCommand final {
    std::string_view text{};                // 不含结尾 CR 的命令文本，如 "AT+CSQ"
    uint32_t timeoutMs = kDefaultTimeoutMs; // 从命令发出起算的总超时
    AtLineFn onLine = nullptr;              // 中间响应行
    AtRawFn onRaw = nullptr;                // 定长原始数据（需配合 rawPrefix）
    AtDoneFn onDone = nullptr;              // 完成通知
    void *ctx = nullptr;                    // 上述回调的用户上下文
    std::span<const uint8_t> payload{};     // 非空：收到 '>' 提示符后发送的定长数据（≤512 B）
    std::string_view rawPrefix{};           // 非空：该前缀行 "+QIRD: <n>" 之后紧随 n 字节原始数据
};

/** 端口配置：写出钩子与 URC 前缀表。 */
struct AtPort final {
    void *ctx = nullptr;
    AtWriteFn write = nullptr;
    AtUrcFn onUrc = nullptr;
    std::span<const std::string_view> urcPrefixes{}; // 行以任一前缀开头即判为 URC
};

/** 引擎统计（用于控制台与测试断言）。 */
struct AtStats final {
    uint32_t lines = 0U;      // 收到的非空行
    uint32_t urcs = 0U;       // 分发的 URC 行
    uint32_t stray = 0U;      // 无命令在途且非 URC 的游离行
    uint32_t overflow = 0U;   // 超长被丢弃的行
    uint32_t timeouts = 0U;   // 命令超时总次数
    uint32_t writeFails = 0U; // 写出失败次数
};

/** AT 命令引擎。 */
class AtEngine final {
public:
    /** 行缓冲容量（含全部有效字符，超长行丢弃）。 */
    static constexpr size_t kLineBytes = 512U;
    /** 单条命令文本容量（含结尾 CR 占位）。 */
    static constexpr size_t kCmdBytes = 128U;
    /** 命令队列深度（含在途命令）。 */
    static constexpr size_t kQueueDepth = 3U;
    /** 提示符数据与单次 QISEND 上限。 */
    static constexpr size_t kMaxPayload = 512U;

    constexpr AtEngine() noexcept = default;
    ~AtEngine() = default;
    AtEngine(const AtEngine &) = delete;
    AtEngine &operator=(const AtEngine &) = delete;
    AtEngine(AtEngine &&) = delete;
    AtEngine &operator=(AtEngine &&) = delete;

    /**
     * @brief  绑定端口（写出钩子与 URC 表）
     * @param  port: 端口配置，urcPrefixes 指向的静态表须在引擎生命周期内有效
     * @note   启动期单线程调用，不阻塞、不分配堆。
     */
    void bind(const AtPort &port) noexcept;

    /**
     * @brief  提交一条命令；队列空闲时立即发出
     * @param  cmd:   命令描述
     * @param  nowMs: 当前 tick 毫秒
     * @retval true  已入队（或已发出）
     * @retval false 通道已失效、队列满、文本过长/为空或 payload 超限
     * @note   仅 kNet 任务；不阻塞、不分配堆；可在完成回调内调用。
     */
    [[nodiscard]] bool submit(const AtCommand &cmd, uint32_t nowMs) noexcept;

    /**
     * @brief  喂入串口接收字节，驱动分帧、URC 分发、完成回调与下一条命令发送
     * @param  bytes: 刚收到的字节
     * @param  nowMs: 当前 tick 毫秒
     * @note   仅 kNet 任务；不阻塞、不分配堆。
     */
    void feed(std::span<const uint8_t> bytes, uint32_t nowMs) noexcept;

    /**
     * @brief  检查在途命令超时并启动队列中的下一条
     * @param  nowMs: 当前 tick 毫秒
     * @note   仅 kNet 任务；不阻塞、不分配堆。
     */
    void poll(uint32_t nowMs) noexcept;

    /**
     * @brief  取消全部命令（以 kAborted 回调）、清除分帧状态与失效标记；模块断电重启后调用
     * @param  nowMs: 当前 tick 毫秒
     * @note   仅 kNet 任务；回调内不得再提交命令（提交会被下一次 reset 清除）。
     */
    void reset(uint32_t nowMs) noexcept;

    /**
     * @brief  距在途命令超时的剩余毫秒
     * @param  nowMs: 当前 tick 毫秒
     * @retval 剩余毫秒（已超时返回 0）；无在途命令返回 UINT32_MAX
     * @note   纯查询，供 kNet 计算等待超时。
     */
    [[nodiscard]] uint32_t msToDeadline(uint32_t nowMs) const noexcept;

    /**
     * @brief  通道是否已判失效（连续无响应达阈值）
     * @retval true 已失效，须 reset 前不再接受命令
     * @note   纯查询。
     */
    [[nodiscard]] bool dead() const noexcept
    {
        return dead_;
    }

    /**
     * @brief  是否有命令在途或排队
     * @retval true 队列非空
     * @note   纯查询。
     */
    [[nodiscard]] bool busy() const noexcept
    {
        return count_ != 0U;
    }

    /**
     * @brief  排队（含在途）命令数
     * @retval 命令数
     * @note   纯查询。
     */
    [[nodiscard]] size_t pending() const noexcept
    {
        return count_;
    }

    /**
     * @brief  读取统计计数
     * @retval 统计快照引用
     * @note   纯查询。
     */
    [[nodiscard]] const AtStats &stats() const noexcept
    {
        return stats_;
    }

    /**
     * @brief  设置连续无响应判失效阈值（默认 3）
     * @param  threshold: 阈值，0 视为 1
     * @note   启动期调用。
     */
    void setDeadThreshold(uint8_t threshold) noexcept
    {
        deadThreshold_ = (threshold == 0U) ? 1U : threshold;
    }

private:
    /** 引擎接收状态。 */
    enum class State : uint8_t {
        kIdle = 0,
        kWaitResponse,
        kWaitPrompt,
        kRaw,
    };

    /** 队列槽：命令文本副本与回调。 */
    struct Slot final {
        std::array<char, kCmdBytes> text{};
        uint8_t len = 0U; // 不含 CR
        uint32_t timeoutMs = 0U;
        AtLineFn onLine = nullptr;
        AtRawFn onRaw = nullptr;
        AtDoneFn onDone = nullptr;
        void *ctx = nullptr;
        std::span<const uint8_t> payload{};
        std::string_view rawPrefix{};
    };

    void startFront(uint32_t nowMs) noexcept;
    void finish(AtResult result, int32_t code, uint32_t nowMs) noexcept;
    void abortAll(uint32_t nowMs) noexcept;
    void onByte(uint8_t byte, uint32_t nowMs) noexcept;
    void handleLine(std::string_view line, uint32_t nowMs) noexcept;
    [[nodiscard]] bool isUrc(std::string_view line) const noexcept;
    [[nodiscard]] bool belongsToFront(std::string_view line) const noexcept;
    void dispatchUrc(std::string_view line) noexcept;

    AtPort port_{};
    std::array<Slot, kQueueDepth> queue_{};
    size_t head_ = 0U;
    size_t count_ = 0U;
    State state_ = State::kIdle;
    uint32_t deadlineMs_ = 0U;
    std::array<char, kLineBytes> line_{};
    size_t lineLen_ = 0U;
    bool overflow_ = false;
    bool skipSpace_ = false;
    size_t rawRemaining_ = 0U;
    uint8_t timeouts_ = 0U;
    uint8_t deadThreshold_ = kDefaultDeadThreshold;
    bool dead_ = false;
    AtStats stats_{};
};

} // namespace cellular
