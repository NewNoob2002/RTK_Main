// 4G 链路状态机（ADR-0021 §2.5/§2.9）：Off→PowerOn→Sync→SimCheck→Register→PdpUp→Ready，
// L1 连接级 / L2 承载级 / L3 模块级恢复与退避、Ready 巡检、关机序列、socket 连接槽框架。
// 纯 C++20，无 IDF/FreeRTOS 头；非线程安全，只能由 kNet 任务调用；不阻塞、不分配堆，超时全部为 tick 比较。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "cellular/AtEngine.hpp"
#include "cellular/Policy.hpp"
#include "cellular/Types.hpp"

namespace cellular {

/** 链路对硬件的钩子（由 NetModule 提供，主机测试用假实现）。 */
struct LinkHooks final {
    void *ctx = nullptr;
    bool (*setPower)(void *ctx,
                     bool on) noexcept = nullptr; // 控制 PWREN（GPIO26），返回 GPIO 是否成功
    bool (*setBaud)(void *ctx, uint32_t baud) noexcept = nullptr; // 切换 UART1 波特率
};

/** socket 连接事件回调（NTRIP/MQTT 等上层使用，N1 只提供框架）。 */
struct ConnEvents final {
    void *ctx = nullptr;
    void (*onOpened)(void *ctx, uint8_t id, bool ok) noexcept = nullptr;
    void (*onClosed)(void *ctx, uint8_t id) noexcept = nullptr;
    void (*onData)(void *ctx, uint8_t id, std::span<const uint8_t> data) noexcept = nullptr;
    void (*onSendDone)(void *ctx, uint8_t id, bool ok) noexcept = nullptr;
};

/** 运营商配置：APN 为空则不下发 AT+QICSGP。 */
struct LinkConfig final {
    static constexpr size_t kFieldBytes = 32U;
    std::array<char, kFieldBytes> apn{};
    std::array<char, kFieldBytes> user{};
    std::array<char, kFieldBytes> pass{};
    uint8_t auth = 0U; // 0 无 / 1 PAP / 2 CHAP
};

/** 4G 链路状态机。 */
class LinkFsm final {
public:
    /** 连接目标主机名最大长度（含结尾 0）。 */
    static constexpr size_t kHostBytes = 64U;

    constexpr LinkFsm() noexcept = default;
    ~LinkFsm() = default;
    LinkFsm(const LinkFsm &) = delete;
    LinkFsm &operator=(const LinkFsm &) = delete;
    LinkFsm(LinkFsm &&) = delete;
    LinkFsm &operator=(LinkFsm &&) = delete;

    /**
     * @brief  绑定 AT 引擎、硬件钩子与连接事件回调
     * @param  engine: AT 引擎，生命周期须覆盖本对象
     * @param  hooks:  硬件钩子
     * @param  events: 连接事件回调（可全空）
     * @note   启动期单线程调用；本函数不改变电源状态。
     */
    void bind(AtEngine &engine, const LinkHooks &hooks, const ConnEvents &events) noexcept;

    /**
     * @brief  设置运营商配置（APN/用户名/密码），下一次进入 PdpUp 时生效
     * @param  apn:  APN 文本（超长截断）
     * @param  user: 用户名
     * @param  pass: 密码
     * @param  auth: 认证类型 0/1/2
     * @note   仅 kNet 任务；不分配堆。
     */
    void setConfig(std::string_view apn, std::string_view user, std::string_view pass,
                   uint8_t auth) noexcept;

    /**
     * @brief  使能/禁用 4G（net.enable）；禁用时走优雅关闭序列
     * @param  enabled: 是否允许上电
     * @note   仅 kNet 任务。
     */
    void setEnabled(bool enabled) noexcept
    {
        enabled_ = enabled;
    }

    /**
     * @brief  外部电源策略（低电量、Wi-Fi 配网模式）是否允许上电
     * @param  allowed: false 时不上电，已上电则优雅关闭
     * @note   仅 kNet 任务。
     */
    void setPowerAllowed(bool allowed) noexcept
    {
        powerAllowed_ = allowed;
    }

