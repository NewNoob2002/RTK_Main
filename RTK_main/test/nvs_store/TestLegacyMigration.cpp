// 用真实启动迁移、Registry 和 C6 双副本接口 对照旧 EEPROM 样本及 NVS 故障路径。
// 单进程单线程，每个 CTest 场景从空 NVS 开始。
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <source_location>
#include <span>
#include <string_view>

#include "FakeNvs.h"
#include "NvsStoreBoot.h"
#include "fw/Log.hpp"
#include "modules/storage/LegacyMigration.hpp"

// 下列偏移、宽度与期望字节独立引用 ADR，避免复制被测常量造成漏检。
// NOLINTBEGIN(readability-magic-numbers)
namespace {

using app::store::migrateLegacyEeprom;
using Blob = std::array<uint8_t, 2280>;
using Pn = std::array<uint8_t, NVS_STORE_PN_BYTES>;

unsigned s_scratch_calls = 0;
unsigned s_summary_logs = 0;
bool s_scratch_fails = false;

/** @brief 始终检查期望并报告调用位置，Release 构建同样执行。 */
void check(bool passed, std::source_location where = std::source_location::current())
{
    if (!passed) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): 主机失败诊断保留源行号。
        std::fprintf(stderr, "%s:%u: check failed\n", where.file_name(), where.line());
        std::abort();
    }
}

/** @brief 提供测试用启动缓冲，可模拟分配失败。 */
esp_err_t withScratch(nvs_store_recover_fn read)
{
    ++s_scratch_calls;
    if (s_scratch_fails) {
        return read(nullptr, 0U);
    }
    std::array<uint8_t, NVS_STORE_EEPROM_MAX> buffer{};
    return read(buffer.data(), buffer.size());
}

/** @brief 调用固件实际迁移入口，模拟 cfg 和系统实例的初始化流程。 */
void boot()
{
    nvs_store_set_boot_scratch(&withScratch);
    nvs_store_set_boot_migration(&migrateLegacyEeprom);
    check(nvs_store_init() == ESP_OK);
}

/** @brief 以只读或读写方式打开测试所需命名空间。 */
nvs_handle_t open(const char *partition, const char *ns, nvs_open_mode_t mode)
{
    nvs_handle_t handle = 0U;
    check(nvs_open_from_partition(partition, ns, mode, &handle) == ESP_OK);
    return handle;
}

