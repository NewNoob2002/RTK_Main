// 用真实 GnssModule 和内存 NVS 重现接收机延迟重启与启动切速。
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "../../test/fixtures/Cm680Version.hpp"

#include "AppContext.hpp"
#include "app/SettingsBackend.hpp"
#include "board/BoardProfile.hpp"
#include "def/GnssSubDef.hpp"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "fw/Log.hpp"
#include "fw/Trace.hpp"
#include "modules/gnss/GnssCmdPack.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "modules/storage/StoreWriteGate.hpp"
#include "rtos/EspStatus.hpp"
#include "settings/Types.hpp"
#include "UbloxExt.hpp"

extern "C" {
#include "nvs.h"
}

namespace rtos {

/** 以测试线程模拟 kIo，并手动推进真实执行器队列。 */
struct ExecutorTestAccess {
    /** @brief 绑定测试线程为执行器所有者。 @param exec: 测试执行器。 @param owner: 测试 TCB。 */
    static void bind(Executor &exec, TaskHandle_t owner) noexcept
    {
        exec.task_ = owner;
        fake_current_task = owner;
    }

    /** @brief 消费本轮排队命令。 @param exec: 测试执行器。 */
    static void drain(Executor &exec) noexcept
    {
        fw::Command cmd{};
        uint32_t count = 0U;
        while (xQueueReceive(exec.cmd_q_, &cmd, 0) == pdTRUE) {
            exec.processCommand(cmd, count);
        }
    }
};

} // namespace rtos

namespace {

constinit app::AppContext s_initialContext{};
constinit app::AppContext s_failureContext{};
constinit app::AppContext s_dispatchContext{};
constinit app::AppContext s_pppContext{};
constinit app::AppContext s_dispatchCmdContext{};
constinit std::array<app::AppContext, 16U> s_targetBaudContexts{};
constinit std::array<app::AppContext, 8U> s_f9pContexts{};
constinit app::AppContext *s_context = &s_initialContext;
uint32_t s_now = 0;
uint32_t s_hostBaud = 0;
uint32_t s_receiverBaud = 460800;
uint32_t s_readyAt = 0;
unsigned s_baudCommands = 0;
unsigned s_gpioChanges = 0;
unsigned s_gpioConfigs = 0;
bool s_p2 = false;
bool s_sentSatsInfo = false;
unsigned s_saveCommands = 0;
unsigned s_fresetCommands = 0;
unsigned s_modeBaseCommands = 0;
unsigned s_pppCommands = 0;
unsigned s_satMaskCommands = 0;
bool s_applyBaud = true;
bool s_rejectBaud = false;
bool s_rejectSave = false;
bool s_rejectSatMask = false;
bool s_expectBaudSave = false;
bool s_bootBaudMode = false;
bool s_verifiedAfterBaud = false;
bool s_saveAfterVerify = false;
uint32_t s_baudFailTraces = 0U;
uint32_t s_lastBaudFailTarget = 0U;
uint32_t s_lastBaudFailResult = 0U;
nvs_handle_t s_nvs = 0U;
std::string s_rx;
bool s_f9pMode = false;
std::string_view s_f9pFw = "HPG 1.32";
bool s_f9pDropSatAck = false;
bool s_f9pRecordTx = false;
bool s_f9pFailNextWrite = false;
unsigned s_f9pSatValsets = 0U;
std::string s_f9pTxLog;
std::array<uint8_t, 128U> s_lastF9pSatValset{};
size_t s_lastF9pSatValsetLen = 0U;
bool s_f9pNakGgaMsgout = false;
bool s_f9pNakHighPrecPort = false;
uint16_t s_f9pLastRateMeasMs = 0U;
unsigned s_f9pRateMeasWrites = 0U;
uint8_t s_f9pLastGgaEpochs = 0xFFU;
unsigned s_f9pGgaWrites = 0U;
uint8_t s_f9pLastGsvEpochs = 0xFFU;
unsigned s_f9pGsvWrites = 0U;
unsigned s_f9pHighPrecNaks = 0U;

/** @brief 构造完整 UBX 帧并追加到模拟串口接收缓冲。 @param cls: 类别。 @param id: 编号。 @param payload: 载荷。 */
void pushUbxRx(uint8_t cls, uint8_t id, std::span<const uint8_t> payload)
{
    gnss::CmdBuf frame{};
    if (!gnss::ublox::encodeUbxFrame(frame, cls, id, payload)) {
        std::abort();
    }
    const auto bytes = frame.view();
    s_rx.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

/** @brief 生成并注入 F9P MON-VER 响应帧。 @param fw: FWVER 文本。 */
void pushF9pMonVer(std::string_view fw)
{
    std::array<uint8_t, 100U> payload{};
    std::memcpy(payload.data(), "EXT CORE 1.00", 13U);
    std::memcpy(payload.data() + 30U, "00190000", 8U);
    std::memcpy(payload.data() + 40U, "MOD=ZED-F9P", 11U);
    std::string fwExt = "FWVER=";
    fwExt += fw;
    std::memcpy(payload.data() + 70U, fwExt.data(), std::min(fwExt.size(), size_t{29U}));
    pushUbxRx(0x0AU, 0x04U, payload);
}

/** @brief 读取 VALSET 帧内指定键的小端值（1/2/4 字节）。 @param raw: 完整 UBX 帧。 @param targetKey: 目标键。 @param[out] outVal: 输出值。 @retval true 命中；false 未包含。 */
bool findValsetKeyValue(std::span<const uint8_t> raw, uint32_t targetKey, uint32_t &outVal)
{
    if (!gnss::ublox::isValsetFrame(raw)) {
        return false;
    }
    const size_t end = raw.size() - 2U;
    size_t pos = 10U;
    while (pos + 4U <= end) {
        const uint32_t key = static_cast<uint32_t>(raw[pos]) |
                             (static_cast<uint32_t>(raw[pos + 1U]) << 8U) |
                             (static_cast<uint32_t>(raw[pos + 2U]) << 16U) |
                             (static_cast<uint32_t>(raw[pos + 3U]) << 24U);
        const uint32_t sizeCode = (key >> 28U) & 0x07U;
        size_t width = 0U;
        if (sizeCode == 1U || sizeCode == 2U) {
            width = 1U;
        } else if (sizeCode == 3U) {
            width = 2U;
        } else if (sizeCode == 4U) {
            width = 4U;
        } else if (sizeCode == 5U) {
            width = 8U;
        } else {
            return false;
        }
        if (pos + 4U + width > end) {
            return false;
        }
        if (key == targetKey && width <= 4U) {
            uint32_t val = 0U;
            for (size_t i = 0U; i < width; ++i) {
                val |= static_cast<uint32_t>(raw[pos + 4U + i]) << (8U * i);
            }
            outVal = val;
            return true;
        }
        pos += 4U + width;
    }
    return false;
}

/** @brief 处理 F9P 模拟模式下的串口下行字节流（记录原始字节、响应 MON-VER/SEC-UNIQID/VALSET）。 @param bytes: 写入字节。 */
void handleF9pUartWrite(std::string_view bytes)
{
    if (s_f9pRecordTx) {
        s_f9pTxLog.append(bytes.data(), bytes.size());
    }
    const auto raw =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()};
    if ((raw.size() == 8U) && (raw[0] == 0xB5U) && (raw[1] == 0x62U)) {
        if ((raw[2] == 0x0AU) && (raw[3] == 0x04U)) {
            pushF9pMonVer(s_f9pFw);
            return;
        }
        if ((raw[2] == 0x27U) && (raw[3] == 0x03U)) {
            constexpr std::array<uint8_t, 9U> kUniq{0x01U, 0x00U, 0x00U, 0x00U, 0xA1U,
                                                    0x0BU, 0xC2U, 0x3DU, 0x4EU};
            pushUbxRx(0x27U, 0x03U, kUniq);
            return;
        }
    }
    if ((raw.size() >= 12U) && (raw[0] == 0xB5U) && (raw[1] == 0x62U) && (raw[2] == 0x06U) &&
        (raw[3] == 0x8AU)) {
        std::array<uint32_t, gnss::ublox::kMaxExtractedValsetKeys> keys{};
        const size_t keyCount = gnss::ublox::extractValsetKeys(raw, keys);
        if ((keyCount > 0U) && (keys[0] == 0x40520001U) && (raw.size() >= 20U)) {
            s_receiverBaud =
                static_cast<uint32_t>(raw[14]) | (static_cast<uint32_t>(raw[15]) << 8U) |
                (static_cast<uint32_t>(raw[16]) << 16U) | (static_cast<uint32_t>(raw[17]) << 24U);
            return;
        }
        constexpr std::array<uint8_t, 2U> kAckPayload{0x06U, 0x8AU};
        if (s_f9pNakHighPrecPort) {
            uint32_t dummy = 0U;
            if (findValsetKeyValue(raw, 0x10930006U, dummy) ||
                findValsetKeyValue(raw, 0x20930007U, dummy)) {
                ++s_f9pHighPrecNaks;
                pushUbxRx(0x05U, 0x00U, kAckPayload);
                return;
            }
        }
        if (s_f9pNakGgaMsgout && (keyCount == 1U) && (keys[0] == 0x209100BBU)) {
            pushUbxRx(0x05U, 0x00U, kAckPayload);
            return;
        }
        uint32_t val = 0U;
        if (findValsetKeyValue(raw, 0x30210001U, val)) {
            s_f9pLastRateMeasMs = static_cast<uint16_t>(val);
            ++s_f9pRateMeasWrites;
        }
        if ((keyCount == 1U) && findValsetKeyValue(raw, 0x209100BBU, val)) {
            s_f9pLastGgaEpochs = static_cast<uint8_t>(val);
            ++s_f9pGgaWrites;
        }
        if ((keyCount == 1U) && findValsetKeyValue(raw, 0x209100C5U, val)) {
            s_f9pLastGsvEpochs = static_cast<uint8_t>(val);
            ++s_f9pGsvWrites;
        }
        const bool isSat = (keyCount > 0U) && (keys[0] == 0x201100A4U);
        if (isSat) {
            ++s_f9pSatValsets;
            s_lastF9pSatValsetLen = std::min(raw.size(), s_lastF9pSatValset.size());
            std::memcpy(s_lastF9pSatValset.data(), raw.data(), s_lastF9pSatValsetLen);
            if (s_f9pDropSatAck) {
                return;
            }
        }
        pushUbxRx(0x05U, 0x01U, kAckPayload);
    }
}

/** @brief 生成定长 RTCM3 帧字节串（0xD3 + 10 位长度 + 载荷 + 3 字节 CRC 占位）。 @param payloadLen: 载荷字节数。 @retval 完整帧。 */
std::string makeRtcm3Frame(size_t payloadLen)
{
    std::string out(3U + payloadLen + 3U, '\x55');
    out[0] = static_cast<char>(0xD3U);
    out[1] = static_cast<char>((payloadLen >> 8U) & 0x03U);
    out[2] = static_cast<char>(payloadLen & 0xFFU);
    return out;
}

/** @brief 捕获测试期间发出的 GNSS 切速失败 trace 事件。 @param ctx: 未使用。 @param ev: 追踪事件。 @note 单线程主机测试。 */
void captureBaudTrace(void *ctx, const fw::TraceEvent &ev) noexcept
{
    static_cast<void>(ctx);
    if ((ev.cat == fw::TraceCat::kGnss) &&
        (ev.code == static_cast<uint8_t>(app::GnssSubTrace::kBaudFail))) {
        ++s_baudFailTraces;
        s_lastBaudFailTarget = ev.a;
        s_lastBaudFailResult = ev.b;
    }
}

void check(bool ok, const char *expr, int line)
{
    if (!ok) {
        std::fprintf(stderr, "GNSS line %d: %s\n", line, expr);
        std::abort();
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void advance(uint32_t ms)
{
    for (uint32_t end = s_now + ms; s_now < end; s_now += 20U) {
        s_context->gnss.wait(0U);
        static_cast<void>(s_context->gnss.poll(s_now));
    }
}

fw::CommandResult command(uint8_t verb, std::span<const uint8_t> args = {})
{
    fw::Command cmd{};
    cmd.verb = verb;
    CHECK(args.size() <= cmd.args.size());
    cmd.argLen = static_cast<uint8_t>(args.size());
    std::ranges::copy(args, cmd.args.begin());
    fw::CommandResult result{};
    s_context->gnss.onCommand(cmd, result);
    return result;
}

void startModule(nvs_handle_t handle, uint32_t preferred, bool applyBaud, bool p2 = false)
{
    s_rx.clear();
    s_readyAt = 0U;
    s_receiverBaud = 460800U;
    s_baudCommands = 0U;
    s_gpioChanges = 0U;
    s_gpioConfigs = 0U;
    s_p2 = p2;
    s_sentSatsInfo = false;
    s_saveCommands = 0U;
    s_applyBaud = applyBaud;
    s_expectBaudSave = false;
    s_bootBaudMode = false;
    s_verifiedAfterBaud = false;
    s_saveAfterVerify = false;
    CHECK(nvs_set_u32(handle, "baud", preferred) == ESP_OK);
    CHECK(nvs_set_u32(handle, "cache_baud", 460800U) == ESP_OK);
    CHECK(nvs_set_u8(handle, "family", static_cast<uint8_t>(gnss::Family::kUnicore)) == ESP_OK);
    CHECK(nvs_set_u8(handle, "model", static_cast<uint8_t>(gnss::Model::kUm980)) == ESP_OK);
    CHECK(nvs_set_u8(handle, "rate", p2 ? 1U : 5U) == ESP_OK);
    CHECK(nvs_set_u8(handle, "gsv", 0U) == ESP_OK);
    auto out = app::data::GnssOutConfig::defaults();
    CHECK(fw::isOk(out.applyInterval(0U, -3)));
    std::array<uint8_t, decltype(out)::kBlobBytes> bytes{};
    size_t len = 0U;
    CHECK(fw::isOk(out.encode(bytes, len)));
    CHECK(nvs_set_blob(handle, "out", bytes.data(), len) == ESP_OK);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kConfigKeys).failed == 0U);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kCacheKeys).failed == 0U);
    s_context->bootConfig.gnssBaud = preferred;
    s_context->bootConfig.userNvsReady = true;
    const auto id = p2 ? app::board::BoardId::kP2V141 : app::board::BoardId::kE1DevKit;
    s_context->boardProfile = app::board::byId(id);
    CHECK(s_context->boardProfile != nullptr);
    CHECK(fw::isOk(s_context->gnss.init()));
    CHECK(s_hostBaud == preferred);
    CHECK(s_gpioConfigs == (p2 ? 0U : 1U));
    CHECK(s_gpioChanges == (p2 ? 0U : 1U));
    CHECK(fw::isOk(s_context->gnss.start()));
}

