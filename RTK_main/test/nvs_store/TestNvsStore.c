// 真实 nvs_store 在双实例内存替身上的故障与保留策略回归。
// 每个场景由 CTest 启动一个新进程，只在主机单线程运行。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FakeNvs.h"
#include "NvsStore.h"

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

static const uint8_t k_pn[NVS_STORE_PN_BYTES] = "0015190311111";
static const uint8_t k_other_pn[NVS_STORE_PN_BYTES] = "0015150300000";
static const char *const k_cfg = NVS_STORE_CFG_PARTITION;
static const char *const k_system = NVS_STORE_SYSTEM_PARTITION;
static const char *const k_fac = NVS_STORE_FACTORY_NAMESPACE;
static const char *const k_backup = NVS_STORE_BACKUP_NAMESPACE;

/** @brief 以读写模式为测试预填命名空间。 */
static nvs_handle_t seed_handle(const char *partition, const char *ns)
{
    nvs_handle_t handle = 0;
    CHECK(nvs_open_from_partition(partition, ns, NVS_READWRITE, &handle) == ESP_OK);
    return handle;
}

/** @brief 在任一分区预填原始 blob。 */
static void seed_blob(const char *partition, const char *ns, const char *key, const void *bytes,
                      size_t len)
{
    const nvs_handle_t handle = seed_handle(partition, ns);
    CHECK(nvs_set_blob(handle, key, bytes, len) == ESP_OK);
    CHECK(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
}

/** @brief 在任一分区读回或确认缺失。 */
static void expect_blob(const char *partition, const char *ns, const char *key,
                        const void *expected, size_t len)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(partition, ns, NVS_READONLY, &handle);
    size_t size = 0;
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, key, NULL, &size);
    }
    if (expected != NULL) {
        uint8_t bytes[2560];
        CHECK(err == ESP_OK && size == len && size <= sizeof(bytes));
        CHECK(nvs_get_blob(handle, key, bytes, &size) == ESP_OK);
        CHECK(memcmp(bytes, expected, len) == 0);
    } else {
        CHECK(err == ESP_ERR_NVS_NOT_FOUND);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
}

