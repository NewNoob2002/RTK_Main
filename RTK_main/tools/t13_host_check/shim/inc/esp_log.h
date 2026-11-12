#pragma once

/* 日志在 host 上直接写 stderr；回归只看驱动行为，不看日志文本。 */
void fake_log(const char *level, const char *tag, const char *fmt, ...);

#define ESP_LOGE(tag, fmt, ...) fake_log("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fake_log("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fake_log("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) fake_log("D", tag, fmt, ##__VA_ARGS__)