    /**
     * @brief  是否允许“新上电”（低电量迟滞）；只拦 Off→上电，不影响已在线的模块
     * @param  allowed: false 时 Off 阶段不上电；在线模块保持，直到 PW1 低电关机流程
     * @note   仅 kNet 任务。
     */
    void setStartAllowed(bool allowed) noexcept
    {
        startAllowed_ = allowed;
    }

    /**
     * @brief  开始关机序列：QICLOSE → AT+QPOWD=1 → 等 POWERED DOWN → 拉低 PWREN，总计 ≤2 s
     * @param  nowMs: 当前 tick 毫秒
     * @param  deadlineMs: PW1 给出的绝对截止时刻；0 表示以 nowMs + 2 s 为准。kNet 晚于 PW1 起步时
     *                     必须沿用同一截止时刻，不得重新计 2 s
     * @note   PW1 kQuiesce 参与者调用（经 kNet 任务）；AT 通道失效或模块未起来则直接断电。
     */
    void beginShutdown(uint32_t nowMs, uint32_t deadlineMs = 0U) noexcept;

    /**
     * @brief  关机序列是否已完成（PWREN 已拉低）
     * @retval true 已断电或本就关闭
     * @note   纯查询。
     */
    [[nodiscard]] bool shutdownDone() const noexcept
    {
        return !shuttingDown_ && (phase_ == Phase::kOff);
    }

    /**
     * @brief  手动触发 L3 模块复位（控制台 net reset）
     * @param  nowMs: 当前 tick 毫秒
     * @retval true  已进入断电保持
     * @retval false 当前未上电或在关机中
     * @note   仅 kNet 任务。
     */
    bool requestReset(uint32_t nowMs) noexcept;

    /**
     * @brief  推进状态机：定时、命令提交、超时检查
     * @param  nowMs: 当前 tick 毫秒
     * @note   仅 kNet 任务；不阻塞、不分配堆。须在 engine.poll 之后调用。
     */
    void poll(uint32_t nowMs) noexcept;

    /**
     * @brief  处理 AT 引擎分发的 URC 行
     * @param  line:  URC 整行
     * @param  nowMs: 当前 tick 毫秒
     * @note   仅 kNet 任务；不阻塞。
     */
    void onUrc(std::string_view line, uint32_t nowMs) noexcept;

    /**
     * @brief  调试通道（控制台 AT）应答行旁路：非 READY 的 +CPIN: 同步给 FSM 的 SIM 状态
     * @param  line: 应答整行
     * @note   仅 kNet 任务；在引擎行回调内调用，故只记录，由下一次 poll 处理（避免回调内重置引擎）。
     *         READY 不在此处理（由 FSM 自己的查询确认）。
     */
    void onDiagLine(std::string_view line) noexcept;

    /**
     * @brief  距下一次需要运行 poll 的毫秒数（供 kNet 计算队列等待超时）
     * @param  nowMs: 当前 tick 毫秒
     * @retval 毫秒，0 表示立即；无定时需求返回 UINT32_MAX
     * @note   纯查询。
     */
    [[nodiscard]] uint32_t msToNextWork(uint32_t nowMs) const noexcept;

    /**
     * @brief  生成当前状态快照（含剩余退避毫秒）
     * @param  nowMs: 当前 tick 毫秒
     * @retval 快照值拷贝
     * @note   纯查询。
     */
    [[nodiscard]] LinkSnapshot snapshot(uint32_t nowMs) const noexcept;

    /**
     * @brief  当前阶段
     * @retval 阶段枚举
     * @note   纯查询。
     */
    [[nodiscard]] Phase phase() const noexcept
    {
        return phase_;
    }