void checkStoredDefaults(nvs_handle_t handle)
{
    uint32_t baud = 0U;
    uint8_t value = 0U;
    CHECK(nvs_get_u32(handle, "baud", &baud) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(nvs_get_u8(handle, "rate", &value) == ESP_OK && value == 1U);
    CHECK(nvs_get_u8(handle, "gsv", &value) == ESP_OK && value == 1U);
    CHECK(nvs_get_u8(handle, "family", &value) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(nvs_get_u8(handle, "model", &value) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(nvs_get_u32(handle, "cache_baud", &baud) == ESP_ERR_NVS_NOT_FOUND);
    app::data::GnssOutConfig out{};
    std::array<uint8_t, decltype(out)::kBlobBytes> bytes{};
    size_t len = bytes.size();
    CHECK(nvs_get_blob(handle, "out", bytes.data(), &len) == ESP_OK);
    CHECK(fw::isOk(decltype(out)::decode(bytes, out)));
    CHECK(out.intervals == decltype(out)::defaults().intervals);
    CHECK(out.mask == decltype(out)::defaults().mask);
    CHECK(nvs_get_u8(handle, "mode", &value) == ESP_OK && value == 0U);
    CHECK(nvs_get_u8(handle, "base_type", &value) == ESP_OK && value == 0U);
    uint16_t u16Val = 0U;
    CHECK(nvs_get_u16(handle, "svy_s", &u16Val) == ESP_OK && u16Val == 20U);
    CHECK(nvs_get_u16(handle, "svy_acc_cm", &u16Val) == ESP_OK && u16Val == 150U);
    CHECK(nvs_get_u16(handle, "svy_to_s", &u16Val) == ESP_OK && u16Val == 120U);
    CHECK(nvs_get_u8(handle, "msm", &value) == ESP_OK && value == 4U);
}

} // namespace

app::AppContext &app::appContext() noexcept
{
    return *s_context;
}
uint32_t rtos::nowMs() noexcept
{
    return s_now;
}
fw::Status rtos::fromEspErr(esp_err_t code) noexcept
{
    return code == ESP_OK ? fw::Status::kOk : fw::Status::kIoError;
}
extern "C" void vTaskDelay(TickType_t ticks)
{
    s_now += ticks;
}

/** @brief 验证 E1 电源只驱动 GPIO25。 */
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    CHECK(!s_p2 && gpio == 25 && level <= 1U);
    if (s_gpioChanges == 0U) {
        CHECK(level == 0U);
    }
    ++s_gpioChanges;
    return ESP_OK;
}

/** @brief 验证 E1 GPIO25 的输出配置。 */
esp_err_t gpio_config(const gpio_config_t *config)
{
    CHECK(!s_p2 && config->pin_bit_mask == (1ULL << 25U) && config->mode == GPIO_MODE_OUTPUT);
    ++s_gpioConfigs;
    return ESP_OK;
}

esp_err_t uart_set_baudrate(uart_port_t, uint32_t baud)
{
    s_hostBaud = baud;
    return ESP_OK;
}
esp_err_t uart_param_config(uart_port_t port, const uart_config_t *config)
{
    return uart_set_baudrate(port, static_cast<uint32_t>(config->baud_rate));
}
esp_err_t uart_flush_input(uart_port_t)
{
    s_rx.clear();
    return ESP_OK;
}

int uart_read_bytes(uart_port_t, void *out, size_t max, TickType_t)
{
    const size_t count = std::min(s_rx.size(), max);
    std::memcpy(out, s_rx.data(), count);
    s_rx.erase(0, count);
    return static_cast<int>(count);
}

int uart_write_bytes(uart_port_t, const void *raw, size_t len)
{
    const std::string_view bytes{static_cast<const char *>(raw), len};
    if (s_hostBaud != s_receiverBaud || s_now < s_readyAt) {
        return static_cast<int>(len);
    }
    if (s_f9pMode) {
        if (s_f9pFailNextWrite) {
            s_f9pFailNextWrite = false;
            return -1;
        }
        handleF9pUartWrite(bytes);
        return static_cast<int>(len);
    }
    const std::string_view baudDefault = s_p2 ? "CONFIG COM1 115200\r\n" : "CONFIG COM2 115200\r\n";
    const std::string_view baudMid = s_p2 ? "CONFIG COM1 230400\r\n" : "CONFIG COM2 230400\r\n";
    const std::string_view baudFast = s_p2 ? "CONFIG COM1 460800\r\n" : "CONFIG COM2 460800\r\n";
    if (bytes == "FRESET\r\n") {
        ++s_fresetCommands;
        s_readyAt = s_now + 3500U;
        s_receiverBaud = 115200U;
    } else if (bytes == baudDefault || bytes == baudMid || bytes == baudFast) {
        ++s_baudCommands;
        if (s_rejectBaud) {
            std::string reply{"command,"};
            reply += bytes.substr(0, bytes.size() - 2U);
            reply += ",response: ERROR";
            unsigned checksum = s_p2 ? static_cast<unsigned>('$') : 0U;
            for (char ch : reply) {
                checksum ^= static_cast<unsigned char>(ch);
            }
            std::array<char, 6> suffix{};
            (void)std::snprintf(suffix.data(), suffix.size(), "*%02X\r\n", checksum);
            s_rx += "$" + reply + suffix.data();
        } else if (s_applyBaud) {
            s_receiverBaud =
                (bytes == baudDefault) ? 115200U : ((bytes == baudMid) ? 230400U : 460800U);
            s_expectBaudSave = true;
            s_verifiedAfterBaud = false;
            s_saveAfterVerify = false;
        }
    } else if (bytes == "VERSIONA\r\n") {
        s_verifiedAfterBaud = true;
        constexpr std::string_view kUm980Version =
            "#VERSIONA,0,GPS,UNKNOWN,0,0,0,0,0,249;\"UM980\",\"R4.10Build11833\",\"HRPT00-S10C-P\","
            "\"2310415000001-MD22A2244202679\",\"ff3b7c6f3c5fd90b\",\"2023/11/24\"*3f55fb4d\r\n";
        if (s_p2) {
            s_rx += "$command,VERSIONA,response: OK*45\r\n";
            s_rx += gnss_test::kCm680VersionFrames.front();
        } else {
            s_rx += kUm980Version;
        }
    } else if (s_p2 && bytes == "UNLOG COM1\r\n") {
        s_rx += "$command,UNLOG COM1,response: ERROR*0D\r\n"
                "$command,LOG COM1,response: ERROR*16\r\n";
    } else if (bytes.ends_with("\r\n")) {
        if (bytes.starts_with("MODE BASE")) {
            ++s_modeBaseCommands;
        }
        if (bytes.starts_with("CONFIG PPP") || bytes.starts_with("CONFIG SIGNALGROUP")) {
            ++s_pppCommands;
        }
        if (bytes.starts_with("MASK ") || bytes.starts_with("UNMASK ")) {
            ++s_satMaskCommands;
        }
        const bool save = bytes == "SAVECONFIG\r\n";
        if (save) {
            ++s_saveCommands;
            s_saveAfterVerify = s_verifiedAfterBaud;
            if (s_expectBaudSave) {
                uint32_t stored = 0U;
                if (!s_bootBaudMode) {
                    CHECK(nvs_get_u32(s_nvs, "baud", &stored) == ESP_OK &&
                          stored == s_receiverBaud);
                }
                CHECK(nvs_get_u32(s_nvs, "cache_baud", &stored) == ESP_OK &&
                      stored == s_receiverBaud);
                s_expectBaudSave = false;
            }
        }
        const bool satsInfo = s_p2 && bytes.find("SATSINFOA") != std::string_view::npos;
        s_sentSatsInfo = s_sentSatsInfo || satsInfo;
        if (satsInfo && bytes.starts_with("SATSINFOA ")) {
            s_rx += "PARSING FAILD NO MATCHING FUNC SATSINFOA\r\n";
            return static_cast<int>(len);
        }
        const bool subsecondGsa = s_p2 && bytes.starts_with("GPGSA COM1 0.");
        std::string reply{"command,"};
        reply += bytes.substr(0, bytes.size() - 2U);
        const bool rejectSat = s_rejectSatMask && (bytes == "MASK GLO\r\n");
        const bool reject = satsInfo || subsecondGsa || (save && s_rejectSave) || rejectSat;
        reply += reject ? ",response: ERROR" : ",response: OK";
        unsigned checksum = s_p2 ? static_cast<unsigned>('$') : 0U;
        for (char ch : reply) {
            checksum ^= static_cast<unsigned char>(ch);
        }
        std::array<char, 6> suffix{};
        (void)std::snprintf(suffix.data(), suffix.size(), "*%02X\r\n", checksum);
        s_rx += "$" + reply + suffix.data();
    }
    return static_cast<int>(len);
}

/** @brief 验证启用 GNSS 的真实模块只尝试一次启动切速，复位写出后立即恢复 NVS。 */
void testGnssFactoryReset()
{
    std::array<uint8_t, app::GnssModule::kDemuxBufBytes> demux{};
    std::array<uint8_t, app::kRtcmPipeBytes> pipe{};
    s_context->bindContexts();
    CHECK(fw::isOk(s_context->gnss.bindStorage(demux)));
    CHECK(fw::isOk(s_context->rtcmPipe.bindStorage(pipe)));
    nvs_handle_t nvs = 0U;
    CHECK(nvs_open("gnss", NVS_READWRITE, &nvs) == ESP_OK);
    s_nvs = nvs;

    startModule(nvs, 115200U, true);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    // G-baud-save：启动从 460800 切到 115200 成功后发送一次 SAVECONFIG（在切速验证之后）。
    CHECK(s_hostBaud == 115200U && s_baudCommands == 1U && s_saveCommands == 1U &&
          s_saveAfterVerify);

    const std::array<uint8_t, 1> rate{2U};
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kSetRate), rate).status));
    advance(500U);
    uint8_t storedRate = 0U;
    CHECK(nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK && storedRate == 2U);
    auto out = app::data::GnssOutConfig::defaults();
    CHECK(fw::isOk(out.applyInterval(0U, -5)));
    std::array<uint8_t, decltype(out)::kBlobBytes> outBytes{};
    size_t len = 0U;
    CHECK(fw::isOk(out.encode(outBytes, len)));
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssSubVerb::kSetOut), outBytes).status));
    advance(500U);
    CHECK(s_context->data.gnssOutCfg.read().intervals == out.intervals && s_saveCommands == 1U);

    constexpr std::array<uint8_t, 4> kBaud460800{0x00U, 0x08U, 0x07U, 0x00U};
    s_rejectSave = true;
    const auto result = command(static_cast<uint8_t>(app::GnssMaintenanceVerb::kBaud), kBaud460800);
    CHECK(fw::isOk(result.status));
    CHECK(s_context->data.gnssPendingNvs.load(std::memory_order_acquire));
    // 接收机可完成切速，但超过保存期限不得启动 NVS 提交。
    app::store::startPowerSave(s_now - app::board::kShutdownSaveTimeoutMs);
    advance(500U);
    uint32_t stored = 0U;
    CHECK(nvs_get_u32(nvs, "baud", &stored) == ESP_OK && stored == 115200U);
    CHECK(s_hostBaud == 460800U && s_saveCommands == 1U);
    CHECK(s_context->data.gnssPendingNvs.load(std::memory_order_acquire));
    app::store::resumePowerSave();
    advance(500U);
    CHECK(nvs_get_u32(nvs, "baud", &stored) == ESP_OK && stored == 460800U);
    CHECK(!s_context->data.gnssPendingNvs.load(std::memory_order_acquire));
    CHECK(s_hostBaud == 460800U && s_saveCommands == 2U);
    CHECK(s_context->data.gnssStatus.read().ctrl.maintenanceResult == gnss::MaintenanceResult::kOk);
    s_rejectSave = false;

    const auto reset = command(static_cast<uint8_t>(app::GnssMaintenanceVerb::kFactoryReset));
    CHECK(fw::isOk(reset.status));
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kProbing);
    CHECK(s_context->data.gnssStatus.read().ctrl.maintenanceResult == gnss::MaintenanceResult::kOk);
    checkStoredDefaults(nvs);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kProbing);
    advance(8000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_hostBaud == 115200U && s_saveCommands == 2U);
    checkStoredDefaults(nvs);
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssMaintenanceVerb::kSave)).status));
    advance(100U);
    CHECK(s_saveCommands == 3U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    s_context = &s_failureContext;
    s_context->bindContexts();
    CHECK(fw::isOk(s_context->gnss.bindStorage(demux)));
    CHECK(fw::isOk(s_context->rtcmPipe.bindStorage(pipe)));
    startModule(nvs, 115200U, false);
    advance(4000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_hostBaud == 460800U && s_baudCommands == 1U && s_saveCommands == 0U);
    CHECK(s_context->data.gnssBaudFailCount.load(std::memory_order_relaxed) == 1U);
    CHECK(nvs_get_u32(nvs, "baud", &stored) == ESP_OK && stored == 115200U);
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 460800U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    nvs_close(nvs);
}

/** @brief 用真实 kIo 模块与执行器验证 PPP/0x06 延迟结单、失败保留 NVS 与重探重放。 @note 主机单线程。 */
void testGnssPhonePpp()
{
    std::array<uint8_t, app::GnssModule::kDemuxBufBytes> demux{};
    std::array<uint8_t, app::kRtcmPipeBytes> pipe{};
    s_context = &s_pppContext;
    s_context->bindContexts();
    s_context->data.gnssLatestGsv.bindLock({});
    CHECK(fw::isOk(s_context->gnss.bindStorage(demux)));
    CHECK(fw::isOk(s_context->rtcmPipe.bindStorage(pipe)));
    nvs_handle_t nvs = 0U;
    CHECK(nvs_open("gnss", NVS_READWRITE, &nvs) == ESP_OK);
    s_nvs = nvs;
    startModule(nvs, 460800U, true);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    const auto um980Caps = s_context->data.gnssCaps.read();
    CHECK(um980Caps.valid && um980Caps.pppSupported && (um980Caps.constMask == 0x1FU));

    rtos::Executor executor{};
    app::ExecutorControlStorage<4U> control{};
    std::array<StackType_t, 256U> stack{};
    const auto execId = app::id(app::ExecKind::kIo);
    const auto target = app::id(app::ModuleKind::kGnss);
    fw::ModuleOps ops{};
    ops.onCommand = [](void *self, const fw::Command &cmd, fw::CommandResult &out) noexcept {
        static_cast<app::GnssModule *>(self)->onCommand(cmd, out);
    };
    const std::array modules{fw::Module{
        .name = "gnss", .id = target, .executor = execId, .self = &s_context->gnss, .ops = &ops}};
    CHECK(fw::isOk(executor.init({.name = "io", .id = execId, .priority = 9U}, modules,
                                 app::viewOf(control, stack))));
    s_context->ioExecutor = &executor;
    rtos::ExecutorTestAccess::bind(executor, &control.tcb);

    auto postPhone = [&](const fw::Command &cmd) {
        rtos::Ticket ticket{};
        CHECK(fw::isOk(executor.post(cmd, rtos::Caller::kPhone, ticket)));
        return ticket;
    };

    fw::Command request{
        .target = target, .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplyPpp), .argLen = 1U};
    request.args[0] = 1U;
    const auto pppOn = postPhone(request);
    rtos::ExecutorTestAccess::drain(executor);
    fw::CommandResult pollRes{};
    advance(20U);
    CHECK(executor.poll(pppOn, pollRes) == rtos::PollState::kPending);
    advance(200U);
    CHECK(executor.poll(pppOn, pollRes) == rtos::PollState::kDone);
    CHECK(pollRes.seq == pppOn.seq && pollRes.status == fw::Result::kOk);
    CHECK(s_context->data.gnssPppEnabled.read() == true);
    uint32_t stored = 0U;
    CHECK(app::store::readValue(app::store::kPhonePppKey, stored) == ESP_OK && stored == 1U);

    s_rejectSave = true;
    request.args[0] = 0U;
    const auto pppOffFail = postPhone(request);
    rtos::ExecutorTestAccess::drain(executor);
    advance(200U);
    s_rejectSave = false;
    CHECK(executor.poll(pppOffFail, pollRes) == rtos::PollState::kDone);
    CHECK(pollRes.seq == pppOffFail.seq && pollRes.status == fw::Result::kIoError);
    CHECK(s_context->data.gnssPppEnabled.read() == true);
    CHECK(app::store::readValue(app::store::kPhonePppKey, stored) == ESP_OK && stored == 1U);

    const auto saves = s_saveCommands;
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kProbe)).status));
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_saveCommands == saves + 1U);

    // 0x06 截止角/星座：成功写入 NVS、NACK 保留旧值、重探后重放。
    const core::SatTrackConfig satOk{
        .cutoffDeg = 10U, .gps = 1U, .glo = 0U, .bds = 1U, .gal = 1U, .sbas = 0U};
    fw::Command satCmd{.target = target,
                       .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack)};
    const auto satEncoded = core::encodeSatTrackConfig(satOk);
    satCmd.argLen = static_cast<uint8_t>(satEncoded.size());
    std::copy(satEncoded.begin(), satEncoded.end(), satCmd.args.begin());
    s_satMaskCommands = 0U;
    const auto satTicket = postPhone(satCmd);
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(satTicket, pollRes) == rtos::PollState::kPending);
    advance(300U);
    CHECK(executor.poll(satTicket, pollRes) == rtos::PollState::kDone);
    CHECK(pollRes.seq == satTicket.seq && pollRes.status == fw::Result::kOk);
    CHECK(s_satMaskCommands == 5U);
    CHECK(s_context->data.gnssSatTrack.read() == satOk);
    std::array<uint8_t, core::kSatConfigBlobBytes> storedSat{};
    size_t storedSatLen = 0U;
    CHECK(app::store::readBlob(app::store::kPhoneSatCfgKey, storedSat, storedSatLen) == ESP_OK);
    CHECK(storedSatLen == satEncoded.size() && storedSat == satEncoded);

    // NACK 失败时保留旧 NVS 值
    s_rejectSatMask = true;
    satCmd.args[0] = 20U;
    const auto satFailTicket = postPhone(satCmd);
    rtos::ExecutorTestAccess::drain(executor);
    advance(300U);
    s_rejectSatMask = false;
    CHECK(executor.poll(satFailTicket, pollRes) == rtos::PollState::kDone);
    CHECK(pollRes.seq == satFailTicket.seq && pollRes.status == fw::Result::kIoError);
    CHECK(s_context->data.gnssSatTrack.read() == satOk);

    // 重探后重放 0x06 配置，并验证 GSV 单句缓存
    const unsigned satBeforeReprobe = s_satMaskCommands;
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kProbe)).status));
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_satMaskCommands == satBeforeReprobe + 5U);
    constexpr std::string_view kGsvSample = "$GPGSV,1,1,01,01,45,120,42*4D\r\n";
    s_rx += kGsvSample;
    advance(40U);
    const auto gsv = s_context->data.gnssLatestGsv.read();
    CHECK(gsv.len == kGsvSample.size() &&
          std::memcmp(gsv.bytes.data(), kGsvSample.data(), gsv.len) == 0);

    // T2：PPP 与星座序列在途时 kProbe 必须能打断序列并以 kInvalidState 结单。
    request.args[0] = 0U;
    const auto pppAbort = postPhone(request);
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(pppAbort, pollRes) == rtos::PollState::kPending);
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kProbe)).status));
    CHECK(executor.poll(pppAbort, pollRes) == rtos::PollState::kDone);
    CHECK(pollRes.seq == pppAbort.seq && pollRes.status == fw::Result::kInvalidState);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);

    satCmd.args[0] = 15U;
    const auto satAbort = postPhone(satCmd);
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(satAbort, pollRes) == rtos::PollState::kPending);
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kProbe)).status));
    CHECK(executor.poll(satAbort, pollRes) == rtos::PollState::kDone);
    CHECK(pollRes.seq == satAbort.seq && pollRes.status == fw::Result::kInvalidState);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);

    rtos::ExecutorTestAccess::bind(executor, nullptr);
    s_context->ioExecutor = nullptr;
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    nvs_close(nvs);
}

