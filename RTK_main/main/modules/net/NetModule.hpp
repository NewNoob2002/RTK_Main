// P2 4G 联网模块（ADR-0021 / N1）：EC600U 的 UART1 驱动、GPIO26 PWREN、AtEngine/LinkFsm 胶水。
// 线程模型：init() 启动前单线程；start()/wait()/poll()/onCommand 仅 kNet 执行器任务；
// 关机参与者回调与控制台 AT 邮箱接口可跨任务调用，只经原子量与单写者缓冲交互，不阻塞。
// 资源（ADR-0021 §2.1 / N1 复审 #11）：只在 kCellular 板型 + net.enable=1 + 非配网时启动期分配 NetBlock
// （执行器、AtEngine/LinkFsm 与邮箱），其余情况本类仅占数十字节静态 DRAM，不装 UART、不建任务。
// 堆档位：启动期一次性（UART 驱动在 init 安装；运行期无堆分配）。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "board/PowerLogic.hpp"
#include "cellular/AtEngine.hpp"
#include "cellular/LinkFsm.hpp"
#include "cli/Console.hpp"
#include "def/NetDef.hpp"
#include "fw/Command.hpp"
#include "fw/Result.hpp"
#include "fw/Status.hpp"
#include "modules/net/NetMailbox.hpp"
#include "modules/net/NetPolicy.hpp"
#include "modules/net/NetStore.hpp"

namespace app {
struct AppContext;
struct ModuleTestAccess;

/** 4G 联网模块：板型含 Feature::kCellular 且已分配运行块时驱动 EC600U 到 Ready 并维持恢复；否则不在位。 */
class NetModule final {
    friend struct ModuleTestAccess;

public:
    static constexpr uint32_t kUartRxBufBytes =
        2048U; // B，UART1 驱动 RX 环形缓冲（460800 bps 约 44 ms）
    // TX 环形缓冲为 0：uart_write_bytes 只阻塞到字节写进 128 B 硬件 FIFO（有界，随波特率），
    // 不再有 portMAX_DELAY 的环形缓冲等待，同时省 512 B DRAM。
    static constexpr uint32_t kUartTxBufBytes = 0U;
    static constexpr int kUartQueueLen = 16;           // UART 事件队列深度
    static constexpr uint32_t kPublishPeriodMs = 500U; // ms，DataCenter 快照发布周期
    static constexpr uint32_t kWriteTimeoutMs = 50U;   // ms，写 UART 后等待发送完成的上限
    static constexpr size_t kRxChunkBytes = 128U;      // B，单次读 UART 的块大小

    using AtState = NetMailbox::State;

    /** kNet 运行态：启动期分配（NetBlock），禁用时不存在。 */
    struct Runtime {
        cellular::AtEngine engine{};
        cellular::LinkFsm fsm{};
        net_store::Saved cfg{};
        NetMailbox mailbox{};
        NetPowerGate gate{};
        std::array<uint8_t, kRxChunkBytes> rxChunk{};
        void *uartQueue = nullptr; // QueueHandle_t，头文件不引入 FreeRTOS
        uint32_t uartOverflows = 0U;
        uint32_t shutdownDeadlineMs = 0U; // kNet 记录的 PW1 绝对截止时刻
        uint32_t lastPublishMs = 0U;
        cellular::Phase lastPhase = cellular::Phase::kOff;
        bool uartInstalled = false;
        bool forcedOn = false; // DEV net power on
        bool devOff = false;   // DEV net power off
        bool shutdownStarted = false;
        bool published = false;
        // 跨任务原子量：kLink（PW1）写 shutdownReq/StartMs，kNet 写 shutdownDone/phaseMirror。
        std::atomic<bool> shutdownReq{false};
        std::atomic<bool> shutdownDone{false};
        std::atomic<uint32_t> shutdownStartMs{0U};
        std::atomic<uint8_t> phaseMirror{static_cast<uint8_t>(cellular::Phase::kOff)};
    };

    constexpr NetModule() noexcept = default;
    ~NetModule() = default;
    NetModule(const NetModule &) = delete;
    NetModule &operator=(const NetModule &) = delete;
    NetModule(NetModule &&) = delete;
    NetModule &operator=(NetModule &&) = delete;

