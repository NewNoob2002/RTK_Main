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
#include "board/BoardProfile.hpp"
#include "def/GnssSubDef.hpp"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "fw/Trace.hpp"
#include "modules/gnss/GnssCmdPack.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "rtos/EspStatus.hpp"

extern "C" {
#include "nvs.h"
}

namespace {

constinit app::AppContext s_initialContext{};
constinit app::AppContext s_failureContext{};
constinit app::AppContext s_dispatchContext{};
constinit app::AppContext s_pppContext{};
constinit app::AppContext s_dispatchCmdContext{};
constinit std::array<app::AppContext, 13U> s_targetBaudContexts{};
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
    advance(500U);
    uint32_t stored = 0U;
    CHECK(nvs_get_u32(nvs, "baud", &stored) == ESP_OK && stored == 460800U);
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

/** @brief 用真实 kIo 模块验证 PPP 成功提交、失败保留 NVS 与重探重放。 @note 主机单线程。 */
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

    fw::Command request{};
    request.verb = static_cast<uint8_t>(app::GnssSubVerb::kPhonePpp);
    request.argLen = 1U;
    request.seq = 1U;
    request.args[0] = 1U;
    s_context->data.phonePppCommand.write(request);
    advance(20U);
    CHECK(s_context->data.phonePppResult.read().seq == 0U);
    advance(200U);
    const auto enabled = s_context->data.phonePppResult.read();
    CHECK(enabled.seq == request.seq && enabled.status == fw::Result::kOk);
    CHECK(s_context->data.phonePppEnabled.read() == true);
    uint32_t stored = 0U;
    CHECK(app::store::readValue(app::store::kPhonePppKey, stored) == ESP_OK && stored == 1U);

    s_rejectSave = true;
    request.seq = 2U;
    request.args[0] = 0U;
    s_context->data.phonePppCommand.write(request);
    advance(200U);
    s_rejectSave = false;
    const auto rejected = s_context->data.phonePppResult.read();
    CHECK(rejected.seq == request.seq && rejected.status == fw::Result::kIoError);
    CHECK(s_context->data.phonePppEnabled.read() == true);
    CHECK(app::store::readValue(app::store::kPhonePppKey, stored) == ESP_OK && stored == 1U);

    const auto saves = s_saveCommands;
    CHECK(fw::isOk(command(static_cast<uint8_t>(app::GnssVerb::kProbe)).status));
    advance(3000U);
    CHECK(s_context->data.gnssStatus.read().state == gnss::State::kRunning);
    CHECK(s_saveCommands == saves + 1U);
    CHECK(s_context->data.phonePppResult.read().seq == request.seq);

    // 0x06 截止角/星座：成功写入 NVS、NACK 保留旧值、重探后重放。
    const core::SatTrackConfig satOk{
        .cutoffDeg = 10U, .gps = 1U, .glo = 0U, .bds = 1U, .gal = 1U, .sbas = 0U};
    fw::Command satCmd{};
    satCmd.verb = static_cast<uint8_t>(app::GnssSubVerb::kPhoneSatCfg);
    satCmd.seq = 1U;
    const auto satEncoded = core::encodeSatTrackConfig(satOk);
    satCmd.argLen = static_cast<uint8_t>(satEncoded.size());
    std::copy(satEncoded.begin(), satEncoded.end(), satCmd.args.begin());
    s_satMaskCommands = 0U;
    s_context->data.phoneSatCommand.write(satCmd);
    advance(300U);
    const auto satDone = s_context->data.phoneSatResult.read();
    CHECK(satDone.seq == satCmd.seq && satDone.status == fw::Result::kOk);
    CHECK(s_satMaskCommands == 5U);
    CHECK(s_context->data.phoneSatConfig.read() == satOk);
    std::array<uint8_t, core::kSatConfigBlobBytes> storedSat{};
    size_t storedSatLen = 0U;
    CHECK(app::store::readBlob(app::store::kPhoneSatCfgKey, storedSat, storedSatLen) == ESP_OK);
    CHECK(storedSatLen == satEncoded.size() && storedSat == satEncoded);

    // NACK 失败时保留旧 NVS 值
    s_rejectSatMask = true;
    satCmd.seq = 2U;
    satCmd.args[0] = 20U;
    s_context->data.phoneSatCommand.write(satCmd);
    advance(300U);
    s_rejectSatMask = false;
    const auto satFail = s_context->data.phoneSatResult.read();
    CHECK(satFail.seq == satCmd.seq && satFail.status == fw::Result::kIoError);
    CHECK(s_context->data.phoneSatConfig.read() == satOk);

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
    CHECK(status.rxPeriodMs[kGsaIndex] == 100U);
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
    // CM680 不支持 PPP：重放与命令均不下发任何 CONFIG PPP / CONFIG SIGNALGROUP。
    s_pppCommands = 0U;
    fw::Command cm680Ppp{};
    cm680Ppp.verb = static_cast<uint8_t>(app::GnssSubVerb::kPhonePpp);
    cm680Ppp.seq = 1U;
    cm680Ppp.argLen = 1U;
    cm680Ppp.args[0] = 1U;
    s_context->data.phonePppCommand.write(cm680Ppp);
    advance(100U);
    const auto cm680PppRes = s_context->data.phonePppResult.read();
    CHECK(cm680PppRes.seq == cm680Ppp.seq && cm680PppRes.status == fw::Result::kNotSupported);
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
 * @brief 验证网页→GNSS 桥接：控制台交错不串单、超时取消不执行、GGA 门控 NVS 失败/禁写回报与退出立即落盘。
 * @note 单线程主机测试，覆盖交叉评审 P1 #3、P1 #4、P2 #6。
 */
void testWebGnssBridgeAndGgaPersist()
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