    /**
     * @brief  请求打开 socket 连接（TCP），连接成功/失败经 ConnEvents 通知，失败自动 L1 退避重开
     * @param  id:   连接槽 0–2
     * @param  host: 主机名或 IP
     * @param  port: 端口
     * @retval true  已登记
     * @retval false id 越界、host 过长或该槽仍在使用
     * @note   仅 kNet 任务；未 Ready 时登记，进入 Ready 后执行。
     */
    [[nodiscard]] bool openConn(uint8_t id, std::string_view host, uint16_t port) noexcept;

    /**
     * @brief  请求关闭连接并取消自动重开
     * @param  id: 连接槽 0–2
     * @retval true  已登记
     * @retval false id 越界
     * @note   仅 kNet 任务。
     */
    [[nodiscard]] bool closeConn(uint8_t id) noexcept;

    /**
     * @brief  通过已打开连接发送数据（AT+QISEND + '>' + 定长数据）
     * @param  id:      连接槽 0–2
     * @param  payload: 数据（≤256 B），内存须保持到 onSendDone
     * @param  nowMs:   当前 tick 毫秒
     * @retval true  已提交
     * @retval false 未打开、正忙、过长或引擎队列满
     * @note   仅 kNet 任务。
     */
    [[nodiscard]] bool sendConn(uint8_t id, std::span<const uint8_t> payload,
                                uint32_t nowMs) noexcept;

    /**
     * @brief  连接槽是否处于已打开状态
     * @param  id: 连接槽 0–2
     * @retval true 已打开
     * @note   纯查询。
     */
    [[nodiscard]] bool connOpen(uint8_t id) const noexcept;

private:
    /** FSM 自己提交的命令种类（同一时刻最多一条在途）。 */
    enum class Op : uint8_t {
        kNone = 0,
        kAt,
        kAte0,
        kCmee,
        kIpr,
        kGsn,
        kQccid,
        kCpin,
        kCereg,
        kCreg,
        kQicsgp,
        kQiactQuery,
        kQiact,
        kQideact,
        kCsq,
        kHealthCereg,
        kHealthCpin,
        kQpowd,
        kQiclose,
    };

    /** Sync 阶段的初始化链步骤。 */
    enum class SyncStep : uint8_t { kAt = 0, kAte0, kCmee, kIpr, kAtVerify, kGsn, kQccid, kDone };
    /** PdpUp 阶段步骤。 */
    enum class PdpStep : uint8_t { kDeact = 0, kCfg, kQuery, kActivate, kCpin };
    /** 关机序列步骤。 */
    enum class ShutStep : uint8_t { kClose = 0, kQpowd, kWaitDown };
    /** socket 槽状态。 */
    enum class ConnState : uint8_t { kIdle = 0, kWantOpen, kOpening, kOpen, kBackoff, kClosing };

    struct Conn final {
        ConnState state = ConnState::kIdle;
        std::array<char, kHostBytes> host{};
        uint16_t port = 0U;
        uint8_t fails = 0U;       // 连续失败次数
        bool closeFirst = false;  // QIOPEN 结果超时：重开前先 QICLOSE 该槽
        uint32_t dueMs = 0U;      // kBackoff 到期 / kOpening 超时
        bool recvPending = false; // 收到 recv URC 待读
        bool wantClose = false;
    };

    // AT 回调转发（静态函数 + this 上下文，避免 std::function）。
    static void thunkDone(void *ctx, const AtResponse &rsp) noexcept;
    static void thunkLine(void *ctx, std::string_view line) noexcept;
    static void thunkConnDone(void *ctx, const AtResponse &rsp) noexcept;
    static void thunkRaw(void *ctx, std::span<const uint8_t> chunk) noexcept;

    bool submitOp(Op op, std::string_view text, uint32_t timeoutMs, uint32_t nowMs) noexcept;
    void onOpDone(const AtResponse &rsp) noexcept;
    void onOpLine(std::string_view line) noexcept;
    void onConnDone(const AtResponse &rsp) noexcept;