    /**
     * @brief 绑定所属应用上下文
     * @param context: 固件唯一应用上下文，生命周期覆盖模块
     * @note 仅启动前单线程调用，不分配、不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }

    /**
     * @brief 绑定启动期分配的运行态；未绑定（禁用/E1）时 init 为空操作且 present() 恒为 false
     * @param runtime: 已构造的运行态，须活到重启
     * @note 仅启动前单线程调用，不分配、不阻塞，非 ISR。
     */
    void bindRuntime(Runtime &runtime) noexcept
    {
        rt_ = &runtime;
    }

    /**
     * @brief 启动前单线程：读取 net.* 持久化配置、安装 UART1、配置 PWREN 输出并绑定 AT 引擎与状态机
     * @retval fw::Result::kOk 初始化完成，或未分配运行态/板型不含 Feature::kCellular（空操作）
     * @retval fw::Result::kInvalidState 重复初始化
     * @retval 其他 UART/GPIO 驱动错误映射
     * @note 仅启动任务调用，阻塞于 NVS 与驱动安装，非 ISR；失败时 initFailed 已回滚。
     */
    [[nodiscard]] fw::Result init() noexcept;

    /**
     * @brief 执行器启动阶段回调（无额外资源）
     * @retval fw::Result::kOk 总是成功
     * @note 仅 kNet 任务，非 ISR。
     */
    [[nodiscard]] fw::Result start() noexcept
    {
        return fw::Result::kOk;
    }

    /**
     * @brief kNet 执行器 idle 等待：阻塞在 UART 事件队列上，直至有数据、下一个定时点或 maxMs
     * @param maxMs: 执行器允许的最长等待毫秒
     * @note 仅 kNet 任务；FIFO 溢出/缓冲满时清输入、计数并通知 AtEngine 丢失字节，非 ISR。
     */
    void wait(uint32_t maxMs) noexcept;

    /**
     * @brief 非阻塞驱动：电源策略、关机序列、AT 邮箱、UART 收发、状态机与快照发布
     * @param nowMs: 当前单调时钟毫秒
     * @retval true 仍有积压（UART 还有字节待读），执行器应尽快再次调用
     * @retval false 本轮无积压
     * @note 仅 kNet 任务，单轮不超过 roundBudget，非 ISR。
     */
    [[nodiscard]] bool poll(uint32_t nowMs) noexcept;

    /**
     * @brief 处理 DEV 电源命令（net power on/off、net reset）；net.* 设置由 StorageModule 持久化
     * @param cmd: 命令（verb 见 NetVerb）
     * @param[out] res: 结果（status 与 detail）
     * @note 仅 kNet 任务，不阻塞，非 ISR。
     */
    void onCommand(const fw::Command &cmd, fw::CommandResult &res) noexcept;

    /**
     * @brief 释放 UART1 驱动并把 PWREN 拉低（启动回滚/测试复位用）
     * @retval fw::Status::kOk 已释放或本就未安装
     * @note 仅启动任务在所有执行器停止后调用，非 ISR。
     */
    [[nodiscard]] fw::Status stopBoot() noexcept;

    /**
     * @brief 向电源模块登记 PW1 kQuiesce 关机参与者（仅板型在位且 init 成功时登记）
     * @retval fw::Status::kOk 已登记，或模块不在位无需登记
     * @retval 其他 registerShutdownParticipant 返回的错误
     * @note 仅启动前单线程调用，非 ISR。
     */
    [[nodiscard]] fw::Status registerShutdown() noexcept;

    /**
     * @brief 模块在位（运行态已分配、板型含 kCellular 且 init 成功）
     * @retval true 在位
     * @note 任意任务，原子读。
     */
    [[nodiscard]] bool present() const noexcept
    {
        return present_.load(std::memory_order_acquire);
    }

    /**
     * @brief 提交一条控制台 AT 透传命令（DEV）；一次只允许一条在途
     * @param text: AT 命令文本（不含 CR，长度 < kNetAtCmdBytes）
     * @retval fw::Status::kOk 已提交
     * @retval fw::Status::kInvalidArg 文本为空、过长或被 NetMailbox::forbidden 拒绝
     * @retval fw::Status::kBusy 上一条尚未释放（含旧请求仍在执行）
     * @retval fw::Status::kInvalidState 模块不在位、UART 未上电或关机已开始
     * @note 仅 console 任务调用，不阻塞，非 ISR；只读原子镜像，不碰 FSM 非原子状态。
     */
    [[nodiscard]] fw::Status submitAt(std::string_view text) noexcept;

