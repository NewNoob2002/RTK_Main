// SDL2 主机模拟 OLED 终端。真实 UI 状态机和 WouoUI 输出到模拟 SH1106 页缓冲。
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "modules/oled/OledUi.hpp"

namespace {

using namespace app;
using namespace app::oled;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

ScreenBuff s_panel{};

struct Options {
    uint32_t fps = 30U;
    uint32_t slow = 1U;
    bool blue = false;
    bool headless = false;
    bool bootStandby = false;
    fs::path script{};
    fs::path dump{};
    fs::path golden{};
};

struct Simulator {
    ViewModel model{};
    UiController controller{};
    ScreenBuff frame{};
    bool statusBarValid = false;
    std::vector<uint32_t> shortKeys{};
    uint32_t shutdownStartedMs = 0U;
    board::ShutdownReason shutdownReason = board::ShutdownReason::kKey;
    bool shuttingDown = false;
    bool longKeyPending = false;

    /** @brief 初始化真实 UI 和模拟屏的发送回调。 */
    explicit Simulator(bool bootStandby = false)
    {
        WouoUI_GraphSetSendBuffFun(
            [](ScreenBuff fb) { std::memcpy(s_panel, fb, sizeof(s_panel)); });
        if (bootStandby) {
            setScene(9U, 0U);
        } else {
            setScene(2U, 0U);
            controller.reset(board::BootMode::kNormal, 0U);
        }
    }

    /** @brief 用编号 1–9 选择可重复的模拟硬件快照（9 为充电待机模式）。 */
    void setScene(unsigned scene, uint32_t nowMs = 0U)
    {
        model = {};
        sanitizeCopy("P2", model.product);
        sanitizeCopy("P2 V1.41", model.boardName);
        sanitizeCopy("V1.41", model.hwVer);
        sanitizeCopy("V3.1.0", model.fwVer);
        sanitizeCopy("P2000123", model.sn);
        sanitizeCopy("P2-P2000123", model.deviceName);
        sanitizeCopy("R4.10", model.gnssFwVer);
        model.powerActive = model.batteryAdcReady = model.nvsReady = true;
        model.btReady = model.chargerPresent = model.chargerOk = model.gnssEnabled = true;
        model.wifiSupported = true;
        if (scene == 9U) {
            model.bootMode = board::BootMode::kChargeStandby;
            model.batteryPct = 76U;
            model.batteryMv = 4020U;
            model.usbPresent = true;
            model.charging = true;
            shuttingDown = false;
            longKeyPending = false;
            controller.reset(board::BootMode::kChargeStandby, nowMs);
            return;
        }
        if (controller.page() == OledPage::kChargeStandby) {
            controller.reset(board::BootMode::kNormal, nowMs);
        }
        model.gnssState = scene == 8U ? gnss::State::kFailed : gnss::State::kRunning;
        model.gnssModel = gnss::Model::kUm980;
        model.fixValid = scene != 1U && scene != 8U;
        model.fixQuality =
            model.fixValid ? gnss::FixQuality::kRtkFixed : gnss::FixQuality::kUnknown;
        model.batteryPct = scene == 4U ? 8U : 78U;
        model.batteryMv = scene == 4U ? 3380U : 3980U;
        model.usbPresent = model.charging = scene == 3U;
        if (scene == 6U) {
            model.radioMode = RadioMode::kWifiConfig;
            model.wifiReady = true;
            model.wifiClients = 2U;
            sanitizeCopy("P2-P2000123", model.wifiSsid);
        }
        if (scene == 7U) {
            model.pairing = {.windowDeadlineMs = nowMs + 60000U,
                             .requestDeadlineMs = nowMs + 30000U,
                             .id = 1U,
                             .number = 123456U,
                             .suffix = {0x56U, 0x78U, 0x9AU},
                             .windowOpen = true,
                             .hasNumber = true};
            sanitizeCopy("Test Phone", model.pairing.name);
        }
        model.btSessions = scene == 5U ? 2U : 0U;
        if (scene == 5U) {
            sanitizeCopy("56:78:9A", model.btPeerLabel);
        }
        if (model.fixValid) {
            model.utcValid = true;
            model.utcHour = 12U;
            model.utcMinute = 34U;
            model.utcSecond = 56U;
            model.latE9 = 37123456789LL;
            model.lonE9 = 127123456789LL;
            model.altMm = 12800;
            model.hAccCm = 2U;
            model.hdopX100 = 90U;
            model.diffAgeDs = 10U;
            model.satsUsed = 21U;
            model.satsVisible = 29U;
            model.satGroups = {{{8U, 6U}, {8U, 7U}, {7U, 5U}, {6U, 3U}}};
            model.topSnr = {{{'G', 7U, 48U, true},
                             {'B', 11U, 42U, true},
                             {'E', 19U, 33U, true},
                             {'R', 5U, 25U, false}}};
            model.topSnrCount = 4U;
        }
        shuttingDown = false;
    }