/** @brief 放入旧 EEPROM blob，保持 NVS 实例可供启动读取。 */
void seed(std::span<const uint8_t> blob)
{
    check(nvs_store_init() == ESP_OK);
    const auto handle = open(NVS_STORE_SYSTEM_PARTITION, "eeprom", NVS_READWRITE);
    check(nvs_set_blob(handle, "eeprom", blob.data(), blob.size()) == ESP_OK);
    check(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
}

/** @brief 加载任务随附的真机原始 blob。 */
Blob loadGolden(const char *path)
{
    Blob blob{};
    auto *file = std::fopen(path, "rb");
    check(file != nullptr);
    check(std::fread(blob.data(), 1U, blob.size(), file) == blob.size());
    check(std::fgetc(file) == EOF);
    check(std::fclose(file) == 0);
    return blob;
}

/** @brief 检查 cfg 的指定键不存在。 */
void expectMissing(const char *ns, const char *key)
{
    nvs_handle_t handle = 0U;
    auto err = nvs_store_open_cfg(ns, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_find_key(handle, key, nullptr);
        nvs_close(handle);
    }
    check(err == ESP_ERR_NVS_NOT_FOUND);
}

/** @brief 检查迁移结束标记的完整值。 */
void expectDone(std::string_view expected)
{
    const auto handle = open(NVS_STORE_CFG_PARTITION, "mig", NVS_READONLY);
    std::array<char, 8> value{};
    size_t len = value.size();
    check(nvs_get_str(handle, "done", value.data(), &len) == ESP_OK);
    check(len == expected.size() + 1U && value.data() == expected);
    nvs_close(handle);
}

/** @brief 检查一份已提交的定长 blob 值。 */
void expectBlob(const char *partition, const char *ns, const char *key,
                std::span<const uint8_t> expected)
{
    const auto handle = open(partition, ns, NVS_READONLY);
    std::array<uint8_t, NVS_STORE_EEPROM_MAX> value{};
    size_t len = value.size();
    check(nvs_get_blob(handle, key, value.data(), &len) == ESP_OK);
    check(std::ranges::equal(expected, std::span{value}.first(len)));
    nvs_close(handle);
}

/** @brief 检查一次成功迁移的来源长度和三项计数。 */
void expectStats(uint16_t sourceLen, std::array<uint8_t, 3> expected)
{
    const auto handle = open(NVS_STORE_CFG_PARTITION, "mig", NVS_READONLY);
    uint16_t len = 0U;
    check(nvs_get_u16(handle, "src_len", &len) == ESP_OK && len == sourceLen);
    nvs_close(handle);
    expectBlob(NVS_STORE_CFG_PARTITION, "mig", "stats", expected);
}

/** @brief 分别读两个底层 NVS 实例，确认身份完全相同。 */
void expectIdentity(std::string_view sn, const Pn &pn)
{
    for (const bool system : {false, true}) {
        const char *partition = system ? NVS_STORE_SYSTEM_PARTITION : NVS_STORE_CFG_PARTITION;
        const char *ns = system ? NVS_STORE_BACKUP_NAMESPACE : NVS_STORE_FACTORY_NAMESPACE;
        const auto handle = open(partition, ns, NVS_READONLY);
        std::array<char, NVS_STORE_SN_MAX + 1U> value{};
        size_t len = value.size();
        check(nvs_get_str(handle, "sn", value.data(), &len) == ESP_OK);
        check(len == sn.size() + 1U && value.data() == sn);
        nvs_close(handle);
        expectBlob(partition, ns, "pn", pn);
    }
}

constexpr Pn kGoldenPn{'1', '3', '2', '2', '1', '5', '0', '3', '0', '1', '0', '3', '2'};

/** @brief 以真机样本扩展到 2560 B，确认 V3.1 长度分支及源字节保留。 */
void checkV31(const Blob &golden)
{
    std::array<uint8_t, 2560> v31{};
    v31.fill(0xFFU);
    std::ranges::copy(golden, v31.begin());
    seed(v31);
    boot();
    expectDone("v1");
    expectStats(2560U, {2U, 0U, 0U});
    expectIdentity("202061126", kGoldenPn);
    expectBlob(NVS_STORE_SYSTEM_PARTITION, "eeprom", "eeprom", v31);
}

/** @brief SN/PN 满宽时保留 16 字节，SN 的字符串终止符在目标中追加。 */
void checkBoundaries()
{
    Blob blob{};
    blob.fill(0xFFU);
    std::ranges::fill(std::span{blob}.subspan(200U, 16U), 'S');
    std::ranges::fill(std::span{blob}.subspan(216U, 16U), 'P');
    Pn pn{};
    pn.fill('P');
    seed(blob);
    boot();
    expectDone("v1");
    expectStats(2280U, {2U, 0U, 0U});
    expectIdentity("SSSSSSSSSSSSSSSS", pn);
}

/** @brief 验证非法身份与未设置的控制字符或空串计数。 */
void checkText(std::string_view scenario)
{
    Blob blob{};
    blob.fill(0xFFU);
    if (scenario == "invalid") {
        std::ranges::copy(std::string_view{"a b"}, std::span{blob}.subspan(200U).begin());
        blob[203U] = 0U;
        blob[216U] = 0x80U;
    } else {
        blob[200U] = '\n';
        blob[216U] = 0U;
    }
    seed(blob);
    boot();
    expectDone("v1");
    if (scenario == "invalid") {
        expectStats(2280U, {0U, 0U, 2U});
    } else {
        expectStats(2280U, {0U, 2U, 0U});
    }
    expectMissing(NVS_STORE_FACTORY_NAMESPACE, "sn");
    expectMissing(NVS_STORE_FACTORY_NAMESPACE, "pn");
}

/** @brief 预存出厂身份，验证原值优先。 */
void checkPreserve(const Blob &golden)
{
    seed(golden);
    constexpr Pn kNewPn{'N', 'E', 'W'};
    check(nvs_store_set_sn("NEW-SN") == ESP_OK);
    check(nvs_store_set_pn(kNewPn.data()) == ESP_OK);
    boot();
    expectDone("v1");
    expectStats(2280U, {0U, 2U, 0U});
    expectIdentity("NEW-SN", kNewPn);
}

/** @brief cfg 中存在异类型 SN 时保留原键，并继续迁移 PN。 */
void checkExistingType(const Blob &golden)
{
    seed(golden);
    auto handle = open(NVS_STORE_CFG_PARTITION, NVS_STORE_FACTORY_NAMESPACE, NVS_READWRITE);
    check(nvs_set_u8(handle, "sn", 9U) == ESP_OK);
    check(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
    boot();
    expectDone("v1");
    expectStats(2280U, {1U, 1U, 0U});
    handle = open(NVS_STORE_CFG_PARTITION, NVS_STORE_FACTORY_NAMESPACE, NVS_READONLY);
    uint8_t value = 0U;
    check(nvs_get_u8(handle, "sn", &value) == ESP_OK && value == 9U);
    nvs_close(handle);
    expectBlob(NVS_STORE_CFG_PARTITION, NVS_STORE_FACTORY_NAMESPACE, "pn", kGoldenPn);
    expectBlob(NVS_STORE_SYSTEM_PARTITION, NVS_STORE_BACKUP_NAMESPACE, "pn", kGoldenPn);
}

/** @brief 完成标记存在时即使源仍存在也不申请缓冲或重写。 */
void expectNoRepeat()
{
    const auto writes = fake_nvs_writes();
    const auto buffers = s_scratch_calls;
    boot();
    check(fake_nvs_writes() == writes && s_scratch_calls == buffers);
}

/** @brief 校验中途写入、读回、标记提交、分配失败均可重试。 */
void checkFailure(std::string_view scenario, const Blob &golden)
{
    seed(golden);
    std::array<uint8_t, 3> expected{0U, 2U, 0U};
    if (scenario == "write_failure") {
        fake_nvs_fail_key_write("pn");
        expected = {1U, 1U, 0U};
    } else if (scenario == "readback_failure") {
        fake_nvs_corrupt_key_read("pn");
    } else if (scenario == "marker_failure") {
        fake_nvs_fail_key_commit("done");
    } else {
        s_scratch_fails = true;
        expected = {2U, 0U, 0U};
    }
    boot();
    expectMissing(NVS_STORE_MIG_NAMESPACE, "done");
    check(s_summary_logs == 0U);
    if (scenario == "write_failure") {
        std::array<char, NVS_STORE_SN_MAX + 1U> sn{};
        check(nvs_store_get_sn(sn.data(), sn.size()) == ESP_OK);
        check(std::string_view{sn.data()} == "202061126");
        expectMissing(NVS_STORE_FACTORY_NAMESPACE, "pn");
        check(nvs_store_set_sn("UPDATED") == ESP_OK);
    }
    s_scratch_fails = false;
    boot();
    expectDone("v1");
    expectStats(2280U, expected);
    check(s_summary_logs == 1U && s_scratch_calls == 2U);
    expectIdentity(scenario == "write_failure" ? "UPDATED" : "202061126", kGoldenPn);
    expectNoRepeat();
}

/** @brief 无源、不支持长度、全空源均只按其约定提交元数据。 */
void checkEmpty(std::string_view scenario)
{
    std::array<uint8_t, 2561> empty{};
    empty.fill(0xFFU);
    if (scenario == "none") {
        boot();
        expectDone("none");
    } else if (scenario == "bad_len" || scenario == "bad_len_large") {
        seed(std::span{empty}.first(scenario == "bad_len" ? 2279U : empty.size()));
        boot();
        expectDone("bad_len");
    } else {
        seed(std::span{empty}.first(2280U));
        boot();
        expectDone("v1");
        expectStats(2280U, {0U, 2U, 0U});
    }
    check(s_scratch_calls == (scenario == "unset" ? 1U : 0U));
    expectMissing(NVS_STORE_FACTORY_NAMESPACE, "sn");
    expectMissing(NVS_STORE_FACTORY_NAMESPACE, "pn");
    expectNoRepeat();
}

/** @brief 金样验证以及恢复出厂后保留迁移标记和身份的验证。 */
void checkGolden(const Blob &golden, bool reset)
{
    seed(golden);
    boot();
    expectDone("v1");
    expectStats(2280U, {2U, 0U, 0U});
    expectIdentity("202061126", kGoldenPn);
    expectBlob(NVS_STORE_SYSTEM_PARTITION, "eeprom", "eeprom", golden);
    if (reset) {
        const auto handle = open(NVS_STORE_CFG_PARTITION, "rtk", NVS_READWRITE);
        check(nvs_set_u8(handle, "pair_next", 1U) == ESP_OK);
        check(nvs_commit(handle) == ESP_OK);
        nvs_close(handle);
        check(nvs_store_erase_user() == ESP_OK);
        expectNoRepeat();
        expectDone("v1");
        expectIdentity("202061126", kGoldenPn);
        expectBlob(NVS_STORE_SYSTEM_PARTITION, "eeprom", "eeprom", golden);
        expectMissing("rtk", "pair_next");
    }
    expectNoRepeat();
}

} // namespace

namespace fw {

/** @brief 捕获迁移摘要，不启用主机日志设施。 */
void logLine(LogLevel, TraceCat cat, const char *fmt, ...) noexcept
{
    if (cat == TraceCat::kStore && std::string_view{fmt}.starts_with("Legacy config migrated:")) {
        ++s_summary_logs;
    }
}

} // namespace fw

/** @brief 每个场景独立运行以隔离 NVS 替身状态。 */
// NOLINTNEXTLINE(misc-const-correctness): main 的标准签名。
int main(int argc, char **argv)
{
    check(argc == 3);
    const auto args = std::span{argv, size_t{3U}};
    const std::string_view scenario{args[1U]};
    if (scenario == "none" || scenario == "unset" || scenario.starts_with("bad_len")) {
        checkEmpty(scenario);
    } else if (scenario == "boundaries") {
        checkBoundaries();
    } else if (scenario == "invalid" || scenario == "control_text") {
        checkText(scenario);
    } else {
        const auto golden = loadGolden(args[2U]);
        if (scenario == "v31") {
            checkV31(golden);
        } else if (scenario == "preserve") {
            checkPreserve(golden);
        } else if (scenario == "existing_type") {
            checkExistingType(golden);
        } else if (scenario.ends_with("_failure")) {
            checkFailure(scenario, golden);
        } else {
            check(scenario == "golden" || scenario == "freset");
            checkGolden(golden, scenario == "freset");
        }
    }
}
// NOLINTEND(readability-magic-numbers)