namespace {

/**
 * @brief 验证表驱动 gnss.base 10 个键 NVS 往返一致性及非法值回退行为。
 * @param nvs: 已打开的测试 NVS 句柄。
 */
void testBaseKeysTableRoundTrip(nvs_handle_t nvs)
{
    app::GnssBaseConfig cfg = app::GnssBaseConfig::defaults();
    cfg.mode = app::WorkModeKind::kBase;
    cfg.baseType = app::BaseType::kFixed;
    cfg.svyS = 123U;
    cfg.svyAccCm = 450U;
    cfg.svyToS = 600U;
    cfg.fixLatE9 = -31234567890LL;
    cfg.fixLonE9 = 121473701000LL;
    cfg.fixHeightMm = -54321;
    cfg.msm = app::GnssBaseConfig::kMsm7;
    cfg.rtcmOut = app::GnssBaseConfig::defaultRtcmOut(app::GnssBaseConfig::kMsm7);
    cfg.rtcmOut[0U] = -2;
    cfg.rtcmOut[2U] = 1;

    s_context->gnss.writeBaseConfig(cfg);
    CHECK(fw::isOk(s_context->storage.saveKeys(app::gnss_store::kBaseKeys)));

    s_context->gnss.writeBaseConfig(app::GnssBaseConfig::defaults());
    const auto loaded = s_context->storage.loadKeys(app::gnss_store::kBaseKeys);
    CHECK((loaded.saved == app::gnss_store::kBaseKeys.size()) && (loaded.failed == 0U));
    const auto got = s_context->gnss.readBaseConfig();
    CHECK((got.mode == cfg.mode) && (got.baseType == cfg.baseType));
    CHECK((got.svyS == cfg.svyS) && (got.svyAccCm == cfg.svyAccCm) && (got.svyToS == cfg.svyToS));
    CHECK((got.fixLatE9 == cfg.fixLatE9) && (got.fixLonE9 == cfg.fixLonE9) &&
          (got.fixHeightMm == cfg.fixHeightMm));
    CHECK((got.msm == cfg.msm) && (got.rtcmOut == cfg.rtcmOut));

    // 非法 NVS 值（范围越界或长度/版本错误）不得污染有效配置，应报 failed 并回退默认。
    CHECK(nvs_set_u8(nvs, "mode", 9U) == ESP_OK);
    CHECK(nvs_set_u16(nvs, "svy_s", 2U) == ESP_OK);
    const std::array<uint8_t, 4U> badLat{1U, 2U, 3U, 4U};
    CHECK(nvs_set_blob(nvs, "fix_lat", badLat.data(), badLat.size()) == ESP_OK);
    std::array<uint8_t, app::GnssBaseConfig::kRtcmOutBlobBytes> badRtcm{};
    badRtcm[0U] = 99U;
    CHECK(nvs_set_blob(nvs, "rtcm_out", badRtcm.data(), badRtcm.size()) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "msm", 5U) == ESP_OK);
    s_context->gnss.writeBaseConfig(app::GnssBaseConfig::defaults());
    const auto badLoad = s_context->storage.loadKeys(app::gnss_store::kBaseKeys);
    CHECK(badLoad.failed == 5U);
}

/**
 * @brief 验证控制台打包函数产生的命令经 GnssModule::onCommand 正确落到各处理分支。
 * @param nvs: 已打开的测试 NVS 句柄。
 */
void testConsoleCommandDispatch(nvs_handle_t nvs)
{
    const auto runPack = [](const fw::Command &cmd) noexcept {
        fw::CommandResult res{};
        s_context->gnss.onCommand(cmd, res);
        return res;
    };

    // 1. kSetBase（4 种分包子包）：各自校验并原子落盘，非法子包拒绝。
    CHECK(fw::isOk(runPack(app::detail::packGnssBaseSurvey(45U, 200U, 300U)).status));
    CHECK(runPack(app::detail::packGnssBaseSurvey(1U, 200U, 300U)).status ==
          fw::Result::kInvalidArg);
    uint16_t u16 = 0U;
    CHECK(nvs_get_u16(nvs, "svy_s", &u16) == ESP_OK && u16 == 45U);
    CHECK(fw::isOk(
        runPack(app::detail::packGnssBaseFixed(31234567890LL, -121473701000LL, 12345)).status));
    auto rtcm = app::GnssBaseConfig::defaultRtcmOut(app::GnssBaseConfig::kMsm7);
    rtcm[0U] = -2;
    CHECK(
        fw::isOk(runPack(app::detail::packGnssBaseRtcm(app::GnssBaseConfig::kMsm7, rtcm)).status));
    CHECK(fw::isOk(runPack(app::detail::packGnssBaseMode(app::WorkModeKind::kBase)).status));
    const auto cfg = s_context->gnss.readBaseConfig();
    CHECK((cfg.mode == app::WorkModeKind::kBase) && (cfg.baseType == app::BaseType::kFixed) &&
          (cfg.msm == app::GnssBaseConfig::kMsm7) && (cfg.rtcmOut[0U] == -2));

    // 2. gnss mode base / gnss out / gnss config / gnss baud / gnss save / gnss freset。
    s_modeBaseCommands = 0U;
    CHECK(fw::isOk(runPack(app::detail::packGnssMode(app::WorkModeKind::kBase, true)).status));
    advance(500U);
    CHECK(s_modeBaseCommands >= 1U);

    auto outCfg = app::data::GnssOutConfig::defaults();
    CHECK(fw::isOk(outCfg.applyInterval(0U, -5)));
    bool packed = false;
    CHECK(fw::isOk(runPack(app::detail::packGnssOut(outCfg, packed)).status) && packed);
    advance(500U);
    CHECK(s_context->data.gnssOutCfg.read().intervals == outCfg.intervals);

    const uint32_t rbBefore = s_context->data.gnssReadback.read().seq;
    CHECK(fw::isOk(runPack(app::detail::packGnssReadback()).status));
    advance(2500U);
    CHECK(s_context->data.gnssReadback.read().seq != rbBefore);

    CHECK(
        fw::isOk(runPack(app::detail::packGnssMaintenance(app::GnssMaintenanceVerb::kBaud, 460800U))
                     .status));
    advance(500U);
    CHECK(s_hostBaud == 460800U);

    const unsigned savesBefore = s_saveCommands;
    CHECK(fw::isOk(
        runPack(app::detail::packGnssMaintenance(app::GnssMaintenanceVerb::kSave)).status));
    advance(100U);
    CHECK(s_saveCommands == savesBefore + 1U);

    const unsigned fresetsBefore = s_fresetCommands;
    CHECK(fw::isOk(
        runPack(app::detail::packGnssMaintenance(app::GnssMaintenanceVerb::kFactoryReset)).status));
    CHECK(s_fresetCommands == fresetsBefore + 1U);
    checkStoredDefaults(nvs);
}

/**
 * @brief 用会拒绝亚秒 GSA 的 CM680 替身验证 rate 10 完成配置和持久化。
 * @param nvs: 已打开的 GNSS NVS 替身句柄。
 * @note 单线程主机测试。
 */
void testCm680Rate10(nvs_handle_t nvs)
{
    constexpr size_t kGsaIndex = static_cast<size_t>(gnss::OutputMsg::kGsa);
    const uint32_t before = s_context->data.gnssStatus.read().applySeq;
    fw::Command cmd{};
    cmd.target = app::id(app::ModuleKind::kGnss);
    cmd.verb = static_cast<uint8_t>(app::GnssVerb::kSetRate);
    cmd.argLen = 1U;
    cmd.args[0] = 10U;
    fw::CommandResult result{};
    s_context->gnss.onCommand(cmd, result);
    CHECK(fw::isOk(result.status));
    advance(500U);
    const auto status = s_context->data.gnssStatus.read();
    CHECK(status.applySeq != before && status.lastApply == app::GnssApply::kOk);
    CHECK(status.state == gnss::State::kRunning && status.failedMask == 0U);
    CHECK(status.rxPeriodMs[kGsaIndex] == 1000U);
    uint8_t storedRate = 0U;
    CHECK(nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK && storedRate == 10U);
}

} // namespace

/**
 * @brief 执行 gnss.base 表驱动往返及 console->onCommand 分流集成验证。
 */
void testBaseKeysAndCommandDispatch()
{
    std::array<uint8_t, app::GnssModule::kDemuxBufBytes> demux{};
    std::array<uint8_t, app::kRtcmPipeBytes> pipe{};
    s_context = &s_dispatchContext;
    s_context->bindContexts();
    CHECK(fw::isOk(s_context->gnss.bindStorage(demux)));
    CHECK(fw::isOk(s_context->rtcmPipe.bindStorage(pipe)));

    nvs_handle_t nvs = 0U;
    CHECK(nvs_open("gnss", NVS_READWRITE, &nvs) == ESP_OK);
    s_nvs = nvs;
    startModule(nvs, 115200U, true);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);

    testBaseKeysTableRoundTrip(nvs);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 使用全新 AppContext 并恢复默认 gnss.base 键后测试命令打包 -> onCommand 分流。
    s_context = &s_dispatchCmdContext;
    s_context->bindContexts();
    CHECK(fw::isOk(s_context->gnss.bindStorage(demux)));
    CHECK(fw::isOk(s_context->rtcmPipe.bindStorage(pipe)));
    s_context->gnss.writeBaseConfig(app::GnssBaseConfig::defaults());
    CHECK(fw::isOk(s_context->storage.saveKeys(app::gnss_store::kBaseKeys)));
    startModule(nvs, 115200U, true, true);
    advance(3000U);
    const auto status = s_context->data.gnssStatus.read();
    CHECK(status.state == gnss::State::kRunning && status.family == gnss::Family::kOptisens &&
          status.model == gnss::Model::kCm680);
    uint8_t cachedFamily = 0U;
    uint8_t cachedModel = 0U;
    CHECK(nvs_get_u8(nvs, "family", &cachedFamily) == ESP_OK && cachedFamily == 4U);
    CHECK(nvs_get_u8(nvs, "model", &cachedModel) == ESP_OK && cachedModel == 6U);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kCacheKeys).failed == 0U);
    testCm680Rate10(nvs);
    // CM680 不支持 PPP 但支持 MASK/UNMASK（含 SBAS）：gnssCaps 与命令结果一致且不下发 CONFIG PPP。
    const auto cm680Caps = s_context->data.gnssCaps.read();
    CHECK(cm680Caps.valid && !cm680Caps.pppSupported && (cm680Caps.constMask == 0x1FU));
    s_pppCommands = 0U;
    fw::Command cm680Ppp{};
    cm680Ppp.verb = static_cast<uint8_t>(app::GnssSubVerb::kApplyPpp);
    cm680Ppp.seq = 1U;
    cm680Ppp.argLen = 1U;
    cm680Ppp.args[0] = 1U;
    fw::CommandResult cm680PppRes{};
    s_context->gnss.onCommand(cm680Ppp, cm680PppRes);
    CHECK(cm680PppRes.status == fw::Result::kNotSupported);
    CHECK(s_pppCommands == 0U);
    testConsoleCommandDispatch(nvs);
    CHECK(!s_sentSatsInfo);
    CHECK(s_gpioConfigs == 0U && s_gpioChanges == 0U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    nvs_close(nvs);
}

