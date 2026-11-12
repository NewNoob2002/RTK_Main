#!/usr/bin/env python3
# 一次性独立复审替身：编译真实 app_main.cpp，验证电源保持先于后续初始化及失败短路。
# Generated-by: Codex
# Owner: UNKNOWN（待人工审查签字）
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
work = Path(tempfile.mkdtemp(prefix="rtk-power-hold-review-"))
(work / "driver").mkdir()
(work / "driver/gpio.h").write_text('''#pragma once
#include <cstdint>
#include "esp_err.h"
enum gpio_num_t { GPIO_NUM_26 = 26 };
enum { GPIO_MODE_OUTPUT = 2, GPIO_PULLUP_DISABLE = 0, GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0 };
struct gpio_config_t { uint64_t pin_bit_mask; int mode; int pull_up_en; int pull_down_en; int intr_type; };
esp_err_t gpio_set_level(gpio_num_t, uint32_t);
esp_err_t gpio_config(const gpio_config_t *);
''')
(work / "esp_mac.h").write_text('''#pragma once
#include <cstdint>
#include "esp_err.h"
enum esp_mac_type_t { ESP_MAC_BT };
esp_err_t esp_read_mac(uint8_t *, esp_mac_type_t);
''')
reasons = ['UNKNOWN', 'POWERON', 'EXT', 'SW', 'PANIC', 'INT_WDT', 'TASK_WDT', 'WDT', 'DEEPSLEEP', 'BROWNOUT', 'SDIO', 'USB', 'JTAG', 'EFUSE', 'PWR_GLITCH', 'CPU_LOCKUP']
(work / "esp_system.h").write_text('#pragma once\nenum esp_reset_reason_t { ' + ','.join('ESP_RST_' + r for r in reasons) + ' };\nesp_reset_reason_t esp_reset_reason();\n')
(work / "smoke.cpp").write_text(r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "app_link.hpp"
#include "app_console.hpp"
#include "rtk_bt_spp.h"
extern "C" void app_main(void);
static char events[32];
static unsigned pos;
static int fail_at;
static void record(char value) { assert(pos < sizeof(events)-1); events[pos++] = value; events[pos] = '\0'; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t value) { assert(pos == 0); assert(pin == GPIO_NUM_26 && value == 1); record('L'); return fail_at == 1 ? ESP_FAIL : ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *cfg) { assert(pos == 1); assert(cfg->pin_bit_mask == (1ULL << 26)); assert(cfg->mode == GPIO_MODE_OUTPUT); assert(cfg->pull_up_en == GPIO_PULLUP_DISABLE && cfg->pull_down_en == GPIO_PULLDOWN_DISABLE && cfg->intr_type == GPIO_INTR_DISABLE); record('C'); return fail_at == 2 ? ESP_FAIL : ESP_OK; }
const char *esp_err_to_name(esp_err_t) { return "fake"; }
void fake_log(const char *, const char *, const char *, ...) {}
esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
esp_err_t esp_read_mac(uint8_t *, esp_mac_type_t) { assert(false); return ESP_FAIL; }
extern "C" esp_err_t rtk_store_init(void) { record('I'); return ESP_OK; }
extern "C" esp_err_t rtk_store_get_sn(char *out, size_t size) { assert(size >= 5); std::memcpy(out, "1234", 5); record('S'); return ESP_OK; }
extern "C" esp_err_t rtk_bt_spp_init(const char *name) { assert(std::strcmp(name, "E1-Lite-1234") == 0); record('B'); return ESP_OK; }
namespace rtk::app {
bool derive_sn_from_mac(std::span<const uint8_t, core::kAddrLen>, std::span<char, RTK_STORE_SN_MAX+1U>) noexcept { assert(false); return false; }
bool build_device_name(std::string_view sn, std::span<char, kDeviceNameBuf> name) noexcept { assert(sn == "1234"); std::snprintf(name.data(), name.size(), "E1-Lite-%.*s", int(sn.size()), sn.data()); record('N'); return true; }
bool load_whitelist(std::span<core::BtAddr>, size_t &count) noexcept { count = 0; record('W'); return true; }
const char *policy_name(core::Policy) noexcept { return "CONCURRENT"; }
void note_bt_failure() noexcept { assert(false); }
bool start(core::Policy, std::span<const core::BtAddr>, const char *sn) noexcept { assert(std::strcmp(sn, "1234") == 0); record('T'); return true; }
esp_err_t start_console() noexcept { record('H'); return ESP_OK; }
}
int main() {
    const char *expected[] = {"LCISWNBTH", "L", "LC"};
    for (int scenario = 0; scenario != 3; ++scenario) {
        pos = 0; events[0] = '\0'; fail_at = scenario;
        app_main();
        assert(std::strcmp(events, expected[scenario]) == 0);
        std::printf("PASS scenario=%d order=%s\n", scenario, events);
    }
}
''')
command = ['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-I' + str(work),
           '-I' + str(root / 'tools/t13_host_check/shim/inc'),
           '-I' + str(root / 'main'), '-I' + str(root / 'components/rtk_core/include'),
           '-I' + str(root / 'components/rtk_store/include'), '-I' + str(root / 'components/rtk_bt/include'),
           str(root / 'main/app_main.cpp'), str(work / 'smoke.cpp'), '-o', str(work / 'smoke')]
print(' '.join(command), flush=True)
subprocess.run(command, check=True)
subprocess.run([str(work / 'smoke')], check=True)
