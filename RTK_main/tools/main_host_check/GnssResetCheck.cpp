// 用真实 GnssModule 和内存 NVS 重现接收机延迟重启与启动切速。
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "AppContext.hpp"
#include "def/GnssSubDef.hpp"
#include "driver/uart.h"
#include "modules/gnss/GnssCmdPack.hpp"
#include "modules/gnss/GnssKeys.hpp"
#include "rtos/EspStatus.hpp"

extern "C" {
#include "nvs.h"
}

namespace {

constinit app::AppContext s_initialContext{};
constinit app::AppContext s_failureContext{};
constinit app::AppContext s_dispatchContext{};
constinit app::AppContext s_dispatchCmdContext{};
constinit app::AppContext *s_context = &s_initialContext;
uint32_t s_now = 0;
uint32_t s_hostBaud = 0;
uint32_t s_receiverBaud = 460800;
uint32_t s_readyAt = 0;
unsigned s_baudCommands = 0;
unsigned s_saveCommands = 0;
unsigned s_fresetCommands = 0;
unsigned s_modeBaseCommands = 0;
bool s_applyBaud = true;
bool s_rejectSave = false;
bool s_expectBaudSave = false;
nvs_handle_t s_nvs = 0U;
std::string s_rx;

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

void startModule(nvs_handle_t handle, uint32_t preferred, bool applyBaud)
{
    s_rx.clear();
    s_readyAt = 0U;
    s_receiverBaud = 460800U;
    s_baudCommands = 0U;
    s_saveCommands = 0U;
    s_applyBaud = applyBaud;
    CHECK(nvs_set_u32(handle, "baud", preferred) == ESP_OK);
    CHECK(nvs_set_u32(handle, "cache_baud", 460800U) == ESP_OK);
    CHECK(nvs_set_u8(handle, "family", static_cast<uint8_t>(gnss::Family::kUnicore)) == ESP_OK);
    CHECK(nvs_set_u8(handle, "model", static_cast<uint8_t>(gnss::Model::kUm980)) == ESP_OK);
    CHECK(nvs_set_u8(handle, "rate", 5U) == ESP_OK);
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
    CHECK(fw::isOk(s_context->gnss.init()));
    CHECK(s_hostBaud == preferred);
    CHECK(fw::isOk(s_context->gnss.start()));
}

void checkStoredDefaults(nvs_handle_t handle)
{
    uint32_t baud = 0U;
    uint8_t value = 0U;
    CHECK(nvs_get_u32(handle, "baud", &baud) == ESP_OK && baud == 115200U);
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
    if (bytes == "FRESET\r\n") {
        ++s_fresetCommands;
        s_readyAt = s_now + 3500U;
        s_receiverBaud = 115200U;
    } else if (bytes == "CONFIG COM2 115200\r\n" || bytes == "CONFIG COM2 460800\r\n") {
        ++s_baudCommands;
        if (s_applyBaud) {
            s_receiverBaud = bytes == "CONFIG COM2 115200\r\n" ? 115200U : 460800U;
            s_expectBaudSave = true;
        }
    } else if (bytes == "VERSIONA\r\n") {
        // 来自 TestGnssUnicoreReceiver 的 UM980 实机 VERSIONA 样本。
        s_rx +=
            "#VERSIONA,0,GPS,UNKNOWN,0,0,0,0,0,249;\"UM980\",\"R4.10Build11833\",\"HRPT00-S10C-P\","
            "\"2310415000001-MD22A2244202679\",\"ff3b7c6f3c5fd90b\",\"2023/11/24\"*3f55fb4d\r\n";
    } else if (bytes.ends_with("\r\n")) {
        if (bytes.starts_with("MODE BASE")) {
            ++s_modeBaseCommands;
        }
        const bool save = bytes == "SAVECONFIG\r\n";
        if (save) {
            ++s_saveCommands;
            if (s_expectBaudSave) {
                uint32_t stored = 0U;
                CHECK(nvs_get_u32(s_nvs, "baud", &stored) == ESP_OK && stored == s_receiverBaud);
                CHECK(nvs_get_u32(s_nvs, "cache_baud", &stored) == ESP_OK &&
                      stored == s_receiverBaud);
                s_expectBaudSave = false;
            }
        }
        std::string reply{"command,"};
        reply += bytes.substr(0, bytes.size() - 2U);
        reply += save && s_rejectSave ? ",response: ERROR" : ",response: OK";
        unsigned checksum = 0;
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
    CHECK(s_hostBaud == 115200U && s_baudCommands == 1U && s_saveCommands == 1U);

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
    CHECK(nvs_get_u32(nvs, "baud", &stored) == ESP_OK && stored == 115200U);
    CHECK(nvs_get_u32(nvs, "cache_baud", &stored) == ESP_OK && stored == 460800U);
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
    startModule(nvs, 115200U, true);
    advance(3000U);
    testConsoleCommandDispatch(nvs);
    CHECK(fw::isOk(s_context->gnss.stopBoot()));
    nvs_close(nvs);
}