namespace {

/**
 * @brief 擦除 GNSS 命名空间下的探测缓存与用户波特率覆盖键。
 * @param nvs: 已打开的 NVS 句柄。
 * @note 仅单线程主机测试调用。
 */
void eraseGnssBaudAndCache(nvs_handle_t nvs)
{
    for (const char *key : {"baud", "family", "model", "cache_baud"}) {
        const esp_err_t err = nvs_erase_key(nvs, key);
        CHECK((err == ESP_OK) || (err == ESP_ERR_NVS_NOT_FOUND));
    }
    CHECK(nvs_commit(nvs) == ESP_OK);
}

/**
 * @brief 在独立 AppContext 上以给定接收机速率与 NVS 覆盖启动 P2(CM680) GNSS 模块。
 * @param slot: s_targetBaudContexts 数组下标。
 * @param nvs: 已打开的 NVS 句柄。
 * @param demux: 外供分流区。
 * @param pipe: 外供 RTCM 管道区。
 * @param receiverBaud: 模拟接收机当前串口波特率。
 * @param userBaud: 用户 NVS gnss/baud 值（0 表示不设置）。
 * @param applyBaud: 接收机是否响应改速命令。
 * @param rejectBaud: 接收机是否对改速命令返回 ERROR。
 * @note 仅单线程主机测试调用。
 */
void bootP2TargetCase(size_t slot, nvs_handle_t nvs, std::span<uint8_t> demux,
                      std::span<uint8_t> pipe, uint32_t receiverBaud, uint32_t userBaud,
                      bool applyBaud, bool rejectBaud)
{
    s_context = &s_targetBaudContexts[slot];
    s_context->bindContexts();
    CHECK(fw::isOk(s_context->gnss.bindStorage(demux)));
    CHECK(fw::isOk(s_context->rtcmPipe.bindStorage(pipe)));
    s_rx.clear();
    s_readyAt = 0U;
    s_receiverBaud = receiverBaud;
    s_baudCommands = 0U;
    s_saveCommands = 0U;
    s_gpioChanges = 0U;
    s_gpioConfigs = 0U;
    s_p2 = true;
    s_sentSatsInfo = false;
    s_applyBaud = applyBaud;
    s_rejectBaud = rejectBaud;
    s_expectBaudSave = false;
    s_bootBaudMode = true;
    s_verifiedAfterBaud = false;
    s_saveAfterVerify = false;
    if (userBaud != 0U) {
        CHECK(nvs_set_u32(nvs, "baud", userBaud) == ESP_OK);
    } else {
        const esp_err_t err = nvs_erase_key(nvs, "baud");
        CHECK((err == ESP_OK) || (err == ESP_ERR_NVS_NOT_FOUND));
    }
    CHECK(nvs_commit(nvs) == ESP_OK);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kConfigKeys).failed == 0U);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kCacheKeys).failed == 0U);
    s_context->bootConfig.gnssBaud = userBaud;
    s_context->bootConfig.userNvsReady = true;
    s_context->boardProfile = app::board::byId(app::board::BoardId::kP2V141);
    CHECK(s_context->boardProfile != nullptr);
    CHECK(fw::isOk(s_context->gnss.init()));
    CHECK(fw::isOk(s_context->gnss.start()));
}

} // namespace

/**
 * @brief 验证 G-baud460k 启动运行切速策略：默认 460800 不写 SAVECONFIG、同速不切、NVS 覆盖、冷热重探与失败单次回退。
 * @note 单线程主机测试。
 */
