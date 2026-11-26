/*
 * ExternalSpeak - I2S 功放（MAX98357A 类，带 SD 使能脚）声压 / 续航测试固件
 * 引脚: LRC(WS)=GPIO25, BCLK=GPIO26, DIN=GPIO22, SD=GPIO21
 * 串口 115200，回车结束，支持回显与退格；输入 help 查看命令：
 *   f <hz>                 连续单音 (50..16000 Hz)
 *   v <0-100>              数字音量百分比 (100 = 0 dBFS 满幅)
 *   s | b | a | d | t      扫频 / 蜂鸣 / 警笛 / 双音 / 恢复连续单音
 *   w                      切换波形 正弦/方波
 *   p <duty> [period_s] [b|a|d|t]
 *                          周期报警：每 period_s 秒(默认 10)内响 duty% 时间，
 *                          其余时间 SD 关断功放；用于测 20%/50% 等占空比下的续航
 *   p                      查看周期报警运行状态
 *   last                   上一次运行记录（掉电前最后一次写入 NVS 的数据）
 *   st                     当前状态
 *   on | off | x           SD 使能 / 关断（off/x 同时停止任何播放与周期报警）
 * 开机静音: SD 保持低电平，收到播放命令才打开功放
 *
 * 续航测试流程：满电 -> p 20 10 -> 电池耗尽 -> 充电/外接供电后上电，
 * 开机日志或 last 命令给出运行时长（NVS 每 60 s 保存一次，误差 < 60 s）。
 * Assisted-by: Claude Code (claude-opus-5-5)
 */
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PIN_I2S_WS   GPIO_NUM_25
#define PIN_I2S_BCLK GPIO_NUM_26
#define PIN_I2S_DOUT GPIO_NUM_22
#define PIN_AMP_SD   GPIO_NUM_21

#define SAMPLE_RATE   44100U
#define FRAME_SAMPLES 256U
#define CONSOLE_UART  UART_NUM_0

#define PERIOD_DEFAULT_S 10U
#define PERIOD_MAX_S     3600U
/* NVS 保存间隔：决定掉电后记录的最大误差；每分钟一次，20 h 约 1200 次写，NVS 自带磨损均衡 */
#define STATS_INTERVAL_S 60U

#define NVS_NS      "extspk"
#define NVS_KEY_RUN "run"
#define RUN_MAGIC   0x52554E31U /* "RUN1"，结构变化时更换以丢弃旧记录 */

typedef enum { MODE_TONE, MODE_SWEEP, MODE_BEEP, MODE_SIREN, MODE_DUAL } play_mode_t;

/* 周期报警运行记录，整体以 blob 写入 NVS */
typedef struct {
    uint32_t magic;
    uint32_t period_s;
    uint32_t elapsed_s; /* 从开始周期报警到最后一次保存的时长 */
    uint32_t on_s;      /* 其中功放实际发声的累计时长 */
    uint32_t cycles;    /* 已开始的发声周期数 */
    uint8_t duty;
    uint8_t volume;     /* 百分比 */
    uint8_t mode;       /* play_mode_t */
    uint8_t square;
} run_rec_t;

static const char *TAG = "ext_spk";
static i2s_chan_handle_t s_tx;

/* 由控制任务写、音频任务读；32 位对齐写在 ESP32 上原子，测试固件可接受 */
static volatile float s_freq_hz = 1000.0f;
static volatile float s_volume = 0.3f; /* 首次出声默认 30%，避免过响 */
static volatile play_mode_t s_mode = MODE_TONE;
static volatile bool s_square = false;
static volatile bool s_playing = false; /* false 时音频任务不产生波形 */

/* 周期报警配置：多字段需一致读取，用临界区保护 */
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_pa_active = false;
static uint32_t s_pa_period_s = PERIOD_DEFAULT_S;
static uint8_t s_pa_duty = 0U;
static int64_t s_pa_start_us = 0;
static uint32_t s_pa_gen = 0U; /* 每次重新开始加 1，音频任务据此重置计数 */

/* 运行统计：音频任务写，统计/控制任务读 */
static volatile uint32_t s_stat_cycles = 0U;
static volatile uint64_t s_stat_on_samples = 0U; /* 64 位读写非原子，读时进临界区 */

