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
constexpr uint32_t kShutdownPollMs = 20U;
constexpr uint32_t kPowerFaultRetryMs = 100U; // PWREN 拉低失败后的重试间隔
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
    cmd.quietFirst = quietNext_;
    quietNext_ = false;
    // 查询应答与同前缀 URC 按行形状区分（+CEREG?/+CREG? 的 <n>,<stat>；+CPIN 本身无歧义）。
    if ((op == Op::kCereg) || (op == Op::kCreg) || (op == Op::kHealthCereg)) {
        cmd.match = &isRegQueryLine;
        cmd.matchPrefix =
            (op == Op::kCreg) ? std::string_view{"+CREG:"} : std::string_view{"+CEREG:"};
    } else if ((op == Op::kCpin) || (op == Op::kHealthCpin)) {
        cmd.match = &isCpinLine;
        cmd.matchPrefix = "+CPIN:";
    }
    // 先登记 op 再提交：写失败等同步路径不会在 submit 内回调完成，但保持登记在前更稳妥。
    const Op previous = op_;
    const uint32_t previousToken = opToken_;
    cmd.token = nextToken();
    op_ = op;
    opPending_ = true;
    opToken_ = cmd.token;
    if (!engine_->submit(cmd, nowMs)) {
        op_ = previous;
        opPending_ = false;
        opToken_ = previousToken;
        return false;
    }
    return true;
}

/**
 * @brief 生成下一个非零提交 token（过期应答丢弃用）
 * @retval 非零 token
 * @note 仅 kNet 执行器任务调用，非 ISR。
 */
uint32_t LinkFsm::nextToken() noexcept
{
    ++opSeq_;
    if (opSeq_ == 0U) {
        ++opSeq_;
    }
    return opSeq_;
}

/**
 * @brief 提交一条连接类命令并登记其 token
 * @param cmd: 已填好文本/回调的命令（本函数写入 token）
 * @param nowMs: 当前毫秒时间戳
 * @retval true 已入队
 * @note 仅 kNet 执行器任务调用，非 ISR；失败时不改 connToken_。
 */
bool LinkFsm::submitConnCmd(AtCommand &cmd, uint32_t nowMs) noexcept
{
    const uint32_t previous = connToken_;
    cmd.token = nextToken();
    connToken_ = cmd.token;
    if (!engine_->submit(cmd, nowMs)) {
        connToken_ = previous;
        return false;
    }
    return true;
}

/**
 * @brief 取消引擎中属于本 FSM 的在途命令并丢弃其登记
 * @param nowMs: 当前毫秒时间戳
 * @note 之后到达的旧应答由 AtEngine 重新同步握手隔离；仅 kNet 执行器任务调用，非 ISR。
 */
void LinkFsm::abortInflight(uint32_t nowMs) noexcept
{
    if ((engine_ != nullptr) && (opPending_ || connBusy_)) {
        engine_->reset(nowMs);
    }
    clearOps();
}

/**
 * @brief 记录 Ready 阶段首次丢失注册的时刻（只记第一次，用于 180 s 升级 L3）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR。
 */