void testGnssTargetBaudPolicy()
{
    std::array<uint8_t, app::GnssModule::kDemuxBufBytes> demux{};
    std::array<uint8_t, app::kRtcmPipeBytes> pipe{};
    nvs_handle_t nvs = 0U;
    CHECK(nvs_open("gnss", NVS_READWRITE, &nvs) == ESP_OK);
    s_nvs = nvs;
    const fw::Result installed = fw::traceInstall(&captureBaudTrace, nullptr, &rtos::nowMs);
    CHECK(fw::isOk(installed) || (installed == fw::Result::kInvalidState));
    fw::traceSetMask(fw::traceBit(fw::TraceCat::kGnss));

    // 1. 首次冷启动（无 NVS gnss/baud 与缓存，CM680 出厂 115200）→ 自动切 460800、验证后发 1 次 SAVECONFIG、不写 gnss/baud、hint 记 460800 与 family=4。
    eraseGnssBaudAndCache(nvs);
    bootP2TargetCase(0U, nvs, demux, pipe, 115200U, 0U, true, false);
    advance(3000U);
    auto st = s_context->data.gnssStatus.read();
    uint32_t stored = 0U;
    uint8_t storedFamily = 0U;
    CHECK(st.state == gnss::State::kRunning && st.family == gnss::Family::kOptisens &&
          st.model == gnss::Model::kCm680);
    CHECK(st.hostBaud == 460800U && s_hostBaud == 460800U && s_receiverBaud == 460800U);
    CHECK(s_baudCommands == 1U && s_saveCommands == 1U && s_saveAfterVerify);
    CHECK(s_context->data.gnssBaudFailCount.load(std::memory_order_relaxed) == 0U);
    CHECK(nvs_get_u32(nvs, "baud", &stored) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 460800U);
    CHECK(nvs_get_u8(nvs, "family", &storedFamily) == ESP_OK && storedFamily == 4U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 2. ESP32 单独复位（新缓存 family=4 model=6 cache_baud=460800，模块仍在 460800）→ 第一候选即命中，目标等于当前不切换、不发 SAVECONFIG。
    bootP2TargetCase(1U, nvs, demux, pipe, 460800U, 0U, true, false);
    advance(2500U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.family == gnss::Family::kOptisens &&
          st.model == gnss::Model::kCm680 && st.hostBaud == 460800U);
    CHECK(st.ctrl.probesSent == 1U && s_baudCommands == 0U && s_saveCommands == 0U);
    CHECK(s_context->data.gnssBaudFailCount.load(std::memory_order_relaxed) == 0U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 3. 模块掉电冷启动（cache_baud 仍为 460800，模块回出厂 115200）→ 候选表 115200 命中后切回 460800，验证后发 1 次 SAVECONFIG。
    bootP2TargetCase(2U, nvs, demux, pipe, 115200U, 0U, true, false);
    advance(3000U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.hostBaud == 460800U && s_receiverBaud == 460800U);
    CHECK(st.ctrl.probesSent == 3U && s_baudCommands == 1U && s_saveCommands == 1U &&
          s_saveAfterVerify);
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 460800U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 4a. 用户 NVS gnss/baud=115200 覆盖 Kconfig（目标 115200 == 当前 115200）→ 不切换、不发 SAVECONFIG。
    eraseGnssBaudAndCache(nvs);
    bootP2TargetCase(3U, nvs, demux, pipe, 115200U, 115200U, true, false);
    advance(2500U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.hostBaud == 115200U);
    CHECK(s_baudCommands == 0U && s_saveCommands == 0U);
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 115200U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 4b. 用户 NVS gnss/baud=230400 覆盖 Kconfig（模块在 115200）→ 切 230400，验证后发 1 次 SAVECONFIG，hint 记 230400。
    eraseGnssBaudAndCache(nvs);
    bootP2TargetCase(4U, nvs, demux, pipe, 115200U, 230400U, true, false);
    advance(3000U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.hostBaud == 230400U && s_receiverBaud == 230400U);
    CHECK(s_baudCommands == 1U && s_saveCommands == 1U && s_saveAfterVerify);
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 230400U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 5. 切速后探测超时：回退 115200 重探一次，记 trace 与 baud_fail=1，不重复尝试，hint 记实际 115200。
    eraseGnssBaudAndCache(nvs);
    s_baudFailTraces = 0U;
    s_lastBaudFailTarget = 0U;
    s_lastBaudFailResult = 0U;
    bootP2TargetCase(5U, nvs, demux, pipe, 115200U, 0U, false, false);
    advance(4500U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.hostBaud == 115200U);
    CHECK(s_baudCommands == 1U && s_saveCommands == 0U);
    CHECK(s_context->data.gnssBaudFailCount.load(std::memory_order_relaxed) == 1U);
    CHECK(s_baudFailTraces == 1U && s_lastBaudFailTarget == 460800U &&
          s_lastBaudFailResult == static_cast<uint32_t>(gnss::MaintenanceResult::kTimeout));
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 115200U);
    advance(3000U);
    CHECK(s_baudCommands == 1U &&
          s_context->data.gnssBaudFailCount.load(std::memory_order_relaxed) == 1U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 6. 接收机显式拒绝（NACK）：保持 115200 不卡死，记 trace 与 baud_fail=1，hint 记 115200。
    eraseGnssBaudAndCache(nvs);
    s_baudFailTraces = 0U;
    s_lastBaudFailTarget = 0U;
    s_lastBaudFailResult = 0U;
    bootP2TargetCase(6U, nvs, demux, pipe, 115200U, 0U, false, true);
    advance(3500U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.hostBaud == 115200U);
    CHECK(s_baudCommands == 1U && s_saveCommands == 0U);
    CHECK(s_context->data.gnssBaudFailCount.load(std::memory_order_relaxed) == 1U);
    CHECK(s_baudFailTraces == 1U && s_lastBaudFailTarget == 460800U &&
          s_lastBaudFailResult == static_cast<uint32_t>(gnss::MaintenanceResult::kNack));
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 115200U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 7. 旧缓存迁移（现网已存 family=1 model=6 cache_baud=460800）：正确识别为 CM680/optisens，并把 NVS 缓存更新为 family=4。
    eraseGnssBaudAndCache(nvs);
    CHECK(nvs_set_u8(nvs, "family", static_cast<uint8_t>(gnss::Family::kUnicore)) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "model", static_cast<uint8_t>(gnss::Model::kCm680)) == ESP_OK);
    CHECK(nvs_set_u32(nvs, "cache_baud", 460800U) == ESP_OK);
    CHECK(nvs_commit(nvs) == ESP_OK);
    bootP2TargetCase(7U, nvs, demux, pipe, 460800U, 0U, true, false);
    advance(2500U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.family == gnss::Family::kOptisens &&
          st.model == gnss::Model::kCm680 && st.hostBaud == 460800U);
    CHECK(nvs_get_u8(nvs, "family", &storedFamily) == ESP_OK && storedFamily == 4U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 8. 新缓存（family=4 model=6 cache_baud=460800）重启首候选即命中，0 次 SAVECONFIG。
    bootP2TargetCase(8U, nvs, demux, pipe, 460800U, 0U, true, false);
    advance(2500U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.family == gnss::Family::kOptisens &&
          st.model == gnss::Model::kCm680 && st.ctrl.probesSent == 1U && s_saveCommands == 0U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 9. G-baud-save：冷启动 115200 切 460800 成功但 SAVECONFIG 返回 NACK → baud_fail 不变、保持 460800、状态 running、不写 gnss/baud。
    eraseGnssBaudAndCache(nvs);
    s_rejectSave = true;
    bootP2TargetCase(11U, nvs, demux, pipe, 115200U, 0U, true, false);
    advance(3500U);
    st = s_context->data.gnssStatus.read();
    CHECK(st.state == gnss::State::kRunning && st.hostBaud == 460800U && s_receiverBaud == 460800U);
    CHECK(s_baudCommands == 1U && s_saveCommands == 1U && s_saveAfterVerify);
    CHECK(s_context->data.gnssBaudFailCount.load(std::memory_order_relaxed) == 0U);
    CHECK(nvs_get_u32(nvs, "baud", &stored) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 460800U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    s_rejectSave = false;

    s_rejectBaud = false;
    s_bootBaudMode = false;
    fw::traceSetMask(0U);
    nvs_close(nvs);
}

/**
 * @brief 验证 Q1 GGA 质量门控的模块级联动：立即生效、去抖落盘、阻断丢弃/RMC 放行、3 连续达标恢复与 NVS 重启回读。
 * @note 单线程主机测试。
 */
void testGgaGateNvsAndModuleFlow()
{
    std::array<uint8_t, app::GnssModule::kDemuxBufBytes> demux{};
    std::array<uint8_t, app::kRtcmPipeBytes> pipe{};
    std::array<uint8_t, app::data::kGnssLatestTotalBytes> latestBuf{};
    nvs_handle_t nvs = 0U;
    CHECK(nvs_open("gnss", NVS_READWRITE, &nvs) == ESP_OK);
    s_nvs = nvs;

    eraseGnssBaudAndCache(nvs);
    for (const char *key : {"gga_gate", "gga_min_sats"}) {
        const esp_err_t err = nvs_erase_key(nvs, key);
        CHECK((err == ESP_OK) || (err == ESP_ERR_NVS_NOT_FOUND));
    }
    CHECK(nvs_commit(nvs) == ESP_OK);

    // 1. 冷启动默认关闭、min_sats=8。
    bootP2TargetCase(9U, nvs, demux, pipe, 115200U, 0U, true, false);
    s_context->data.gnssLatest.bindLock({});
    CHECK(fw::isOk(s_context->data.gnssLatest.init(latestBuf)));
    advance(3000U);
    auto gateSt = s_context->data.ggaGateStatus.read();
    CHECK(!gateSt.enabled && !gateSt.blocked && (gateSt.minSats == 8U));
    CHECK(s_context->gnss.ggaAllowed());

    // 2. 下发 kSetGgaGate(on, min_sats=12)：立即生效，去抖 500 ms 前不写盘，之后写盘。
    const fw::Command gateCmd = app::detail::packGnssGate(true, true, true, 12U);
    fw::CommandResult gateRes{};
    s_context->gnss.onCommand(gateCmd, gateRes);
    CHECK(fw::isOk(gateRes.status));
    gateSt = s_context->data.ggaGateStatus.read();
    CHECK(gateSt.enabled && !gateSt.blocked && (gateSt.minSats == 12U));
    uint8_t nvsVal = 0U;
    advance(200U);
    CHECK(nvs_get_u8(nvs, "gga_gate", &nvsVal) == ESP_ERR_NVS_NOT_FOUND);
    advance(500U);
    CHECK(nvs_get_u8(nvs, "gga_gate", &nvsVal) == ESP_OK && nvsVal == 1U);
    CHECK(nvs_get_u8(nvs, "gga_min_sats", &nvsVal) == ESP_OK && nvsVal == 12U);

    // 3. 喂入低卫星 GGA（10 < 12）与有效 RMC：GGA 被阻断且不写入最新缓存，RMC 照常写入。
    constexpr std::string_view kBadGga =
        "$GNGGA,080000.00,3112.34567,N,12123.45678,E,4,10,0.8,15.2,M,-5.0,M,1.0,0001*7F\r\n";
    constexpr std::string_view kGoodGga =
        "$GNGGA,080001.00,3112.34567,N,12123.45678,E,4,12,0.8,15.2,M,-5.0,M,1.0,0001*7C\r\n";
    constexpr std::string_view kRmc =
        "$GNRMC,080000.00,A,3112.34567,N,12123.45678,E,0.0,0.0,191126,,,A*4D\r\n";
    s_rx += kBadGga;
    s_rx += kRmc;
    advance(60U);
    gateSt = s_context->data.ggaGateStatus.read();
    CHECK(gateSt.blocked && !s_context->gnss.ggaAllowed());
    CHECK((gateSt.ggaBlocked == 1U) && (gateSt.gateTrans == 1U));
    std::array<uint8_t, proto::kMaxPayload> qbuf{};
    const auto ggaQ1 =
        s_context->data.gnssLatest.query(static_cast<uint8_t>(gnss::OutputMsg::kGga), qbuf);
    CHECK(!ggaQ1.ok);
    const auto rmcQ =
        s_context->data.gnssLatest.query(static_cast<uint8_t>(gnss::OutputMsg::kRmc), qbuf);
    CHECK(rmcQ.ok && (rmcQ.len == kRmc.size()));

    // 4. 喂入连续 3 条达标 GGA：前 2 条仍阻断，第 3 条放行并更新最新缓存。
    s_rx += kGoodGga;
    s_rx += kGoodGga;
    advance(60U);
    gateSt = s_context->data.ggaGateStatus.read();
    CHECK(gateSt.blocked && (gateSt.ggaBlocked == 3U) && (gateSt.gateTrans == 1U));
    s_rx += kGoodGga;
    advance(60U);
    gateSt = s_context->data.ggaGateStatus.read();
    CHECK(!gateSt.blocked && s_context->gnss.ggaAllowed());
    CHECK((gateSt.ggaBlocked == 3U) && (gateSt.gateTrans == 2U));
    const auto ggaQ2 =
        s_context->data.gnssLatest.query(static_cast<uint8_t>(gnss::OutputMsg::kGga), qbuf);
    CHECK(ggaQ2.ok && (ggaQ2.len == kGoodGga.size()));
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 5. 重启新模块实例：从 NVS 回读 gga_gate=1, gga_min_sats=12 并自动生效。
    bootP2TargetCase(10U, nvs, demux, pipe, 460800U, 0U, true, false);
    gateSt = s_context->data.ggaGateStatus.read();
    CHECK(gateSt.enabled && !gateSt.blocked && (gateSt.minSats == 12U));
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    nvs_close(nvs);
}

/**
 * @brief 验证 GNSS 关键设置的异步结单、网页单键落盘、失败回滚和控制台门控去抖。
 * @note 单线程主机测试，覆盖交叉评审 P1 #3、P1 #4、P2 #6。
 */
void testGnssSettingsBusAndGgaPersist()
{
    std::array<uint8_t, app::GnssModule::kDemuxBufBytes> demux{};
    std::array<uint8_t, app::kRtcmPipeBytes> pipe{};
    nvs_handle_t nvs = 0U;
    CHECK(nvs_open("gnss", NVS_READWRITE, &nvs) == ESP_OK);
    s_nvs = nvs;

    eraseGnssBaudAndCache(nvs);
    for (const char *key : {"rate", "gga_gate", "gga_min_sats"}) {
        const esp_err_t err = nvs_erase_key(nvs, key);
        CHECK((err == ESP_OK) || (err == ESP_ERR_NVS_NOT_FOUND));
    }
    CHECK(nvs_commit(nvs) == ESP_OK);

    bootP2TargetCase(12U, nvs, demux, pipe, 115200U, 0U, true, false);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);

    rtos::Executor executor{};
    app::ExecutorControlStorage<4U> control{};
    std::array<StackType_t, 256U> stack{};
    const auto execId = app::id(app::ExecKind::kIo);
    const auto target = app::id(app::ModuleKind::kGnss);
    fw::ModuleOps ops{};
    ops.onCommand = [](void *self, const fw::Command &cmd, fw::CommandResult &out) noexcept {
        static_cast<app::GnssModule *>(self)->onCommand(cmd, out);
    };
    const std::array modules{fw::Module{
        .name = "gnss", .id = target, .executor = execId, .self = &s_context->gnss, .ops = &ops}};
    CHECK(fw::isOk(executor.init({.name = "io", .id = execId, .priority = 9U}, modules,
                                 app::viewOf(control, stack))));
    s_context->ioExecutor = &executor;
    rtos::ExecutorTestAccess::bind(executor, &control.tcb);

    auto makeRateCmd = [target](app::GnssSubVerb verb, uint8_t hz) {
        fw::Command cmd{.target = target, .verb = static_cast<uint8_t>(verb), .argLen = 1U};
        cmd.args[0] = hz;
        return cmd;
    };
    auto post = [&](const fw::Command &cmd) {
        rtos::Ticket ticket{};
        CHECK(fw::isOk(executor.post(cmd, rtos::Caller::kWeb, ticket)));
        return ticket;
    };
    fw::CommandResult result{};
    auto expectDone = [&](const rtos::Ticket &ticket, fw::Result status) {
        CHECK(executor.poll(ticket, result) == rtos::PollState::kDone);
        CHECK(result.seq == ticket.seq && result.status == status);
    };

    // 控制台的在途变更不能被另一个凭据误认成自己的结果。
    auto console = makeRateCmd(app::GnssSubVerb::kApplyRate, 5U);
    console.verb = static_cast<uint8_t>(app::GnssVerb::kSetRate);
    s_context->gnss.onCommand(console, result);
    CHECK(fw::isOk(result.status));
    const auto busy = post(makeRateCmd(app::GnssSubVerb::kApplyRate, 10U));
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(busy, fw::Result::kBusy);
    advance(500U);

    // 网页等候实际写入 NVS，并在完成后看到更新过的能力。
    const auto rate = post(makeRateCmd(app::GnssSubVerb::kApplyRate, 10U));
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(rate, result) == rtos::PollState::kPending);
    advance(500U);
    expectDone(rate, fw::Result::kOk);
    CHECK(s_context->data.gnssCaps.read().navRateHz == 10U);
    uint8_t storedRate = 0U;
    CHECK((nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK) && (storedRate == 10U));

    const auto cancelled = post(makeRateCmd(app::GnssSubVerb::kApplyRate, 2U));
    executor.cancel(cancelled);
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(cancelled, result) == rtos::PollState::kInvalid);
    CHECK(s_context->data.gnssRateHz.load(std::memory_order_acquire) == 10U);

    const auto badRate = post(makeRateCmd(app::GnssSubVerb::kApplyRate, 20U));
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(badRate, fw::Result::kInvalidArg);

    // 网页单键落盘在同一轮完成。门控开关与阈值相互独立。
    fw::Command gateCmd = app::detail::packGnssGate(true, true, false, 0U);
    gateCmd.verb = static_cast<uint8_t>(app::GnssSubVerb::kApplyGgaGate);
    const auto gate = post(gateCmd);
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(gate, fw::Result::kOk);
    uint8_t nvsGate = 0U;
    uint8_t nvsMin = 0U;
    CHECK((nvs_get_u8(nvs, "gga_gate", &nvsGate) == ESP_OK) && (nvsGate == 1U));
    CHECK(nvs_get_u8(nvs, "gga_min_sats", &nvsMin) == ESP_ERR_NVS_NOT_FOUND);

    const auto badGga =
        "$GNGGA,080000.00,3112.34567,N,12123.45678,E,4,10,0.8,15.2,M,-5.0,M,1.0,0001*7F\r\n";
    // 先把阈值设为 11，确认门控处于阻断，再验证失败的关闭操作无副作用。
    auto threshold = app::detail::packGnssGate(false, false, true, 11U);
    threshold.verb = gateCmd.verb;
    const auto minSats = post(threshold);
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(minSats, fw::Result::kOk);
    s_rx += badGga;
    advance(60U);
    const auto beforeGate = s_context->data.ggaGateStatus.read();
    CHECK(beforeGate.enabled && beforeGate.blocked && !s_context->gnss.ggaAllowed());
    const uint32_t rev = s_context->data.configRev.load(std::memory_order_acquire);

    nvs_shim_set_write_fail(ESP_FAIL);
    gateCmd.args[1] = 0U;
    const auto failed = post(gateCmd);
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(failed, fw::Result::kIoError);
    nvs_shim_set_write_fail(ESP_OK);
    advance(1600U);
    const auto afterGate = s_context->data.ggaGateStatus.read();
    CHECK(afterGate.enabled && afterGate.blocked && !s_context->gnss.ggaAllowed());
    CHECK(afterGate.minSats == beforeGate.minSats && afterGate.gateTrans == beforeGate.gateTrans);
    CHECK(s_context->data.configRev.load(std::memory_order_acquire) == rev);
    CHECK(s_context->data.ggaGatePersist.load(std::memory_order_acquire) ==
          app::data::GgaGatePersist::kClean);
    CHECK((nvs_get_u8(nvs, "gga_gate", &nvsGate) == ESP_OK) && (nvsGate == 1U));

    s_context->bootConfig.safeDefaults = true;
    const auto readonly = post(gateCmd);
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(readonly, fw::Result::kInvalidState);
    s_context->bootConfig.safeDefaults = false;

    // 控制台仍去抖，退出命令可立即保存已存在的去抖修改。
    auto consoleGate = app::detail::packGnssGate(true, false, true, 9U);
    s_context->gnss.onCommand(consoleGate, result);
    CHECK(fw::isOk(result.status));
    CHECK((nvs_get_u8(nvs, "gga_min_sats", &nvsMin) == ESP_OK) && (nvsMin == 11U));
    const auto flush =
        post({.target = target, .verb = static_cast<uint8_t>(app::GnssSubVerb::kFlushGgaGate)});
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(flush, fw::Result::kOk);
    CHECK((nvs_get_u8(nvs, "gga_gate", &nvsGate) == ESP_OK) && (nvsGate == 0U));
    CHECK((nvs_get_u8(nvs, "gga_min_sats", &nvsMin) == ESP_OK) && (nvsMin == 9U));

    // 写失败后的去抖配置仍在关机待写快照中，队尾冲刷跳过退避。
    consoleGate = app::detail::packGnssGate(false, false, true, 8U);
    s_context->gnss.onCommand(consoleGate, result);
    CHECK(fw::isOk(result.status));
    nvs_shim_set_write_fail(ESP_FAIL);
    advance(520U);
    CHECK(s_context->data.ggaGatePersist.load(std::memory_order_acquire) ==
          app::data::GgaGatePersist::kFailed);
    CHECK(s_context->data.gnssPendingNvs.load(std::memory_order_acquire));
    nvs_shim_set_write_fail(ESP_OK);
    app::store::startPowerSave(s_now - app::board::kShutdownSaveTimeoutMs);
    advance(1000U);
    CHECK(s_context->data.gnssPendingNvs.load(std::memory_order_acquire));
    CHECK((nvs_get_u8(nvs, "gga_min_sats", &nvsMin) == ESP_OK) && (nvsMin == 9U));
    const auto expiredFlush =
        post({.target = target, .verb = static_cast<uint8_t>(app::GnssSubVerb::kFlushGgaGate)});
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(expiredFlush, fw::Result::kIoError);
    CHECK((nvs_get_u8(nvs, "gga_min_sats", &nvsMin) == ESP_OK) && (nvsMin == 9U));
    app::store::resumePowerSave();
    const auto shutdownFlush =
        post({.target = target, .verb = static_cast<uint8_t>(app::GnssSubVerb::kFlushGgaGate)});
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(shutdownFlush, fw::Result::kOk);
    CHECK(!s_context->data.gnssPendingNvs.load(std::memory_order_acquire));
    CHECK((nvs_get_u8(nvs, "gga_min_sats", &nvsMin) == ESP_OK) && (nvsMin == 8U));

    // 未完成的频率设置被重探立即终止，重探后的输出计划使用旧配置。
    const auto interruptedRate = post(makeRateCmd(app::GnssSubVerb::kApplyRate, 2U));
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(interruptedRate, result) == rtos::PollState::kPending);
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kProbe)).status));
    expectDone(interruptedRate, fw::Result::kInvalidState);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_context->data.gnssCaps.read().navRateHz == 10U);
    CHECK((nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK) && (storedRate == 10U));

    // 保存期间第二条维护命令回忙，复位立即终止第一条。
    const fw::Command save{.target = target,
                           .verb = static_cast<uint8_t>(app::GnssSubVerb::kSaveReceiver)};
    const auto interruptedSave = post(save);
    const auto overlappingSave = post(save);
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(interruptedSave, result) == rtos::PollState::kPending);
    expectDone(overlappingSave, fw::Result::kBusy);
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kReset)).status));
    expectDone(interruptedSave, fw::Result::kInvalidState);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    const auto savedAfterReset = post(save);
    rtos::ExecutorTestAccess::drain(executor);
    advance(500U);
    expectDone(savedAfterReset, fw::Result::kOk);

    // P2-1/P2-2 窗口 1：kMaintenance（kSave 在途）期间 kApplyRate / kPatchOutput 走延迟两阶段提交而非 kBusy。
    const auto maintSave1 = post(save);
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(maintSave1, result) == rtos::PollState::kPending);
    const uint32_t applySeqBeforeMaintRate = s_context->data.gnssStatus.read().applySeq;
    const auto maintRate = post(makeRateCmd(app::GnssSubVerb::kApplyRate, 2U));
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(maintRate, result) == rtos::PollState::kPending);
    advance(500U);
    expectDone(maintSave1, fw::Result::kOk);
    expectDone(maintRate, fw::Result::kOk);
    CHECK(s_context->data.gnssStatus.read().applySeq != applySeqBeforeMaintRate);
    CHECK(s_context->data.gnssCaps.read().navRateHz == 2U);
    CHECK((nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK) && (storedRate == 2U));

    const auto maintSave2 = post(save);
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(maintSave2, result) == rtos::PollState::kPending);
    fw::Command patchGga{.target = target,
                         .verb = static_cast<uint8_t>(app::GnssSubVerb::kPatchOutput),
                         .argLen = 2U};
    patchGga.args[0] = static_cast<uint8_t>(gnss::OutputMsg::kGga);
    patchGga.args[1] = static_cast<uint8_t>(int8_t{2}); // 500 ms (2 Hz)
    const auto maintOut = post(patchGga);
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(maintOut, result) == rtos::PollState::kPending);
    advance(500U);
    expectDone(maintSave2, fw::Result::kOk);
    expectDone(maintOut, fw::Result::kOk);
    CHECK(s_context->data.gnssOutCfg.read().intervals[0U] == 2);

    // T1：kReadback 与 kConfiguring 瞬态下 kApplyRate / kSetRate 必须返回 kBusy 且不改 NVS。
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssSubVerb::kReadback)).status));
    advance(20U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kReadback);
    const auto rbRateTicket = post(makeRateCmd(app::GnssSubVerb::kApplyRate, 5U));
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(rbRateTicket, fw::Result::kBusy);
    const std::array<uint8_t, 1U> legacyRate5{5U};
    CHECK(command(static_cast<uint8_t>(app::GnssVerb::kSetRate), legacyRate5).status ==
          fw::Result::kBusy);
    CHECK((nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK) && (storedRate == 2U));
    advance(2500U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);

    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kProbe)).status));
    for (int step = 0;
         step < 150 && s_context->data.gnssStatus.read().state != gnss::State::kConfiguring;
         ++step) {
        advance(20U);
    }
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kConfiguring);
    const auto cfgRateTicket = post(makeRateCmd(app::GnssSubVerb::kApplyRate, 5U));
    rtos::ExecutorTestAccess::drain(executor);
    expectDone(cfgRateTicket, fw::Result::kBusy);
    CHECK((nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK) && (storedRate == 2U));
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);

    rtos::ExecutorTestAccess::bind(executor, nullptr);
    s_context->ioExecutor = nullptr;
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // P2-1/P2-2 窗口 2：探测未完成（model == kUnknown，state != kRunning）时，
    // kApplyRate / kPatchOutput 立即落盘、推进 applySeq 并返回 kOk（与旧版一致）。
    bootP2TargetCase(14U, nvs, demux, pipe, 115200U, 0U, true, false);
    CHECK(s_context->data.gnssStatus.read().state != gnss::State::kRunning);
    CHECK(!s_context->data.gnssCaps.read().valid);
    {
        rtos::Executor preExec{};
        app::ExecutorControlStorage<4U> preControl{};
        std::array<StackType_t, 256U> preStack{};
        const std::array preMods{fw::Module{.name = "gnss",
                                            .id = target,
                                            .executor = execId,
                                            .self = &s_context->gnss,
                                            .ops = &ops}};
        CHECK(fw::isOk(preExec.init({.name = "io", .id = execId, .priority = 9U}, preMods,
                                    app::viewOf(preControl, preStack))));
        s_context->ioExecutor = &preExec;
        rtos::ExecutorTestAccess::bind(preExec, &preControl.tcb);

        const uint32_t seqBeforeRate = s_context->data.gnssStatus.read().applySeq;
        rtos::Ticket preRateTicket{};
        CHECK(fw::isOk(preExec.post(makeRateCmd(app::GnssSubVerb::kApplyRate, 5U),
                                    rtos::Caller::kConsole, preRateTicket)));
        rtos::ExecutorTestAccess::drain(preExec);
        CHECK(preExec.poll(preRateTicket, result) == rtos::PollState::kDone);
        CHECK(result.status == fw::Result::kOk);
        CHECK(s_context->data.gnssStatus.read().applySeq != seqBeforeRate);
        CHECK((nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK) && (storedRate == 5U));

        const uint32_t seqBeforeOut = s_context->data.gnssStatus.read().applySeq;
        fw::Command prePatch{.target = target,
                             .verb = static_cast<uint8_t>(app::GnssSubVerb::kPatchOutput),
                             .argLen = 2U};
        prePatch.args[0] = static_cast<uint8_t>(gnss::OutputMsg::kGga);
        prePatch.args[1] = static_cast<uint8_t>(int8_t{-5}); // 5000 ms (1/5 Hz)
        rtos::Ticket preOutTicket{};
        CHECK(fw::isOk(preExec.post(prePatch, rtos::Caller::kConsole, preOutTicket)));
        rtos::ExecutorTestAccess::drain(preExec);
        CHECK(preExec.poll(preOutTicket, result) == rtos::PollState::kDone);
        CHECK(result.status == fw::Result::kOk);
        CHECK(s_context->data.gnssStatus.read().applySeq != seqBeforeOut);
        CHECK(s_context->data.gnssOutCfg.read().intervals[0U] == -5);

        rtos::ExecutorTestAccess::bind(preExec, nullptr);
        s_context->ioExecutor = nullptr;
    }
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    // 低定位频率启动时保留合法的高频偏好，超表的旧周期回落到最慢档。
    constexpr auto kGga = static_cast<size_t>(gnss::OutputMsg::kGga);
    constexpr auto kRmc = static_cast<size_t>(gnss::OutputMsg::kRmc);
    auto out = app::data::GnssOutConfig::defaults();
    CHECK(fw::isOk(out.applyInterval(kGga, -2)));
    CHECK(fw::isOk(out.applyInterval(kRmc, 5)));
    std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> blob{};
    size_t len = 0U;
    CHECK(fw::isOk(out.encode(blob, len)));
    CHECK(nvs_set_blob(nvs, "out", blob.data(), len) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "rate", 1U) == ESP_OK);
    CHECK(nvs_commit(nvs) == ESP_OK);

    bootP2TargetCase(13U, nvs, demux, pipe, 460800U, 0U, true, false);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    const auto restored = s_context->data.gnssOutCfg.read();
    CHECK(restored.intervals[kGga] == -1 && restored.intervals[kRmc] == 5);
    CHECK(s_context->data.gnssStatus.read().rxPeriodMs[kGga] == 1000U);
    CHECK(s_context->data.gnssStatus.read().rxPeriodMs[kRmc] == 1000U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // T5：预存 20 Hz 但探测型号（CM680）最高支持 10 Hz → 回退到 10 Hz 且回写修正 NVS。
    CHECK(nvs_set_u8(nvs, "rate", 20U) == ESP_OK);
    CHECK(nvs_commit(nvs) == ESP_OK);
    bootP2TargetCase(15U, nvs, demux, pipe, 460800U, 0U, true, false);
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_context->data.gnssCaps.read().navRateHz == 10U);
    CHECK(s_context->data.gnssRateHz.load(std::memory_order_acquire) == 10U);
    CHECK((nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK) && (storedRate == 10U));
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    nvs_close(nvs);
}

