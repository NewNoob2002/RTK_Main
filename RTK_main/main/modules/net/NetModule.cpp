// P2 4G 联网模块实现（ADR-0021 / N1）：UART1 驱动、PWREN 控制与关机参与者。
// net.* 持久化由 StorageModule 负责（NetStore），本文件只在启动期读取一次。
// 线程：init 启动前单线程；wait/poll/onCommand 仅 kNet 执行器任务；
//       shutdownBegin/shutdownIsDone/shutdownTimeout/submitAt 可跨任务（仅原子量与单写者缓冲）。
// 堆档位：启动期一次性（uart_driver_install 在 init 内分配 RX 环形缓冲与事件队列）；运行期无堆分配。
#include "modules/net/NetModule.hpp"

#include <algorithm>
#include <cstring>
#include <span>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "AppConfig.hpp"
#include "AppContext.hpp"
#include "board/BoardProfile.hpp"
#include "cellular/Policy.hpp"
#include "def/NetDef.hpp"
#include "fw/Log.hpp"
#include "modules/power/PowerModule.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {

constexpr uint32_t kBaudBoot = 115200U;
using Field = net_store::Field;

/**
 * @brief 把框架状态码映射为模块结果码
 * @param s: 框架状态码
 * @retval 对应结果码；未映射值归为 kIoError
 * @note 纯函数，线程/ISR 安全。
 */
fw::Result toResult(fw::Status s) noexcept
{
    switch (s) {
    case fw::Status::kOk:
        return fw::Result::kOk;
    case fw::Status::kInvalidArg:
        return fw::Result::kInvalidArg;
    case fw::Status::kNoSpace:
        return fw::Result::kNoSpace;
    case fw::Status::kBusy:
        return fw::Result::kBusy;
    case fw::Status::kNotSupported:
        return fw::Result::kNotSupported;
    case fw::Status::kInvalidState:
        return fw::Result::kInvalidState;
    default:
        return fw::Result::kIoError;
    }
}

/**
 * @brief 取定长字段的文本视图
 * @param field: 以 0 结尾的字段
 * @retval 视图（不含结尾 0）
 * @note 纯函数，线程/ISR 安全。
 */
std::string_view fieldView(const Field &field) noexcept
{
    return std::string_view(field.data(), ::strnlen(field.data(), field.size()));
}

/**
 * @brief 把 UART 事件队列唤醒一次（事件类型无意义，wait() 会忽略）
 * @param queue: UART 事件队列句柄，可为 nullptr
 * @note 任意任务可调用，不阻塞。
 */
void wakeQueue(void *queue) noexcept
{
    if (queue == nullptr) {
        return;
    }
    const uart_event_t ev{.type = UART_EVENT_MAX, .size = 0U, .timeout_flag = false};
    static_cast<void>(xQueueSend(static_cast<QueueHandle_t>(queue), &ev, 0));
}

} // namespace

/**
 * @brief LinkFsm 电源钩子：拉高/拉低 PWREN 并清 UART 输入
 * @param ctx: 回调上下文指针
 * @param on: true=上电，false=断电
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。gpio_set_level 失败只能记录错误
 *       （钩子无返回值）：上电失败由 FSM 的探测超时/恢复阶梯兜底。
 */
void NetModule::powerHook(void *ctx, bool on) noexcept
{
    const auto *self = static_cast<const NetModule *>(ctx);
    if (self->pwrenGpio_ >= 0) {
        // 引脚号来自板级表，已核实为输出 GPIO；on 对应高电平（PWRKEY 已硬件接地）。
        const esp_err_t err =
            gpio_set_level(static_cast<gpio_num_t>(self->pwrenGpio_), on ? 1U : 0U);
        if (err != ESP_OK) {
            FW_LOGE(fw::TraceCat::kSys, "net: PWREN %s failed err=%d", on ? "high" : "low",
                    static_cast<int>(err));
        }
    }
    if (self->rt_ != nullptr && self->rt_->uartInstalled) {
        static_cast<void>(uart_flush_input(static_cast<uart_port_t>(self->uartPort_)));
    }
    FW_LOGI(fw::TraceCat::kSys, "net: PWREN %s", on ? "high" : "low");
}