/** @brief 预填某个身份副本。 */
static void seed_identity(bool cfg, const char *sn, const uint8_t *pn)
{
    const nvs_handle_t handle = seed_handle(cfg ? k_cfg : k_system, cfg ? k_fac : k_backup);
    CHECK(nvs_set_str(handle, "sn", sn) == ESP_OK);
    CHECK(nvs_set_blob(handle, "pn", pn, NVS_STORE_PN_BYTES) == ESP_OK);
    CHECK(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
}

/** @brief 检查两个实例的 SN/PN 实际存储与主入口读取。 */
static void expect_identity(const char *sn, const uint8_t *pn)
{
    for (unsigned cfg = 0; cfg < 2U; ++cfg) {
        const char *partition = cfg ? k_cfg : k_system;
        const char *ns = cfg ? k_fac : k_backup;
        nvs_handle_t handle = 0;
        char text[NVS_STORE_SN_MAX + 1U] = {0};
        size_t len = sizeof(text);
        CHECK(nvs_open_from_partition(partition, ns, NVS_READONLY, &handle) == ESP_OK);
        const esp_err_t err = nvs_get_str(handle, "sn", text, &len);
        CHECK(sn != NULL ? err == ESP_OK && strcmp(text, sn) == 0 : err == ESP_ERR_NVS_NOT_FOUND);
        nvs_close(handle);
        expect_blob(partition, ns, "pn", pn, NVS_STORE_PN_BYTES);
    }
    char sn_text[NVS_STORE_SN_MAX + 1U] = {0};
    uint8_t pn_bytes[NVS_STORE_PN_BYTES] = {0};
    const esp_err_t sn_err = nvs_store_get_sn(sn_text, sizeof(sn_text));
    const esp_err_t pn_err = nvs_store_get_pn(pn_bytes);
    CHECK(sn != NULL ? sn_err == ESP_OK && strcmp(sn_text, sn) == 0
                     : sn_err == ESP_ERR_NVS_NOT_FOUND && sn_text[0] == '\0');
    CHECK(pn != NULL ? pn_err == ESP_OK && memcmp(pn_bytes, pn, sizeof(pn_bytes)) == 0
                     : pn_err == ESP_ERR_NVS_NOT_FOUND);
}

/** @brief 覆盖已有、缺失、损坏与不一致，且相同值不重复写。 */
static void test_copies(const char *scenario)
{
    if (strcmp(scenario, "missing_cfg") != 0) {
        seed_identity(true, "CFG", k_pn);
    }
    if (strcmp(scenario, "missing_system") != 0) {
        const bool different = strcmp(scenario, "different") == 0;
        seed_identity(false, different ? "BACKUP" : "CFG", different ? k_other_pn : k_pn);
    }
    if (strcmp(scenario, "corrupt") == 0) {
        seed_blob(k_cfg, k_fac, "sn", k_other_pn, sizeof(k_other_pn));
        seed_blob(k_system, k_backup, "pn", k_other_pn, 1U);
    }
    CHECK(nvs_store_init() == ESP_OK);
    expect_identity("CFG", k_pn);
    CHECK(fake_nvs_warnings() == (strcmp(scenario, "different") == 0 ? 2U : 0U));
    const unsigned writes = fake_nvs_writes();
    CHECK(nvs_store_init() == ESP_OK);
    CHECK(fake_nvs_writes() == writes);

    CHECK(nvs_store_set_sn("NEW") == ESP_OK);
    CHECK(nvs_store_set_pn(k_other_pn) == ESP_OK);
    expect_identity("NEW", k_other_pn);
}

/** @brief 主机提供启动用的临时恢复缓冲。 */
static esp_err_t with_scratch(nvs_store_recover_fn recover)
{
    uint8_t blob[NVS_STORE_EEPROM_MAX];
    return recover(blob, sizeof(blob));
}

/** @brief 冷启动初始化失败只擦本实例，用另一侧修复身份。 */
static void test_recovery(bool cfg, esp_err_t init_error)
{
    uint8_t eeprom[NVS_STORE_EEPROM_MAX] = {0xDE, 0xAD};
    eeprom[sizeof(eeprom) - 1U] = 0x5AU;
    seed_identity(!cfg, "SERIAL", k_pn);
    seed_identity(cfg, "STALE", k_other_pn);
    seed_blob(k_cfg, "rtk", "marker", k_pn, 1U);
    seed_blob(k_system, "phy", "marker", k_pn, 1U);
    seed_blob(k_system, "eeprom", "eeprom", eeprom, sizeof(eeprom));
    nvs_store_set_boot_scratch(with_scratch);
    fake_nvs_fail_init(cfg, init_error);
    CHECK(nvs_store_init() == ESP_OK);
    CHECK(fake_nvs_erases(cfg) == 1U && fake_nvs_erases(!cfg) == 0U && fake_nvs_errors() > 0U);
    CHECK(fake_nvs_eeprom_loss_logs() == (cfg ? 0U : 1U));
    expect_identity("SERIAL", k_pn);
    expect_blob(k_cfg, "rtk", "marker", cfg ? NULL : k_pn, 1U);
    expect_blob(k_system, "phy", "marker", cfg ? k_pn : NULL, 1U);
    expect_blob(k_system, "eeprom", "eeprom", cfg ? eeprom : NULL, sizeof(eeprom));
}

/** @brief 跨启动完成 PN 搬迁，删除失败时保留旧键并在重试时不覆盖权威副本。 */
static void test_legacy_pn(void)
{
    seed_blob(k_cfg, "rtk", "pn", k_pn, sizeof(k_pn));
    fake_nvs_fail_write(true, ESP_ERR_NO_MEM);
    CHECK(nvs_store_init() == ESP_OK);
    expect_blob(k_cfg, "rtk", "pn", k_pn, sizeof(k_pn));
    expect_blob(k_system, k_backup, "pn", NULL, 0U);
    fake_nvs_fail_delete();
    CHECK(nvs_store_init() == ESP_OK);
    expect_identity(NULL, k_pn);
    expect_blob(k_cfg, "rtk", "pn", k_pn, sizeof(k_pn));
    CHECK(nvs_store_set_pn(k_other_pn) == ESP_OK);
    const unsigned writes = fake_nvs_writes();
    CHECK(nvs_store_init() == ESP_OK);
    CHECK(fake_nvs_writes() == writes);
    expect_blob(k_cfg, "rtk", "pn", NULL, 0U);
    expect_identity(NULL, k_other_pn);
}

/** @brief 双写主副本失败时维持旧值，备份先失败时不写主副本。 */
static void test_failed_write(void)
{
    seed_identity(true, "OLD", k_pn);
    seed_identity(false, "OLD", k_pn);
    fake_nvs_fail_write(true, ESP_ERR_NO_MEM);
    CHECK(nvs_store_set_sn("NEW") == ESP_ERR_NO_MEM);
    fake_nvs_fail_write(false, ESP_ERR_NO_MEM);
    CHECK(nvs_store_set_pn(k_other_pn) == ESP_ERR_NO_MEM);
    expect_identity("OLD", k_pn);
    char out[] = "invalid";
    CHECK(nvs_store_get_sn(out, sizeof(out)) == ESP_ERR_INVALID_ARG && out[0] == '\0');
}

/** @brief 核对用户与出厂擦除范围，两个 NVS 实例都保持挂载。 */
static void test_reset(bool factory)
{
    const char *const user[] = {"gnss", "rtk", "link", "dbg", "boot", "net", "ntrip", "fence"};
    const char *const radio[] = {"bt_config.conf", "bt_config", "nvs.net80211"};
    seed_identity(true, "KEEP", k_pn);
    seed_identity(false, "KEEP", k_pn);
    seed_blob(k_cfg, k_fac, "reg", k_pn, 1U);
    seed_blob(k_cfg, "mig", "done", k_pn, 1U);
    seed_blob(k_system, "phy", "marker", k_pn, 1U);
    seed_blob(k_system, "eeprom", "eeprom", k_pn, sizeof(k_pn));
    for (size_t i = 0; i < sizeof(user) / sizeof(user[0]); ++i) {
        seed_blob(k_cfg, user[i], "marker", k_pn, 1U);
    }
    for (size_t i = 0; i < sizeof(radio) / sizeof(radio[0]); ++i) {
        seed_blob(k_system, radio[i], "marker", k_pn, 1U);
    }
    CHECK(nvs_store_erase_user() == ESP_OK);
    if (factory) {
        CHECK(nvs_store_erase_factory() == ESP_OK);
    }
    CHECK(nvs_store_init() == ESP_OK);
    CHECK(fake_nvs_erases(true) == 0U && fake_nvs_erases(false) == 0U);
    for (size_t i = 0; i < sizeof(user) / sizeof(user[0]); ++i) {
        expect_blob(k_cfg, user[i], "marker", NULL, 0U);
    }
    for (size_t i = 0; i < sizeof(radio) / sizeof(radio[0]); ++i) {
        expect_blob(k_system, radio[i], "marker", NULL, 0U);
    }
    expect_identity(factory ? NULL : "KEEP", factory ? NULL : k_pn);
    expect_blob(k_cfg, k_fac, "reg", factory ? NULL : k_pn, 1U);
    expect_blob(k_cfg, "mig", "done", k_pn, 1U);
    expect_blob(k_system, "phy", "marker", k_pn, 1U);
    expect_blob(k_system, "eeprom", "eeprom", k_pn, sizeof(k_pn));
}

/** @brief 验证白名单仍用 cfg 的 rtk 命名空间。 */
static void test_whitelist(void)
{
    const uint8_t addr[][NVS_STORE_ADDR_LEN] = {{1U, 2U}};
    uint8_t out[1][NVS_STORE_ADDR_LEN] = {0};
    size_t count = 0;
    CHECK(nvs_store_set_alarms(addr, 1U) == ESP_OK);
    CHECK(nvs_store_get_alarms(out, 1U, &count) == ESP_OK);
    CHECK(count == 1U && memcmp(out[0], addr[0], sizeof(out[0])) == 0);
    expect_blob(k_system, "rtk", "alarm_wl", NULL, 0U);
}

/** @brief CTest 场景入口。 */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    CHECK(nvs_store_init() == ESP_OK);
    const char *name = argv[1];
    if (strcmp(name, "cfg_recovery") == 0) {
        test_recovery(true, ESP_ERR_NVS_NEW_VERSION_FOUND);
    } else if (strcmp(name, "system_recovery") == 0) {
        test_recovery(false, ESP_ERR_NVS_NO_FREE_PAGES);
    } else if (strcmp(name, "system_recovery_new_version") == 0) {
        test_recovery(false, ESP_ERR_NVS_NEW_VERSION_FOUND);
    } else if (strcmp(name, "legacy_pn") == 0) {
        test_legacy_pn();
    } else if (strcmp(name, "write_failure") == 0) {
        test_failed_write();
    } else if (strcmp(name, "freset") == 0 || strcmp(name, "factory_erase") == 0) {
        test_reset(strcmp(name, "factory_erase") == 0);
    } else if (strcmp(name, "whitelist") == 0) {
        test_whitelist();
    } else {
        test_copies(name);
    }
    return EXIT_SUCCESS;
}
