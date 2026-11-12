/*
 * ExternalSpeak - I2S 功放（MAX98357A 类，带 SD 使能脚）声压测试固件
 * 引脚: LRC(WS)=GPIO25, BCLK=GPIO26, DIN=GPIO22, SD=GPIO21
 * 串口命令(115200, 回车结束):
 *   f <hz>      设置单音频率 (50..16000)
 *   v <0-100>   设置数字音量百分比 (100 = 0 dBFS 满幅)
 *   s           扫频 200Hz..8kHz, 每点 1.5 s, 便于测各频点 dB
 *   b           蜂鸣模式: 2.7kHz 0.5s 开/0.5s 关 (模拟提示音)
 *   a           警笛: 1.2k->3.2kHz 线性滑音, 周期 1 s
 *   d           双音报警: 2.7k/1.8kHz 每 250 ms 交替
 *   w           切换波形 正弦/方波 (方波同峰值 RMS 高约 3 dB)
 *   t           恢复连续单音
 *   on / off    SD 脚使能/关断功放 (x 同 off)
 * 开机静音: SD 保持低电平，收到 f/s/b/a/d/t/on 才打开功放
 * Assisted-by: Claude Code (claude-opus-5-5)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_I2S_WS   GPIO_NUM_25
#define PIN_I2S_BCLK GPIO_NUM_26
#define PIN_I2S_DOUT GPIO_NUM_22
#define PIN_AMP_SD   GPIO_NUM_21

#define SAMPLE_RATE  44100U
#define FRAME_SAMPLES 256U
#define CONSOLE_UART UART_NUM_0

typedef enum { MODE_TONE, MODE_SWEEP, MODE_BEEP, MODE_SIREN, MODE_DUAL } play_mode_t;

static const char *TAG = "ext_spk";
static i2s_chan_handle_t s_tx;

/* 由控制任务写、音频任务读；32 位对齐写在 ESP32 上原子，测试固件可接受 */
static volatile float s_freq_hz = 1000.0f;
static volatile float s_volume = 0.3f; /* 首次出声默认 30%，避免过响 */
static volatile play_mode_t s_mode = MODE_TONE;
static volatile bool s_square = false;

static void amp_enable(bool on)
{
    gpio_set_level(PIN_AMP_SD, on ? 1 : 0);
    ESP_LOGI(TAG, "amp %s", on ? "ON" : "OFF(shutdown)");
}

static void i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* 欠载时输出 0，避免爆音 */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

static void audio_task(void *arg)
{
    (void)arg;
    static int16_t buf[FRAME_SAMPLES * 2U];
    float phase = 0.0f;
    const float two_pi = 6.28318530718f;
    const float sweep_pts[] = {200, 300, 500, 800, 1000, 1500, 2000, 2700, 3000, 4000, 5000, 6000, 8000};
    const size_t n_pts = sizeof(sweep_pts) / sizeof(sweep_pts[0]);
    uint32_t samples = 0U;
    size_t sweep_idx = 0U;
    play_mode_t last_mode = MODE_TONE;

    for (;;) {
        play_mode_t mode = s_mode;
        if (mode != last_mode) {
            samples = 0U;
            sweep_idx = 0U;
            last_mode = mode;
            if (mode == MODE_SWEEP) {
                ESP_LOGI(TAG, "sweep %.0f Hz", sweep_pts[0]);
            }
        }

        float freq = s_freq_hz;
        bool gate = true;
        if (mode == MODE_SWEEP) {
            if (samples >= (SAMPLE_RATE * 3U / 2U)) {
                samples = 0U;
                sweep_idx = (sweep_idx + 1U) % n_pts;
                ESP_LOGI(TAG, "sweep %.0f Hz", sweep_pts[sweep_idx]);
            }
            freq = sweep_pts[sweep_idx];
        } else if (mode == MODE_BEEP) {
            freq = 2700.0f;
            gate = (samples % SAMPLE_RATE) < (SAMPLE_RATE / 2U);
        } else if (mode == MODE_SIREN) {
            freq = 1200.0f + 2000.0f * (float)(samples % SAMPLE_RATE) / (float)SAMPLE_RATE;
        } else if (mode == MODE_DUAL) {
            freq = ((samples / (SAMPLE_RATE / 4U)) % 2U == 0U) ? 2700.0f : 1800.0f;
        } else {
            /* MODE_TONE */
        }
        const bool square = s_square;

        const float amp = gate ? (s_volume * 32767.0f) : 0.0f;
        const float step = two_pi * freq / (float)SAMPLE_RATE;
        for (size_t i = 0; i < FRAME_SAMPLES; i++) {
            float sv = sinf(phase);
            if (square) {
                sv = (sv >= 0.0f) ? 1.0f : -1.0f;
            }
            int16_t v = (int16_t)(amp * sv);
            buf[2U * i] = v;      /* L */
            buf[2U * i + 1U] = v; /* R：MAX98357A 默认 (L+R)/2，两路相同 */
            phase += step;
            if (phase >= two_pi) {
                phase -= two_pi;
            }
        }
        samples += FRAME_SAMPLES;

        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write err %s", esp_err_to_name(err));
        }
    }
}