/** @brief 在 P2 板上以模拟 F9P 接收机启动 GnssModule 并推进至 kRunning。 */
static void bootF9pModule(size_t slot, nvs_handle_t nvs, std::span<uint8_t> demux,
                          std::span<uint8_t> pipe, std::string_view fw,
                          std::optional<core::SatTrackConfig> presetSat = std::nullopt,
                          uint8_t presetRate = 1U, bool nakHighPrec = false)
{
    s_f9pMode = true;
    s_f9pFw = fw;
    s_f9pDropSatAck = false;
    s_f9pRecordTx = false;
    s_f9pFailNextWrite = false;
    s_f9pNakGgaMsgout = false;
    s_f9pNakHighPrecPort = nakHighPrec;
    s_f9pLastRateMeasMs = 0U;
    s_f9pRateMeasWrites = 0U;
    s_f9pLastGgaEpochs = 0xFFU;
    s_f9pGgaWrites = 0U;
    s_f9pLastGsvEpochs = 0xFFU;
    s_f9pGsvWrites = 0U;
    s_f9pHighPrecNaks = 0U;
    s_f9pSatValsets = 0U;
    s_f9pTxLog.clear();
    s_lastF9pSatValsetLen = 0U;
    s_rx.clear();
    s_readyAt = 0U;
    s_receiverBaud = 115200U;
    s_p2 = true;
    s_context = &s_f9pContexts[slot];
    s_context->bindContexts();
    s_context->data.gnssLatestGsv.bindLock({});
    CHECK(fw::isOk(s_context->gnss.bindStorage(demux)));
    CHECK(fw::isOk(s_context->rtcmPipe.bindStorage(pipe)));
    CHECK(nvs_set_u32(nvs, "baud", 115200U) == ESP_OK);
    CHECK(nvs_set_u32(nvs, "cache_baud", 115200U) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "family", static_cast<uint8_t>(gnss::Family::kUblox)) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "model", static_cast<uint8_t>(gnss::Model::kZedF9p)) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "rate", presetRate) == ESP_OK);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kConfigKeys).failed == 0U);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kCacheKeys).failed == 0U);
    s_context->bootConfig.gnssBaud = 115200U;
    s_context->bootConfig.userNvsReady = true;
    s_context->boardProfile = app::board::byId(app::board::BoardId::kP2V141);
    CHECK(s_context->boardProfile != nullptr);
    if (presetSat.has_value()) {
        s_context->data.gnssSatTrack.write(*presetSat);
    }
    CHECK(fw::isOk(s_context->gnss.init()));
    CHECK(fw::isOk(s_context->gnss.start()));
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
}

/** @brief 验证 F9P 600 ms 稳定期、快速连发 kBusy 与 NVS 写入失败自动回滚（RU1-02/04/06/08）。 */
static void verifyF9pSettleBusyAndRollback(rtos::Executor &executor)
{
    const auto target = app::id(app::ModuleKind::kGnss);
    auto postCmd = [&](const fw::Command &cmd) {
        rtos::Ticket t{};
        CHECK(fw::isOk(executor.post(cmd, rtos::Caller::kWeb, t)));
        rtos::ExecutorTestAccess::drain(executor);
        return t;
    };
    fw::CommandResult res{};

    // RU1-02：F9P 拒绝 gnss.const.gps 与 gps=0，且不写串口
    const unsigned valsetsBefore = s_f9pSatValsets;
    fw::Command gpsPatch{.target = target,
                         .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack),
                         .argLen = 2U};
    gpsPatch.args[0] = settings::const_bit::kGps;
    gpsPatch.args[1] = 0U;
    CHECK(executor.poll(postCmd(gpsPatch), res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kNotSupported);
    CHECK(s_f9pSatValsets == valsetsBefore);

    // RU1-04/08：1 字节 gnss.cutoff=20 下发后 ACK 立即到达，但须在 600 ms 稳定期内保持 kPending 且拒连发
    fw::Command cutoffCmd{.target = target,
                          .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack),
                          .argLen = 1U};
    cutoffCmd.args[0] = 20U;
    const auto t1 = postCmd(cutoffCmd);
    advance(60U);
    CHECK(executor.poll(t1, res) == rtos::PollState::kPending);
    // 稳定期内快速连发新请求必须立即回 kBusy
    cutoffCmd.args[0] = 25U;
    CHECK(executor.poll(postCmd(cutoffCmd), res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kBusy);
    advance(620U);
    CHECK(executor.poll(t1, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kOk);
    CHECK(s_context->data.gnssSatTrack.read()->cutoffDeg == 20U);

    // RU1-06：NVS 写盘失败时尽最大努力回写旧快照（cutoff=20）并向调用方报 kIoError
    nvs_shim_set_write_fail(ESP_FAIL);
    const unsigned beforeFail = s_f9pSatValsets;
    cutoffCmd.args[0] = 35U;
    const auto tFail = postCmd(cutoffCmd);
    advance(1500U);
    nvs_shim_set_write_fail(ESP_OK);
    CHECK(executor.poll(tFail, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kIoError);
    // 发生 2 次写（新值 35 + 回滚旧值 20），且末帧 MINELEV 为 20、快照仍为 20
    CHECK(s_f9pSatValsets == beforeFail + 2U);
    CHECK((s_lastF9pSatValsetLen > 14U) && (s_lastF9pSatValset[14U] == 20U));
    CHECK(s_context->data.gnssSatTrack.read()->cutoffDeg == 20U);
}

/** @brief 验证 F9P 跨 512 B 块 RTCM3 帧边界插入 UBX 与超时/重同步期零溢出排空（RU1-07）。 */
static void verifyF9pRtcmFramingAndDrain(rtos::Executor &executor)
{
    const auto target = app::id(app::ModuleKind::kGnss);
    // 600 B 的 RTCM3 帧跨过 512 B 块边界：先排空首块 512 B（帧未结束），再发起 F9P 卫星命令
    const std::string rtcm600 = makeRtcm3Frame(594U);
    CHECK(rtcm600.size() == 600U);
    s_f9pTxLog.clear();
    s_f9pRecordTx = true;
    CHECK(s_context->rtcmPipe.push(std::span<const uint8_t>{
        reinterpret_cast<const uint8_t *>(rtcm600.data()), rtcm600.size()}));
    // 单次 poll 只写 1 块 512 B，此时处于 RTCM3 帧中部
    static_cast<void>(s_context->gnss.poll(s_now));
    CHECK(s_f9pTxLog.size() == 512U);
    // 发起 F9P 截止角修改，下一次 poll 必须先发完剩余 88 B RTCM3 再在偏移 600 插入 UBX VALSET
    fw::Command cutoffCmd{.target = target,
                          .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack),
                          .argLen = 1U};
    cutoffCmd.args[0] = 30U;
    rtos::Ticket tBoundary{};
    CHECK(fw::isOk(executor.post(cutoffCmd, rtos::Caller::kWeb, tBoundary)));
    rtos::ExecutorTestAccess::drain(executor);
    advance(700U);
    fw::CommandResult res{};
    CHECK(executor.poll(tBoundary, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kOk);
    CHECK(s_f9pTxLog.size() > 602U);
    CHECK(std::memcmp(s_f9pTxLog.data(), rtcm600.data(), 600U) == 0);
    CHECK((static_cast<uint8_t>(s_f9pTxLog[600U]) == 0xB5U) &&
          (static_cast<uint8_t>(s_f9pTxLog[601U]) == 0x62U));
    s_f9pRecordTx = false;

    // 满载管道在 1000 ms 超时 + 300 ms 重同步期间持续排空，总写入 > 4096 B 仍零溢出
    s_f9pDropSatAck = true;
    const uint32_t droppedBefore = s_context->rtcmPipe.stats().droppedBytes;
    const uint32_t writtenBefore = s_context->rtcmPipe.stats().writtenBytes;
    cutoffCmd.args[0] = 15U;
    rtos::Ticket tTimeout{};
    CHECK(fw::isOk(executor.post(cutoffCmd, rtos::Caller::kWeb, tTimeout)));
    rtos::ExecutorTestAccess::drain(executor);
    const std::string rtcm400 = makeRtcm3Frame(394U);
    for (unsigned step = 0U; step < 75U; ++step) {
        if (step < 14U) {
            CHECK(s_context->rtcmPipe.push(std::span<const uint8_t>{
                reinterpret_cast<const uint8_t *>(rtcm400.data()), rtcm400.size()}));
        }
        advance(20U);
    }
    s_f9pDropSatAck = false;
    CHECK(executor.poll(tTimeout, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kTimeout);
    CHECK(s_context->rtcmPipe.stats().droppedBytes == droppedBefore);
    CHECK((s_context->rtcmPipe.stats().writtenBytes - writtenBefore) >= 5600U);
}

/** @brief 验证遗留 gps=0 NVS blob 在重放失败后，6 字节 gps=0 仍拒但 1/2 字节补丁归一化 GPS=1 恢复（NRU1-02）。 */
static void verifyF9pLegacyGps0FailedReplay(nvs_handle_t nvs, std::span<uint8_t> demux,
                                            std::span<uint8_t> pipe)
{
    const core::SatTrackConfig legacyBlob{
        .cutoffDeg = 12U, .gps = 0U, .glo = 1U, .bds = 1U, .gal = 1U, .sbas = 0U};
    const auto encodedLegacy = core::encodeSatTrackConfig(legacyBlob);
    CHECK(app::store::writeBlob(app::store::kPhoneSatCfgKey, encodedLegacy) == ESP_OK);
    // 启动期间丢弃卫星 VALSET ACK，使冷启重放超时失败，gnssSatTrack 保持 gps=0。
    s_f9pMode = true;
    s_f9pFw = "HPG 1.32";
    s_f9pDropSatAck = true;
    s_f9pRecordTx = false;
    s_f9pFailNextWrite = false;
    s_f9pSatValsets = 0U;
    s_f9pTxLog.clear();
    s_lastF9pSatValsetLen = 0U;
    s_rx.clear();
    s_readyAt = 0U;
    s_receiverBaud = 115200U;
    s_p2 = true;
    s_context = &s_f9pContexts[2U];
    s_context->bindContexts();
    s_context->data.gnssLatestGsv.bindLock({});
    CHECK(fw::isOk(s_context->gnss.bindStorage(demux)));
    CHECK(fw::isOk(s_context->rtcmPipe.bindStorage(pipe)));
    CHECK(nvs_set_u32(nvs, "baud", 115200U) == ESP_OK);
    CHECK(nvs_set_u32(nvs, "cache_baud", 115200U) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "family", static_cast<uint8_t>(gnss::Family::kUblox)) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "model", static_cast<uint8_t>(gnss::Model::kZedF9p)) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "rate", 1U) == ESP_OK);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kConfigKeys).failed == 0U);
    CHECK(s_context->storage.loadKeys(app::gnss_store::kCacheKeys).failed == 0U);
    s_context->bootConfig.gnssBaud = 115200U;
    s_context->bootConfig.userNvsReady = true;
    s_context->boardProfile = app::board::byId(app::board::BoardId::kP2V141);
    s_context->data.gnssSatTrack.write(legacyBlob);
    CHECK(fw::isOk(s_context->gnss.init()));
    CHECK(fw::isOk(s_context->gnss.start()));
    advance(4500U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_f9pSatValsets >= 1U);
    // 重放失败后 RAM 快照仍为 gps=0。
    CHECK(s_context->data.gnssSatTrack.read()->gps == 0U);
    s_f9pDropSatAck = false;

    rtos::Executor executor{};
    app::ExecutorControlStorage<4U> control{};
    std::array<StackType_t, 256U> stack{};
    const auto execId = app::id(app::ExecKind::kIo);
    const auto target = app::id(app::ModuleKind::kGnss);
    fw::ModuleOps ops{};
    ops.onCommand = [](void *self, const fw::Command &cmd, fw::CommandResult &out) noexcept {
        static_cast<app::GnssModule *>(self)->onCommand(cmd, out);
    };
    const std::array modules{fw::Module{
        .name = "gnss", .id = target, .executor = execId, .self = &s_context->gnss, .ops = &ops}};
    CHECK(fw::isOk(executor.init({.name = "io", .id = execId, .priority = 9U}, modules,
                                 app::viewOf(control, stack))));
    s_context->ioExecutor = &executor;
    rtos::ExecutorTestAccess::bind(executor, &control.tcb);
    auto postCmd = [&](const fw::Command &cmd) {
        rtos::Ticket t{};
        CHECK(fw::isOk(executor.post(cmd, rtos::Caller::kWeb, t)));
        rtos::ExecutorTestAccess::drain(executor);
        return t;
    };
    fw::CommandResult res{};

    // 1. 6 字节全量请求含 gps=0 仍必须被拒（kInvalidArg）。
    fw::Command fullGps0{.target = target,
                         .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack),
                         .argLen = static_cast<uint8_t>(encodedLegacy.size())};
    std::memcpy(fullGps0.args.data(), encodedLegacy.data(), encodedLegacy.size());
    CHECK(executor.poll(postCmd(fullGps0), res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kInvalidArg);

    // 2. 1 字节 cutoff 补丁在 gps=0 遗留态下归一化 gps=1 并成功落盘，不得锁死网页配置。
    fw::Command cutoffCmd{.target = target,
                          .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack),
                          .argLen = 1U};
    cutoffCmd.args[0] = 22U;
    const auto tCutoff = postCmd(cutoffCmd);
    advance(700U);
    CHECK(executor.poll(tCutoff, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kOk);
    const auto afterCutoff = s_context->data.gnssSatTrack.read();
    CHECK(afterCutoff.has_value() && (afterCutoff->cutoffDeg == 22U) && (afterCutoff->gps == 1U));

    // 3. 2 字节非 GPS 星座补丁在 gps=0 遗留态下同样归一化 gps=1 并成功落盘。
    s_context->data.gnssSatTrack.write(legacyBlob);
    fw::Command bdsPatch{.target = target,
                         .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack),
                         .argLen = 2U};
    bdsPatch.args[0] = settings::const_bit::kBds;
    bdsPatch.args[1] = 0U;
    const auto tBds = postCmd(bdsPatch);
    advance(700U);
    CHECK(executor.poll(tBds, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kOk);
    const auto afterBds = s_context->data.gnssSatTrack.read();
    CHECK(afterBds.has_value() && (afterBds->bds == 0U) && (afterBds->gps == 1U));

    rtos::ExecutorTestAccess::bind(executor, nullptr);
    s_context->ioExecutor = nullptr;
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
}