static run_rec_t s_prev_run;   /* 开机时从 NVS 读到的上一次记录 */
static bool s_prev_valid = false;
static bool s_amp_on = false;

static const char *mode_name(play_mode_t m)
{
    switch (m) {
    case MODE_SWEEP:
        return "sweep";
    case MODE_BEEP:
        return "beep";
    case MODE_SIREN:
        return "siren";
    case MODE_DUAL:
        return "dual";
    default:
        return "tone";
    }
}

static void fmt_hms(uint32_t s, char *out, size_t n)
{
    snprintf(out, n, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, s / 3600U, (s / 60U) % 60U, s % 60U);
}

static void amp_set(bool on, bool verbose)
{
    gpio_set_level(PIN_AMP_SD, on ? 1 : 0);
    s_amp_on = on;
    if (verbose) {
        ESP_LOGI(TAG, "amp %s", on ? "ON" : "OFF(shutdown)");
    }
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

/* ---------- 周期报警状态 ---------- */

static void pa_snapshot(bool *active, uint32_t *period_s, uint8_t *duty, int64_t *start_us, uint32_t *gen)
{
    portENTER_CRITICAL(&s_cfg_mux);
    *active = s_pa_active;
    *period_s = s_pa_period_s;
    *duty = s_pa_duty;
    *start_us = s_pa_start_us;
    *gen = s_pa_gen;
    portEXIT_CRITICAL(&s_cfg_mux);
}

static uint32_t stat_on_seconds(void)
{
    portENTER_CRITICAL(&s_cfg_mux);
    uint64_t on = s_stat_on_samples;
    portEXIT_CRITICAL(&s_cfg_mux);
    return (uint32_t)(on / SAMPLE_RATE);
}

static void run_rec_fill(run_rec_t *r)
{
    bool active;
    uint32_t period_s;
    uint8_t duty;
    int64_t start_us;
    uint32_t gen;
    pa_snapshot(&active, &period_s, &duty, &start_us, &gen);
    memset(r, 0, sizeof(*r));
    r->magic = RUN_MAGIC;
    r->period_s = period_s;
    r->duty = duty;
    r->elapsed_s = (uint32_t)((esp_timer_get_time() - start_us) / 1000000LL);
    r->on_s = stat_on_seconds();
    r->cycles = s_stat_cycles;
    r->volume = (uint8_t)lroundf(s_volume * 100.0f);
    r->mode = (uint8_t)s_mode;
    r->square = s_square ? 1U : 0U;
}

static void run_rec_print(const char *title, const run_rec_t *r)
{
    char el[16];
    char on[16];
    fmt_hms(r->elapsed_s, el, sizeof(el));
    fmt_hms(r->on_s, on, sizeof(on));
    ESP_LOGI(TAG, "%s: duty %u%% period %" PRIu32 " s %s/%s vol %u%% | run %s (%" PRIu32 " s) on %s cycles %" PRIu32,
             title, r->duty, r->period_s, mode_name((play_mode_t)r->mode), r->square ? "square" : "sine", r->volume,
             el, r->elapsed_s, on, r->cycles);
}

static void run_rec_save(const run_rec_t *r)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(h, NVS_KEY_RUN, r, sizeof(*r));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs save: %s", esp_err_to_name(err));
    }
}

static void run_rec_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* 首次上电命名空间不存在 */
    }
    size_t len = sizeof(s_prev_run);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_RUN, &s_prev_run, &len);
    nvs_close(h);
    s_prev_valid = (err == ESP_OK) && (len == sizeof(s_prev_run)) && (s_prev_run.magic == RUN_MAGIC);
}