    auto makeRateCmd = [](uint32_t seq, uint8_t hz) {
        fw::Command cmd{};
        cmd.target = app::id(app::ModuleKind::kGnss);
        cmd.verb = static_cast<uint8_t>(app::GnssVerb::kSetRate);
        cmd.seq = seq;
        cmd.argLen = 1U;
        cmd.args[0] = hz;
        return cmd;
    };

    // 1. 控制台先发起 kSetRate(5) 在途，此时网页提交 kSetRate(10) 须按自身 seq 返回 kBusy，
    //    且控制台应用完成推进全局 applySeq 时不得误写成网页成功（P1 #3）。
    fw::CommandResult consoleRes{};
    s_context->gnss.onCommand(makeRateCmd(0U, 5U), consoleRes);
    CHECK(fw::isOk(consoleRes.status));
    s_context->data.webGnssCommand.write(makeRateCmd(1U, 10U));
    advance(20U);
    auto out = s_context->data.webGnssOutcome.read();
    CHECK((out.seq == 1U) && (out.phase == app::data::WebGnssPhase::kDone) &&
          (out.status == fw::Result::kBusy));
    const uint32_t applyBefore = s_context->data.gnssStatus.read().applySeq;
    advance(500U);
    CHECK(s_context->data.gnssStatus.read().applySeq != applyBefore);
    out = s_context->data.webGnssOutcome.read();
    CHECK((out.seq == 1U) && (out.status == fw::Result::kBusy));

    // 2. 控制台完成后网页再提交 kSetRate(10) (seq=2)：先回 kAccepted，重配落盘后按 seq=2 回 kDone/kOk。
    s_context->data.webGnssCommand.write(makeRateCmd(2U, 10U));
    advance(20U);
    out = s_context->data.webGnssOutcome.read();
    CHECK((out.seq == 2U) && (out.phase == app::data::WebGnssPhase::kAccepted));
    advance(500U);
    out = s_context->data.webGnssOutcome.read();
    CHECK((out.seq == 2U) && (out.phase == app::data::WebGnssPhase::kDone) &&
          (out.status == fw::Result::kOk));
    uint8_t storedRate = 0U;
    CHECK((nvs_get_u8(nvs, "rate", &storedRate) == ESP_OK) && (storedRate == 10U));

    // 3. 超时取消语义（P2 #6）：kIo 消费前已写入 webGnssCancelSeq=3 → 跳过执行并回 kCancelled。
    s_context->data.webGnssCommand.write(makeRateCmd(3U, 2U));
    s_context->data.webGnssCancelSeq.store(3U, std::memory_order_release);
    advance(20U);
    out = s_context->data.webGnssOutcome.read();
    CHECK((out.seq == 3U) && (out.phase == app::data::WebGnssPhase::kCancelled));
    CHECK(s_context->data.gnssRateHz.load(std::memory_order_acquire) == 10U);

    // 4. GGA 门控 NVS 写失败（P1 #4）：网页提交 kSetGgaGate(on, 11) (seq=4)，NVS 故障时回错误且标记 kFailed；
    //    NVS 恢复后触发 webGnssFlushGgaSeq 立即落盘成功，状态回到 kClean。
    nvs_shim_set_write_fail(ESP_FAIL);
    fw::Command gateCmd = app::detail::packGnssGate(true, true, true, 11U);
    gateCmd.seq = 4U;
    s_context->data.webGnssCommand.write(gateCmd);
    advance(20U);
    out = s_context->data.webGnssOutcome.read();
    CHECK((out.seq == 4U) && (out.phase == app::data::WebGnssPhase::kAccepted));
    advance(600U);
    out = s_context->data.webGnssOutcome.read();
    CHECK((out.seq == 4U) && (out.phase == app::data::WebGnssPhase::kDone) &&
          !fw::isOk(out.status));
    CHECK(s_context->data.ggaGatePersist.load(std::memory_order_acquire) ==
          app::data::GgaGatePersist::kFailed);

    nvs_shim_set_write_fail(ESP_OK);
    s_context->data.webGnssFlushGgaSeq.store(1U, std::memory_order_release);
    advance(20U);
    CHECK(s_context->data.webGnssFlushGgaDoneSeq.load(std::memory_order_acquire) == 1U);
    CHECK(s_context->data.ggaGatePersist.load(std::memory_order_acquire) ==
          app::data::GgaGatePersist::kClean);
    uint8_t nvsGate = 0U;
    uint8_t nvsMin = 0U;
    CHECK((nvs_get_u8(nvs, "gga_gate", &nvsGate) == ESP_OK) && (nvsGate == 1U));
    CHECK((nvs_get_u8(nvs, "gga_min_sats", &nvsMin) == ESP_OK) && (nvsMin == 11U));

    // 5. 禁写模式（safeDefaults=true）下提交 GGA 门控：直接回报 kInvalidState 且持久化态为 kFailed。
    s_context->bootConfig.safeDefaults = true;
    fw::Command gateCmd2 = app::detail::packGnssGate(true, false, true, 8U);
    gateCmd2.seq = 5U;
    s_context->data.webGnssCommand.write(gateCmd2);
    advance(20U);
    out = s_context->data.webGnssOutcome.read();
    CHECK((out.seq == 5U) && (out.phase == app::data::WebGnssPhase::kDone) &&
          (out.status == fw::Result::kInvalidState));
    CHECK(s_context->data.ggaGatePersist.load(std::memory_order_acquire) ==
          app::data::GgaGatePersist::kFailed);
    s_context->bootConfig.safeDefaults = false;

    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    nvs_close(nvs);
}