/**
 * @brief LinkFsm 波特率钩子：切换 UART 波特率并清输入
 * @param ctx: 回调上下文指针
 * @param baud: 目标波特率
 * @retval true 成功/条件成立
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool NetModule::baudHook(void *ctx, uint32_t baud) noexcept
{
    const auto *self = static_cast<const NetModule *>(ctx);
    if (self->rt_ == nullptr || !self->rt_->uartInstalled) {
        return false;
    }
    const auto port = static_cast<uart_port_t>(self->uartPort_);
    // 波特率上限 460800，转换为 IDF 的 uint32 无损。
    const bool ok = uart_set_baudrate(port, baud) == ESP_OK;
    static_cast<void>(uart_flush_input(port));
    return ok;
}

/**
 * @brief AtEngine 写钩子：把字节写入 UART，并有界等待发送完成
 * @param ctx: 回调上下文指针
 * @param bytes: 待写字节
 * @retval true 全部写入且在 kWriteTimeoutMs 内发送完成
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。TX 环形缓冲为 0，uart_write_bytes 只阻塞到
 *       字节进入硬件 FIFO（无流控，随波特率有界），随后 uart_wait_tx_done 以超时兜底。
 */
bool NetModule::writeHook(void *ctx, std::span<const uint8_t> bytes) noexcept
{
    const auto *self = static_cast<const NetModule *>(ctx);
    if (self->rt_ == nullptr || !self->rt_->uartInstalled || bytes.empty()) {
        return false;
    }
    const auto port = static_cast<uart_port_t>(self->uartPort_);
    const int n = uart_write_bytes(port, bytes.data(), bytes.size());
    if ((n < 0) || (static_cast<size_t>(n) != bytes.size())) {
        return false;
    }
    return uart_wait_tx_done(port, pdMS_TO_TICKS(kWriteTimeoutMs)) == ESP_OK;
}

/**
 * @brief AtEngine URC 钩子：转发给 LinkFsm
 * @param ctx: 回调上下文指针
 * @param line: 一行文本（不含换行）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::urcHook(void *ctx, std::string_view line) noexcept
{
    const auto *self = static_cast<NetModule *>(ctx);
    self->rt_->fsm.onUrc(line, rtos::nowMs());
}

/**
 * @brief 连接打开结果回调（仅记录日志）
 * @param id: 连接编号
 * @param ok: 是否成功
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::connOpened(void * /*ctx*/, uint8_t id, bool ok) noexcept
{
    FW_LOGI(fw::TraceCat::kSys, "net: conn%u open %s", static_cast<unsigned>(id),
            ok ? "ok" : "fail");
}

/**
 * @brief 连接关闭回调（仅记录日志）
 * @param id: 连接编号
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::connClosed(void * /*ctx*/, uint8_t id) noexcept
{
    FW_LOGI(fw::TraceCat::kSys, "net: conn%u closed", static_cast<unsigned>(id));
}

/**
 * @brief 连接收包回调（N1 无消费者，丢弃）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::connData(void * /*ctx*/, uint8_t /*id*/, std::span<const uint8_t> /*data*/) noexcept
{
    // N1 无上层消费者（NTRIP 在后续卡片接入）；数据在此丢弃。
}

/**
 * @brief 连接发送完成回调（N1 无消费者）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::connSendDone(void * /*ctx*/, uint8_t /*id*/, bool /*ok*/) noexcept {}

/**
 * @brief 控制台 AT 响应行回调转发
 * @param ctx: 回调上下文指针
 * @param line: 一行文本（不含换行）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::atLineThunk(void *ctx, std::string_view line) noexcept
{
    static_cast<NetModule *>(ctx)->rt_->mailbox.onLine(line);
}

/**
 * @brief 控制台 AT 完成回调转发（按 token 丢弃过期结果）
 * @param ctx: 回调上下文指针
 * @param rsp: 命令响应
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::atDoneThunk(void *ctx, const cellular::AtResponse &rsp) noexcept
{
    static_cast<NetModule *>(ctx)->rt_->mailbox.onDone(rsp);
}

/**
 * @brief 关机参与者 begin：记录 PW1 起点并置位关机请求，唤醒 kNet
 * @param stage: 关机阶段（本模块不区分，统一在 kQuiesce 处理）
 * @param ctx: NetModule 实例指针
 * @note 由 PowerModule 任务调用，仅写原子量与投递唤醒事件；非 ISR，不分配堆，不阻塞。
 */