    void enterPhase(Phase phase, uint32_t nowMs) noexcept;
    void enterL3(uint32_t nowMs, bool manual = false) noexcept;
    void enterL2(uint32_t nowMs, bool deactivate) noexcept;
    void enterReady(uint32_t nowMs) noexcept;
    bool cutPower(uint32_t nowMs) noexcept;
    void enterPowerFault(uint32_t nowMs, bool resumeCycle = false) noexcept;
    /**
     * @brief  阶段是否属于“模块已上电并完成同步”的在线阶段（不依赖枚举顺序，r5 F5）
     * @param  p: 待判定的阶段
     * @retval true 为 SimCheck/Register/PdpUp/Ready 之一
     * @note   纯函数，仅 kNet 任务调用。
     */
    [[nodiscard]] static constexpr bool isOnlinePhase(Phase p) noexcept
    {
        return (p == Phase::kSimCheck) || (p == Phase::kRegister) || (p == Phase::kPdpUp) ||
               (p == Phase::kReady);
    }
    void pollPowerFault(uint32_t nowMs) noexcept;
    void startStop(uint32_t nowMs, uint32_t deadlineMs) noexcept;
    void finishStop(uint32_t nowMs) noexcept;
    void handleModuleRestart(uint32_t nowMs) noexcept;
    void clearOps() noexcept;
    void pollPowerCycle(uint32_t nowMs) noexcept;
    void startPowerOn(uint32_t nowMs) noexcept;
    void notifyAllClosed() noexcept;
    void clearStaleClose() noexcept;

    void pollOff(uint32_t nowMs) noexcept;
    void pollSync(uint32_t nowMs) noexcept;
    void submitSyncStep(uint32_t nowMs) noexcept;
    void onSyncDone(const AtResponse &rsp, uint32_t nowMs) noexcept;
    void syncFailed(uint32_t nowMs) noexcept;
    void pollSim(uint32_t nowMs) noexcept;
    void pollRegister(uint32_t nowMs) noexcept;
    void pollPdp(uint32_t nowMs) noexcept;
    void onPdpDone(const AtResponse &rsp) noexcept;
    void failPdpRecovery(uint32_t nowMs) noexcept;
    void pollReady(uint32_t nowMs) noexcept;
    void pollConns(uint32_t nowMs) noexcept;
    void pollShutdown(uint32_t nowMs) noexcept;
    void onSimState(SimStatus sim, uint32_t nowMs) noexcept;
    [[nodiscard]] uint32_t noSimHoldMs() const noexcept;
    void connFailed(uint8_t id, uint32_t nowMs) noexcept;
    void connLost(uint8_t id, uint32_t nowMs) noexcept;
    [[nodiscard]] bool engineBusy() const noexcept;
    void queryRegisterNow(uint32_t nowMs, Op op) noexcept;
    void abortInflight(uint32_t nowMs) noexcept;
    void markRegLost(uint32_t nowMs) noexcept;
    [[nodiscard]] bool submitConnCmd(AtCommand &cmd, uint32_t nowMs) noexcept;
    [[nodiscard]] uint32_t nextToken() noexcept;

    AtEngine *engine_ = nullptr;
    LinkHooks hooks_{};
    ConnEvents events_{};
    LinkConfig cfg_{};
    LinkSnapshot snap_{};

    bool enabled_ = false;
    bool powerAllowed_ = true;
    bool shuttingDown_ = false;
    bool gracefulOff_ = false; // 因禁用/不允许上电而关闭（非 PW1 关机）

    Phase phase_ = Phase::kOff;
    uint32_t phaseStartMs_ = 0U;
    uint32_t nextMs_ = 0U; // 下一次动作时刻
    uint32_t powerOnMs_ = 0U;
    uint32_t syncDeadlineMs_ = 0U;
    uint32_t cycleEndMs_ = 0U;
    uint32_t shutStartMs_ = 0U;
    uint32_t regStartMs_ = 0U;
    uint32_t healthDueMs_ = 0U;