    /** @brief 开始模拟物理按键按住（用于 600..2500 ms 浮层与充电待机唤醒环）。 */
    void keyDown(uint32_t nowMs)
    {
        model.keyHold.active = true;
        model.keyHold.standbyConfirm = false;
        model.keyHold.startMs = nowMs;
        model.keyHold.thresholdMs = board::kDefaultLongPressMs;
    }

    /** @brief 结束模拟物理按键按住。 */
    void keyUp()
    {
        model.keyHold.active = false;
    }

    /** @brief 模拟长按 2.5 s 到达：正常模式通知 UI 进入关机确认页，待机模式锁定满环。 */
    void triggerLongKey()
    {
        model.keyHold.active = false;
        if (model.bootMode == board::BootMode::kChargeStandby) {
            model.keyHold.standbyConfirm = true;
        } else {
            longKeyPending = true;
        }
    }

    /** @brief 进入模拟关机阶段序列。 */
    void shutdown(uint32_t nowMs, board::ShutdownReason reason = board::ShutdownReason::kKey)
    {
        shuttingDown = true;
        shutdownStartedMs = nowMs;
        shutdownReason = reason;
    }

    /** @brief 推进真实按键判定和渲染，经 WouoUI 回调写入页缓冲。 */
    void draw(uint32_t nowMs)
    {
        model.uptimeMs = nowMs;
        if (shuttingDown) {
            updateShutdown(nowMs - shutdownStartedMs);
        }
        const auto decision = controller.step(nowMs, shortKeys, model, longKeyPending);
        shortKeys.clear();
        longKeyPending = false;
        statusBarValid = renderUi(controller.renderState(nowMs), model, frame, statusBarValid);
        WouoUI_BuffSend();
        applyAction(decision.action.action, nowMs);
    }

private:
    /** @brief 为纯 UI 动作更新模拟设备快照。 */
    void applyAction(OledAction action, uint32_t nowMs)
    {
        switch (action) {
        case OledAction::kWifi:
            setScene(6U, nowMs);
            break;
        case OledAction::kBluetooth:
            setScene(5U, nowMs);
            break;
        case OledAction::kTogglePair:
            model.pairing.windowOpen = !model.pairing.windowOpen;
            model.pairing.windowDeadlineMs = nowMs + 60000U;
            break;
        case OledAction::kPairAccept:
        case OledAction::kPairReject:
            model.pairing.id = 0U;
            break;
        case OledAction::kShutdownKey:
            shutdown(nowMs, board::ShutdownReason::kKey);
            break;
        case OledAction::kNone:
            break;
        }
    }

