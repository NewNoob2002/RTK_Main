// 4G 链路状态机实现（ADR-0021 §2.5/§2.9）。仅 kNet 任务调用；不阻塞、不分配堆。
#include "cellular/LinkFsm.hpp"

#include <algorithm>
#include <limits>

#include "cellular/AtParse.hpp"

namespace cellular {
namespace {

constexpr uint32_t kForever = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kPowerOnSettleMs = 500U;
constexpr uint32_t kBaudSwitchSettleMs = 100U;
constexpr uint32_t kBaudSwitchExtendMs = 5000U;
constexpr uint32_t kShutdownTotalMs = 2000U;
constexpr uint32_t kShutdownPollMs = 20U;
constexpr uint32_t kCloseTimeoutMs = 300U;
constexpr uint8_t kImeiMinLen = 14U;
constexpr uint8_t kHealthSeqCpin = 3U; // 巡检序列第三步：CPIN?
constexpr uint32_t kDecimalBase = 10U;
constexpr size_t kU32DecimalDigits = 10U;

/** 无堆命令文本拼接器：越界置 ok=false，结果以视图返回。 */
struct Writer final {
    char *buf = nullptr;
    size_t cap = 0U;
    size_t len = 0U;
    bool ok = true;

    /**
     * @brief  追加字符串
     * @param  text: 要追加的文本
     * @note   容量不足时置 ok=false 且不写入。
     */
    void str(std::string_view text) noexcept
    {
        if ((len + text.size()) >= cap) {
            ok = false;
            return;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): 上方已校验 len+size<cap
        std::copy(text.begin(), text.end(), buf + len);
        len += text.size();
    }

    /**
     * @brief  追加无符号十进制数
     * @param  value: 数值
     * @note   最多 10 位。
     */
    void num(uint32_t value) noexcept
    {
        std::array<char, kU32DecimalDigits> tmp{};
        size_t n = 0U;
        while (n < tmp.size()) {
            tmp[n] = static_cast<char>('0' + (value % kDecimalBase)); // 余数 0..9，窄化无损
            value /= kDecimalBase;
            ++n;
            if (value == 0U) {
                break;
            }
        }
        while (n != 0U) {
            --n;
            str(std::string_view{&tmp[n], 1U});
        }
    }

