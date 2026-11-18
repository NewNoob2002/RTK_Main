// NVS 内存替身的故障注入和观察接口，单线程测试使用。
#pragma once

#include <stdbool.h>

typedef enum {
    FAKE_NVS_OK,
    FAKE_NVS_WRITE,
    FAKE_NVS_COMMIT,
    FAKE_NVS_MISMATCH,
    FAKE_NVS_DELETE,
    FAKE_NVS_FACTORY_INIT,
    FAKE_NVS_USER_INIT,
    FAKE_NVS_DIRTY_READ,
} fake_nvs_fault_t;

void fake_nvs_inject(fake_nvs_fault_t fault);
void fake_nvs_seed(bool factory, const char *sn);
bool fake_nvs_has_sn(bool factory, const char *sn);
unsigned fake_nvs_user_erases(void);
unsigned fake_nvs_migrations(void);
unsigned fake_nvs_warnings(void);