    /** @brief 注入阶段快照，停留由脚本时间或交互时间推进。 */
    void updateShutdown(uint32_t elapsed)
    {
        using board::ShutdownDoneBit;
        using board::ShutdownStage;
        model.shutdownReason = shutdownReason;
        model.shutdownDoneMask = board::shutdownDoneBit(ShutdownDoneBit::kAnnounced);
        model.shutdownStage = ShutdownStage::kAnnounce;
        if (elapsed >= board::kMinStageVisibleMs) {
            model.shutdownStage = ShutdownStage::kSave;
            model.shutdownDoneMask |= board::shutdownDoneBit(ShutdownDoneBit::kSaveFences);
        }
        if (elapsed >= 2U * board::kMinStageVisibleMs) {
            model.shutdownStage = ShutdownStage::kQuiesce;
            model.shutdownDoneMask |= board::shutdownDoneBit(ShutdownDoneBit::kSaveFlushed) |
                                      board::shutdownDoneBit(ShutdownDoneBit::kPeripheralsSafe);
        }
        if (elapsed >= 3U * board::kMinStageVisibleMs) {
            model.shutdownStage = ShutdownStage::kCutPower;
            model.shutdownDoneMask |=
                board::shutdownDoneBit(ShutdownDoneBit::kCutPower) |
                board::shutdownDoneBit(ShutdownDoneBit::kRadioQuiesced) |
                board::shutdownDoneBit(model.usbPresent ? ShutdownDoneBit::kUsbOffReady
                                                        : ShutdownDoneBit::kBatteryOffReady);
        }
    }
};

/** @brief 读取一个限定范围的无符号整数参数。 */
bool boundedNumber(std::string_view text, uint32_t &out, uint32_t upper)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size() && out > 0U && out <= upper;
}

/** @brief 解析主机工具选项。 */
bool parseOptions(int argc, char **argv, Options &out)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--headless") {
            out.headless = true;
            continue;
        }
        if (i + 1 >= argc) {
            return false;
        }
        const std::string_view value{argv[++i]};
        if (arg == "--script") {
            out.script = value;
        } else if (arg == "--dump") {
            out.dump = value;
        } else if (arg == "--golden") {
            out.golden = value;
        } else if (arg == "--color" && (value == "blue" || value == "white")) {
            out.blue = value == "blue";
        } else if (arg == "--fps" && boundedNumber(value, out.fps, 120U)) {
        } else if (arg == "--slow" && boundedNumber(value, out.slow, 100U)) {
        } else if (arg == "--boot" && (value == "normal" || value == "standby")) {
            out.bootStandby = value == "standby";
        } else {
            return false;
        }
    }
    return out.headless ? !out.script.empty() : out.script.empty() && out.golden.empty();
}

/** @brief 读模拟 SH1106 缓冲内的一个显示像素。 */
bool pixel(size_t x, size_t y)
{
    return (s_panel[y / 8U][x] & (1U << (y % 8U))) != 0U;
}

/** @brief 将 SH1106 列位序转换为 PBM P4 横向位序。 */
std::string pbm()
{
    std::string bytes = "P4\n128 64\n";
    for (size_t y = 0; y < kHeight; ++y) {
        for (size_t x = 0; x < kWidth; x += 8U) {
            uint8_t byte = 0U;
            for (size_t bit = 0; bit < 8U; ++bit) {
                if (pixel(x + bit, y)) {
                    byte |= static_cast<uint8_t>(0x80U >> bit);
                }
            }
            bytes.push_back(static_cast<char>(byte));
        }
    }
    return bytes;
}

/** @brief 写入一帧 PBM，传回文件写入结果。 */
bool writeFrame(const fs::path &path, const std::string &bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file.close(), file);
}

/** @brief 检查脚本页码或保存并比对指定的关键帧。 */
bool checkFrame(const Options &opts, const Simulator &sim, std::string_view verb,
                const std::string &arg)
{
    if (verb == "check") {
        return arg == oledPageName(sim.controller.page());
    }
    if (arg.empty() ||
        arg.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) {
        return false;
    }
    const std::string bytes = pbm();
    if (!opts.dump.empty() && !writeFrame(opts.dump / (arg + ".pbm"), bytes)) {
        return false;
    }
    if (opts.golden.empty()) {
        return true;
    }
    std::ifstream file(opts.golden / (arg + ".pbm"), std::ios::binary);
    return file && std::string(std::istreambuf_iterator<char>(file), {}) == bytes;
}