static void handle_cmd(char *line)
{
    char *arg = strchr(line, ' ');
    if (arg != NULL) {
        *arg++ = '\0';
    }
    if (strcmp(line, "f") == 0 && arg != NULL) {
        long hz = strtol(arg, NULL, 10);
        if (hz < 50 || hz > 16000) {
            ESP_LOGW(TAG, "freq out of range 50..16000");
            return;
        }
        s_freq_hz = (float)hz;
        s_mode = MODE_TONE;
        amp_enable(true);
        ESP_LOGI(TAG, "tone %ld Hz", hz);
    } else if (strcmp(line, "v") == 0 && arg != NULL) {
        long pct = strtol(arg, NULL, 10);
        if (pct < 0 || pct > 100) {
            ESP_LOGW(TAG, "volume out of range 0..100");
            return;
        }
        s_volume = (float)pct / 100.0f;
        ESP_LOGI(TAG, "volume %ld%% (%.1f dBFS)", pct, pct > 0 ? 20.0f * log10f(pct / 100.0f) : -INFINITY);
    } else if (strcmp(line, "s") == 0) {
        s_mode = MODE_SWEEP;
        amp_enable(true);
    } else if (strcmp(line, "b") == 0) {
        s_mode = MODE_BEEP;
        amp_enable(true);
        ESP_LOGI(TAG, "beep 2700 Hz");
    } else if (strcmp(line, "a") == 0) {
        s_mode = MODE_SIREN;
        amp_enable(true);
        ESP_LOGI(TAG, "siren 1200->3200 Hz");
    } else if (strcmp(line, "d") == 0) {
        s_mode = MODE_DUAL;
        amp_enable(true);
        ESP_LOGI(TAG, "dual tone 2700/1800 Hz");
    } else if (strcmp(line, "w") == 0) {
        s_square = !s_square;
        ESP_LOGI(TAG, "wave %s", s_square ? "square" : "sine");
    } else if (strcmp(line, "t") == 0) {
        s_mode = MODE_TONE;
        amp_enable(true);
        ESP_LOGI(TAG, "tone %.0f Hz", s_freq_hz);
    } else if (strcmp(line, "on") == 0) {
        amp_enable(true);
    } else if (strcmp(line, "off") == 0 || strcmp(line, "x") == 0) {
        amp_enable(false);
    } else if (line[0] != '\0') {
        ESP_LOGW(TAG, "cmd: f <hz> | v <0-100> | s | b | a | d | w | t | on | off | x");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[32];
    size_t len = 0U;
    uint8_t ch;
    for (;;) {
        if (uart_read_bytes(CONSOLE_UART, &ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            handle_cmd(line);
            len = 0U;
        } else if (len < sizeof(line) - 1U) {
            line[len++] = (char)ch;
        } else {
            len = 0U; /* 超长丢弃 */
        }
    }
}

void app_main(void)
{
    const gpio_config_t sd_cfg = {
        .pin_bit_mask = 1ULL << PIN_AMP_SD,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sd_cfg));
    gpio_set_level(PIN_AMP_SD, 0); /* I2S 时钟稳定前保持关断，减少上电爆音 */

    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));
    i2s_init();

    xTaskCreate(audio_task, "audio", 4096, NULL, 10, NULL);
    xTaskCreate(console_task, "console", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "ready (muted, amp OFF). cmd: f <hz> | v <0-100> | s | b | a | d | w | t | on | off | x");
}
