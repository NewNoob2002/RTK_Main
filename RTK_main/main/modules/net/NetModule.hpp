// P2 4G 联网模块（ADR-0021 / N1）：EC600U 的 UART1 驱动、GPIO26 PWREN、AtEngine/LinkFsm 胶水。
// 线程模型：init() 启动前单线程；start()/wait()/poll()/onCommand 仅 kNet 执行器任务；
// 关机参与者回调与控制台 AT 邮箱接口可跨任务调用，只经原子量与单写者缓冲交互，不阻塞。
// 堆档位：启动期一次性（UART 驱动在 init 安装；运行期无堆分配）。仅 Feature::kCellular 板型工作，其余板型零触碰。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "cellular/AtEngine.hpp"
#include "cli/Console.hpp"
#include "cellular/LinkFsm.hpp"
#include "board/PowerLogic.hpp"
#include "def/NetDef.hpp"
#include "fw/Command.hpp"
#include "fw/Result.hpp"
#include "fw/Status.hpp"

namespace app {
struct AppContext;
struct ModuleTestAccess;

/** 4G 联网模块：板型含 Feature::kCellular 时驱动 EC600U 到 Ready 并维持恢复；其余板型 init 为空操作。 */
class NetModule final {
    friend struct ModuleTestAccess;

public:
    static constexpr uint32_t kUartRxBufBytes =
        2048U; // B，UART1 驱动 RX 环形缓冲（460800 bps 约 44 ms）
    static constexpr uint32_t kUartTxBufBytes =
        512U;                                // B，UART1 驱动 TX 环形缓冲（≥ 单次 QISEND 载荷）
    static constexpr int kUartQueueLen = 16; // UART 事件队列深度
    static constexpr uint32_t kPublishPeriodMs = 500U; // ms，DataCenter 快照发布周期
    static constexpr uint8_t kLowBatteryResumeMargin =
        5U; // %，低电量禁用后需回升到阈值+该余量才允许再次上电

    /** 控制台 AT 透传邮箱状态。 */
    enum class AtState : uint8_t {
        kIdle = 0, ///< 空闲，可提交
        kFilling,  ///< 提交方正在写命令文本
        kPending,  ///< 已提交，等待 kNet 取走
        kRunning,  ///< 已提交到 AT 引擎，等待应答
        kDone,     ///< 已完成，响应可读，读完须 releaseAt()
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
     * @note 仅启动前单线程调用，不阻塞，非 ISR。
     */
    void bindContext(AppContext &context) noexcept
    {
        context_ = &context;
    }

    /**
     * @brief 启动前单线程：读取 net.* 持久化配置、安装 UART1、配置 PWREN 输出并绑定 AT 引擎与状态机
     * @retval fw::Result::kOk 初始化完成，或板型不含 Feature::kCellular（空操作）
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
     * @note 仅 kNet 任务；处理 FIFO 溢出/缓冲满事件（清输入并计数），非 ISR。
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
     * @brief 处理 net.* 设置写入与 DEV 电源命令
     * @param cmd: 命令（verb 见 NetVerb）
     * @param[out] res: 结果（status 与 detail）
     * @note 仅 kNet 任务；写入 NVS 可阻塞数毫秒（设置写入非高频），非 ISR。
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
     * @brief 模块在位（板型含 kCellular 且 init 成功）
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
     * @retval fw::Status::kInvalidArg 文本为空、过长或属于禁止命令（ATE1/AT+IPR/AT+QISWTMD）
     * @retval fw::Status::kBusy 上一条尚未释放
     * @retval fw::Status::kInvalidState 模块不在位或 UART 未上电
     * @note 仅 console 任务调用，不阻塞，非 ISR。
     */
    [[nodiscard]] fw::Status submitAt(std::string_view text) noexcept;

    /**
     * @brief 读取 AT 邮箱状态
     * @retval 当前状态；kDone 时可用 atResponse() 读取
     * @note 任意任务，原子读。
     */
    [[nodiscard]] AtState atState() const noexcept
    {
        return static_cast<AtState>(atState_.load(std::memory_order_acquire));
    }