/** @brief 验证拆包 RTCM3 帧未到帧尾时控制器写操作必须延后至帧尾或 200 ms stall（NRU1-03）。 */
static void verifyF9pControllerWriteRtcmSplitFrame(rtos::Executor &executor)
{
    const auto target = app::id(app::ModuleKind::kGnss);
    const std::string rtcm200 = makeRtcm3Frame(194U);
    CHECK(rtcm200.size() == 200U);
    s_f9pTxLog.clear();
    s_f9pRecordTx = true;

    // 先仅推送前 80 B（RTCM3 帧拆包，后 120 B 尚未到达）。
    CHECK(s_context->rtcmPipe.push(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(rtcm200.data()), 80U}));
    static_cast<void>(s_context->gnss.poll(s_now));
    CHECK(s_f9pTxLog.size() == 80U);

    // 发起会触发控制器写（kConfiguring VALSET）的速率切换，帧尾未到前禁止插写 UBX。
    fw::Command rateCmd{
        .target = target, .verb = static_cast<uint8_t>(app::GnssVerb::kSetRate), .argLen = 1U};
    rateCmd.args[0] = 5U;
    rtos::Ticket tRate{};
    CHECK(fw::isOk(executor.post(rateCmd, rtos::Caller::kWeb, tRate)));
    rtos::ExecutorTestAccess::drain(executor);
    advance(60U);
    CHECK(s_f9pTxLog.size() == 80U);

    // 推送剩余 120 B 完成该 RTCM3 帧：控制器 UBX 写必须在精确偏移 200 处开始。
    CHECK(s_context->rtcmPipe.push(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(rtcm200.data()) + 80U, 120U}));
    advance(600U);
    fw::CommandResult res{};
    CHECK(executor.poll(tRate, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kOk);
    CHECK(s_f9pTxLog.size() > 202U);
    CHECK(std::memcmp(s_f9pTxLog.data(), rtcm200.data(), 200U) == 0);
    CHECK((static_cast<uint8_t>(s_f9pTxLog[200U]) == 0xB5U) &&
          (static_cast<uint8_t>(s_f9pTxLog[201U]) == 0x62U));
    // 等待重配后触发的 sat_replay_ 稳定期结束，避免下一条 kSetRate 命中 kBusy。
    advance(700U);

    // 验证 200 ms stall 兜底：半帧后永不补齐后半段，200 ms 内不插写，超过 200 ms 自动推进不死锁。
    s_f9pTxLog.clear();
    CHECK(s_context->rtcmPipe.push(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t *>(rtcm200.data()), 40U}));
    static_cast<void>(s_context->gnss.poll(s_now));
    CHECK(s_f9pTxLog.size() == 40U);
    rateCmd.args[0] = 1U;
    rtos::Ticket tStall{};
    CHECK(fw::isOk(executor.post(rateCmd, rtos::Caller::kWeb, tStall)));
    rtos::ExecutorTestAccess::drain(executor);
    advance(100U);
    CHECK(s_f9pTxLog.size() == 40U);
    advance(800U);
    CHECK(executor.poll(tStall, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kOk);
    CHECK((s_f9pTxLog.size() > 42U) && (static_cast<uint8_t>(s_f9pTxLog[40U]) == 0xB5U) &&
          (static_cast<uint8_t>(s_f9pTxLog[41U]) == 0x62U));
    s_f9pRecordTx = false;
    advance(700U);
}

/** @brief 验证 writeSeqTx 串口写失败时立即清理 ValsetTracker，不触发 1 s 假超时与 300 ms resync 阻塞（NRU1-04）。 */
static void verifyF9pSeqWriteFailClearsTracker(rtos::Executor &executor)
{
    const auto target = app::id(app::ModuleKind::kGnss);
    fw::Command cutoffCmd{.target = target,
                          .verb = static_cast<uint8_t>(app::GnssSubVerb::kApplySatTrack),
                          .argLen = 1U};
    cutoffCmd.args[0] = 18U;
    s_f9pFailNextWrite = true;
    rtos::Ticket tFail{};
    CHECK(fw::isOk(executor.post(cutoffCmd, rtos::Caller::kWeb, tFail)));
    rtos::ExecutorTestAccess::drain(executor);
    advance(40U);
    fw::CommandResult res{};
    CHECK(executor.poll(tFail, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kIoError);

    // 越过 1000 ms 进入若残留 in_flight_ 会触发的 300 ms resync 窗口，新请求仍必须立刻被受理。
    advance(1050U);
    cutoffCmd.args[0] = 19U;
    rtos::Ticket tNext{};
    CHECK(fw::isOk(executor.post(cutoffCmd, rtos::Caller::kWeb, tNext)));
    rtos::ExecutorTestAccess::drain(executor);
    CHECK(executor.poll(tNext, res) == rtos::PollState::kPending);
    advance(700U);
    CHECK(executor.poll(tNext, res) == rtos::PollState::kDone);
    CHECK(res.status == fw::Result::kOk);
    CHECK(s_context->data.gnssSatTrack.read()->cutoffDeg == 19U);
}

/** @brief 捕获 kGnss 类 E 级日志中含“rate compensation failed”的条数（MT1 补偿写入失败分支观测）。 */
static unsigned s_rateCompFailLogs = 0U;

/**
 * @brief 直写日志 sink：统计 GNSS 改频补偿写入失败日志。
 * @param hdr: 记录头。
 * @param payload: 文本正文。
 * @note 单线程主机测试。
 */
static void captureGnssLog(void * /*ctx*/, const fw::LogHeader &hdr,
                           std::span<const uint8_t> payload) noexcept
{
    const std::string_view text{reinterpret_cast<const char *>(payload.data()), payload.size()};
    if (hdr.cat == fw::TraceCat::kGnss && hdr.level == fw::LogLevel::kError &&
        text.find("rate compensation failed") != std::string_view::npos) {
        ++s_rateCompFailLogs;
    }
}

/** @brief 步骤 5c 留下的 NVS 残留（新 rate + 旧 out），供步骤 6b 冷启动自愈验证。 */
static std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> s_mixedResidueOut{};
static uint8_t s_mixedResidueRate = 0U;

/** @brief 读取 NVS gnss/out 原始 blob。 @param nvs: 句柄。 @param[out] blob: 输出。 */
static void readOutBlob(nvs_handle_t nvs,
                        std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> &blob)
{
    size_t len = blob.size();
    CHECK(nvs_get_blob(nvs, "out", blob.data(), &len) == ESP_OK && len == blob.size());
}

/** @brief U2 端到端回归：F9P caps、gnss.out.gsv=0、1->5 Hz 500 ms 周期对齐持久化、频率回滚恢复 CFG-RATE-MEAS、改频不重放星座与 HPG 1.12 高精度 NAK 降级。 */
static void verifyF9pRateOutputAndRollback(nvs_handle_t nvs, std::span<uint8_t> demux,
                                           std::span<uint8_t> pipe)
{
    // 预置默认 gnss/out 与 1 Hz，启动 F9P（slot 3）。
    auto initOut = app::data::GnssOutConfig::defaults();
    std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> blob{};
    size_t blobLen = 0U;
    CHECK(fw::isOk(initOut.encode(blob, blobLen)));
    CHECK(nvs_set_blob(nvs, "out", blob.data(), blobLen) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "gsv", 1U) == ESP_OK);
    CHECK(nvs_commit(nvs) == ESP_OK);

    bootF9pModule(3U, nvs, demux, pipe, "HPG 1.32", std::nullopt, 1U, false);
    // 1. caps 含 gnss.rate={1,2,5} 与 5 条 gnss.out.*（1 Hz 下 GGA/GST/RMC={0,1000}, GSA={0,1000}, GSV={0,1000,5000}）。
    const auto caps1 = s_context->data.gnssCaps.read();
    CHECK(caps1.valid && caps1.navRateHz == 1U && caps1.navRateCount == 3U);
    CHECK(caps1.navRatesHz[0] == 1U && caps1.navRatesHz[1] == 2U && caps1.navRatesHz[2] == 5U);
    constexpr auto kDomGga = static_cast<size_t>(settings::OutKeyIndex::kGga);
    constexpr auto kDomGsa = static_cast<size_t>(settings::OutKeyIndex::kGsa);
    constexpr auto kDomGsv = static_cast<size_t>(settings::OutKeyIndex::kGsv);
    CHECK(caps1.outPeriods[kDomGga].supported && caps1.outPeriods[kDomGga].count == 2U &&
          caps1.outPeriods[kDomGga].values[0] == 0U &&
          caps1.outPeriods[kDomGga].values[1] == 1000U);
    CHECK(caps1.outPeriods[kDomGsa].supported && caps1.outPeriods[kDomGsa].count == 2U &&
          caps1.outPeriods[kDomGsa].values[1] == 1000U);
    CHECK(caps1.outPeriods[kDomGsv].supported && caps1.outPeriods[kDomGsv].count == 3U &&
          caps1.outPeriods[kDomGsv].values[1] == 1000U &&
          caps1.outPeriods[kDomGsv].values[2] == 5000U);

    rtos::Executor executor{};
    app::ExecutorControlStorage<4U> control{};
    std::array<StackType_t, 256U> stack{};
    const auto execId = app::id(app::ExecKind::kIo);
    const auto target = app::id(app::ModuleKind::kGnss);
    fw::ModuleOps ops{};
    ops.onCommand = [](void *self, const fw::Command &cmd, fw::CommandResult &out) noexcept {
        static_cast<app::GnssModule *>(self)->onCommand(cmd, out);
    };
    const std::array modules{fw::Module{
        .name = "gnss", .id = target, .executor = execId, .self = &s_context->gnss, .ops = &ops}};
    CHECK(fw::isOk(executor.init({.name = "io", .id = execId, .priority = 9U}, modules,
                                 app::viewOf(control, stack))));
    s_context->ioExecutor = &executor;
    rtos::ExecutorTestAccess::bind(executor, &control.tcb);
    app::bindSettings(*s_context);

    auto runSetting = [&](std::string_view key, std::string_view val, settings::Caller caller,
                          uint32_t waitMs) -> fw::Result {
        settings::Ticket t{};
        const auto err = s_context->settings.set(key, val, caller, t);
        CHECK(err == settings::Error::kOk);
        rtos::ExecutorTestAccess::drain(executor);
        advance(waitMs);
        fw::CommandResult r{};
        CHECK(s_context->settings.poll(t, r) == settings::PollState::kDone);
        return r.status;
    };

    // 2. config set gnss.out.gsv 0 成功并下发 VALSET（0x209100C5=0），再恢复为 5000 ms。
    settings::Value got{};
    CHECK(s_context->settings.get("gnss.out.gsv", got) && got.number == 5000);
    const unsigned gsvWritesBefore = s_f9pGsvWrites;
    CHECK(fw::isOk(runSetting("gnss.out.gsv", "0", settings::Caller::kConsole, 600U)));
    CHECK(s_f9pGsvWrites > gsvWritesBefore && s_f9pLastGsvEpochs == 0U);
    CHECK(s_context->settings.get("gnss.out.gsv", got) && got.number == 0);
    CHECK(fw::isOk(runSetting("gnss.out.gsv", "5000", settings::Caller::kConsole, 600U)));
    CHECK(s_f9pLastGsvEpochs == 5U);

    // 3. 记录调频前星座重放次数；切到 2 Hz 设 GGA=500 ms，再切 5 Hz -> 500 ms 对齐为 1000 ms 并持久化，不触发星座 VALSET。
    const unsigned satBefore = s_f9pSatValsets;
    CHECK(fw::isOk(runSetting("gnss.rate", "2", settings::Caller::kConsole, 800U)));
    CHECK(s_f9pLastRateMeasMs == 500U);
    CHECK(s_f9pSatValsets == satBefore);
    CHECK(fw::isOk(runSetting("gnss.out.gga", "500", settings::Caller::kWeb, 600U)));
    CHECK(s_f9pLastGgaEpochs == 1U);
    CHECK(s_context->settings.get("gnss.out.gga", got) && got.number == 500);

    CHECK(fw::isOk(runSetting("gnss.rate", "5", settings::Caller::kConsole, 800U)));
    CHECK(s_f9pLastRateMeasMs == 200U);
    CHECK(s_f9pLastGgaEpochs == 5U);  // 1000 ms @ 5 Hz -> epochs=5
    CHECK(s_f9pLastGsvEpochs == 25U); // 5000 ms @ 5 Hz -> epochs=25
    CHECK(s_f9pSatValsets == satBefore);
    const auto caps5 = s_context->data.gnssCaps.read();
    CHECK(caps5.navRateHz == 5U && caps5.outPeriods[kDomGga].count == 3U &&
          caps5.outPeriods[kDomGga].values[0] == 0U &&
          caps5.outPeriods[kDomGga].values[1] == 200U &&
          caps5.outPeriods[kDomGga].values[2] == 1000U);
    CHECK(s_context->settings.get("gnss.out.gga", got) && got.number == 1000);
    // NVS gnss/out 已持久化对齐后的 1000 ms（interval=-1）。
    std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> nvsBlob{};
    size_t nvsLen = nvsBlob.size();
    CHECK(nvs_get_blob(nvs, "out", nvsBlob.data(), &nvsLen) == ESP_OK);
    app::data::GnssOutConfig persistedOut{};
    CHECK(fw::isOk(app::data::GnssOutConfig::decode(nvsBlob, persistedOut)));
    CHECK(persistedOut.intervals[static_cast<size_t>(gnss::OutputMsg::kGga)] == -1);
    // 5 Hz 下拒绝 500 ms（settings 服务与直接命令入口双门控）。
    settings::Ticket badTicket{};
    CHECK(s_context->settings.set("gnss.out.gga", "500", settings::Caller::kWeb, badTicket) ==
          settings::Error::kBadValue);
    fw::Command direct500{.target = target,
                          .verb = static_cast<uint8_t>(app::GnssSubVerb::kPatchOutput),
                          .argLen = 2U};
    direct500.args[0] = static_cast<uint8_t>(gnss::OutputMsg::kGga);
    direct500.args[1] = 2U; // 500 ms
    rtos::Ticket tDirect{};
    CHECK(fw::isOk(executor.post(direct500, rtos::Caller::kConsole, tDirect)));
    rtos::ExecutorTestAccess::drain(executor);
    fw::CommandResult directRes{};
    CHECK(executor.poll(tDirect, directRes) == rtos::PollState::kDone);
    CHECK(directRes.status == fw::Result::kInvalidArg);

    // 4. 1 Hz 含显式 500 ms（经 gnss setout 写入）切 5 Hz -> 500 ms 对齐为 1000 ms 并持久化。
    CHECK(fw::isOk(runSetting("gnss.rate", "1", settings::Caller::kConsole, 800U)));
    CHECK(s_f9pLastRateMeasMs == 1000U);
    auto out500 = s_context->data.gnssOutCfg.read();
    CHECK(fw::isOk(out500.applyInterval(static_cast<size_t>(gnss::OutputMsg::kRmc), 2))); // 500 ms
    fw::Command setOut500{.target = target,
                          .verb = static_cast<uint8_t>(app::GnssSubVerb::kSetOut)};
    size_t setOutLen = 0U;
    CHECK(fw::isOk(out500.encode(setOut500.args, setOutLen)));
    setOut500.argLen = static_cast<uint8_t>(setOutLen);
    rtos::Ticket tSetOut{};
    CHECK(fw::isOk(executor.post(setOut500, rtos::Caller::kConsole, tSetOut)));
    rtos::ExecutorTestAccess::drain(executor);
    advance(800U);
    CHECK(fw::isOk(runSetting("gnss.rate", "5", settings::Caller::kConsole, 800U)));
    nvsLen = nvsBlob.size();
    CHECK(nvs_get_blob(nvs, "out", nvsBlob.data(), &nvsLen) == ESP_OK);
    CHECK(fw::isOk(app::data::GnssOutConfig::decode(nvsBlob, persistedOut)));
    CHECK(persistedOut.intervals[static_cast<size_t>(gnss::OutputMsg::kRmc)] == -1);
    CHECK(s_f9pSatValsets == satBefore);

    // 5. 频率变更失败回滚：5 Hz -> 1 Hz 时 CFG-RATE-MEAS=1000 已 ACK，但 GGA MSGOUT NAK 触发回滚，
    //    必须重发 CFG-RATE-MEAS=200 恢复旧频率且不触发星座重放。
    s_f9pNakGgaMsgout = true;
    settings::Ticket rollbackTicket{};
    CHECK(s_context->settings.set("gnss.rate", "1", settings::Caller::kConsole, rollbackTicket) ==
          settings::Error::kOk);
    rtos::ExecutorTestAccess::drain(executor);
    advance(400U);
    CHECK(s_f9pLastRateMeasMs == 1000U); // 新频率已写入接收机
    // 等待 GGA NAK 重试耗尽进入回滚；放开 GGA NAK 让回滚重配顺利完成。
    advance(2500U);
    fw::CommandResult rbRes{};
    CHECK(s_context->settings.poll(rollbackTicket, rbRes) == settings::PollState::kDone);
    CHECK(!fw::isOk(rbRes.status));
    s_f9pNakGgaMsgout = false;
    advance(1200U);
    CHECK(s_f9pLastRateMeasMs == 200U); // 回滚已恢复旧 CFG-RATE-MEAS=200
    uint8_t nvsRate = 0U;
    CHECK(nvs_get_u8(nvs, "rate", &nvsRate) == ESP_OK && nvsRate == 5U);
    CHECK(s_f9pSatValsets == satBefore);

    // 5b. MT1 RU2-02：运行中改频（两阶段）时 out 键写失败，NVS rate 必须补偿写回旧值，不留“新 rate + 旧 out”。
    CHECK(fw::isOk(runSetting("gnss.rate", "1", settings::Caller::kConsole, 800U)));
    auto outMixed = s_context->data.gnssOutCfg.read();
    CHECK(
        fw::isOk(outMixed.applyInterval(static_cast<size_t>(gnss::OutputMsg::kRmc), 2))); // 500 ms
    fw::Command setOutMixed{.target = target,
                            .verb = static_cast<uint8_t>(app::GnssSubVerb::kSetOut)};
    size_t setOutMixedLen = 0U;
    CHECK(fw::isOk(outMixed.encode(setOutMixed.args, setOutMixedLen)));
    setOutMixed.argLen = static_cast<uint8_t>(setOutMixedLen);
    rtos::Ticket tSetOutMixed{};
    CHECK(fw::isOk(executor.post(setOutMixed, rtos::Caller::kConsole, tSetOutMixed)));
    rtos::ExecutorTestAccess::drain(executor);
    advance(800U);
    std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> oldOutBlob{};
    readOutBlob(nvs, oldOutBlob);
    CHECK(nvs_get_u8(nvs, "rate", &nvsRate) == ESP_OK && nvsRate == 1U);
    const auto oldOutRam = s_context->data.gnssOutCfg.read();
    const unsigned compLogsBefore = s_rateCompFailLogs;
    const uint32_t applySeqBefore = s_context->data.gnssStatus.read().applySeq;

    nvs_shim_add_key_fail("out", ESP_FAIL, 0U);
    CHECK(!fw::isOk(runSetting("gnss.rate", "5", settings::Caller::kConsole, 800U)));
    nvs_shim_clear_key_fail();
    advance(1200U); // 回滚重配 CFG-RATE-MEAS
    const auto afterFail = s_context->data.gnssStatus.read();
    CHECK(afterFail.lastApply == app::GnssApply::kRestored && afterFail.applySeq != applySeqBefore);
    CHECK(s_context->settings.get("gnss.rate", got) && got.number == 1);
    CHECK(s_context->data.gnssOutCfg.read().intervals == oldOutRam.intervals);
    CHECK(nvs_get_u8(nvs, "rate", &nvsRate) == ESP_OK && nvsRate == 1U); // 补偿写回旧 rate
    std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> nowOutBlob{};
    readOutBlob(nvs, nowOutBlob);
    CHECK(nowOutBlob == oldOutBlob);
    CHECK(s_rateCompFailLogs == compLogsBefore); // 补偿成功不记 E
    CHECK(s_f9pLastRateMeasMs == 1000U);

    // 5c. 补偿写入也失败：返回原始 out 错误，记 E 日志，NVS 残留新 rate（由下次启动对齐自愈，见步骤 6/6b）。
    nvs_shim_add_key_fail("out", ESP_FAIL, 0U);
    nvs_shim_add_key_fail("rate", ESP_FAIL, 1U); // 第一次（新 rate）成功，补偿写入失败
    CHECK(!fw::isOk(runSetting("gnss.rate", "5", settings::Caller::kConsole, 800U)));
    nvs_shim_clear_key_fail();
    advance(1200U);
    CHECK(s_rateCompFailLogs == compLogsBefore + 1U);
    CHECK(s_context->data.gnssStatus.read().lastApply == app::GnssApply::kRestored);
    CHECK(s_context->settings.get("gnss.rate", got) && got.number == 1); // RAM 仍回滚
    CHECK(s_context->data.gnssOutCfg.read().intervals == oldOutRam.intervals);
    CHECK(nvs_get_u8(nvs, "rate", &nvsRate) == ESP_OK && nvsRate == 5U); // 残留：新 rate + 旧 out
    readOutBlob(nvs, nowOutBlob);
    CHECK(nowOutBlob == oldOutBlob);
    s_mixedResidueOut = oldOutBlob;
    s_mixedResidueRate = nvsRate;

    rtos::ExecutorTestAccess::bind(executor, nullptr);
    s_context->ioExecutor = nullptr;
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 6. 冷启动 restoreOutputPeriods：NVS 预存 rate=5 + GGA=500 ms -> 启动对齐为 1000 ms 并写回 NVS。
    auto badBootOut = app::data::GnssOutConfig::defaults();
    CHECK(fw::isOk(badBootOut.applyInterval(static_cast<size_t>(gnss::OutputMsg::kGga), 2)));
    CHECK(fw::isOk(badBootOut.encode(blob, blobLen)));
    CHECK(nvs_set_blob(nvs, "out", blob.data(), blobLen) == ESP_OK);
    CHECK(nvs_commit(nvs) == ESP_OK);
    bootF9pModule(4U, nvs, demux, pipe, "HPG 1.32", std::nullopt, 5U, false);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(
        s_context->data.gnssStatus.read().rxPeriodMs[static_cast<size_t>(gnss::OutputMsg::kGga)] ==
        1000U);
    nvsLen = nvsBlob.size();
    CHECK(nvs_get_blob(nvs, "out", nvsBlob.data(), &nvsLen) == ESP_OK);
    CHECK(fw::isOk(app::data::GnssOutConfig::decode(nvsBlob, persistedOut)));
    CHECK(persistedOut.intervals[static_cast<size_t>(gnss::OutputMsg::kGga)] == -1);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 6b. MT1：步骤 5c 的真实残留（rate=5 + RMC=500 ms）冷启动同样被 restoreOutputPeriods 对齐并写回 out。
    CHECK(s_mixedResidueRate == 5U);
    CHECK(nvs_set_blob(nvs, "out", s_mixedResidueOut.data(), s_mixedResidueOut.size()) == ESP_OK);
    CHECK(nvs_commit(nvs) == ESP_OK);
    bootF9pModule(6U, nvs, demux, pipe, "HPG 1.32", std::nullopt, s_mixedResidueRate, false);
    CHECK(
        s_context->data.gnssStatus.read().rxPeriodMs[static_cast<size_t>(gnss::OutputMsg::kRmc)] ==
        1000U);
    nvsLen = nvsBlob.size();
    CHECK(nvs_get_blob(nvs, "out", nvsBlob.data(), &nvsLen) == ESP_OK);
    CHECK(fw::isOk(app::data::GnssOutConfig::decode(nvsBlob, persistedOut)));
    CHECK(persistedOut.intervals[static_cast<size_t>(gnss::OutputMsg::kRmc)] == -1);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 7. HPG 1.12 NMEA 高精度 VALSET NAK 降级：非致命回退至 3 键基础配置，正常进入 kRunning。
    bootF9pModule(5U, nvs, demux, pipe, "HPG 1.12", std::nullopt, 1U, true);
    CHECK(s_f9pHighPrecNaks >= 1U);
    CHECK(s_f9pLastRateMeasMs == 1000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_context->data.gnssCaps.read().valid);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
}

