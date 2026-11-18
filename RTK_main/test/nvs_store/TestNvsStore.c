// 回归真实 nvs_store 的双分区启动与 SN 迁移。
// 每个场景由 CTest 启动一个新进程，只在主机单线程运行。
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FakeNvs.h"
#include "nvs_flash.h"
#include "NvsStore.h"

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

static const char k_legacy[] = "LEGACY123";
static const char k_factory[] = "FACTORY88";

/** @brief 核对组件读出的有效 SN。 */
static void expect_sn(const char *expected)
{
    char out[NVS_STORE_SN_MAX + 1U] = {0};
    CHECK(nvs_store_get_sn(out, sizeof(out)) == ESP_OK);
    CHECK(strcmp(out, expected) == 0);
}

/** @brief 校验迁移、白名单跨启动保留及第二次启动的幂等性。 */
static void test_success(void)
{
    CHECK(nvs_flash_init() == ESP_OK);
    const uint8_t addr[][NVS_STORE_ADDR_LEN] = {{1U, 2U}};
    CHECK(nvs_store_set_alarms(addr, 1U) == ESP_OK);
    fake_nvs_seed(false, k_legacy);

    CHECK(nvs_store_init() == ESP_OK);
    CHECK(nvs_store_factory_available());
    expect_sn(k_legacy);
    CHECK(fake_nvs_has_sn(true, k_legacy));
    CHECK(fake_nvs_has_sn(false, NULL));
    CHECK(fake_nvs_migrations() == 1U);

    CHECK(nvs_store_init() == ESP_OK);
    expect_sn(k_legacy);
    CHECK(fake_nvs_migrations() == 1U);
    uint8_t saved[1][NVS_STORE_ADDR_LEN] = {0};
    size_t count = 0U;
    CHECK(nvs_store_get_alarms(saved, 1U, &count) == ESP_OK);
    CHECK(count == 1U && memcmp(addr[0], saved[0], sizeof(saved[0])) == 0);
}

/** @brief 故障时保留旧键，下次启动可完成。 */
static void test_retry(fake_nvs_fault_t fault)
{
    fake_nvs_seed(false, k_legacy);
    fake_nvs_inject(fault);
    CHECK(nvs_store_init() == ESP_OK);
    CHECK(nvs_store_factory_available());
    CHECK(fake_nvs_has_sn(false, k_legacy));
    CHECK(fake_nvs_migrations() == 0U && fake_nvs_warnings() == 1U);
    expect_sn(k_legacy);

    CHECK(nvs_store_init() == ESP_OK);
    expect_sn(k_legacy);
    CHECK(fake_nvs_has_sn(true, k_legacy) && fake_nvs_has_sn(false, NULL));
    CHECK(fake_nvs_migrations() == 1U);
}

/** @brief 出厂初始化失败时回退读取，出厂写入明确失败。 */
static void test_unavailable(void)
{
    fake_nvs_seed(false, k_legacy);
    fake_nvs_seed(true, k_factory);
    fake_nvs_inject(FAKE_NVS_FACTORY_INIT);
    CHECK(nvs_store_init() == ESP_OK);
    CHECK(!nvs_store_factory_available() && fake_nvs_user_erases() == 0U);
    expect_sn(k_legacy);
    CHECK(nvs_store_set_sn(k_legacy) == ESP_ERR_INVALID_STATE);
    CHECK(fake_nvs_has_sn(true, k_factory));
}

/** @brief 参数非法与后端弄脏输出后返回错误时都清零可写的首字节。 */
static void test_clear_output(void)
{
    fake_nvs_seed(true, k_factory);
    CHECK(nvs_store_init() == ESP_OK);
    char out[NVS_STORE_SN_MAX + 1U] = "dirty";
    CHECK(nvs_store_get_sn(out, 1U) == ESP_ERR_INVALID_ARG && out[0] == '\0');
    fake_nvs_inject(FAKE_NVS_DIRTY_READ);
    CHECK(nvs_store_get_sn(out, sizeof(out)) == ESP_FAIL && out[0] == '\0');
}

/** @brief 用户分区擦除重试不影响出厂 SN。 */
static void test_erase_isolation(void)
{
    fake_nvs_seed(false, k_legacy);
    fake_nvs_seed(true, k_factory);
    fake_nvs_inject(FAKE_NVS_USER_INIT);
    CHECK(nvs_store_init() == ESP_OK && fake_nvs_user_erases() == 1U);
    CHECK(fake_nvs_has_sn(false, NULL));
    expect_sn(k_factory);
}

/** @brief 正常出厂 SN 优先于旧键，写入只改出厂分区。 */
static void test_existing_factory(void)
{
    fake_nvs_seed(false, k_legacy);
    fake_nvs_seed(true, k_factory);
    CHECK(nvs_store_init() == ESP_OK);
    expect_sn(k_factory);
    CHECK(fake_nvs_migrations() == 0U);
    CHECK(nvs_store_set_sn("NEW8") == ESP_OK);
    CHECK(fake_nvs_has_sn(false, k_legacy) && fake_nvs_has_sn(true, "NEW8"));
}

/** @brief 出厂显式写入后不再重试旧 SN 的迁移。 */
static void test_override(void)
{
    fake_nvs_seed(false, k_legacy);
    fake_nvs_inject(FAKE_NVS_DELETE);
    CHECK(nvs_store_init() == ESP_OK);
    CHECK(nvs_store_set_sn(k_factory) == ESP_OK);
    CHECK(nvs_store_init() == ESP_OK);
    expect_sn(k_factory);
    CHECK(fake_nvs_has_sn(false, k_legacy));
}

/** @brief CTest 场景入口。 */
static void test_explicit_erase(bool factory)
{
    fake_nvs_seed(true, k_factory);
    CHECK(nvs_store_init() == ESP_OK);
    fake_nvs_seed(false, k_legacy);
    esp_err_t (*const erase)(void) = factory ? nvs_store_erase_factory : nvs_store_erase_user;
    fake_nvs_inject(FAKE_NVS_ERASE);
    CHECK(erase() == ESP_FAIL);
    CHECK(fake_nvs_has_sn(true, k_factory) && fake_nvs_has_sn(false, k_legacy));
    CHECK(erase() == ESP_OK);
    CHECK(fake_nvs_has_sn(true, factory ? NULL : k_factory));
    CHECK(fake_nvs_has_sn(false, factory ? k_legacy : NULL));
}

/** @brief CTest 场景入口。 */
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const char *name = argv[1];
    if (strcmp(name, "success") == 0) {
        test_success();
    } else if (strcmp(name, "write_failure") == 0) {
        test_retry(FAKE_NVS_WRITE);
    } else if (strcmp(name, "verify_mismatch") == 0) {
        test_retry(FAKE_NVS_MISMATCH);
    } else if (strcmp(name, "delete_failure") == 0) {
        test_retry(FAKE_NVS_DELETE);
    } else if (strcmp(name, "factory_unavailable") == 0) {
        test_unavailable();
    } else if (strcmp(name, "clear_output") == 0) {
        test_clear_output();
    } else if (strcmp(name, "erase_isolation") == 0) {
        test_erase_isolation();
    } else if (strcmp(name, "existing_factory") == 0) {
        test_existing_factory();
    } else if (strcmp(name, "factory_override") == 0) {
        test_override();
    } else if (strcmp(name, "commit_failure") == 0) {
        test_retry(FAKE_NVS_COMMIT);
    } else if (strcmp(name, "erase_user") == 0) {
        test_explicit_erase(false);
    } else if (strcmp(name, "erase_factory") == 0) {
        test_explicit_erase(true);
    } else {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