    /**
     * @brief 读取 AT 响应（仅 atState()==kDone 时有效）
     * @param[out] ok: 命令是否以 OK 结束
     * @retval 以 0 结尾的响应文本视图（行以 '\n' 分隔，超长截断）
     * @note 仅 console 任务；读完须调用 releaseAt()。
     */
    [[nodiscard]] std::string_view atResponse(bool &ok) const noexcept
    {
        ok = atOk_;
        return std::string_view(atRsp_.data(), atRspLen_);
    }

    /**
     * @brief 释放 AT 邮箱，允许下一次提交
     * @note 仅 console 任务，任意状态可调用（取消等待时也用）。
     */
    void releaseAt() noexcept
    {
        atState_.store(static_cast<uint8_t>(AtState::kIdle), std::memory_order_release);
    }

    /**
     * @brief 判断 AT 命令是否被禁止（会改变通道约定的命令）
     * @param text: AT 命令文本
     * @retval true 禁止：ATE1、AT+IPR、AT+QISWTMD
     * @note 纯函数，大小写不敏感，线程/ISR 安全。
     */
    [[nodiscard]] static bool atForbidden(std::string_view text) noexcept;

private:
    /** 配置的持久化键与运行态合并视图（仅 kNet 任务读写）。 */
    struct Config {
        bool enable = false;
        std::array<char, cellular::LinkConfig::kFieldBytes> apn{};
        std::array<char, cellular::LinkConfig::kFieldBytes> user{};
        std::array<char, cellular::LinkConfig::kFieldBytes> pass{};
    };

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

    [[nodiscard]] fw::Result initUart() noexcept;
    [[nodiscard]] fw::Result initFailed(fw::Result cause) noexcept;
    void loadConfig() noexcept;
    void applyConfig() noexcept;
    void publishConfig() noexcept;
    [[nodiscard]] fw::Status persistText(uint8_t verb) noexcept;
    void updatePolicy(uint32_t nowMs) noexcept;
    void serviceShutdown(uint32_t nowMs) noexcept;
    void serviceAtMailbox(uint32_t nowMs) noexcept;
    [[nodiscard]] bool drainUart(uint32_t nowMs) noexcept;
    void publishStatus(uint32_t nowMs) noexcept;
    void onAtLine(std::string_view line) noexcept;
    void onAtDone(const cellular::AtResponse &rsp) noexcept;

    AppContext *context_ = nullptr;
    std::atomic<bool> present_{false};
    bool initialized_ = false;
    bool uartInstalled_ = false;
    int uartPort_ = -1;
    int pwrenGpio_ = -1;
    void *uartQueue_ = nullptr; // QueueHandle_t，头文件不引入 FreeRTOS

    cellular::AtEngine engine_{};
    cellular::LinkFsm fsm_{};
    Config cfg_{};
    bool forcedOn_ = false; // DEV net power on：忽略 net.enable 强制上电
    bool devOff_ = false;   // DEV net power off：保持关闭
    bool lowBatteryLatched_ = false;
    bool lastPowerAllowed_ = true;
    bool lastEnabled_ = false;
    uint32_t lastPublishMs_ = 0U;
    bool published_ = false;
    cellular::Phase lastPhase_ = cellular::Phase::kOff;
    std::array<uint8_t, 256> rxChunk_{};
    uint32_t uartOverflows_ = 0U;

    // 关机序列：begin（kLink）置位请求，kNet 执行并置位完成；二者只经原子量交互。
    std::atomic<bool> shutdownReq_{false};
    std::atomic<bool> shutdownDone_{false};
    bool shutdownStarted_ = false;
    bool shutdownRegistered_ = false;

    // 控制台 AT 邮箱：console 单写命令、kNet 单写响应，状态量发布数据所有权。
    std::atomic<uint8_t> atState_{static_cast<uint8_t>(AtState::kIdle)};
    std::array<char, kNetAtCmdBytes> atCmd_{};
    std::array<char, kNetAtRspBytes> atRsp_{};
    size_t atCmdLen_ = 0U;
    size_t atRspLen_ = 0U;
    bool atOk_ = false;
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