    /**
     * @brief 读取 AT 邮箱状态
     * @retval 当前状态；kDone 时可用 atResponse() 读取；未在位恒为 kIdle
     * @note 任意任务，原子读。
     */
    [[nodiscard]] AtState atState() const noexcept
    {
        return (rt_ != nullptr) ? rt_->mailbox.state() : AtState::kIdle;
    }

    /**
     * @brief 读取 AT 响应（仅 atState()==kDone 时有效）
     * @param[out] ok: 命令是否以 OK 结束
     * @retval 响应文本视图（行以 '\n' 分隔，超长截断，凭据行已脱敏）
     * @note 仅 console 任务；读完须调用 releaseAt()。
     */
    [[nodiscard]] std::string_view atResponse(bool &ok) const noexcept
    {
        ok = false;
        return (rt_ != nullptr) ? rt_->mailbox.response(ok) : std::string_view{};
    }

    /**
     * @brief 释放 AT 邮箱，允许下一次提交；执行中的请求只标记放弃，终结后才复用缓冲
     * @note 仅 console 任务，任意状态可调用（取消等待时也用）。
     */
    void releaseAt() noexcept
    {
        if (rt_ != nullptr) {
            rt_->mailbox.release();
        }
    }

    /**
     * @brief 判断 AT 命令是否被禁止（会改变通道约定、串联命令或进入数据模式的命令）
     * @param text: AT 命令文本
     * @retval true 禁止；规则见 NetMailbox::forbidden
     * @note 纯函数，大小写不敏感，线程/ISR 安全。
     */
    [[nodiscard]] static bool atForbidden(std::string_view text) noexcept
    {
        return NetMailbox::forbidden(text);
    }

private:
    static void powerHook(void *ctx, bool on) noexcept;
    static bool baudHook(void *ctx, uint32_t baud) noexcept;
    static bool writeHook(void *ctx, std::span<const uint8_t> bytes) noexcept;
    static void urcHook(void *ctx, std::string_view line) noexcept;
    static void connOpened(void *ctx, uint8_t id, bool ok) noexcept;
    static void connClosed(void *ctx, uint8_t id) noexcept;
    static void connData(void *ctx, uint8_t id, std::span<const uint8_t> data) noexcept;
    static void connSendDone(void *ctx, uint8_t id, bool ok) noexcept;
    static void atLineThunk(void *ctx, std::string_view line) noexcept;
    static void atDoneThunk(void *ctx, const cellular::AtResponse &rsp) noexcept;
    static void shutdownBegin(board::ShutdownStage stage, void *ctx) noexcept;
    static bool shutdownIsDone(void *ctx) noexcept;
    static void shutdownTimeout(void *ctx) noexcept;

    [[nodiscard]] fw::Result initUart() noexcept;
    [[nodiscard]] fw::Result initFailed(fw::Result cause) noexcept;
    /** 拉低 PWREN（受控硬切）；返回 gpio_set_level 是否成功。任意任务可调用。 */
    [[nodiscard]] bool cutPower() const noexcept;
    void applyConfig() noexcept;
    void updatePolicy(uint32_t nowMs) noexcept;
    void serviceShutdown(uint32_t nowMs) noexcept;
    void serviceAtMailbox(uint32_t nowMs) noexcept;
    [[nodiscard]] bool drainUart(uint32_t nowMs) noexcept;
    void publishStatus(uint32_t nowMs) noexcept;

    AppContext *context_ = nullptr;
    Runtime *rt_ = nullptr;
    std::atomic<bool> present_{false};
    bool initialized_ = false;
    bool shutdownRegistered_ = false;
    int uartPort_ = -1;
    int pwrenGpio_ = -1;
};

/**
 * @brief 取得 net 控制台命令表（status 所有档位；power/reset/at 仅 DEV 档可执行）
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取，不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Cmd> netCmds() noexcept;

/**
 * @brief 取得 net 子命令动词表
 * @retval 固件生命周期有效的静态只读表视图
 * @note 启动任务登记时读取，不分配、不阻塞，非 ISR。
 */
[[nodiscard]] std::span<const cli::Verb> netVerbs() noexcept;

} // namespace app