/** @brief 按脚本时间推进固定间隔的真实 UI 帧。 */
bool runScript(const Options &opts, Simulator &sim)
{
    std::ifstream script(opts.script);
    if (!script) {
        return false;
    }
    uint32_t renderedMs = 0U;
    unsigned frameIdx = 0U;
    const uint32_t frameMs = std::max(1U, 1000U / opts.fps / opts.slow);
    if (!opts.dump.empty()) {
        std::ofstream progress(opts.dump / "frames.csv");
        progress
            << "frame,time_ms,page,transition,start_ms,progress_q10,x,y,w,h,content_offset_y\n";
    }
    auto draw = [&](uint32_t nowMs) {
        sim.draw(nowMs);
        if (opts.dump.empty()) {
            return true;
        }
        const auto state = sim.controller.renderState(nowMs);
        const auto &t = *state.transition;
        const auto pose =
            t.pose(nowMs, {0, 0, static_cast<int16_t>(kWidth), static_cast<int16_t>(kHeight)});
        const auto &rect = pose.viewport;
        std::ofstream progress(opts.dump / "frames.csv", std::ios::app);
        progress << frameIdx << ',' << nowMs << ',' << oledPageName(state.page) << ','
                 << static_cast<unsigned>(t.kind) << ',' << t.startMs << ',' << t.progressQ10(nowMs)
                 << ',' << rect.x << ',' << rect.y << ',' << rect.w << ',' << rect.h << ','
                 << pose.contentOffsetY << '\n';
        return progress &&
               writeFrame(opts.dump / ("frame-" + std::to_string(frameIdx++) + ".pbm"), pbm());
    };
    for (std::string line; std::getline(script, line);) {
        std::istringstream row(line);
        row >> std::ws;
        if (row.eof() || row.peek() == '#') {
            continue;
        }
        uint32_t at = 0U;
        std::string verb;
        std::string arg;
        if (!(row >> at >> verb) || at < renderedMs || at >= 24U * 3600000U) {
            return false;
        }
        static_cast<void>(row >> arg);
        for (uint32_t next = renderedMs + frameMs; next < at; next += frameMs) {
            if (!draw(next)) {
                return false;
            }
        }
        renderedMs = at;
        if (verb == "short") {
            sim.shortKeys.push_back(at);
        } else if (verb == "key_down") {
            sim.keyDown(at);
        } else if (verb == "key_up") {
            sim.keyUp();
        } else if (verb == "long") {
            sim.triggerLongKey();
        } else if (verb == "shutdown") {
            auto reason = board::ShutdownReason::kKey;
            if (arg == "console") {
                reason = board::ShutdownReason::kConsole;
            } else if (arg == "low_battery") {
                reason = board::ShutdownReason::kLowBattery;
            } else if (arg == "phone") {
                reason = board::ShutdownReason::kPhone;
            }
            sim.shutdown(at, reason);
        } else if (verb == "scene") {
            uint32_t scene = 0U;
            if (!boundedNumber(arg, scene, 9U)) {
                return false;
            }
            sim.setScene(scene, at);
        } else if (verb != "check" && verb != "shot") {
            return false;
        }
        if (!draw(at) ||
            ((verb == "check" || verb == "shot") && !checkFrame(opts, sim, verb, arg))) {
            std::cerr << "script failure at " << at << " ms: " << verb << ' ' << arg << " (page "
                      << oledPageName(sim.controller.page()) << ")\n";
            return false;
        }
    }
    return script.eof();
}

/** @brief 处理 SDL 键盘，空格松开投递短按，持续按下触发一次长按。 */
struct Keyboard {
    bool pressed = false;
    bool longFired = false;
    uint32_t pressedAtMs = 0U;