void LinkFsm::markRegLost(uint32_t nowMs) noexcept
{
    if (!regLost_) {
        regLost_ = true;
        regLostMs_ = nowMs;
    }
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
    auto *self = static_cast<LinkFsm *>(ctx);
    if (self->engine_->frontToken() == self->opToken_) { // 过期命令的行不得写入当前操作状态
        self->onOpLine(line);
    }
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
    if ((self->connOpId_ < kConnSlots) && (self->engine_->frontToken() == self->connToken_)) {
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
    case Op::kHealthCpin: {
        // 同一次查询期间出现的 +CPIN 行（应答或同前缀 URC）以“最坏状态”为准：
        // 非 READY 不会被其后到达的 READY 覆盖，也不会因查询超时/出错而丢失。
        SimStatus seen = SimStatus::kUnknown;
        if (parseCpin(line, seen) && (!gotSim_ || (seen != SimStatus::kReady))) {
            pendingSim_ = seen;
            gotSim_ = true;
        }
        break;
    }
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
    if (rsp.token != opToken_) {
        return; // 过期完成（命令提交后已发生 L2/L3/SIM 丢失等恢复）：不得推进当前阶段
    }
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
        } else if (gotSim_ && (ok || (pendingSim_ != SimStatus::kReady))) {
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
                markRegLost(now);
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
    if (engine_ != nullptr) {
        // 同步阶段（波特率探测）不做握手隔离；之后任何命令超时/取消都必须先重新同步。
        engine_->setResyncEnabled(isOnlinePhase(phase));
    }
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
        deactRan_ = false;
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
    opToken_ = 0U;
    connToken_ = 0U;
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
 * @brief 清除全部槽位的“重开前须先 QICLOSE”标记（模块断电后 socket 不存在）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::clearStaleClose() noexcept
{
    for (Conn &conn : conns_) {
        conn.closeFirst = false;
    }
}

/**
 * @brief 断开模块电源并复位相关状态
 * @param nowMs: 当前毫秒时间戳
 * @retval true PWREN 已确认拉低（进入 Off）
 * @retval false 写 GPIO 失败（进入 PowerFault 由调用方重试）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool LinkFsm::cutPower(uint32_t nowMs) noexcept
{
    if (engine_ != nullptr) {
        engine_->powerDown(nowMs); // 断电即结算全部在途/排队命令并清空接收状态（r4 R2）
    }
    const bool ok = (hooks_.setPower == nullptr) || hooks_.setPower(hooks_.ctx, false);
    if (ok) {
        offSinceValid_ = true;
        offSinceMs_ = nowMs;
    }
    return ok;
}

/**
 * @brief 进入电源故障阶段：PWREN 拉低失败，周期重试直到 GPIO 成功（r4 R5）
 * @param nowMs: 当前毫秒时间戳
 * @param resumeCycle: true 表示来自 L3：GPIO 成功后转入 PowerCycle，并按首次尝试时保存的冷却时长 cycleHoldMs_ 保持低电平（r5 F5）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。此阶段不算断电完成（shutdownDone 为 false）。
 */
void LinkFsm::enterPowerFault(uint32_t nowMs, bool resumeCycle) noexcept
{
    faultResumeCycle_ = resumeCycle;
    snap_.pdpUp = false;
    enterPhase(Phase::kPowerFault, nowMs);
    nextMs_ = nowMs + kPowerFaultRetryMs;
}

/**
 * @brief 轮询电源故障阶段：重试拉低 PWREN，成功后转 Off
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollPowerFault(uint32_t nowMs) noexcept
{
    if (!reached(nowMs, nextMs_)) {
        return;
    }
    if (cutPower(nowMs)) {
        if (faultResumeCycle_) {
            faultResumeCycle_ = false;
            // 冷却时长在首次尝试拉低时已保存；PWREN 实际在此刻才拉低，低电平按原冷却时长保持（r5 F5）。
            cycleEndMs_ = nowMs + cycleHoldMs_;
            enterPhase(Phase::kPowerCycle, nowMs);
        } else {
            enterPhase(Phase::kOff, nowMs);
        }
    } else {
        nextMs_ = nowMs + kPowerFaultRetryMs;
    }
}

/**
 * @brief 开始上电流程
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::startPowerOn(uint32_t nowMs) noexcept
{
    if (hooks_.setPower != nullptr) {
        static_cast<void>(hooks_.setPower(hooks_.ctx, true));
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
    clearStaleClose();   // 断电后模块侧 socket 全部消失
    cycleHoldMs_ = hold; // 首次尝试拉低时即保存冷却时长（GPIO 失败重试成功后仍按此保持，r5 F5）
    cycleEndMs_ = nowMs + hold;
    const bool cut = cutPower(nowMs);
    simWasBad_ = false;
    regLost_ = false;
    pdpForceDeact_ = false;
    pdpFails_ = 0U;
    l2Count_ = 0U;
    snap_.pdpUp = false;
    snap_.reg = RegStatus::kUnknown;
    snap_.sim = SimStatus::kUnknown;
    snap_.csq = kCsqUnknown;
    if (!cut) {
        enterPowerFault(nowMs, true);
        return;
    }
    enterPhase(Phase::kPowerCycle, nowMs);
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
    abortInflight(nowMs); // 在途的 QIACT/巡检/连接命令的结果不得再推进新一轮 PDP 流程
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
    if ((phase_ == Phase::kOff) || (phase_ == Phase::kPowerCycle) ||
        (phase_ == Phase::kPowerFault) || shuttingDown_) {
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

void LinkFsm::onDiagLine(std::string_view line) noexcept
{
    SimStatus sim = SimStatus::kUnknown;
    if (parseCpin(line, sim) && (sim != SimStatus::kReady) && (sim != SimStatus::kUnknown)) {
        diagSim_ = sim;
    }
}

void LinkFsm::beginShutdown(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    latched_ = true;
    const uint32_t deadline = (deadlineMs != 0U) ? deadlineMs : (nowMs + kShutdownTotalMs);
    if (!shuttingDown_) {
        startStop(nowMs, deadline);
    } else if (static_cast<int32_t>(deadline - shutDeadlineMs_) < 0) {
        shutDeadlineMs_ = deadline; // 已在关机：PW1 截止更早时以 PW1 为准
    }
}

/**
 * @brief 开始优雅关机/停止流程
 * @param nowMs: 当前毫秒时间戳
 * @param deadlineMs: 关机绝对截止时间戳（毫秒，同 nowMs 时基）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::startStop(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    if (shuttingDown_) {
        return;
    }
    if (phase_ == Phase::kPowerFault) {
        return; // 已在重试拉低 PWREN
    }
    if ((phase_ == Phase::kOff) || (phase_ == Phase::kPowerCycle)) {
        if (cutPower(nowMs)) {
            enterPhase(Phase::kOff, nowMs);
        } else {
            enterPowerFault(nowMs);
        }
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
    shutDeadlineMs_ = deadlineMs;
    shutStep_ = ShutStep::kClose;
    shutCloseId_ = 0U;
    poweredDownSeen_ = false;
    // 在途的巡检/连接命令：令牌作废（其结果不再推进状态），但不取消引擎里的命令——
    // 等它自然完成或超时后再发 QICLOSE/QPOWD，避免其迟到应答冒充关机命令的 OK。
    clearOps();
    // 通道已处于重新同步/静默等待 = 应答不可信：按“AT 无响应”处理，直接断电（裁决 r3 I1）。
    if (engine_->resyncing()) {
        finishStop(nowMs);
    }
}

/**
 * @brief 完成停止流程并断电
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::finishStop(uint32_t nowMs) noexcept
{
    const bool cut = cutPower(nowMs);
    shuttingDown_ = false;
    clearOps();
    snap_.pdpUp = false;
    notifyAllClosed();
    clearStaleClose();
    for (Conn &conn : conns_) {
        if (conn.state == ConnState::kWantOpen) {
            conn.state = ConnState::kIdle;
        }
    }
    if (cut) {
        enterPhase(Phase::kOff, nowMs);
    } else {
        enterPowerFault(nowMs); // GPIO 失败不得宣告关机完成（r4 R5）
    }
}

// NOLINTBEGIN(readability-function-cognitive-complexity): 关机步骤状态机按 ADR-0021 §2.9 平铺，拆分会割裂“截止/通道不可信/等待在途命令”的优先顺序，主机测试逐路径覆盖。
/**
 * @brief 轮询关机流程
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollShutdown(uint32_t nowMs) noexcept
{
    if (reached(nowMs, shutDeadlineMs_)) {
        finishStop(nowMs); // 绝对截止（与 PW1 同一时刻）：无论 QPOWD 进展如何，到点直接断电
        return;
    }
    if (engine_->resyncing() || engine_->dead()) {
        finishStop(nowMs); // 关机命令超时/丢字节：通道不可信，不再等 QPOWD 的 OK，直接断电
        return;
    }
    if (engine_->busy() && !opPending_) {
        return; // 作废的在途命令尚未完成：等其应答/超时后再发关机序列
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

// NOLINTEND(readability-function-cognitive-complexity)

/**
 * @brief 轮询 Off 阶段（等待使能）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollOff(uint32_t nowMs) noexcept
{
    if (!enabled_ || !powerAllowed_ || !startAllowed_ || latched_ || shuttingDown_) {
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
    if (!reached(nowMs, cycleEndMs_)) {
        return;
    }
    if (enabled_ && powerAllowed_ && startAllowed_ && !latched_ && !shuttingDown_) {
        startPowerOn(nowMs);
    } else {
        // 冷却期间被低电/配网/关机锁定：回到 Off 等待放行，不得再拉高 PWREN（裁决 r3 I6）。
        enterPhase(Phase::kOff, nowMs);
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
        quietNext_ = true; // 上电/换波特率后的探测：先等通道静默，避免把启动噪声当应答
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
        if (ok) {
            // QIDEACT 成功会关闭模块侧全部 socket：此前未确认关闭的槽位此后才可重用（裁决 r3 I4）。
            for (Conn &conn : conns_) {
                conn.closeFirst = false;
            }
        }
        // 无论 QIDEACT 成败：之后 QIACT? 仍见该 context 在线即说明承载未真正释放，按恢复失败处理
        // （r4 R4 失败路径；r5 F4 补 QIDEACT OK 但承载仍在线）。
        deactRan_ = true;
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
            if (deactRan_) {
                // QIDEACT 之后承载仍在线：L2 重建未完成，不得宣告 Ready；退避递增并累计升级计数。
                deactRan_ = false;
                failPdpRecovery(now);
            } else {
                enterReady(now);
            }
        } else {
            pdpStep_ = PdpStep::kActivate;
        }
        break;
    case PdpStep::kActivate:
        if (ok) {
            enterReady(now);
            break;
        }
        failPdpRecovery(now);
        break;
    default:
        break;
    }
}

/**
 * @brief PDP 恢复失败：累计失败次数，到阈值升级 L3，否则以递增退避重进 L2
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::failPdpRecovery(uint32_t nowMs) noexcept
{
    if (pdpFails_ < std::numeric_limits<uint8_t>::max()) {
        ++pdpFails_;
    }
    if (pdpFails_ >= kPolicy.qiactFailToL3) {
        enterL3(nowMs);
    } else {
        enterL2(nowMs, true);
    }
}

/**
 * @brief 轮询 Ready 阶段的周期查询（CSQ/注册/PDP）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void LinkFsm::pollReady(uint32_t nowMs) noexcept
{
    if (regLost_ && reached(nowMs, regLostMs_ + kPolicy.registerTimeoutMs)) {
        enterL3(nowMs); // 注册丢失超过 180 s 仍未恢复：与 Register 阶段同级，升级整机复位
        return;
    }
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
            if (w.ok && submitConnCmd(cmd, nowMs)) {
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
        if ((conn.state == ConnState::kWantOpen) && conn.closeFirst && reached(nowMs, conn.dueMs)) {
            // 上次 QIOPEN 结果 URC 超时，模块侧该槽可能仍在连接：先关闭再重开，避免占用/竞态。
            w.str("AT+QICLOSE=");
            w.num(id);
            cmd.text = w.view();
            cmd.timeoutMs = at_timeout::kQiclose;
            if (w.ok && submitConnCmd(cmd, nowMs)) {
                connBusy_ = true;
                connOp_ = ConnOp::kCloseStale;
                connOpId_ = id;
                return;
            }
            continue;
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
            if (w.ok && submitConnCmd(cmd, nowMs)) {
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
            conn.closeFirst = true; // connFailed 之后：重开前必须先 QICLOSE（见 kWantOpen 分支）
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
            cmd.rawMax = kReadChunkBytes; // 声明长度超过请求长度即协议错误，防止多读/越界
            if (w.ok && submitConnCmd(cmd, nowMs)) {
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
    if (rsp.token != connToken_) {
        return; // 过期完成：该连接命令已被恢复流程取消，结果不得套用到当前槽状态
    }
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
    case ConnOp::kCloseStale:
        if (ok) {
            conn.closeFirst = false;
        } else {
            // QICLOSE 未确认：不得在该槽位上重开（模块侧可能仍占用），升级 L2（QIDEACT 关闭全部 socket）。
            enterL2(rsp.nowMs, true);
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
    conn.closeFirst = false;
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
    // QISEND 只允许在已切到 460800 之后（115200 阶段一律拒绝）。单次载荷上限 kSendChunkBytes=256 B
    // （460800 下线上约 5.6 ms，保证 kNet 轮内余量；r5 F6 终裁：不把剩余量传入接口，靠固定上限保证）。
    if (curBaud_ < kPolicy.fastBaud) {
        return false;
    }
    Writer w{cmdBuf_.data(), cmdBuf_.size()};
    w.str("AT+QISEND=");
    w.num(id);
    w.str(",");
    w.num(static_cast<uint32_t>(payload.size())); // ≤256，无损
    AtCommand cmd;
    cmd.text = w.view();
    cmd.timeoutMs = at_timeout::kQisend;
    cmd.onDone = &LinkFsm::thunkConnDone;
    cmd.ctx = this;
    cmd.payload = payload;
    if (!w.ok || !submitConnCmd(cmd, nowMs)) {
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
    if (phase_ == Phase::kPowerFault) {
        return; // 电源故障中 PWREN 未确认拉低：RDY/CPIN/注册等上电类 URC 一律忽略（r5 F5）
    }
    if (line.starts_with("RDY")) {
        // 模块自行重启（掉电重上电/看门狗）：回到同步阶段重新初始化。
        if (!shuttingDown_ && isOnlinePhase(phase_)) {
            handleModuleRestart(nowMs);
        }
        return;
    }
    if (line.starts_with("+CPIN:")) {
        SimStatus sim = SimStatus::kUnknown;
        if (parseCpin(line, sim) && isOnlinePhase(phase_)) {
            onSimState(sim, nowMs);
        }
        return;
    }
    if (line.starts_with("+CEREG:") || line.starts_with("+CREG:") || line.starts_with("+CGREG:")) {
        if (isRegQueryLine(line)) {
            return; // <n>,<stat> 形状是 CEREG? 的应答，漏到 URC 路径说明是迟到的旧应答，不能当实时状态
        }
        RegStatus reg = RegStatus::kUnknown;
        if (!parseReg(line, reg)) {
            return;
        }
        snap_.reg = reg;
        if (opPending_ &&
            ((op_ == Op::kCereg) || (op_ == Op::kCreg) || (op_ == Op::kHealthCereg))) {
            gotReg_ = false; // 在途查询的应答已被更新的 URC 取代：本轮查询结果作废，下轮重查
        }
        if ((phase_ == Phase::kRegister) && isRegistered(reg)) {
            enterPhase(Phase::kPdpUp, nowMs);
        } else if (phase_ == Phase::kReady) {
            if (!isRegistered(reg)) {
                markRegLost(nowMs);
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
                if (conns_[slot].state == ConnState::kOpen) {
                    conns_[slot].recvPending = true;
                } else if (droppedRecv_ < std::numeric_limits<uint16_t>::max()) {
                    ++droppedRecv_; // 无主槽位（未打开/已关闭）的 recv 通知：丢弃并计数，避免空转
                }
            }
        }
    }
}
// NOLINTEND(readability-function-size, readability-function-cognitive-complexity)

// NOLINTNEXTLINE(readability-function-size): ADR-0021 §2.5 轮询按阶段平铺（超时/退避/巡检/断电），拆分会割裂恢复表，主机测试逐路径覆盖。
void LinkFsm::poll(uint32_t nowMs) noexcept
{
    if (engine_ == nullptr) {
        return;
    }
    if (diagSim_ != SimStatus::kUnknown) {
        const SimStatus sim = diagSim_;
        diagSim_ = SimStatus::kUnknown;
        if ((phase_ != Phase::kOff) && (phase_ != Phase::kPowerFault) &&
            (phase_ != Phase::kPowerCycle) && !shuttingDown_) {
            onSimState(sim, nowMs); // 调试查询看到卡异常：与 FSM 自己的查询同样处理（r4 R3）
        }
    }
    if (phase_ == Phase::kPowerFault) {
        pollPowerFault(nowMs);
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
        startStop(nowMs, nowMs + kShutdownTotalMs);
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
        return (enabled_ && powerAllowed_ && startAllowed_ && !latched_)
                   ? (offSinceValid_ ? remaining(nowMs, offSinceMs_ + kPolicy.powerOffMinMs) : 0U)
                   : kForever;
    case Phase::kPowerFault:
        return remaining(nowMs, nextMs_);
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
        if (regLost_) {
            next = std::min(next, remaining(nowMs, regLostMs_ + kPolicy.registerTimeoutMs));
        }
        // pollConns 在连接命令在途时整体不处理：此时任何槽位都不应缩短等待（完成回调会唤醒）。
        for (const Conn &conn : conns_) {
            if (connBusy_) {
                break;
            }
            const bool isOpen = (conn.state == ConnState::kOpen);
            const bool opening = (conn.state == ConnState::kOpening);
            // 只为“当前就能执行”的连接操作缩短等待：无主槽位的 recv/关闭请求不得让执行器空转。
            const bool runnable =
                (conn.wantClose && (isOpen || opening)) || (conn.recvPending && isOpen);
            if (!connBusy_ && runnable) {
                next = 0U;
            } else if ((!connBusy_ && (conn.state == ConnState::kWantOpen)) ||
                       (conn.state == ConnState::kBackoff) || opening) {
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
    out.droppedRecv = droppedRecv_;
    for (uint8_t id = 0U; id < kConnSlots; ++id) {
        const Conn &conn = conns_[id];
        ConnSnapshot &slot = out.conn[id];
        slot.state = static_cast<uint8_t>(conn.state);
        slot.fails = conn.fails;
        const bool timed = (conn.state == ConnState::kBackoff) ||
                           (conn.state == ConnState::kOpening) ||
                           (conn.state == ConnState::kWantOpen);
        slot.backoffRemainingMs = timed ? remaining(nowMs, conn.dueMs) : 0U;
    }
    if (phase_ == Phase::kPowerCycle) {
        out.backoffRemainingMs = remaining(nowMs, cycleEndMs_);
    } else if ((phase_ == Phase::kPdpUp) && !opPending_) {
        out.backoffRemainingMs = remaining(nowMs, nextMs_);
    }
    return out;
}

} // namespace cellular