void NetModule::shutdownBegin(board::ShutdownStage stage, void *ctx) noexcept
{
    static_cast<void>(stage);
    const auto *self = static_cast<NetModule *>(ctx);
    if (self->rt_ == nullptr) {
        return;
    }
    self->rt_->shutdownStartMs.store(rtos::nowMs(), std::memory_order_release);
    self->rt_->shutdownReq.store(true, std::memory_order_release);
    wakeQueue(self->rt_->uartQueue); // kNet 可能正阻塞在长超时上：让它立刻起步，沿用同一截止时刻
}

/**
 * @brief 关机参与者 isDone：读取关机完成标志
 * @param ctx: NetModule 实例指针
 * @retval true 模块已断电或关机超时收尾完成
 * @note 由 PowerModule 任务调用，仅读原子量；非 ISR，不分配堆。
 */
bool NetModule::shutdownIsDone(void *ctx) noexcept
{
    const auto *self = static_cast<const NetModule *>(ctx);
    return (self->rt_ == nullptr) || self->rt_->shutdownDone.load(std::memory_order_acquire);
}

/**
 * @brief 关机参与者 onTimeout：PW1 有界等待到期仍未完成时，受控硬切 PWREN 兜底
 * @param ctx: NetModule 实例指针
 * @note 由 PowerModule 任务调用，只做一次 gpio_set_level（线程安全）；非 ISR，不分配堆，不阻塞。
 */
void NetModule::shutdownTimeout(void *ctx) noexcept
{
    const auto *self = static_cast<NetModule *>(ctx);
    if (self->rt_ == nullptr) {
        return;
    }
    FW_LOGW(fw::TraceCat::kSys, "net: PW1 timeout, hard-cut PWREN");
    if (self->cutPower()) {
        self->rt_->shutdownDone.store(true, std::memory_order_release);
    }
}

/**
 * @brief 硬切 4G 电源：PWREN 拉低并检查返回值
 * @retval true 已成功拉低；false 无 PWREN 脚或 GPIO 写失败
 * @note 仅 kNet 任务调用；不分配堆。
 */
bool NetModule::cutPower() const noexcept
{
    if (pwrenGpio_ < 0) {
        return true;
    }
    const esp_err_t err = gpio_set_level(static_cast<gpio_num_t>(pwrenGpio_), 0U);
    if (err != ESP_OK) {
        FW_LOGE(fw::TraceCat::kSys, "net: PWREN cut failed err=%d", static_cast<int>(err));
        return false;
    }
    return true;
}

fw::Result NetModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kInvalidState;
    }
    if (rt_ == nullptr) {
        return fw::Result::kOk; // 未分配运行态（net.enable=0/E1/配网）：不触碰 GPIO26 与 UART1
    }
    const board::Profile &profile = board::current();
    if (!board::hasFeature(profile, board::Feature::kCellular) || profile.cellular.uart < 0 ||
        profile.cellular.pwrenGpio < 0) {
        return fw::Result::kOk; // 板型不含 4G
    }
    uartPort_ = profile.cellular.uart;
    pwrenGpio_ = profile.cellular.pwrenGpio;
    net_store::load(rt_->cfg);
    const fw::Result uartResult = initUart();
    if (!fw::isOk(uartResult)) {
        return uartResult;
    }
    cellular::AtPort port;
    port.ctx = this;
    port.write = &NetModule::writeHook;
    port.onUrc = &NetModule::urcHook;
    port.urcPrefixes = std::span<const std::string_view>(cellular::kUrcPrefixes);
    rt_->engine.bind(port);
    rt_->engine.setDeadThreshold(cellular::kPolicy.deadTimeouts);
    cellular::LinkHooks hooks;
    hooks.ctx = this;
    hooks.setPower = &NetModule::powerHook;
    hooks.setBaud = &NetModule::baudHook;
    cellular::ConnEvents events;
    events.ctx = this;
    events.onOpened = &NetModule::connOpened;
    events.onClosed = &NetModule::connClosed;
    events.onData = &NetModule::connData;
    events.onSendDone = &NetModule::connSendDone;
    rt_->fsm.bind(rt_->engine, hooks, events);
    applyConfig();
    initialized_ = true;
    present_.store(true, std::memory_order_release);
    FW_LOGI(fw::TraceCat::kSys, "net: ready uart%d tx=%d rx=%d pwren=GPIO%d en=%d", uartPort_,
            profile.cellular.txGpio, profile.cellular.rxGpio, pwrenGpio_, rt_->cfg.enable ? 1 : 0);
    return fw::Result::kOk;
}