static void pa_start(uint8_t duty, uint32_t period_s, play_mode_t mode)
{
    s_mode = mode;
    s_stat_cycles = 0U;
    portENTER_CRITICAL(&s_cfg_mux);
    s_stat_on_samples = 0U;
    s_pa_duty = duty;
    s_pa_period_s = period_s;
    s_pa_start_us = esp_timer_get_time();
    s_pa_gen++;
    s_pa_active = true;
    portEXIT_CRITICAL(&s_cfg_mux);
    s_playing = true;

    const uint32_t on_ms = period_s * 10U * duty; /* period_s*1000*duty/100 */
    ESP_LOGI(TAG, "periodic start: duty %u%% period %" PRIu32 " s (on %" PRIu32 ".%03" PRIu32 " s / off %" PRIu32
             ".%03" PRIu32 " s) %s vol %.0f%%",
             duty, period_s, on_ms / 1000U, on_ms % 1000U, (period_s * 1000U - on_ms) / 1000U,
             (period_s * 1000U - on_ms) % 1000U, mode_name(mode), s_volume * 100.0f);

    run_rec_t r;
    run_rec_fill(&r);
    run_rec_save(&r); /* 立即覆盖旧记录，避免本次运行不足 60 s 就掉电时误读上一次数据 */
}

/* 停止周期报警并保存最终记录；返回是否原本在运行 */
static bool pa_stop(void)
{
    bool was;
    portENTER_CRITICAL(&s_cfg_mux);
    was = s_pa_active;
    portEXIT_CRITICAL(&s_cfg_mux);
    if (!was) {
        return false;
    }
    run_rec_t r;
    run_rec_fill(&r);
    portENTER_CRITICAL(&s_cfg_mux);
    s_pa_active = false;
    portEXIT_CRITICAL(&s_cfg_mux);
    run_rec_save(&r);
    run_rec_print("periodic stop", &r);
    return true;
}

/* 切到普通播放模式（非周期） */
static void play_mode(play_mode_t mode)
{
    (void)pa_stop();
    s_mode = mode;
    s_playing = true;
    amp_set(true, true);
}

/* ---------- 音频 ---------- */

static void audio_task(void *arg)
{
    (void)arg;
    static int16_t buf[FRAME_SAMPLES * 2U];
    float phase = 0.0f;
    const float two_pi = 6.28318530718f;
    const float sweep_pts[] = {200, 300, 500, 800, 1000, 1500, 2000, 2700, 3000, 4000, 5000, 6000, 8000};
    const size_t n_pts = sizeof(sweep_pts) / sizeof(sweep_pts[0]);
    uint32_t samples = 0U; /* 当前模式（或当前发声段）内的样本计数，用于节奏 */
    size_t sweep_idx = 0U;
    play_mode_t last_mode = MODE_TONE;
    uint32_t last_gen = 0U;
    bool pa_was_on = false;

    for (;;) {
        bool pa_active;
        uint32_t period_s;
        uint8_t duty;
        int64_t start_us;
        uint32_t gen;
        pa_snapshot(&pa_active, &period_s, &duty, &start_us, &gen);

        if (pa_active) {
            if (gen != last_gen) {
                last_gen = gen;
                pa_was_on = false;
            }
            const int64_t period_us = (int64_t)period_s * 1000000LL;
            const int64_t on_us = period_us * duty / 100;
            const int64_t t = (esp_timer_get_time() - start_us) % period_us;
            const bool on = t < on_us;
            if (on && !pa_was_on) {
                samples = 0U; /* 每个发声段从图案起点开始 */
                sweep_idx = 0U;
                phase = 0.0f;
                s_stat_cycles = s_stat_cycles + 1U;
                amp_set(true, false);
            } else if (!on && pa_was_on) {
                amp_set(false, false);
            }
            pa_was_on = on;
            if (!on) {
                /* 静默段不写 I2S：auto_clear 让 DMA 输出 0，功放已关断，CPU 空闲省电 */
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        } else if (pa_was_on) {
            pa_was_on = false; /* 周期报警被停止，后续由命令决定功放状态 */
        }

        if (!s_playing) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

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
        if (pa_active) {
            portENTER_CRITICAL(&s_cfg_mux);
            s_stat_on_samples += FRAME_SAMPLES;
            portEXIT_CRITICAL(&s_cfg_mux);
        }

        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write err %s", esp_err_to_name(err));
        }
    }
}

