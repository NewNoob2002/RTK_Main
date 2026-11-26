// P2 4G 联网模块实现（ADR-0021 / N1）：UART1 驱动、PWREN 控制、net.* 持久化与关机参与者。
// 线程：init 启动前单线程；wait/poll/onCommand 仅 kNet 执行器任务；shutdownBegin/shutdownIsDone/submitAt 可跨任务（仅原子量）。
// 堆档位：启动期一次性（uart_driver_install 在 init 内分配 RX/TX 环形缓冲与事件队列）；运行期无堆分配。
#include "modules/net/NetModule.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
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
#include "fw/StoreKey.hpp"
#include "modules/power/PowerModule.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "rtos/EspStatus.hpp"

namespace app {
namespace {

constexpr uint32_t kBaudBoot = 115200U;
using Field = std::array<char, cellular::LinkConfig::kFieldBytes>;
constexpr size_t kResultTextBytes = 32U;                             // "CME ERROR <code>" 的上限
constexpr size_t kFieldMax = cellular::LinkConfig::kFieldBytes - 1U; // 31，结尾 0 占 1 B

inline constexpr fw::StoreKey kNetEnableKey{
    .ns = "net",
    .key = "en",
    .type = fw::StoreType::kU8,
    .maxSize = 1U,
    .min = 0,
    .max = 1,
    .help = "4G enable (0/1)",
};
inline constexpr fw::StoreKey kNetApnKey{
    .ns = "net",
    .key = "apn",
    .type = fw::StoreType::kBlob,
    .maxSize = 32U,
    .help = "4G APN (len byte + chars)",
};
inline constexpr fw::StoreKey kNetUserKey{
    .ns = "net",
    .key = "usr",
    .type = fw::StoreType::kBlob,
    .maxSize = 32U,
    .help = "4G user (len byte + chars)",
};
inline constexpr fw::StoreKey kNetPassKey{
    .ns = "net",
    .key = "pwd",
    .type = fw::StoreType::kBlob,
    .maxSize = 32U,
    .help = "4G password (len byte + chars)",
};
static_assert(fw::valid(kNetEnableKey) && fw::valid(kNetApnKey) && fw::valid(kNetUserKey) &&
              fw::valid(kNetPassKey));

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
 * @brief 把文本拷入定长字段，超长截断并保证以 0 结尾
 * @param dst: 目标字段
 * @param text: 源文本
 * @retval 实际拷贝字符数
 * @note 纯函数，线程/ISR 安全。
 */
size_t copyField(Field &dst, std::string_view text) noexcept
{
    const size_t n = std::min(text.size(), kFieldMax);
    dst.fill('\0');
    std::memcpy(dst.data(), text.data(), n);
    return n;
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
 * @brief 从 NVS 读取 [len][chars] 文本 blob
 * @param key: 持久化键
 * @param[out] dst: 目标字段
 * @retval true 读到有效值
 * @retval false 不存在或格式非法（dst 保持清零）
 * @note 任意任务，可阻塞于 NVS，非 ISR。
 */
bool readText(const fw::StoreKey &key, Field &dst) noexcept
{
    std::array<uint8_t, cellular::LinkConfig::kFieldBytes> raw{};
    size_t len = 0U;
    dst.fill('\0');
    if (store::readBlob(key, raw, len) != ESP_OK || len == 0U || raw[0] > kFieldMax ||
        static_cast<size_t>(raw[0]) + 1U > len) {
        return false;
    }
    std::memcpy(dst.data(), std::span(raw).subspan(1U).data(), raw[0]);
    return true;
}

/**
 * @brief 把文本写入 NVS 的 [len][chars] blob
 * @param key: 持久化键
 * @param text: 文本（≤31 字符，可为空）
 * @retval true 已写入
 * @note 任意任务，可阻塞于 NVS，非 ISR。
 */
bool writeText(const fw::StoreKey &key, std::string_view text) noexcept
{
    std::array<uint8_t, cellular::LinkConfig::kFieldBytes> raw{};
    raw[0] = static_cast<uint8_t>(text.size());
    std::memcpy(std::span(raw).subspan(1U).data(), text.data(), text.size());
    return store::writeBlob(key, std::span<const uint8_t>(raw.data(), text.size() + 1U)) == ESP_OK;
}

/**
 * @brief 大小写不敏感的前缀判断
 * @param text: 被测文本
 * @param prefix: 前缀（须为大写）
 * @retval true text 以 prefix 开头
 * @note 纯函数，线程/ISR 安全。
 */
bool startsWithNoCase(std::string_view text, std::string_view prefix) noexcept
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0U; i < prefix.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 把一行文本追加进定长响应缓冲（结尾补换行，缓冲满则截断）
 * @param buf: 响应缓冲（保留 1 B 给结尾 0）
 * @param used: 已用字节数
 * @param text: 要追加的文本
 * @retval 追加后的已用字节数
 * @note 纯函数，线程/ISR 安全。
 */
size_t appendLine(std::span<char> buf, size_t used, std::string_view text) noexcept
{
    if (used + 1U >= buf.size()) {
        return used;
    }
    const size_t room = buf.size() - 1U - used;
    const size_t n = std::min(text.size(), room);
    std::memcpy(buf.subspan(used).data(), text.data(), n);
    used += n;
    if (n == text.size() && used + 1U < buf.size()) {
        buf[used] = '\n';
        ++used;
    }
    return used;
}

} // namespace

bool NetModule::atForbidden(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ')) {
        text.remove_prefix(1U);
    }
    return startsWithNoCase(text, "ATE1") || startsWithNoCase(text, "AT+IPR") ||
           startsWithNoCase(text, "AT+QISWTMD");
}

/**
 * @brief LinkFsm 电源钩子：拉高/拉低 PWREN 并清 UART 输入
 * @param ctx: 回调上下文指针
 * @param on: true=上电，false=断电
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::powerHook(void *ctx, bool on) noexcept
{
    const auto *self = static_cast<const NetModule *>(ctx);
    if (self->pwrenGpio_ >= 0) {
        // 引脚号来自板级表，已核实为输出 GPIO；on 对应高电平（PWRKEY 已硬件接地）。
        static_cast<void>(gpio_set_level(static_cast<gpio_num_t>(self->pwrenGpio_), on ? 1U : 0U));
    }
    if (self->uartInstalled_) {
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
    if (!self->uartInstalled_) {
        return false;
    }
    const auto port = static_cast<uart_port_t>(self->uartPort_);
    // 波特率上限 460800，转换为 IDF 的 uint32 无损。
    const bool ok = uart_set_baudrate(port, baud) == ESP_OK;
    static_cast<void>(uart_flush_input(port));
    return ok;
}

/**
 * @brief AtEngine 写钩子：把字节写入 UART
 * @param ctx: 回调上下文指针
 * @param bytes: 待写字节
 * @retval true 成功/条件成立
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool NetModule::writeHook(void *ctx, std::span<const uint8_t> bytes) noexcept
{
    const auto *self = static_cast<const NetModule *>(ctx);
    if (!self->uartInstalled_ || bytes.empty()) {
        return false;
    }
    const int n =
        uart_write_bytes(static_cast<uart_port_t>(self->uartPort_), bytes.data(), bytes.size());
    return (n >= 0) && (static_cast<size_t>(n) == bytes.size());
}

/**
 * @brief AtEngine URC 钩子：转发给 LinkFsm
 * @param ctx: 回调上下文指针
 * @param line: 一行文本（不含换行）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::urcHook(void *ctx, std::string_view line) noexcept
{
    auto *self = static_cast<NetModule *>(ctx);
    self->fsm_.onUrc(line, rtos::nowMs());
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
 * @param uint8_t: uint8_t
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
    static_cast<NetModule *>(ctx)->onAtLine(line);
}

/**
 * @brief 控制台 AT 完成回调转发
 * @param ctx: 回调上下文指针
 * @param rsp: 命令响应
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::atDoneThunk(void *ctx, const cellular::AtResponse &rsp) noexcept
{
    static_cast<NetModule *>(ctx)->onAtDone(rsp);
}

/**
 * @brief 关机参与者 begin：置位关机请求
 * @param stage: 关机阶段（本模块不区分，统一在 kQuiesce 处理）
 * @param ctx: NetModule 实例指针
 * @note 由 PowerModule 任务调用，仅写原子量；非 ISR，不分配堆。
 */
void NetModule::shutdownBegin(board::ShutdownStage stage, void *ctx) noexcept
{
    static_cast<void>(stage);
    static_cast<NetModule *>(ctx)->shutdownReq_.store(true, std::memory_order_release);
}

/**
 * @brief 关机参与者 isDone：读取关机完成标志
 * @param ctx: NetModule 实例指针
 * @retval true 模块已断电或关机超时收尾完成
 * @note 由 PowerModule 任务调用，仅读原子量；非 ISR，不分配堆。
 */
bool NetModule::shutdownIsDone(void *ctx) noexcept
{
    return static_cast<NetModule *>(ctx)->shutdownDone_.load(std::memory_order_acquire);
}

fw::Result NetModule::init() noexcept
{
    if (initialized_) {
        return fw::Result::kInvalidState;
    }
    const board::Profile &profile = board::current();
    if (!board::hasFeature(profile, board::Feature::kCellular) || profile.cellular.uart < 0 ||
        profile.cellular.pwrenGpio < 0) {
        return fw::Result::kOk; // 板型不含 4G：不触碰 GPIO26 与 UART1
    }
    uartPort_ = profile.cellular.uart;
    pwrenGpio_ = profile.cellular.pwrenGpio;
    loadConfig();
    const fw::Result uartResult = initUart();
    if (!fw::isOk(uartResult)) {
        return uartResult;
    }
    cellular::AtPort port;
    port.ctx = this;
    port.write = &NetModule::writeHook;
    port.onUrc = &NetModule::urcHook;
    port.urcPrefixes = std::span<const std::string_view>(cellular::kUrcPrefixes);
    engine_.bind(port);
    engine_.setDeadThreshold(cellular::kPolicy.deadTimeouts);
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
    fsm_.bind(engine_, hooks, events);
    applyConfig();
    publishConfig();
    initialized_ = true;
    present_.store(true, std::memory_order_release);
    FW_LOGI(fw::TraceCat::kSys, "net: ready uart%d tx=%d rx=%d pwren=GPIO%d en=%d", uartPort_,
            profile.cellular.txGpio, profile.cellular.rxGpio, pwrenGpio_, cfg_.enable ? 1 : 0);
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
    uartInstalled_ = true;
    uartQueue_ = queue;
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
    if (pwrenGpio_ >= 0) {
        static_cast<void>(gpio_set_level(static_cast<gpio_num_t>(pwrenGpio_), 0U));
    }
    if (uartInstalled_) {
        static_cast<void>(uart_driver_delete(static_cast<uart_port_t>(uartPort_)));
        uartInstalled_ = false;
        uartQueue_ = nullptr;
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
    entry.stage = board::ShutdownStage::kQuiesce;
    entry.milestone = power::ShutdownMilestone::kNetPower;
    const fw::Status status = power::registerShutdownParticipant(entry);
    shutdownRegistered_ = fw::isOk(status);
    return status;
}

/**
 * @brief 从 NVS 加载 net.* 配置
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::loadConfig() noexcept
{
    uint32_t enable = 0U;
    cfg_ = Config{};
    if (store::readValue(kNetEnableKey, enable) == ESP_OK) {
        cfg_.enable = (enable != 0U);
    }
    static_cast<void>(readText(kNetApnKey, cfg_.apn));
    static_cast<void>(readText(kNetUserKey, cfg_.user));
    static_cast<void>(readText(kNetPassKey, cfg_.pass));
}

/**
 * @brief 把当前配置下发给 LinkFsm
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::applyConfig() noexcept
{
    // 用户名非空时按 PAP 认证；未设置则无认证（多数物联网卡只需 APN）。
    const uint8_t auth = fieldView(cfg_.user).empty() ? 0U : 1U;
    fsm_.setConfig(fieldView(cfg_.apn), fieldView(cfg_.user), fieldView(cfg_.pass), auth);
}

/**
 * @brief 发布 net.* 只读视图到 DataCenter
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::publishConfig() noexcept
{
    if (context_ == nullptr) {
        return;
    }
    NetConfigView view;
    view.enable = cfg_.enable;
    view.passSet = !fieldView(cfg_.pass).empty();
    view.apn = cfg_.apn;
    view.user = cfg_.user;
    context_->data.netConfig.write(view);
}

/**
 * @brief 把指定文本键写入 NVS
 * @param verb: 命令动词
 * @retval 见函数实现
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
fw::Status NetModule::persistText(uint8_t verb) noexcept
{
    switch (verb) {
    case static_cast<uint8_t>(NetVerb::kSetApn):
        return writeText(kNetApnKey, fieldView(cfg_.apn)) ? fw::Status::kOk : fw::Status::kIoError;
    case static_cast<uint8_t>(NetVerb::kSetUser):
        return writeText(kNetUserKey, fieldView(cfg_.user)) ? fw::Status::kOk
                                                            : fw::Status::kIoError;
    case static_cast<uint8_t>(NetVerb::kSetPass):
        return writeText(kNetPassKey, fieldView(cfg_.pass)) ? fw::Status::kOk
                                                            : fw::Status::kIoError;
    default:
        return fw::Status::kInvalidArg;
    }
}

void NetModule::onCommand(const fw::Command &cmd, fw::CommandResult &res) noexcept
{
    res.status = fw::Result::kOk;
    res.detail = 0;
    if (!initialized_) {
        res.status = fw::Result::kInvalidState;
        return;
    }
    const uint32_t now = rtos::nowMs();
    const size_t len = cmd.argLen;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): 命令参数字节按文本只读视图
    const std::string_view text(reinterpret_cast<const char *>(cmd.args.data()),
                                std::min(len, cmd.args.size()));
    switch (cmd.verb) {
    case static_cast<uint8_t>(NetVerb::kSetEnable):
        if (len != 1U || cmd.args[0] > 1U) {
            res.status = fw::Result::kInvalidArg;
            return;
        }
        cfg_.enable = (cmd.args[0] != 0U);
        if (store::writeValue(kNetEnableKey, cfg_.enable ? 1U : 0U) != ESP_OK) {
            res.status = fw::Result::kIoError;
        }
        break;
    case static_cast<uint8_t>(NetVerb::kSetApn):
    case static_cast<uint8_t>(NetVerb::kSetUser):
    case static_cast<uint8_t>(NetVerb::kSetPass): {
        if (len > kFieldMax || text.find('"') != std::string_view::npos ||
            text.find('\r') != std::string_view::npos ||
            text.find('\n') != std::string_view::npos) {
            res.status = fw::Result::kInvalidArg; // 双引号/换行会破坏 AT+QICSGP 命令行
            return;
        }
        auto &field = (cmd.verb == static_cast<uint8_t>(NetVerb::kSetApn))    ? cfg_.apn
                      : (cmd.verb == static_cast<uint8_t>(NetVerb::kSetUser)) ? cfg_.user
                                                                              : cfg_.pass;
        static_cast<void>(copyField(field, text));
        res.status = toResult(persistText(cmd.verb));
        break;
    }
    case static_cast<uint8_t>(NetVerb::kPowerOn):
        forcedOn_ = true;
        devOff_ = false;
        break;
    case static_cast<uint8_t>(NetVerb::kPowerOff):
        forcedOn_ = false;
        devOff_ = true;
        break;
    case static_cast<uint8_t>(NetVerb::kReset):
        if (!fsm_.requestReset(now)) {
            res.status = fw::Result::kInvalidState;
        }
        break;
    default:
        res.status = fw::Result::kNotSupported;
        return;
    }
    applyConfig();
    publishConfig();
}

/**
 * @brief 按使能/配网/低电量/关机状态更新上电策略
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::updatePolicy(uint32_t nowMs) noexcept
{
    static_cast<void>(nowMs);
    bool allowed = true;
    if (context_ != nullptr) {
        if (context_->radioMode == RadioMode::kWifiConfig) {
            allowed = false; // Wi-Fi 配网模式期间不给 4G 上电（供电与射频干扰）
        }
        const uint8_t pct = context_->data.powerStatus.read().batteryPct;
        if (pct != board::kBatteryPctUnknown) {
            if (pct < board::kLowBatteryPct) {
                lowBatteryLatched_ = true;
            } else if (pct >= board::kLowBatteryPct + kLowBatteryResumeMargin) {
                lowBatteryLatched_ = false;
            }
        }
    }
    allowed = allowed && !lowBatteryLatched_ && !shutdownStarted_;
    const bool enabled = (cfg_.enable || forcedOn_) && !devOff_ && !shutdownStarted_;
    if (allowed != lastPowerAllowed_ || !published_) {
        fsm_.setPowerAllowed(allowed);
        lastPowerAllowed_ = allowed;
    }
    if (enabled != lastEnabled_ || !published_) {
        fsm_.setEnabled(enabled);
        lastEnabled_ = enabled;
    }
}

/**
 * @brief 处理关机请求并上报完成
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::serviceShutdown(uint32_t nowMs) noexcept
{
    if (!shutdownStarted_ && shutdownReq_.load(std::memory_order_acquire)) {
        shutdownStarted_ = true;
        fsm_.beginShutdown(nowMs);
    }
    if (shutdownStarted_ && fsm_.shutdownDone()) {
        shutdownDone_.store(true, std::memory_order_release);
    }
}

/**
 * @brief 把控制台 AT 邮箱中的命令提交给引擎
 * @param nowMs: 当前毫秒时间戳
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::serviceAtMailbox(uint32_t nowMs) noexcept
{
    if (atState() != AtState::kPending) {
        return;
    }
    cellular::AtCommand cmd;
    cmd.text = std::string_view(atCmd_.data(), atCmdLen_);
    cmd.timeoutMs = cellular::at_timeout::kDebugDefault;
    cmd.onLine = &NetModule::atLineThunk;
    cmd.onDone = &NetModule::atDoneThunk;
    cmd.ctx = this;
    atRspLen_ = 0U;
    atRsp_.fill('\0');
    if (!engine_.submit(cmd, nowMs)) {
        return; // 队列满：下一轮重试
    }
    atState_.store(static_cast<uint8_t>(AtState::kRunning), std::memory_order_release);
}

/**
 * @brief 累积控制台 AT 响应行
 * @param line: 一行文本（不含换行）
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::onAtLine(std::string_view line) noexcept
{
    atRspLen_ = appendLine(std::span<char>(atRsp_), atRspLen_, line);
}

/**
 * @brief 控制台 AT 命令完成：记录结果并置 Done
 * @param rsp: 命令响应
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
void NetModule::onAtDone(const cellular::AtResponse &rsp) noexcept
{
    atOk_ = (rsp.result == cellular::AtResult::kOk);
    if (!atOk_) {
        static constexpr std::array<std::string_view, 5U> kNames{"OK", "ERROR", "CME ERROR",
                                                                 "TIMEOUT", "ABORTED"};
        const size_t idx = static_cast<size_t>(rsp.result);
        const std::string_view name = idx < kNames.size() ? kNames[idx] : std::string_view("?");
        std::array<char, kResultTextBytes> text{};
        if (rsp.result == cellular::AtResult::kCmeError) {
            std::snprintf(text.data(), text.size(), "%.*s %d", static_cast<int>(name.size()),
                          name.data(), static_cast<int>(rsp.errorCode));
        } else {
            std::snprintf(text.data(), text.size(), "%.*s", static_cast<int>(name.size()),
                          name.data());
        }
        atRspLen_ = appendLine(std::span<char>(atRsp_), atRspLen_,
                               std::string_view(text.data(), ::strnlen(text.data(), text.size())));
    }
    atState_.store(static_cast<uint8_t>(AtState::kDone), std::memory_order_release);
}

fw::Status NetModule::submitAt(std::string_view text) noexcept
{
    if (!present() || fsm_.phase() == cellular::Phase::kOff ||
        fsm_.phase() == cellular::Phase::kPowerCycle) {
        return fw::Status::kInvalidState;
    }
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1U);
    }
    if (text.empty() || text.size() >= atCmd_.size() || atForbidden(text)) {
        return fw::Status::kInvalidArg;
    }
    uint8_t expected = static_cast<uint8_t>(AtState::kIdle);
    if (!atState_.compare_exchange_strong(expected, static_cast<uint8_t>(AtState::kFilling),
                                          std::memory_order_acq_rel)) {
        return fw::Status::kBusy;
    }
    std::memcpy(atCmd_.data(), text.data(), text.size());
    atCmdLen_ = text.size();
    atState_.store(static_cast<uint8_t>(AtState::kPending), std::memory_order_release);
    return fw::Status::kOk;
}

/**
 * @brief 读取 UART 并喂给 AtEngine
 * @param nowMs: 当前毫秒时间戳
 * @retval true 成功/条件成立
 * @note 仅 kNet 执行器任务调用，非 ISR；不分配堆。
 */
bool NetModule::drainUart(uint32_t nowMs) noexcept
{
    if (!uartInstalled_) {
        return false;
    }
    const auto port = static_cast<uart_port_t>(uartPort_);
    const int n = uart_read_bytes(port, rxChunk_.data(), rxChunk_.size(), 0);
    if (n <= 0) {
        return false;
    }
    const auto count = static_cast<size_t>(n);
    engine_.feed(std::span<const uint8_t>(rxChunk_.data(), count), nowMs);
    return count == rxChunk_.size(); // 读满一块说明还有积压
}

void NetModule::publishStatus(uint32_t nowMs) noexcept
{
    if (context_ == nullptr) {
        return;
    }
    const cellular::Phase phase = fsm_.phase();
    const bool phaseChanged = (phase != lastPhase_);
    if (published_ && !phaseChanged && ((nowMs - lastPublishMs_) < kPublishPeriodMs)) {
        return;
    }
    const cellular::LinkSnapshot snap = fsm_.snapshot(nowMs);
    context_->data.netStatus.write(snap);
    if (phaseChanged) {
        FW_LOGI(fw::TraceCat::kSys, "net: phase %u->%u sim=%u reg=%u L%u csq=%u",
                static_cast<unsigned>(lastPhase_), static_cast<unsigned>(phase),
                static_cast<unsigned>(snap.sim), static_cast<unsigned>(snap.reg),
                static_cast<unsigned>(snap.level), static_cast<unsigned>(snap.csq));
        lastPhase_ = phase;
    }
    lastPublishMs_ = nowMs;
    published_ = true;
}

void NetModule::wait(uint32_t maxMs) noexcept
{
    if (!initialized_ || uartQueue_ == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(maxMs));
        return;
    }
    const uint32_t now = rtos::nowMs();
    uint32_t timeout = std::min(maxMs, fsm_.msToNextWork(now));
    timeout = std::min(timeout, engine_.msToDeadline(now));
    uart_event_t ev{};
    if (xQueueReceive(static_cast<QueueHandle_t>(uartQueue_), &ev, pdMS_TO_TICKS(timeout)) !=
        pdTRUE) {
        return;
    }
    if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
        // 溢出：丢弃残留输入并复位事件队列，AtEngine 靠下一行边界/超时自行重新同步。
        ++uartOverflows_;
        const auto port = static_cast<uart_port_t>(uartPort_);
        static_cast<void>(uart_flush_input(port));
        xQueueReset(static_cast<QueueHandle_t>(uartQueue_));
    }
}

bool NetModule::poll(uint32_t nowMs) noexcept
{
    if (!initialized_) {
        return false;
    }
    updatePolicy(nowMs);
    serviceShutdown(nowMs);
    serviceAtMailbox(nowMs);
    const bool backlog = drainUart(nowMs);
    engine_.poll(nowMs);
    fsm_.poll(nowMs);
    publishStatus(nowMs);
    return backlog;
}

} // namespace app