fw::Result NetModule::initUart() noexcept
{
    const board::CellularPins &pins = board::current().cellular;
    const auto port = static_cast<uart_port_t>(uartPort_);
    // PWREN 先锁低再切输出，避免上电毛刺把模块意外拉起。
    const auto pwren = static_cast<gpio_num_t>(pwrenGpio_);
    esp_err_t err = gpio_set_level(pwren, 0U);
    if (err != ESP_OK) {
        return toResult(rtos::fromEspErr(err));
    }
    gpio_config_t pwrenCfg{};
    pwrenCfg.pin_bit_mask = 1ULL << static_cast<unsigned>(pwrenGpio_);
    pwrenCfg.mode = GPIO_MODE_OUTPUT;
    pwrenCfg.pull_up_en = GPIO_PULLUP_DISABLE;
    pwrenCfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pwrenCfg.intr_type = GPIO_INTR_DISABLE;
    err = gpio_config(&pwrenCfg);
    if (err != ESP_OK) {
        return toResult(rtos::fromEspErr(err));
    }
    QueueHandle_t queue = nullptr;
    err = uart_driver_install(port, static_cast<int>(kUartRxBufBytes),
                              static_cast<int>(kUartTxBufBytes), kUartQueueLen, &queue, 0);
    if (err != ESP_OK) {
        return toResult(rtos::fromEspErr(err));
    }
    rt_->uartInstalled = true;
    rt_->uartQueue = queue;
    const uart_config_t uartCfg{
        .baud_rate = static_cast<int>(kBaudBoot),
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0U,
        .rx_glitch_filt_thresh = 0U,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {},
    };
    err = uart_param_config(port, &uartCfg);
    if (err != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(err)));
    }
    err = uart_set_pin(port, pins.txGpio, pins.rxGpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return initFailed(toResult(rtos::fromEspErr(err)));
    }
    return fw::Result::kOk;
}

/**
 * @brief init 失败回滚：断电并释放 UART 驱动后返回原错误
 * @param cause: 触发回滚的错误码
 * @retval cause 原样返回
 * @note 仅启动期单线程调用；非 ISR。
 */
fw::Result NetModule::initFailed(fw::Result cause) noexcept
{
    static_cast<void>(stopBoot());
    return cause;
}

fw::Status NetModule::stopBoot() noexcept
{
    static_cast<void>(cutPower());
    if (rt_ != nullptr && rt_->uartInstalled) {
        static_cast<void>(uart_driver_delete(static_cast<uart_port_t>(uartPort_)));
        rt_->uartInstalled = false;
        rt_->uartQueue = nullptr;
    }
    present_.store(false, std::memory_order_release);
    initialized_ = false;
    return fw::Status::kOk;
}

fw::Status NetModule::registerShutdown() noexcept
{
    if (!present() || shutdownRegistered_) {
        return fw::Status::kOk;
    }
    power::ShutdownParticipant entry;
    entry.name = "net_power";
    entry.begin = &NetModule::shutdownBegin;
    entry.isDone = &NetModule::shutdownIsDone;
    entry.userCtx = this;
    entry.onTimeout = &NetModule::shutdownTimeout;
    entry.stage = board::ShutdownStage::kQuiesce;
    entry.milestone = power::ShutdownMilestone::kNetPower;
    const fw::Status status = power::registerShutdownParticipant(entry);
    shutdownRegistered_ = fw::isOk(status);
    return status;
}

/**
 * @brief 把启动期读到的配置下发给 LinkFsm
 * @note 仅启动期单线程调用，非 ISR；不分配堆。
 */
void NetModule::applyConfig() noexcept
{
    // 用户名非空时按 PAP 认证；未设置则无认证（多数物联网卡只需 APN）。
    const net_store::Saved &cfg = rt_->cfg;
    const uint8_t auth = fieldView(cfg.user).empty() ? 0U : 1U;
    rt_->fsm.setConfig(fieldView(cfg.apn), fieldView(cfg.user), fieldView(cfg.pass), auth);
}

void NetModule::onCommand(const fw::Command &cmd, fw::CommandResult &res) noexcept
{
    res.status = fw::Result::kOk;
    res.detail = 0;
    if (!initialized_ || rt_ == nullptr) {
        res.status = fw::Result::kInvalidState;
        return;
    }
    const uint32_t now = rtos::nowMs();
    switch (cmd.verb) {
    case static_cast<uint8_t>(NetVerb::kPowerOn):
        rt_->forcedOn = true;
        rt_->devOff = false;
        break;
    case static_cast<uint8_t>(NetVerb::kPowerOff):
        rt_->forcedOn = false;
        rt_->devOff = true;
        break;
    case static_cast<uint8_t>(NetVerb::kReset):
        if (!rt_->fsm.requestReset(now)) {
            res.status = fw::Result::kInvalidState;
        }
        break;
    default:
        res.status = fw::Result::kNotSupported;
        break;
    }
}