/* 周期报警运行时每 STATS_INTERVAL_S 打印并保存一次；NVS 写放在独立任务，避免阻塞音频 */
static void stats_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(STATS_INTERVAL_S * 1000U));
        bool active;
        portENTER_CRITICAL(&s_cfg_mux);
        active = s_pa_active;
        portEXIT_CRITICAL(&s_cfg_mux);
        if (!active) {
            continue;
        }
        run_rec_t r;
        run_rec_fill(&r);
        run_rec_save(&r);
        run_rec_print("periodic", &r);
    }
}

/* ---------- 控制台 ---------- */

static void print_help(void)
{
    printf("\n"
           "commands:\n"
           "  f <hz>                     continuous tone 50..16000 Hz\n"
           "  v <0-100>                  volume %% (100 = 0 dBFS)\n"
           "  s | b | a | d | t          sweep | beep 2.7k | siren | dual 2.7k/1.8k | tone\n"
           "  w                          toggle sine/square\n"
           "  p <duty> [period_s] [b|a|d|t]\n"
           "                             periodic alarm: sound duty%% of each period\n"
           "                             (default period %u s, sound siren), amp off rest\n"
           "                             e.g. p 20 | p 50 10 b | p 100 (full-time)\n"
           "  p                          periodic status\n"
           "  last                       record of previous run (saved every %u s)\n"
           "  st                         current status\n"
           "  on | off | x               amp enable | stop everything & shutdown amp\n"
           "  help | ?                   this help\n\n",
           PERIOD_DEFAULT_S, STATS_INTERVAL_S);
}

static void print_status(void)
{
    bool active;
    uint32_t period_s;
    uint8_t duty;
    int64_t start_us;
    uint32_t gen;
    pa_snapshot(&active, &period_s, &duty, &start_us, &gen);
    ESP_LOGI(TAG, "status: amp %s, %s, mode %s, %s, freq %.0f Hz, vol %.0f%%, periodic %s", s_amp_on ? "ON" : "OFF",
             s_playing ? "playing" : "idle", mode_name(s_mode), s_square ? "square" : "sine", s_freq_hz,
             s_volume * 100.0f, active ? "running" : "off");
    if (active) {
        run_rec_t r;
        run_rec_fill(&r);
        run_rec_print("periodic", &r);
    }
}