    /** @brief 更新按键并在阈值到达时通知模拟电源。 */
    void update(Simulator &sim, uint32_t realMs, uint32_t uiMs, const SDL_Event *event)
    {
        if (pressed && !longFired && realMs - pressedAtMs >= board::kDefaultLongPressMs) {
            sim.triggerLongKey();
            longFired = true;
        }
        if (event == nullptr || (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)) {
            return;
        }
        const auto key = event->key.keysym.sym;
        if (event->type == SDL_KEYDOWN && !event->key.repeat) {
            if (key == SDLK_SPACE) {
                pressed = true;
                longFired = false;
                pressedAtMs = realMs;
                sim.keyDown(uiMs);
            } else if (key >= SDLK_1 && key <= SDLK_9) {
                sim.setScene(static_cast<unsigned>(key - SDLK_0), uiMs);
            }
        } else if (event->type == SDL_KEYUP && key == SDLK_SPACE && pressed) {
            sim.keyUp();
            if (!longFired && (realMs - pressedAtMs) < board::kKeyShortMaxMs) {
                sim.shortKeys.push_back(uiMs);
            }
            pressed = false;
        }
    }
};

/** @brief 实时推进 UI 并把模拟单色屏放大显示。 */
bool runWindow(const Options &opts, Simulator &sim)
{
    constexpr int kScale = 6;
    constexpr int kWindowWidth = static_cast<int>(kWidth) * kScale;
    constexpr int kWindowHeight = static_cast<int>(kHeight) * kScale;
    constexpr int kPitch = static_cast<int>(kWidth * sizeof(uint32_t));
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *window =
        SDL_CreateWindow("RTK OLED", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWindowWidth,
                         kWindowHeight, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, 0) : nullptr;
    SDL_Texture *texture =
        renderer
            ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                static_cast<int>(kWidth), static_cast<int>(kHeight))
            : nullptr;
    std::array<uint32_t, kWidth * kHeight> rgba{};
    const uint32_t foreground = opts.blue ? 0xFF4CB8FFU : 0xFFF2F7FFU;
    const auto start = Clock::now();
    uint32_t nextFrameMs = 0U;
    Keyboard keys{};
    bool running = texture != nullptr;
    bool ok = running;
    while (running) {
        const auto realMs = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        const uint32_t uiMs = realMs / opts.slow;
        keys.update(sim, realMs, uiMs, nullptr);
        for (SDL_Event event; SDL_PollEvent(&event);) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                running = false;
            }
            keys.update(sim, realMs, uiMs, &event);
        }
        if (realMs >= nextFrameMs) {
            nextFrameMs = realMs + 1000U / opts.fps;
            sim.draw(uiMs);
            for (size_t i = 0U; i < rgba.size(); ++i) {
                rgba[i] = pixel(i % kWidth, i / kWidth) ? foreground : 0xFF000000U;
            }
            if (SDL_UpdateTexture(texture, nullptr, rgba.data(), kPitch) != 0 ||
                SDL_RenderCopy(renderer, texture, nullptr, nullptr) != 0) {
                ok = running = false;
            }
            SDL_RenderPresent(renderer);
        }
        SDL_Delay(1U);
    }
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return ok;
}

} // namespace

/** @brief 启动窗口或运行无窗口脚本。 */
int main(int argc, char **argv)
{
    Options options{};
    if (!parseOptions(argc, argv, options)) {
        std::cerr << "usage: oled_sim [--boot normal|standby] [--fps N] [--slow N]"
                     " [--color blue|white]\n"
                     "       oled_sim --headless [--boot normal|standby] --script FILE"
                     " [--dump DIR] [--golden DIR]\n";
        return 2;
    }
    if (!options.dump.empty()) {
        std::error_code ec;
        fs::create_directories(options.dump, ec);
        if (ec) {
            std::cerr << ec.message() << '\n';
            return 1;
        }
    }
    Simulator sim{options.bootStandby};
    const bool ok = options.headless ? runScript(options, sim) : runWindow(options, sim);
    if (!ok) {
        std::cerr << "oled_sim failed: " << SDL_GetError() << '\n';
    }
    return ok ? 0 : 1;
}