/**
 * @brief 按使能/配网/低电量/关机状态更新上电策略（ADR-0021 §2.9）
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。低电量只禁止“新上电”，在线模块保持。
 */
void NetModule::updatePolicy(uint32_t nowMs) noexcept
{
    static_cast<void>(nowMs);
    Runtime &rt = *rt_;
    NetGateInput in;
    in.shutdown = rt.shutdownStarted;
    if (context_ != nullptr) {
        in.wifiConfig = (context_->radioMode == RadioMode::kWifiConfig);
        in.batteryPct = context_->data.powerStatus.read().batteryPct;
    }
    const NetGateDecision decision = rt.gate.update(in);
    const bool enabled = (rt.cfg.enable || rt.forcedOn) && !rt.devOff && !rt.shutdownStarted;
    rt.fsm.setPowerAllowed(decision.powerAllowed);
    rt.fsm.setStartAllowed(decision.startAllowed);
    rt.fsm.setEnabled(enabled);
}

/**
 * @brief 处理关机请求并上报完成；到 PW1 截止时刻仍未完成则硬切 PWREN
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::serviceShutdown(uint32_t nowMs) noexcept
{
    Runtime &rt = *rt_;
    if (!rt.shutdownStarted && rt.shutdownReq.load(std::memory_order_acquire)) {
        rt.shutdownStarted = true;
        // 沿用 PW1 起点的同一绝对截止时刻，kNet 晚起步也不重新计 2 s（0 保留为“未指定”）。
        uint32_t deadline =
            rt.shutdownStartMs.load(std::memory_order_acquire) + cellular::kShutdownTotalMs;
        if (deadline == 0U) {
            deadline = 1U;
        }
        rt.shutdownDeadlineMs = deadline;
        rt.fsm.beginShutdown(nowMs, deadline);
    }
    if (!rt.shutdownStarted || rt.shutdownDone.load(std::memory_order_acquire)) {
        return;
    }
    if (rt.fsm.shutdownDone()) {
        rt.shutdownDone.store(true, std::memory_order_release);
    } else if (static_cast<int32_t>(nowMs - rt.shutdownDeadlineMs) >= 0) {
        // FSM 到点仍未完成（钩子被拖慢/任务饥饿）：板型受控硬切，成功后才上报完成。
        if (cutPower()) {
            rt.shutdownDone.store(true, std::memory_order_release);
        }
    }
}

/**
 * @brief 把控制台 AT 邮箱中的命令提交给引擎
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::serviceAtMailbox(uint32_t nowMs) noexcept
{
    Runtime &rt = *rt_;
    std::string_view text;
    uint32_t token = 0U;
    if (!rt.mailbox.take(text, token)) {
        return;
    }
    if (rt.shutdownStarted) {
        // 关机开始前提交、开始后才取走的命令：不再发往模块，直接以 ABORTED 结算。
        cellular::AtResponse rsp;
        rsp.result = cellular::AtResult::kAborted;
        rsp.nowMs = nowMs;
        rsp.token = token;
        rt.mailbox.onDone(rsp);
        return;
    }
    cellular::AtCommand cmd;
    cmd.text = text;
    cmd.timeoutMs = cellular::at_timeout::kDebugDefault;
    cmd.onLine = &NetModule::atLineThunk;
    cmd.onDone = &NetModule::atDoneThunk;
    cmd.ctx = this;
    cmd.token = token;
    if (!rt.engine.submit(cmd, nowMs)) {
        rt.mailbox.untake(); // 队列满：下一轮重试
    }
}

fw::Status NetModule::submitAt(std::string_view text) noexcept
{
    if (!present() || rt_ == nullptr) {
        return fw::Status::kInvalidState;
    }
    if (rt_->shutdownReq.load(std::memory_order_acquire)) {
        return fw::Status::kInvalidState; // 关机开始后拒绝新 AT
    }
    // 只读 kNet 发布的原子镜像，不碰 FSM 的非原子成员。
    const auto phase =
        static_cast<cellular::Phase>(rt_->phaseMirror.load(std::memory_order_acquire));
    if (phase == cellular::Phase::kOff || phase == cellular::Phase::kPowerCycle) {
        return fw::Status::kInvalidState;
    }
    const fw::Status status = rt_->mailbox.submit(text);
    if (fw::isOk(status)) {
        wakeQueue(rt_->uartQueue); // 让 kNet 立刻取走，不必等下一个定时点
    }
    return status;
}

/**
 * @brief 读取 UART 并喂给 AtEngine
 * @param nowMs: 当前毫秒时间戳
 * @retval true 读满一块，可能仍有积压
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool NetModule::drainUart(uint32_t nowMs) noexcept
{
    Runtime &rt = *rt_;
    if (!rt.uartInstalled) {
        return false;
    }
    const auto port = static_cast<uart_port_t>(uartPort_);
    const int n = uart_read_bytes(port, rt.rxChunk.data(), rt.rxChunk.size(), 0);
    if (n <= 0) {
        return false;
    }
    const auto count = static_cast<size_t>(n);
    rt.engine.feed(std::span<const uint8_t>(rt.rxChunk.data(), count), nowMs);
    return count == rt.rxChunk.size(); // 读满一块说明还有积压
}

void NetModule::publishStatus(uint32_t nowMs) noexcept
{
    Runtime &rt = *rt_;
    const cellular::Phase phase = rt.fsm.phase();
    rt.phaseMirror.store(static_cast<uint8_t>(phase), std::memory_order_release);
    if (context_ == nullptr) {
        return;
    }
    const bool phaseChanged = (phase != rt.lastPhase);
    if (rt.published && !phaseChanged && ((nowMs - rt.lastPublishMs) < kPublishPeriodMs)) {
        return;
    }
    const cellular::LinkSnapshot snap = rt.fsm.snapshot(nowMs);
    context_->data.netStatus.write(snap);
    if (phaseChanged) {
        FW_LOGI(fw::TraceCat::kSys, "net: phase %u->%u sim=%u reg=%u L%u csq=%u",
                static_cast<unsigned>(rt.lastPhase), static_cast<unsigned>(phase),
                static_cast<unsigned>(snap.sim), static_cast<unsigned>(snap.reg),
                static_cast<unsigned>(snap.level), static_cast<unsigned>(snap.csq));
        rt.lastPhase = phase;
    }
    rt.lastPublishMs = nowMs;
    rt.published = true;
}

void NetModule::wait(uint32_t maxMs) noexcept
{
    if (!initialized_ || rt_ == nullptr || rt_->uartQueue == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(maxMs));
        return;
    }
    Runtime &rt = *rt_;
    const uint32_t now = rtos::nowMs();
    uint32_t timeout = std::min(maxMs, rt.fsm.msToNextWork(now));
    timeout = std::min(timeout, rt.engine.msToDeadline(now));
    if (rt.shutdownReq.load(std::memory_order_acquire) && !rt.shutdownStarted) {
        timeout = 0U; // PW1 已开始而 kNet 尚未起步：不再睡
    } else if (rt.shutdownStarted && !rt.shutdownDone.load(std::memory_order_acquire)) {
        const auto left = static_cast<int32_t>(rt.shutdownDeadlineMs - now);
        timeout = std::min(timeout, (left > 0) ? static_cast<uint32_t>(left) : 0U);
    }
    uart_event_t ev{};
    if (xQueueReceive(static_cast<QueueHandle_t>(rt.uartQueue), &ev, pdMS_TO_TICKS(timeout)) !=
        pdTRUE) {
        return;
    }
    if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
        // 溢出：丢弃残留输入并复位事件队列，同时告知 AtEngine 已丢字节：
        // 正在读原始数据的命令立刻以 kError 终结，其余走重新同步，不把残片误判为应答。
        ++rt.uartOverflows;
        const auto port = static_cast<uart_port_t>(uartPort_);
        static_cast<void>(uart_flush_input(port));
        xQueueReset(static_cast<QueueHandle_t>(rt.uartQueue));
        rt.engine.onRxLost(rtos::nowMs());
    }
}

bool NetModule::poll(uint32_t nowMs) noexcept
{
    if (!initialized_ || rt_ == nullptr) {
        return false;
    }
    Runtime &rt = *rt_;
    updatePolicy(nowMs);
    serviceShutdown(nowMs);
    serviceAtMailbox(nowMs);
    const bool backlog = drainUart(nowMs);
    rt.engine.poll(nowMs);
    rt.fsm.poll(nowMs);
    publishStatus(nowMs);
    return backlog;
}

} // namespace app