static bool parse_long(const char *s, long lo, long hi, long *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (*end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_sound(const char *s, play_mode_t *out)
{
    if (strcmp(s, "b") == 0) {
        *out = MODE_BEEP;
    } else if (strcmp(s, "a") == 0) {
        *out = MODE_SIREN;
    } else if (strcmp(s, "d") == 0) {
        *out = MODE_DUAL;
    } else if (strcmp(s, "t") == 0) {
        *out = MODE_TONE;
    } else {
        return false;
    }
    return true;
}

static void cmd_periodic(int argc, char **argv)
{
    if (argc == 1) {
        bool active;
        uint32_t period_s;
        uint8_t duty;
        int64_t start_us;
        uint32_t gen;
        pa_snapshot(&active, &period_s, &duty, &start_us, &gen);
        if (!active) {
            ESP_LOGI(TAG, "periodic off. usage: p <duty 1-100> [period_s 1-%u] [b|a|d|t]", PERIOD_MAX_S);
            return;
        }
        run_rec_t r;
        run_rec_fill(&r);
        run_rec_print("periodic", &r);
        return;
    }
    long duty;
    long period = PERIOD_DEFAULT_S;
    play_mode_t mode = MODE_SIREN;
    if (!parse_long(argv[1], 1, 100, &duty)) {
        ESP_LOGW(TAG, "duty must be 1..100");
        return;
    }
    if (argc >= 3 && !parse_long(argv[2], 1, PERIOD_MAX_S, &period)) {
        ESP_LOGW(TAG, "period must be 1..%u s", PERIOD_MAX_S);
        return;
    }
    if (argc >= 4 && !parse_sound(argv[3], &mode)) {
        ESP_LOGW(TAG, "sound must be b|a|d|t");
        return;
    }
    (void)pa_stop();
    pa_start((uint8_t)duty, (uint32_t)period, mode);
}

static void handle_cmd(char *line)
{
    char *argv[4];
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok != NULL && argc < 4; tok = strtok(NULL, " \t")) {
        for (char *c = tok; *c != '\0'; c++) {
            *c = (char)tolower((unsigned char)*c);
        }
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }
    const char *cmd = argv[0];
    long val;

    if (strcmp(cmd, "f") == 0) {
        if (argc < 2 || !parse_long(argv[1], 50, 16000, &val)) {
            ESP_LOGW(TAG, "usage: f <50..16000>");
            return;
        }
        s_freq_hz = (float)val;
        play_mode(MODE_TONE);
        ESP_LOGI(TAG, "tone %ld Hz", val);
    } else if (strcmp(cmd, "v") == 0) {
        if (argc < 2 || !parse_long(argv[1], 0, 100, &val)) {
            ESP_LOGW(TAG, "usage: v <0..100>");
            return;
        }
        s_volume = (float)val / 100.0f;
        ESP_LOGI(TAG, "volume %ld%% (%.1f dBFS)", val, val > 0 ? 20.0f * log10f((float)val / 100.0f) : -INFINITY);
    } else if (strcmp(cmd, "s") == 0) {
        play_mode(MODE_SWEEP);
    } else if (strcmp(cmd, "b") == 0) {
        play_mode(MODE_BEEP);
        ESP_LOGI(TAG, "beep 2700 Hz");
    } else if (strcmp(cmd, "a") == 0) {
        play_mode(MODE_SIREN);
        ESP_LOGI(TAG, "siren 1200->3200 Hz");
    } else if (strcmp(cmd, "d") == 0) {
        play_mode(MODE_DUAL);
        ESP_LOGI(TAG, "dual tone 2700/1800 Hz");
    } else if (strcmp(cmd, "t") == 0) {
        play_mode(MODE_TONE);
        ESP_LOGI(TAG, "tone %.0f Hz", s_freq_hz);
    } else if (strcmp(cmd, "w") == 0) {
        s_square = !s_square;
        ESP_LOGI(TAG, "wave %s", s_square ? "square" : "sine");
    } else if (strcmp(cmd, "p") == 0) {
        cmd_periodic(argc, argv);
    } else if (strcmp(cmd, "last") == 0) {
        if (s_prev_valid) {
            run_rec_print("last run", &s_prev_run);
        } else {
            ESP_LOGI(TAG, "no previous run record");
        }
    } else if (strcmp(cmd, "st") == 0 || strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "on") == 0) {
        amp_set(true, true);
    } else if (strcmp(cmd, "off") == 0 || strcmp(cmd, "x") == 0) {
        (void)pa_stop();
        s_playing = false;
        amp_set(false, true);
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0 || strcmp(cmd, "h") == 0) {
        print_help();
    } else {
        ESP_LOGW(TAG, "unknown '%s', type help", cmd);
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[48];
    size_t len = 0U;
    uint8_t ch;
    for (;;) {
        if (uart_read_bytes(CONSOLE_UART, &ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (ch == '\n' && len == 0U) {
                continue; /* CRLF 的 LF 部分 */
            }
            uart_write_bytes(CONSOLE_UART, "\r\n", 2);
            line[len] = '\0';
            handle_cmd(line);
            len = 0U;
        } else if (ch == 0x08U || ch == 0x7FU) {
            if (len > 0U) {
                len--;
                uart_write_bytes(CONSOLE_UART, "\b \b", 3);
            }
        } else if (ch >= 0x20U && ch < 0x7FU) {
            if (len < sizeof(line) - 1U) {
                line[len++] = (char)ch;
                uart_write_bytes(CONSOLE_UART, &ch, 1); /* 回显 */
            }
            /* 超长字符直接忽略，不回显 */
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
    amp_set(false, false); /* I2S 时钟稳定前保持关断，减少上电爆音 */

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    run_rec_load();

    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));
    i2s_init();

    xTaskCreate(audio_task, "audio", 4096, NULL, 10, NULL);
    xTaskCreate(stats_task, "stats", 3072, NULL, 4, NULL);
    xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ready (muted, amp OFF). type help");
    if (s_prev_valid) {
        run_rec_print("last run", &s_prev_run);
    }
}
