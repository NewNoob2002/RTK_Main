// NVS 内存替身的故障注入和观察接口，单线程测试使用。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

void fake_nvs_fail_init(bool cfg, esp_err_t error, bool readable);
void fake_nvs_fail_write(bool cfg, esp_err_t error);
void fake_nvs_fail_delete(void);
unsigned fake_nvs_erases(bool cfg);
unsigned fake_nvs_writes(void);
unsigned fake_nvs_warnings(void);
unsigned fake_nvs_errors(void);