/**
 * @brief MT1 RU2-02：非运行态（kPowerUp，型号已被探测清空）直接落盘路径：rate 单键保存，out 不被改写；
 *        rate 写失败时 RAM 与 NVS 都回到旧值。F9P 型号在非运行态不可达（start/requestReprobe 清空 detected_），
 *        out 联动与补偿由运行态两阶段路径覆盖（见 verifyF9pRateOutputAndRollback 5b/5c）。
 * @param nvs: 测试用 NVS 句柄。
 * @param demux: 分流器缓冲。
 * @param pipe: 管道缓冲。
 */
static void verifyNonRunningRateSave(nvs_handle_t nvs, std::span<uint8_t> demux,
                                     std::span<uint8_t> pipe)
{
    auto initOut = app::data::GnssOutConfig::defaults();
    CHECK(fw::isOk(initOut.applyInterval(static_cast<size_t>(gnss::OutputMsg::kRmc), 2))); // 500 ms
    std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> oldBlob{};
    size_t blobLen = 0U;
    CHECK(fw::isOk(initOut.encode(oldBlob, blobLen)));
    CHECK(nvs_set_blob(nvs, "out", oldBlob.data(), blobLen) == ESP_OK);
    CHECK(nvs_set_u8(nvs, "rate", 1U) == ESP_OK);
    CHECK(nvs_commit(nvs) == ESP_OK);
    bootF9pModule(7U, nvs, demux, pipe, "HPG 1.32", std::nullopt, 1U, false);

    readOutBlob(nvs, oldBlob); // 启动自愈可能已对齐并落盘 out，以启动后的 NVS 为基线
    // kReset 后控制器重新上电：state 为 kPowerUp（非运行、非瞬态），型号已清空。
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kReset)).status));
    const auto st = s_context->data.gnssStatus.read().state;
    CHECK(st != gnss::State::kRunning && st != gnss::State::kMaintenance);
    const std::array<uint8_t, 1U> rate5{5U};
    uint8_t nvsRate = 0U;
    std::array<uint8_t, app::data::GnssOutConfig::kBlobBytes> nowBlob{};

    nvs_shim_add_key_fail("rate", ESP_FAIL, 0U);
    CHECK(!fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kSetRate), rate5).status));
    nvs_shim_clear_key_fail();
    CHECK(nvs_get_u8(nvs, "rate", &nvsRate) == ESP_OK && nvsRate == 1U);
    readOutBlob(nvs, nowBlob);
    CHECK(nowBlob == oldBlob);

    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kSetRate), rate5).status));
    CHECK(nvs_get_u8(nvs, "rate", &nvsRate) == ESP_OK && nvsRate == 5U);
    readOutBlob(nvs, nowBlob);
    CHECK(nowBlob == oldBlob); // 型号未知时不对齐 out，NVS out 保持不变
}

/** @brief F9P 端到端主机回归：覆盖 RU1-02/03/04/06/07/08（能力、强制 GPS 重放、稳定期、回滚、RTCM 帧边界与零溢出）。 */
void testGnssF9pEndToEnd()
{
    std::array<uint8_t, app::GnssModule::kDemuxBufBytes> demux{};
    std::array<uint8_t, app::kRtcmPipeBytes> pipe{};
    nvs_handle_t nvs = 0U;
    CHECK(nvs_open("gnss", NVS_READWRITE, &nvs) == ESP_OK);
    s_nvs = nvs;
    // 进程内只能安装一次直写 sink；用于观测改频补偿写入失败的 E 级日志。
    CHECK(fw::isOk(fw::logInstallDirect(&captureGnssLog, nullptr, []() noexcept { return 0U; })));

    // 场景 1：HPG 1.12 预存旧 gps=0 blob -> 能力隐藏 GPS 与 SBAS，重放强制 GPS=1 且不发 SBAS 键
    const core::SatTrackConfig legacyBlob{
        .cutoffDeg = 12U, .gps = 0U, .glo = 1U, .bds = 1U, .gal = 1U, .sbas = 1U};
    const auto encodedLegacy = core::encodeSatTrackConfig(legacyBlob);
    CHECK(app::store::writeBlob(app::store::kPhoneSatCfgKey, encodedLegacy) == ESP_OK);
    bootF9pModule(0U, nvs, demux, pipe, "HPG 1.12", legacyBlob);
    const auto caps112 = s_context->data.gnssCaps.read();
    CHECK(caps112.valid && ((caps112.constMask & settings::const_bit::kGps) == 0U));
    CHECK((caps112.constMask & settings::const_bit::kSbas) == 0U);
    CHECK((caps112.constMask & (1U << 7U)) != 0U);
    CHECK(s_f9pSatValsets >= 1U);
    std::array<uint32_t, gnss::ublox::kMaxExtractedValsetKeys> replayKeys{};
    CHECK(gnss::ublox::extractValsetKeys(std::span{s_lastF9pSatValset}.first(s_lastF9pSatValsetLen),
                                         replayKeys) == 8U);
    CHECK(s_context->data.gnssSatTrack.read()->gps == 1U);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));

    // 场景 2：HPG 1.32 暴露 SBAS，验证稳定期/连发/回滚/RTCM 帧边界/超时满载排空
    bootF9pModule(1U, nvs, demux, pipe, "HPG 1.32");
    const auto caps132 = s_context->data.gnssCaps.read();
    CHECK((caps132.constMask & settings::const_bit::kSbas) != 0U);
    rtos::Executor executor{};
    app::ExecutorControlStorage<4U> control{};
    std::array<StackType_t, 256U> stack{};
    const auto execId = app::id(app::ExecKind::kIo);
    const auto target = app::id(app::ModuleKind::kGnss);
    fw::ModuleOps ops{};
    ops.onCommand = [](void *self, const fw::Command &cmd, fw::CommandResult &out) noexcept {
        static_cast<app::GnssModule *>(self)->onCommand(cmd, out);
    };
    const std::array modules{fw::Module{
        .name = "gnss", .id = target, .executor = execId, .self = &s_context->gnss, .ops = &ops}};
    CHECK(fw::isOk(executor.init({.name = "io", .id = execId, .priority = 9U}, modules,
                                 app::viewOf(control, stack))));
    s_context->ioExecutor = &executor;
    rtos::ExecutorTestAccess::bind(executor, &control.tcb);
    verifyF9pSettleBusyAndRollback(executor);
    verifyF9pRtcmFramingAndDrain(executor);
    verifyF9pControllerWriteRtcmSplitFrame(executor);
    verifyF9pSeqWriteFailClearsTracker(executor);
    rtos::ExecutorTestAccess::bind(executor, nullptr);
    s_context->ioExecutor = nullptr;
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    verifyF9pLegacyGps0FailedReplay(nvs, demux, pipe);
    verifyF9pRateOutputAndRollback(nvs, demux, pipe);
    verifyNonRunningRateSave(nvs, demux, pipe);
    s_f9pMode = false;
    nvs_close(nvs);
}