    Op op_ = Op::kNone;
    bool opPending_ = false;
    SyncStep syncStep_ = SyncStep::kAt;
    PdpStep pdpStep_ = PdpStep::kQuery;
    ShutStep shutStep_ = ShutStep::kClose;
    uint8_t syncFails_ = 0U;
    uint8_t pdpFails_ = 0U;
    uint8_t l2Count_ = 0U;
    uint8_t l3Count_ = 0U;
    uint8_t shutCloseId_ = 0U;
    bool simWasBad_ = false;
    bool regLost_ = false;
    bool regQueriedCreg_ = false;
    bool poweredDownSeen_ = false;
    bool pdpForceDeact_ = false;
    SimStatus diagSim_ = SimStatus::kUnknown; // 调试通道看到的非 READY SIM 状态，待 poll 处理
    bool deactRan_ = false; // 本轮 L2 已执行 QIDEACT：之后 QIACT? 仍在线即恢复失败
    bool bootBaudSwapped_ = false;
    bool ipSeen_ = false;
    uint32_t curBaud_ = 0U;
    SimStatus pendingSim_ = SimStatus::kUnknown;
    RegStatus pendingReg_ = RegStatus::kUnknown;
    bool gotSim_ = false;
    bool gotReg_ = false;
    uint8_t healthSeq_ = 0U;        // 0 空闲；1 CSQ；2 CEREG?；3 CPIN?
    bool latched_ = false;          // PW1 关机后禁止再上电
    bool faultResumeCycle_ = false; // PowerFault 恢复后进入 L3 冷却（而非 Off）
    uint32_t cycleHoldMs_ = 0U;     // L3 原冷却时长：GPIO 重试成功后低电平仍须保持这么久
    bool offSinceValid_ = false;
    uint32_t offSinceMs_ = 0U;
    uint32_t waitDownMs_ = 0U;

    std::array<Conn, kConnSlots> conns_{};
    bool connBusy_ = false;
    uint8_t connOpId_ = 0U;
    enum class ConnOp : uint8_t {
        kNone = 0,
        kOpen,
        kClose,
        kRead,
        kSend,
        kCloseStale
    } connOp_ = ConnOp::kNone;
    size_t connReadBytes_ = 0U;
    uint32_t opSeq_ = 0U;          // 提交序号，单调递增（0 保留为“无”）
    uint32_t opToken_ = 0U;        // 当前登记的 FSM 操作 token；与回包 token 不符即过期
    uint32_t connToken_ = 0U;      // 当前登记的连接操作 token
    uint32_t shutDeadlineMs_ = 0U; // 关机绝对截止时刻（PW1 与 kNet 共用）
    uint32_t regLostMs_ = 0U;      // Ready 阶段注册丢失起点
    uint32_t simBadSinceMs_ = 0U;  // SimCheck 内 SIM 首次确认异常的时刻（simBadValid_ 为真时有效）
    uint32_t pdpDueMs_ = 0U;       // L2 退避到期：kCpin 步骤先即时查卡，之后 QIDEACT 等到此刻
    uint8_t noSimCycles_ = 0U;     // 无卡整机断电次数（退避指数；见到 READY 清零；不计入 l3Count_）
    bool simBadValid_ = false;
    uint16_t droppedRecv_ = 0U;
    bool startAllowed_ = true;
    bool quietNext_ = false; // 下一次 submitOp 的命令发出前先等通道静默（AtCommand::quietFirst）
    std::array<char, 128> cmdBuf_{};
};

/** AtEngine 需要分流为 URC 的行前缀表（ADR-0021 §2.4）。 */
inline constexpr std::array<std::string_view, 11> kUrcPrefixes{
    "+QIURC:", "+QIOPEN:", "+QMTSTAT:", "+QMTRECV:",    "+CEREG:",  "+CREG:",
    "+CGREG:", "+CPIN:",   "RDY",       "POWERED DOWN", "+QMTOPEN:"};

} // namespace cellular