    /**
     * @brief  取已拼接文本
     * @retval 文本视图
     * @note   纯查询。
     */
    [[nodiscard]] std::string_view view() const noexcept
    {
        return std::string_view{buf, len};
    }
};

/**
 * @brief  tick 回绕安全的“已到期”判断
 * @param  nowMs:      当前毫秒
 * @param  deadlineMs: 截止毫秒
 * @retval true  已到达或越过
 * @note   纯函数；单段等待 < 2^31 ms。
 */
bool reached(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

/**
 * @brief  剩余毫秒（已到期为 0）
 * @param  nowMs:      当前毫秒
 * @param  deadlineMs: 截止毫秒
 * @retval 剩余毫秒
 * @note   纯函数。
 */
uint32_t remaining(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    return reached(nowMs, deadlineMs) ? 0U : (deadlineMs - nowMs);
}

/**
 * @brief  把视图复制到定长数组（超长截断，结尾补 0）
 * @param  src: 源文本
 * @param  dst: 目标数组
 * @note   纯函数。
 */
template <size_t N> void copyField(std::string_view src, std::array<char, N> &dst) noexcept
{
    dst.fill('\0');
    const size_t n = std::min(src.size(), N - 1U);
    std::copy_n(src.begin(), n, dst.begin());
}

/**
 * @brief  取定长数组中的文本视图
 * @param  src: 以 0 结尾的数组
 * @retval 文本视图（不含结尾 0）
 * @note   纯函数。
 */
template <size_t N> std::string_view fieldView(const std::array<char, N> &src) noexcept
{
    size_t n = 0U;
    while ((n < N) && (src[n] != '\0')) {
        ++n;
    }
    return std::string_view{src.data(), n};
}

/**
 * @brief  文本是否全为十进制数字
 * @param  text: 输入
 * @retval true 非空且全为数字
 * @note   纯函数。
 */
bool allDigits(std::string_view text) noexcept
{
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0') && (c <= '9'); });
}

} // namespace

/**
 * @brief 绑定 AtEngine、电源/波特率钩子与连接事件回调
 * @param engine: AT 引擎（调用方持有，生命周期须长于本对象）
 * @param hooks: 电源/波特率钩子
 * @param events: 连接事件回调
 * @note 仅启动期调用一次；非 ISR。
 */
void LinkFsm::bind(AtEngine &engine, const LinkHooks &hooks, const ConnEvents &events) noexcept
{
    engine_ = &engine;
    hooks_ = hooks;
    events_ = events;
    snap_ = LinkSnapshot{};
    snap_.baud = kPolicy.bootBaud;
    curBaud_ = kPolicy.bootBaud;
}

void LinkFsm::setConfig(std::string_view apn, std::string_view user, std::string_view pass,
                        uint8_t auth) noexcept
{
    copyField(apn, cfg_.apn);
    copyField(user, cfg_.user);
    copyField(pass, cfg_.pass);
    cfg_.auth = auth;
}

/**
 * @brief 判断 AT 引擎是否仍有在途命令
 * @retval true 成功/条件成立
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool LinkFsm::engineBusy() const noexcept
{
    return (engine_ == nullptr) || engine_->dead();
}

/**
 * @brief 提交一条状态机内部 AT 操作并登记完成动作
 * @param op: op
 * @param text: text
 * @param timeoutMs: timeoutMs
 * @param nowMs: 当前毫秒时间戳
 * @retval true 成功/条件成立
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool LinkFsm::submitOp(Op op, std::string_view text, uint32_t timeoutMs, uint32_t nowMs) noexcept
{
    AtCommand cmd;
    cmd.text = text;
    cmd.timeoutMs = timeoutMs;
    cmd.onLine = &LinkFsm::thunkLine;
    cmd.onDone = &LinkFsm::thunkDone;
    cmd.ctx = this;
    // 先登记 op 再提交：写失败等同步路径不会在 submit 内回调完成，但保持登记在前更稳妥。
    const Op previous = op_;
    op_ = op;
    opPending_ = true;
    if (!engine_->submit(cmd, nowMs)) {
        op_ = previous;
        opPending_ = false;
        return false;
    }
    return true;
}

/**
 * @brief AtEngine 完成回调转发到 LinkFsm::onOpDone
 * @param ctx: 回调上下文指针
 * @param rsp: 命令响应
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::thunkDone(void *ctx, const AtResponse &rsp) noexcept
{
    static_cast<LinkFsm *>(ctx)->onOpDone(rsp);
}

/**
 * @brief AtEngine 响应行回调转发到 LinkFsm::onOpLine
 * @param ctx: 回调上下文指针
 * @param line: 一行文本（不含换行）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::thunkLine(void *ctx, std::string_view line) noexcept
{
    static_cast<LinkFsm *>(ctx)->onOpLine(line);
}

/**
 * @brief 连接类命令完成回调转发到 onConnDone
 * @param ctx: 回调上下文指针
 * @param rsp: 命令响应
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::thunkConnDone(void *ctx, const AtResponse &rsp) noexcept
{
    static_cast<LinkFsm *>(ctx)->onConnDone(rsp);
}

/**
 * @brief 原始数据发送阶段回调转发
 * @param ctx: 回调上下文指针
 * @param chunk: chunk
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::thunkRaw(void *ctx, std::span<const uint8_t> chunk) noexcept
{
    auto *self = static_cast<LinkFsm *>(ctx);
    if (self->connOpId_ < kConnSlots) {
        self->connReadBytes_ += chunk.size();
        self->conns_[self->connOpId_].fails = 0U; // 收到数据即清 L1 退避
        if (self->events_.onData != nullptr) {
            self->events_.onData(self->events_.ctx, self->connOpId_, chunk);
        }
    }
}

/**
 * @brief 解析当前操作的响应行（SIM/注册/信号/PDP 等）
 * @param line: 一行文本（不含换行）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::onOpLine(std::string_view line) noexcept
{
    switch (op_) {
    case Op::kGsn:
        if (allDigits(line) && (line.size() >= kImeiMinLen)) {
            copyField(line, snap_.imei);
        }
        break;
    case Op::kQccid:
        if (line.starts_with("+QCCID:")) {
            std::string_view v = argsOf(line);
            while (!v.empty() && (v.front() == ' ')) {
                v.remove_prefix(1U);
            }
            copyField(v, snap_.iccid);
        }
        break;
    case Op::kCpin:
    case Op::kHealthCpin:
        gotSim_ = parseCpin(line, pendingSim_) || gotSim_;
        break;
    case Op::kCereg:
    case Op::kCreg:
    case Op::kHealthCereg:
        gotReg_ = parseReg(line, pendingReg_) || gotReg_;
        break;
    case Op::kCsq: {
        uint8_t csq = kCsqUnknown;
        if (parseCsq(line, csq)) {
            snap_.csq = csq;
        }
        break;
    }
    case Op::kQiactQuery:
        if (line.starts_with("+QIACT:")) {
            int32_t cid = 0;
            int32_t state = 0;
            if (parseInt(fieldAt(argsOf(line), 0U), cid) &&
                parseInt(fieldAt(argsOf(line), 1U), state) && (cid == 1) && (state == 1)) {
                ipSeen_ = true;
            }
        }
        break;
    default:
        break;
    }
}

// NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0021 §2.5 状态机按阶段/操作平铺分派，拆分会割裂恢复表，主机测试逐路径覆盖。
/**
 * @brief 处理当前操作完成结果并推进状态
 * @param rsp: 命令响应
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::onOpDone(const AtResponse &rsp) noexcept
{
    opPending_ = false;
    const Op op = op_;
    op_ = Op::kNone;
    if (rsp.result == AtResult::kAborted) {
        return;
    }
    const uint32_t now = rsp.nowMs;
    const bool ok = rsp.result == AtResult::kOk;
    switch (op) {
    case Op::kAt:
    case Op::kAte0:
    case Op::kCmee:
    case Op::kIpr:
    case Op::kGsn:
    case Op::kQccid:
        onSyncDone(rsp, now);
        break;
    case Op::kCpin:
    case Op::kHealthCpin: {
        SimStatus sim = SimStatus::kUnknown;
        bool have = false;
        if (rsp.result == AtResult::kCmeError) {
            sim = simFromCmeCode(rsp.errorCode);
            have = true;
        } else if (ok && gotSim_) {
            sim = pendingSim_;
            have = true;
        }
        gotSim_ = false;
        if (op == Op::kHealthCpin) {
            healthSeq_ = 0U;
            healthDueMs_ = now + kPolicy.healthPeriodMs;
        }
        if (have) {
            onSimState(sim, now);
        }
        break;
    }
    case Op::kCereg:
    case Op::kCreg: {
        const bool haveReg = ok && gotReg_;
        gotReg_ = false;
        if (haveReg) {
            snap_.reg = pendingReg_;
        }
        if (phase_ != Phase::kRegister) {
            break;
        }
        if (haveReg && isRegistered(pendingReg_)) {
            enterPhase(Phase::kPdpUp, now);
        } else if ((op == Op::kCereg) && !regQueriedCreg_) {
            regQueriedCreg_ = true;
            static_cast<void>(submitOp(Op::kCreg, "AT+CREG?", at_timeout::kMedium, now));
        }
        break;
    }
    case Op::kHealthCereg: {
        const bool haveReg = ok && gotReg_;
        gotReg_ = false;
        healthSeq_ = kHealthSeqCpin;
        if (haveReg) {
            snap_.reg = pendingReg_;
            if (!isRegistered(pendingReg_)) {
                regLost_ = true;
            } else if (regLost_) {
                regLost_ = false;
                enterL2(now, true);
            }
        }
        break;
    }
    case Op::kCsq:
        healthSeq_ = 2U;
        break;
    case Op::kQicsgp:
    case Op::kQiactQuery:
    case Op::kQiact:
    case Op::kQideact:
        onPdpDone(rsp);
        break;
    case Op::kQpowd:
        if (ok) {
            shutStep_ = ShutStep::kWaitDown;
            waitDownMs_ = now;
        } else {
            finishStop(now);
        }
        break;
    case Op::kQiclose:
    case Op::kNone:
    default:
        break;
    }
}
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)

/**
 * @brief 切换链路阶段并记录进入时间
 * @param phase: phase
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::enterPhase(Phase phase, uint32_t nowMs) noexcept
{
    phase_ = phase;
    snap_.phase = phase;
    phaseStartMs_ = nowMs;
    nextMs_ = nowMs;
    if (phase == Phase::kRegister) {
        regStartMs_ = nowMs;
        regQueriedCreg_ = false;
        gotReg_ = false;
    } else if (phase == Phase::kPdpUp) {
        pdpStep_ = pdpForceDeact_ ? PdpStep::kDeact
                                  : (fieldView(cfg_.apn).empty() ? PdpStep::kQuery : PdpStep::kCfg);
        pdpForceDeact_ = false;
        ipSeen_ = false;
    }
}

/**
 * @brief 清空待执行操作与连接队列状态
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::clearOps() noexcept
{
    op_ = Op::kNone;
    opPending_ = false;
    connBusy_ = false;
    connOp_ = ConnOp::kNone;
    healthSeq_ = 0U;
    gotSim_ = false;
    gotReg_ = false;
}

/**
 * @brief 通知所有连接已关闭
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::notifyAllClosed() noexcept
{
    for (uint8_t id = 0U; id < kConnSlots; ++id) {
        Conn &conn = conns_[id];
        if (conn.state == ConnState::kIdle) {
            continue;
        }
        const bool wasOpen = (conn.state == ConnState::kOpen);
        // 承载/模块级恢复后由 FSM 自动重开已登记的连接；正在关闭的直接收尾。
        conn.state = conn.wantClose ? ConnState::kIdle : ConnState::kWantOpen;
        conn.wantClose = false;
        conn.recvPending = false;
        if (wasOpen && (events_.onClosed != nullptr)) {
            events_.onClosed(events_.ctx, id);
        }
    }
}

/**
 * @brief 断开模块电源并复位相关状态
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::cutPower(uint32_t nowMs) noexcept
{
    if (hooks_.setPower != nullptr) {
        hooks_.setPower(hooks_.ctx, false);
    }
    offSinceValid_ = true;
    offSinceMs_ = nowMs;
}

/**
 * @brief 开始上电流程
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::startPowerOn(uint32_t nowMs) noexcept
{
    if (hooks_.setPower != nullptr) {
        hooks_.setPower(hooks_.ctx, true);
    }
    powerOnMs_ = nowMs;
    syncDeadlineMs_ = nowMs + kPolicy.syncWindowMs;
    syncStep_ = SyncStep::kAt;
    syncFails_ = 0U;
    curBaud_ = kPolicy.bootBaud;
    snap_.baud = curBaud_;
    if (hooks_.setBaud != nullptr) {
        static_cast<void>(hooks_.setBaud(hooks_.ctx, curBaud_));
    }
    clearOps();
    enterPhase(Phase::kPowerOn, nowMs);
    nextMs_ = nowMs + kPowerOnSettleMs;
}

/**
 * @brief 进入 L3 恢复（整机断电重启）
 * @param nowMs: 当前毫秒时间戳
 * @param manual: manual
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::enterL3(uint32_t nowMs, bool manual) noexcept
{
    snap_.level = RecoveryLevel::kL3Module;
    ++snap_.recoveryCount;
    uint32_t hold = kPolicy.powerOffMinMs; // 人工复位不进入退避升级，只保证最短断电时间
    if (!manual) {
        if (l3Count_ < std::numeric_limits<uint8_t>::max()) {
            ++l3Count_;
        }
        hold = std::max(kPolicy.powerOffMinMs, l3BackoffMs(static_cast<uint8_t>(l3Count_ - 1U)));
    }
    if (engine_ != nullptr) {
        snap_.channelDead = snap_.channelDead || engine_->dead();
        engine_->reset(nowMs);
    }
    clearOps();
    notifyAllClosed();
    cutPower(nowMs);
    simWasBad_ = false;
    regLost_ = false;
    pdpForceDeact_ = false;
    pdpFails_ = 0U;
    l2Count_ = 0U;
    snap_.pdpUp = false;
    snap_.reg = RegStatus::kUnknown;
    snap_.sim = SimStatus::kUnknown;
    snap_.csq = kCsqUnknown;
    enterPhase(Phase::kPowerCycle, nowMs);
    cycleEndMs_ = nowMs + hold;
}

/**
 * @brief 进入 L2 恢复（模块软重启）
 * @param nowMs: 当前毫秒时间戳
 * @param deactivate: deactivate
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::enterL2(uint32_t nowMs, bool deactivate) noexcept
{
    snap_.level = RecoveryLevel::kL2Bearer;
    ++snap_.recoveryCount;
    if (l2Count_ < std::numeric_limits<uint8_t>::max()) {
        ++l2Count_;
    }
    snap_.pdpUp = false;
    notifyAllClosed();
    pdpForceDeact_ = deactivate;
    enterPhase(Phase::kPdpUp, nowMs);
    nextMs_ = nowMs + backoffMs(kPolicy.l2, static_cast<uint8_t>(l2Count_ - 1U));
}

/**
 * @brief 进入 Ready 阶段并清零重试计数
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::enterReady(uint32_t nowMs) noexcept
{
    snap_.pdpUp = true;
    snap_.channelDead = false;
    l2Count_ = 0U;
    l3Count_ = 0U;
    pdpFails_ = 0U;
    regLost_ = false;
    healthSeq_ = 0U;
    healthDueMs_ = nowMs; // 进入 Ready 立即做第一次 CSQ 巡检
    enterPhase(Phase::kReady, nowMs);
    for (Conn &conn : conns_) {
        if (conn.state == ConnState::kWantOpen) {
            conn.dueMs = nowMs;
        }
    }
}

/**
 * @brief 处理模块重启类 URC 并回到同步阶段
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::handleModuleRestart(uint32_t nowMs) noexcept
{
    if (engine_ != nullptr) {
        engine_->reset(nowMs);
    }
    clearOps();
    notifyAllClosed();
    snap_.pdpUp = false;
    snap_.reg = RegStatus::kUnknown;
    snap_.sim = SimStatus::kUnknown;
    simWasBad_ = false;
    regLost_ = false;
    syncStep_ = SyncStep::kAt;
    syncFails_ = 0U;
    syncDeadlineMs_ = nowMs + kPolicy.syncWindowMs;
    enterPhase(Phase::kSync, nowMs);
}

bool LinkFsm::requestReset(uint32_t nowMs) noexcept
{
    if ((phase_ == Phase::kOff) || (phase_ == Phase::kPowerCycle) || shuttingDown_) {
        return false;
    }
    enterL3(nowMs, true);
    return true;
}

/**
 * @brief 根据 SIM 状态更新快照与阶段
 * @param sim: sim
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::onSimState(SimStatus sim, uint32_t nowMs) noexcept
{
    snap_.sim = sim;
    if (sim == SimStatus::kReady) {
        if (simWasBad_) {
            simWasBad_ = false;
            enterL3(nowMs); // SIM 异常后恢复：模块需重启才能重新读卡
            return;
        }
        if (phase_ == Phase::kSimCheck) {
            enterPhase(Phase::kRegister, nowMs);
        }
        return;
    }
    simWasBad_ = true;
    if ((phase_ == Phase::kRegister) || (phase_ == Phase::kPdpUp) || (phase_ == Phase::kReady)) {
        snap_.pdpUp = false;
        notifyAllClosed();
        clearOps();
        if (engine_ != nullptr) {
            engine_->reset(nowMs);
        }
        enterPhase(Phase::kSimCheck, nowMs);
        nextMs_ = nowMs + kPolicy.simPollMs;
    }
}

void LinkFsm::beginShutdown(uint32_t nowMs) noexcept
{
    latched_ = true;
    if (!shuttingDown_) {
        startStop(nowMs);
    }
}

/**
 * @brief 开始优雅关机/停止流程
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::startStop(uint32_t nowMs) noexcept
{
    if (shuttingDown_) {
        return;
    }
    if ((phase_ == Phase::kOff) || (phase_ == Phase::kPowerCycle)) {
        cutPower(nowMs);
        enterPhase(Phase::kOff, nowMs);
        return;
    }
    // 模块尚未起来或 AT 通道已失效：没有可发的关机命令，直接断电。
    if ((phase_ == Phase::kPowerOn) || (phase_ == Phase::kSync) || engineBusy()) {
        if (engine_ != nullptr) {
            engine_->reset(nowMs);
        }
        clearOps();
        finishStop(nowMs);
        return;
    }
    shuttingDown_ = true;
    shutStartMs_ = nowMs;
    shutStep_ = ShutStep::kClose;
    shutCloseId_ = 0U;
    poweredDownSeen_ = false;
    engine_->reset(nowMs); // 丢弃在途的巡检/连接命令，让关机序列立即得到串口
    clearOps();
}

/**
 * @brief 完成停止流程并断电
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::finishStop(uint32_t nowMs) noexcept
{
    cutPower(nowMs);
    shuttingDown_ = false;
    clearOps();
    snap_.pdpUp = false;
    notifyAllClosed();
    for (Conn &conn : conns_) {
        if (conn.state == ConnState::kWantOpen) {
            conn.state = ConnState::kIdle;
        }
    }
    enterPhase(Phase::kOff, nowMs);
}

/**
 * @brief 轮询关机流程
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollShutdown(uint32_t nowMs) noexcept
{
    if (reached(nowMs, shutStartMs_ + kShutdownTotalMs)) {
        finishStop(nowMs); // 总预算 ≤2 s：无论 QPOWD 进展如何，到点直接断电
        return;
    }
    switch (shutStep_) {
    case ShutStep::kClose: {
        if (opPending_) {
            return;
        }
        while (shutCloseId_ < kConnSlots) {
            const uint8_t id = shutCloseId_;
            ++shutCloseId_;
            if ((conns_[id].state == ConnState::kOpen) ||
                (conns_[id].state == ConnState::kOpening)) {
                Writer w{cmdBuf_.data(), cmdBuf_.size()};
                w.str("AT+QICLOSE=");
                w.num(id);
                if (w.ok && submitOp(Op::kQiclose, w.view(), kCloseTimeoutMs, nowMs)) {
                    return;
                }
            }
        }
        shutStep_ = ShutStep::kQpowd;
        break;
    }
    case ShutStep::kQpowd:
        if (opPending_) {
            return;
        }
        if (!submitOp(Op::kQpowd, "AT+QPOWD=1", at_timeout::kQpowd, nowMs)) {
            finishStop(nowMs);
        }
        break;
    case ShutStep::kWaitDown:
        if (poweredDownSeen_ || reached(nowMs, waitDownMs_ + at_timeout::kPoweredDownWait)) {
            finishStop(nowMs);
        }
        break;
    default:
        finishStop(nowMs);
        break;
    }
}

/**
 * @brief 轮询 Off 阶段（等待使能）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollOff(uint32_t nowMs) noexcept
{
    if (!enabled_ || !powerAllowed_ || latched_ || shuttingDown_) {
        return;
    }
    // 断电后至少保持 powerOffMinMs 再上电，避免模块未完全放电。
    if (offSinceValid_ && !reached(nowMs, offSinceMs_ + kPolicy.powerOffMinMs)) {
        return;
    }
    startPowerOn(nowMs);
}

/**
 * @brief 轮询断电冷却阶段
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollPowerCycle(uint32_t nowMs) noexcept
{
    if (reached(nowMs, cycleEndMs_)) {
        startPowerOn(nowMs);
    }
}

/**
 * @brief 提交同步阶段的下一条 AT 步骤
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::submitSyncStep(uint32_t nowMs) noexcept
{
    bool sent = false;
    switch (syncStep_) {
    case SyncStep::kAt:
    case SyncStep::kAtVerify:
        sent = submitOp(Op::kAt, "AT", at_timeout::kSync, nowMs);
        break;
    case SyncStep::kAte0:
        sent = submitOp(Op::kAte0, "ATE0", at_timeout::kSync, nowMs);
        break;
    case SyncStep::kCmee:
        sent = submitOp(Op::kCmee, "AT+CMEE=2", at_timeout::kSync, nowMs);
        break;
    case SyncStep::kIpr:
        if (curBaud_ == kPolicy.fastBaud) {
            syncStep_ = SyncStep::kGsn;
            nextMs_ = nowMs;
            return;
        }
        sent = submitOp(Op::kIpr, "AT+IPR=460800", at_timeout::kMedium, nowMs);
        break;
    case SyncStep::kGsn:
        sent = submitOp(Op::kGsn, "AT+GSN", at_timeout::kShort, nowMs);
        break;
    case SyncStep::kQccid:
        sent = submitOp(Op::kQccid, "AT+QCCID", at_timeout::kMedium, nowMs);
        break;
    case SyncStep::kDone:
    default:
        enterPhase(Phase::kSimCheck, nowMs);
        return;
    }
    if (!sent) {
        nextMs_ = nowMs + kPolicy.syncRetryMs;
    }
}

/**
 * @brief 同步阶段失败处理（计入重试并升级恢复）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::syncFailed(uint32_t nowMs) noexcept
{
    if (syncFails_ < std::numeric_limits<uint8_t>::max()) {
        ++syncFails_;
    }
    nextMs_ = nowMs + kPolicy.syncRetryMs;
    if (engine_->dead()) {
        engine_->reset(nowMs); // 同步窗口内由窗口超时判 L3，不用引擎的“连续无响应”阈值
    }
    const bool probing = (syncStep_ == SyncStep::kAt) || (syncStep_ == SyncStep::kAtVerify);
    if (probing && ((syncFails_ % kPolicy.syncSwapBaudAttempts) == 0U)) {
        curBaud_ = (curBaud_ == kPolicy.bootBaud) ? kPolicy.fastBaud : kPolicy.bootBaud;
        snap_.baud = curBaud_;
        if (hooks_.setBaud != nullptr) {
            static_cast<void>(hooks_.setBaud(hooks_.ctx, curBaud_));
        }
    }
}

// NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0021 §2.5 状态机按阶段/操作平铺分派，拆分会割裂恢复表，主机测试逐路径覆盖。
/**
 * @brief 同步步骤完成回调
 * @param rsp: 命令响应
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::onSyncDone(const AtResponse &rsp, uint32_t nowMs) noexcept
{
    const bool ok = rsp.result == AtResult::kOk;
    nextMs_ = nowMs;
    switch (syncStep_) {
    case SyncStep::kAt:
        if (ok) {
            syncFails_ = 0U;
            syncStep_ = SyncStep::kAte0;
        } else {
            syncFailed(nowMs);
        }
        break;
    case SyncStep::kAte0:
        if (ok) {
            syncStep_ = SyncStep::kCmee;
        } else {
            syncFailed(nowMs);
        }
        break;
    case SyncStep::kCmee:
        if (ok) {
            syncStep_ = SyncStep::kIpr;
        } else {
            syncFailed(nowMs);
        }
        break;
    case SyncStep::kIpr:
        if (ok) {
            curBaud_ = kPolicy.fastBaud;
            snap_.baud = curBaud_;
            if (hooks_.setBaud != nullptr) {
                static_cast<void>(hooks_.setBaud(hooks_.ctx, curBaud_));
            }
            syncStep_ = SyncStep::kAtVerify;
            syncFails_ = 0U;
            nextMs_ = nowMs + kBaudSwitchSettleMs;
            if (!reached(nowMs + kBaudSwitchExtendMs, syncDeadlineMs_)) {
                syncDeadlineMs_ = nowMs + kBaudSwitchExtendMs;
            }
        } else {
            syncStep_ = SyncStep::kGsn; // 切换失败：保持 115200 继续
        }
        break;
    case SyncStep::kAtVerify:
        if (ok) {
            syncFails_ = 0U;
            syncStep_ = SyncStep::kGsn;
        } else {
            syncFailed(nowMs);
        }
        break;
    case SyncStep::kGsn:
        syncStep_ = SyncStep::kQccid;
        break;
    case SyncStep::kQccid:
        syncStep_ = SyncStep::kDone;
        break;
    case SyncStep::kDone:
    default:
        break;
    }
}
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)

/**
 * @brief 轮询同步阶段
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollSync(uint32_t nowMs) noexcept
{
    if (engine_->dead()) {
        engine_->reset(nowMs);
    }
    if (opPending_ || !reached(nowMs, nextMs_)) {
        return;
    }
    if (reached(nowMs, syncDeadlineMs_)) {
        enterL3(nowMs); // 10 s 窗口内 AT 一直无应答：模块级复位
        return;
    }
    submitSyncStep(nowMs);
}

/**
 * @brief 轮询 SIM 检测阶段
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollSim(uint32_t nowMs) noexcept
{
    if (opPending_ || !reached(nowMs, nextMs_)) {
        return;
    }
    gotSim_ = false;
    if (submitOp(Op::kCpin, "AT+CPIN?", at_timeout::kMedium, nowMs)) {
        nextMs_ = nowMs + kPolicy.simPollMs;
    }
}

/**
 * @brief 轮询网络注册阶段
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollRegister(uint32_t nowMs) noexcept
{
    if (reached(nowMs, regStartMs_ + kPolicy.registerTimeoutMs)) {
        enterL3(nowMs);
        return;
    }
    if (opPending_ || !reached(nowMs, nextMs_)) {
        return;
    }
    regQueriedCreg_ = false;
    gotReg_ = false;
    if (submitOp(Op::kCereg, "AT+CEREG?", at_timeout::kMedium, nowMs)) {
        nextMs_ = nowMs + kPolicy.registerPollMs;
    }
}

/**
 * @brief 轮询 PDP 激活阶段
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollPdp(uint32_t nowMs) noexcept
{
    if (opPending_ || !reached(nowMs, nextMs_)) {
        return;
    }
    bool sent = false;
    switch (pdpStep_) {
    case PdpStep::kDeact:
        sent = submitOp(Op::kQideact, "AT+QIDEACT=1", at_timeout::kQideact, nowMs);
        break;
    case PdpStep::kCfg: {
        Writer w{cmdBuf_.data(), cmdBuf_.size()};
        w.str("AT+QICSGP=1,1,\"");
        w.str(fieldView(cfg_.apn));
        w.str("\",\"");
        w.str(fieldView(cfg_.user));
        w.str("\",\"");
        w.str(fieldView(cfg_.pass));
        w.str("\",");
        w.num(cfg_.auth);
        sent = w.ok && submitOp(Op::kQicsgp, w.view(), at_timeout::kMedium, nowMs);
        break;
    }
    case PdpStep::kQuery:
        ipSeen_ = false;
        sent = submitOp(Op::kQiactQuery, "AT+QIACT?", at_timeout::kMedium, nowMs);
        break;
    case PdpStep::kActivate:
        sent = submitOp(Op::kQiact, "AT+QIACT=1", at_timeout::kQiact, nowMs);
        break;
    default:
        break;
    }
    if (!sent) {
        nextMs_ = nowMs + kPolicy.syncRetryMs;
    }
}

/**
 * @brief PDP 配置/激活命令完成回调
 * @param rsp: 命令响应
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::onPdpDone(const AtResponse &rsp) noexcept
{
    const uint32_t now = rsp.nowMs;
    const bool ok = rsp.result == AtResult::kOk;
    nextMs_ = now;
    if (phase_ != Phase::kPdpUp) {
        return;
    }
    switch (pdpStep_) {
    case PdpStep::kDeact:
        pdpStep_ = fieldView(cfg_.apn).empty() ? PdpStep::kQuery : PdpStep::kCfg;
        break;
    case PdpStep::kCfg:
        if (ok) {
            pdpStep_ = PdpStep::kQuery;
            break;
        }
        pdpStep_ = PdpStep::kQuery; // 配置失败也继续尝试激活（卡/运营商可能自带默认 APN）
        break;
    case PdpStep::kQuery:
        if (ok && ipSeen_) {
            enterReady(now);
        } else {
            pdpStep_ = PdpStep::kActivate;
        }
        break;
    case PdpStep::kActivate:
        if (ok) {
            enterReady(now);
            break;
        }
        if (pdpFails_ < std::numeric_limits<uint8_t>::max()) {
            ++pdpFails_;
        }
        if (pdpFails_ >= kPolicy.qiactFailToL3) {
            enterL3(now);
        } else {
            enterL2(now, true);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief 轮询 Ready 阶段的周期查询（CSQ/注册/PDP）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollReady(uint32_t nowMs) noexcept
{
    if ((healthSeq_ == 0U) && reached(nowMs, healthDueMs_)) {
        healthSeq_ = 1U;
    }
    if ((healthSeq_ != 0U) && !opPending_) {
        bool sent = false;
        switch (healthSeq_) {
        case 1U:
            sent = submitOp(Op::kCsq, "AT+CSQ", at_timeout::kShort, nowMs);
            break;
        case 2U:
            gotReg_ = false;
            sent = submitOp(Op::kHealthCereg, "AT+CEREG?", at_timeout::kMedium, nowMs);
            break;
        default:
            gotSim_ = false;
            sent = submitOp(Op::kHealthCpin, "AT+CPIN?", at_timeout::kMedium, nowMs);
            break;
        }
        if (!sent) {
            healthDueMs_ = nowMs + kPolicy.syncRetryMs;
            healthSeq_ = 0U;
        }
    }
    pollConns(nowMs);
}

// NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0021 §2.5 状态机按阶段/操作平铺分派，拆分会割裂恢复表，主机测试逐路径覆盖。
/**
 * @brief 轮询 TCP 连接的打开/发送/关闭请求
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollConns(uint32_t nowMs) noexcept
{
    if (connBusy_) {
        return;
    }
    for (uint8_t id = 0U; id < kConnSlots; ++id) {
        Conn &conn = conns_[id];
        Writer w{cmdBuf_.data(), cmdBuf_.size()};
        AtCommand cmd;
        cmd.onDone = &LinkFsm::thunkConnDone;
        cmd.ctx = this;
        if (conn.wantClose &&
            ((conn.state == ConnState::kOpen) || (conn.state == ConnState::kOpening))) {
            w.str("AT+QICLOSE=");
            w.num(id);
            cmd.text = w.view();
            cmd.timeoutMs = at_timeout::kQiclose;
            if (w.ok && engine_->submit(cmd, nowMs)) {
                connBusy_ = true;
                connOp_ = ConnOp::kClose;
                connOpId_ = id;
                conn.state = ConnState::kClosing;
                return;
            }
            continue;
        }
        if ((conn.state == ConnState::kBackoff) && reached(nowMs, conn.dueMs)) {
            conn.state = ConnState::kWantOpen;
        }
        if ((conn.state == ConnState::kWantOpen) && reached(nowMs, conn.dueMs)) {
            w.str("AT+QIOPEN=1,");
            w.num(id);
            w.str(",\"TCP\",\"");
            w.str(fieldView(conn.host));
            w.str("\",");
            w.num(conn.port);
            w.str(",0,0"); // local_port=0, access_mode=0（缓冲模式，ADR-0021 §2.4）
            cmd.text = w.view();
            cmd.timeoutMs = at_timeout::kMedium;
            if (w.ok && engine_->submit(cmd, nowMs)) {
                connBusy_ = true;
                connOp_ = ConnOp::kOpen;
                connOpId_ = id;
                conn.state = ConnState::kOpening;
                conn.dueMs = nowMs + at_timeout::kQiopenResult;
                return;
            }
            continue;
        }
        if ((conn.state == ConnState::kOpening) && reached(nowMs, conn.dueMs)) {
            connFailed(id, nowMs);
            return;
        }
        if (conn.recvPending && (conn.state == ConnState::kOpen)) {
            w.str("AT+QIRD=");
            w.num(id);
            w.str(",");
            w.num(kReadChunkBytes);
            cmd.text = w.view();
            cmd.timeoutMs = at_timeout::kMedium;
            cmd.onRaw = &LinkFsm::thunkRaw;
            cmd.rawPrefix = "+QIRD:";
            if (w.ok && engine_->submit(cmd, nowMs)) {
                connBusy_ = true;
                connOp_ = ConnOp::kRead;
                connOpId_ = id;
                connReadBytes_ = 0U;
                return;
            }
        }
    }
}
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)

/**
 * @brief 连接类命令完成回调
 * @param rsp: 命令响应
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::onConnDone(const AtResponse &rsp) noexcept
{
    connBusy_ = false;
    const ConnOp op = connOp_;
    connOp_ = ConnOp::kNone;
    if ((rsp.result == AtResult::kAborted) || (connOpId_ >= kConnSlots)) {
        return;
    }
    const uint8_t id = connOpId_;
    Conn &conn = conns_[id];
    const bool ok = rsp.result == AtResult::kOk;
    switch (op) {
    case ConnOp::kOpen:
        if (!ok) {
            connFailed(id, rsp.nowMs); // 命令层失败；成功时等待 +QIOPEN URC
        }
        break;
    case ConnOp::kClose:
        conn.state = ConnState::kIdle;
        conn.wantClose = false;
        conn.recvPending = false;
        if (events_.onClosed != nullptr) {
            events_.onClosed(events_.ctx, id);
        }
        break;
    case ConnOp::kRead:
        if (!ok || (connReadBytes_ == 0U)) {
            conn.recvPending = false; // 读空（+QIRD: 0）或失败：等下一次 recv URC
        }
        break;
    case ConnOp::kSend:
        if (events_.onSendDone != nullptr) {
            events_.onSendDone(events_.ctx, id, ok);
        }
        break;
    case ConnOp::kNone:
    default:
        break;
    }
}

/**
 * @brief 记录连接打开/发送失败并通知上层
 * @param id: 连接编号
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::connFailed(uint8_t id, uint32_t nowMs) noexcept
{
    Conn &conn = conns_[id];
    if (conn.fails < std::numeric_limits<uint8_t>::max()) {
        ++conn.fails;
    }
    const bool wasOpening = (conn.state == ConnState::kOpening);
    conn.recvPending = false;
    if (wasOpening && (events_.onOpened != nullptr)) {
        events_.onOpened(events_.ctx, id, false);
    }
    if (conn.fails >= kPolicy.l1FailToL2) {
        conn.fails = 0U;
        enterL2(nowMs, true);
        return;
    }
    snap_.level = RecoveryLevel::kL1Conn;
    ++snap_.recoveryCount;
    conn.state = ConnState::kBackoff;
    conn.dueMs = nowMs + backoffMs(kPolicy.l1, static_cast<uint8_t>(conn.fails - 1U));
}

/**
 * @brief 记录连接丢失并通知上层
 * @param id: 连接编号
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::connLost(uint8_t id, uint32_t nowMs) noexcept
{
    Conn &conn = conns_[id];
    if ((conn.state == ConnState::kIdle) || (conn.state == ConnState::kBackoff)) {
        return;
    }
    if (conn.state == ConnState::kOpen) {
        if (events_.onClosed != nullptr) {
            events_.onClosed(events_.ctx, id);
        }
        conn.state = ConnState::
            kWantOpen; // 已通知 onClosed；走失败退避路径，且不因 kOpening 重复回调 onOpened
    }
    connFailed(id, nowMs);
}

bool LinkFsm::openConn(uint8_t id, std::string_view host, uint16_t port) noexcept
{
    if ((id >= kConnSlots) || host.empty() || (host.size() >= kHostBytes) ||
        (conns_[id].state != ConnState::kIdle)) {
        return false;
    }
    Conn &conn = conns_[id];
    copyField(host, conn.host);
    conn.port = port;
    conn.fails = 0U;
    conn.dueMs = 0U;
    conn.recvPending = false;
    conn.wantClose = false;
    conn.state = ConnState::kWantOpen;
    return true;
}

bool LinkFsm::closeConn(uint8_t id) noexcept
{
    if (id >= kConnSlots) {
        return false;
    }
    Conn &conn = conns_[id];
    switch (conn.state) {
    case ConnState::kOpen:
    case ConnState::kOpening:
        conn.wantClose = true;
        break;
    case ConnState::kIdle:
    case ConnState::kClosing:
        break;
    case ConnState::kWantOpen:
    case ConnState::kBackoff:
    default:
        conn.state = ConnState::kIdle;
        break;
    }
    return true;
}

bool LinkFsm::sendConn(uint8_t id, std::span<const uint8_t> payload, uint32_t nowMs) noexcept
{
    if ((id >= kConnSlots) || (conns_[id].state != ConnState::kOpen) || connBusy_ ||
        payload.empty() || (payload.size() > kSendChunkBytes)) {
        return false;
    }
    Writer w{cmdBuf_.data(), cmdBuf_.size()};
    w.str("AT+QISEND=");
    w.num(id);
    w.str(",");
    w.num(static_cast<uint32_t>(payload.size())); // ≤512，无损
    AtCommand cmd;
    cmd.text = w.view();
    cmd.timeoutMs = at_timeout::kQisend;
    cmd.onDone = &LinkFsm::thunkConnDone;
    cmd.ctx = this;
    cmd.payload = payload;
    if (!w.ok || !engine_->submit(cmd, nowMs)) {
        return false;
    }
    connBusy_ = true;
    connOp_ = ConnOp::kSend;
    connOpId_ = id;
    return true;
}

bool LinkFsm::connOpen(uint8_t id) const noexcept
{
    return (id < kConnSlots) && (conns_[id].state == ConnState::kOpen);
}

// NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0021 §2.5 状态机按阶段/操作平铺分派，拆分会割裂恢复表，主机测试逐路径覆盖。
void LinkFsm::onUrc(std::string_view line, uint32_t nowMs) noexcept
{
    if (line.starts_with("POWERED DOWN")) {
        poweredDownSeen_ = true;
        return;
    }
    if (line.starts_with("RDY")) {
        // 模块自行重启（掉电重上电/看门狗）：回到同步阶段重新初始化。
        if (!shuttingDown_ && (phase_ >= Phase::kSimCheck)) {
            handleModuleRestart(nowMs);
        }
        return;
    }
    if (line.starts_with("+CPIN:")) {
        SimStatus sim = SimStatus::kUnknown;
        if (parseCpin(line, sim) && (phase_ >= Phase::kSimCheck)) {
            onSimState(sim, nowMs);
        }
        return;
    }
    if (line.starts_with("+CEREG:") || line.starts_with("+CREG:")) {
        RegStatus reg = RegStatus::kUnknown;
        if (!parseReg(line, reg)) {
            return;
        }
        snap_.reg = reg;
        if ((phase_ == Phase::kRegister) && isRegistered(reg)) {
            enterPhase(Phase::kPdpUp, nowMs);
        } else if (phase_ == Phase::kReady) {
            if (!isRegistered(reg)) {
                regLost_ = true;
            } else if (regLost_) {
                regLost_ = false;
                enterL2(nowMs, true);
            }
        }
        return;
    }
    if (line.starts_with("+QIOPEN:")) {
        int32_t id = 0;
        int32_t err = 0;
        const std::string_view args = argsOf(line);
        if (!parseInt(fieldAt(args, 0U), id) || !parseInt(fieldAt(args, 1U), err) || (id < 0) ||
            (id >= static_cast<int32_t>(kConnSlots))) {
            return;
        }
        const auto slot = static_cast<uint8_t>(id); // 已校验 0..2
        if (conns_[slot].state != ConnState::kOpening) {
            return;
        }
        if (err == 0) {
            conns_[slot].state = ConnState::kOpen;
            if (events_.onOpened != nullptr) {
                events_.onOpened(events_.ctx, slot, true);
            }
        } else {
            connFailed(slot, nowMs);
        }
        return;
    }
    std::string_view kind;
    int32_t id = -1;
    if (parseQiurc(line, kind, id)) {
        if (kind == "pdpdeact") {
            if ((phase_ == Phase::kReady) || (phase_ == Phase::kPdpUp)) {
                enterL2(nowMs, true);
            }
        } else if ((id >= 0) && (id < static_cast<int32_t>(kConnSlots))) {
            const auto slot = static_cast<uint8_t>(id); // 已校验 0..2
            if (kind == "closed") {
                connLost(slot, nowMs);
            } else if (kind == "recv") {
                conns_[slot].recvPending = true;
            }
        }
    }
}
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)

void LinkFsm::poll(uint32_t nowMs) noexcept
{
    if (engine_ == nullptr) {
        return;
    }
    if (phase_ == Phase::kOff) {
        pollOff(nowMs);
        return;
    }
    if (shuttingDown_) {
        pollShutdown(nowMs);
        return;
    }
    if (!enabled_ || !powerAllowed_ || latched_) {
        startStop(nowMs);
        return;
    }
    if (engine_->dead() && (phase_ != Phase::kSync) && (phase_ != Phase::kPowerOn) &&
        (phase_ != Phase::kPowerCycle)) {
        enterL3(nowMs); // 连续 3 次无响应：通道失效
        return;
    }
    switch (phase_) {
    case Phase::kPowerCycle:
        pollPowerCycle(nowMs);
        break;
    case Phase::kPowerOn:
        if (reached(nowMs, nextMs_)) {
            enterPhase(Phase::kSync, nowMs);
        }
        break;
    case Phase::kSync:
        pollSync(nowMs);
        break;
    case Phase::kSimCheck:
        pollSim(nowMs);
        break;
    case Phase::kRegister:
        pollRegister(nowMs);
        break;
    case Phase::kPdpUp:
        pollPdp(nowMs);
        break;
    case Phase::kReady:
        pollReady(nowMs);
        break;
    case Phase::kOff:
    default:
        break;
    }
}

// NOLINTBEGIN(readability-function-size, readability-function-cognitive-complexity): ADR-0021 §2.5 状态机按阶段/操作平铺分派，拆分会割裂恢复表，主机测试逐路径覆盖。
uint32_t LinkFsm::msToNextWork(uint32_t nowMs) const noexcept
{
    if (engine_ == nullptr) {
        return kForever;
    }
    if (shuttingDown_) {
        return kShutdownPollMs;
    }
    switch (phase_) {
    case Phase::kOff:
        return (enabled_ && powerAllowed_ && !latched_)
                   ? (offSinceValid_ ? remaining(nowMs, offSinceMs_ + kPolicy.powerOffMinMs) : 0U)
                   : kForever;
    case Phase::kPowerCycle:
        return remaining(nowMs, cycleEndMs_);
    case Phase::kRegister:
        if (opPending_) {
            return remaining(nowMs, regStartMs_ + kPolicy.registerTimeoutMs);
        }
        return std::min(remaining(nowMs, nextMs_),
                        remaining(nowMs, regStartMs_ + kPolicy.registerTimeoutMs));
    case Phase::kPowerOn:
    case Phase::kSync:
    case Phase::kSimCheck:
    case Phase::kPdpUp:
        return opPending_ ? kForever : remaining(nowMs, nextMs_);
    case Phase::kReady: {
        uint32_t next = (healthSeq_ == 0U) ? remaining(nowMs, healthDueMs_) : 0U;
        for (const Conn &conn : conns_) {
            if (!connBusy_ &&
                (conn.wantClose || conn.recvPending || (conn.state == ConnState::kWantOpen))) {
                next = std::min(next, remaining(nowMs, conn.dueMs));
                if (conn.wantClose || conn.recvPending) {
                    next = 0U;
                }
            } else if ((conn.state == ConnState::kBackoff) || (conn.state == ConnState::kOpening)) {
                next = std::min(next, remaining(nowMs, conn.dueMs));
            }
        }
        return (opPending_ && (healthSeq_ != 0U)) ? kForever : next;
    }
    default:
        return kForever;
    }
}
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)

LinkSnapshot LinkFsm::snapshot(uint32_t nowMs) const noexcept
{
    LinkSnapshot out = snap_;
    out.channelDead = out.channelDead || ((engine_ != nullptr) && engine_->dead());
    out.backoffRemainingMs = 0U;
    if (phase_ == Phase::kPowerCycle) {
        out.backoffRemainingMs = remaining(nowMs, cycleEndMs_);
    } else if ((phase_ == Phase::kPdpUp) && !opPending_) {
        out.backoffRemainingMs = remaining(nowMs, nextMs_);
    }
    return out;
}

} // namespace cellular
