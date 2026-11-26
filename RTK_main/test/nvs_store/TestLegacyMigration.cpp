// 用真实启动迁移、Registry 和 StoreKeys 对照旧 EEPROM 样本及 NVS 故障路径。
// 单进程单线程，每个 CTest 场景从空 NVS 开始。
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <source_location>
#include <span>
#include <string_view>

#include "FakeNvs.h"
#include "NvsStoreBoot.h"
#include "fw/Log.hpp"
#include "modules/storage/LegacyEeprom.hpp"
#include "modules/storage/LegacyMigration.hpp"
#include "rtos/EspStatus.hpp"

// 下列偏移、宽度与期望字节独立引用 ADR 和手机协议，避免复制被测常量造成漏检。
// NOLINTBEGIN(readability-magic-numbers)
namespace {

using app::store::decodeLegacyEeprom;
using app::store::migrateLegacyEeprom;
using Blob = std::array<uint8_t, 2280>;

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

/** @brief 解码一个合法长度的样本，确保每条检查访问的是实际解码结果。 */
app::store::LegacyFields decode(const Blob &blob, uint8_t rate)
{
    const auto fields = decodeLegacyEeprom(blob, rate);
    if (!fields) {
        std::abort();
    }
    return *fields;
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

/** @brief 按旧 EEPROM 小端格式设置一个 32 位字段。 */
void setLe32(std::span<uint8_t> blob, size_t offset, uint32_t value)
{
    for (auto &byte : blob.subspan(offset, 4U)) {
        byte = static_cast<uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
}

/** @brief 向擦除态或样本 blob 注入以 NUL 结尾的文本。 */
void setText(std::span<uint8_t> blob, size_t offset, std::string_view text)
{
    std::memcpy(blob.subspan(offset).data(), text.data(), text.size());
    blob[offset + text.size()] = 0U;
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
void expectIdentity(std::string_view sn, const core::PhonePn &pn)
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

constexpr core::PhonePn kGoldenPn{'1', '3', '2', '2', '1', '5', '0', '3', '0', '1', '0', '3', '2'};

/** @brief 构造覆盖所有字段的合成样本，基于真机样本按 §2.5 打补丁。 */
Blob patched(Blob blob)
{
    std::ranges::copy(std::array<uint8_t, 9>{0, 1, 0, 1, 2, 3, 1, 0x80, 5},
                      std::span{blob}.subspan(300U).begin());
    setLe32(blob, 310U, 90U);
    blob[314U] = 1U;
    setText(blob, 320U, "json.example");
    setLe32(blob, 352U, 0x1234U);
    blob[356U] = 1U;
    setText(blob, 360U, "client-id");
    setText(blob, 376U, "test-user");
    setText(blob, 392U, "test-pass");
    setText(blob, 408U, "broker.example");
    setLe32(blob, 440U, 0xFFFFU);
    return blob;
}

/** @brief 校验合成 2560 B 的字段、帧与 round-trip 写入。 */
void checkFields(const Blob &golden)
{
    std::array<uint8_t, 2560> v31{};
    v31.fill(0xFFU);
    std::ranges::copy(patched(golden), v31.begin());
    seed(v31);
    const auto gnss = open(NVS_STORE_CFG_PARTITION, "gnss", NVS_READWRITE);
    check(nvs_set_u8(gnss, "rate", 5U) == ESP_OK);
    nvs_close(gnss);
    boot();
    expectDone("v1");
    expectStats(2560U, {21U, 0U, 2U});
    expectIdentity("202061126", kGoldenPn);
    // 2 Hz -> 500 ms，与 5 Hz 不整除，距 200 ms 比距 1000 ms 更近。
    // GSA 3 Hz -> 1000 ms；GSV 1 Hz -> SATSINFO/GSV 共用 -1；GST 为负值关闭。
    constexpr std::array<uint8_t, 8> kOutput{1, 0x37, 5, 0xFF, 1, 0, 5, 0xFF};
    constexpr std::array<uint8_t, 6> kSatellite{90, 0, 0, 1, 1, 1};
    expectBlob(NVS_STORE_CFG_PARTITION, "gnss", "out", kOutput);
    expectBlob(NVS_STORE_CFG_PARTITION, "rtk", "sat_cfg", kSatellite);
    std::array<uint8_t, 42> json{};
    setText(json, 0U, "json.example");
    setLe32(json, 32U, 0x1234U);
    expectBlob(NVS_STORE_CFG_PARTITION, "rtk", "json_srv", json);
    std::array<uint8_t, 86> tb{};
    tb[0] = 1U;
    setText(tb, 2U, "client-id");
    setText(tb, 18U, "test-user");
    setText(tb, 34U, "test-pass");
    setText(tb, 50U, "broker.example");
    setLe32(tb, 82U, 0xFFFFU);
    expectBlob(NVS_STORE_CFG_PARTITION, "rtk", "tb_cfg", tb);
    expectBlob(NVS_STORE_SYSTEM_PARTITION, "eeprom", "eeprom", v31);
}

/** @brief 对照有符号频率、截止角、定长文本及真实协议边界。 */
void checkBoundaries()
{
    Blob blob{};
    blob.fill(0xFFU);
    check(!decodeLegacyEeprom(std::span{blob}.first(2279U), 1U));
    check(!decodeLegacyEeprom(blob, 0U));
    check(!decodeLegacyEeprom(blob, 3U));
    const auto putSwitches = [&blob](uint8_t value) {
        constexpr std::array<size_t, 6> kSwitches{300, 301, 302, 303, 314, 356};
        for (const auto offset : kSwitches) {
            blob[offset] = value;
        }
    };
    putSwitches(2U);
    setLe32(blob, 310U, 91U);
    setText(blob, 200U, "a b");  // SN 空格不通过 factory.sn。
    blob[216] = 0x80U;           // PN 非 ASCII。
    setText(blob, 320U, "x\ny"); // 所有旧文本的控制字符视为未设置。
    setText(blob, 360U, "x\ty");
    setText(blob, 376U, "x\ry");
    setText(blob, 392U, "x\x7Fy");
    setText(blob, 408U, "");
    auto fields = decode(blob, 1U);
    check(fields.skipped == 18U && fields.invalid == 3U);
    check(fields.satellite == core::SatTrackConfig::defaults());

    // 16 和 32 字节定长字段都可能没有 NUL，下一字段不得被访问。
    std::fill_n(std::span{blob}.subspan(200U).begin(), 16U, 'S');
    std::fill_n(std::span{blob}.subspan(216U).begin(), 16U, 'P');
    std::fill_n(std::span{blob}.subspan(320U).begin(), 32U, 'J');
    std::fill_n(std::span{blob}.subspan(360U).begin(), 16U, 'C');
    std::fill_n(std::span{blob}.subspan(376U).begin(), 16U, 'U');
    std::fill_n(std::span{blob}.subspan(392U).begin(), 16U, 'P');
    std::fill_n(std::span{blob}.subspan(408U).begin(), 32U, 'H');
    setLe32(blob, 352U, 0U);
    setLe32(blob, 440U, 0x80000000U);
    putSwitches(0U);
    setLe32(blob, 310U, 1U);
    std::fill_n(std::span{blob}.subspan(304U).begin(), 5U, uint8_t{127U});
    fields = decode(blob, 20U);
    check(fields.skipped == 0U && fields.invalid == 5U);
    check(std::string_view(fields.sn.data()) == "SSSSSSSSSSSSSSSS");
    check(std::ranges::all_of(fields.pn, [](uint8_t c) { return c == 'P'; }));
    check((fields.satellite == core::SatTrackConfig{1U, 0U, 0U, 0U, 0U, 0U}));
    check((fields.output.intervals == std::array<int8_t, 6>{20, -1, 1, 20, 20, -1}));
    check(fields.json.front() == 'J' && fields.json[31] == 'J' && fields.json[32] == 0U);
    check(fields.thingsboard[17] == 'C' && fields.thingsboard[33] == 'U');
    check(fields.thingsboard[49] == 'P' && fields.thingsboard[81] == 'H');
    check(fields.thingsboard.back() == 0x80U);
    setLe32(blob, 310U, 0U);
    check(decode(blob, 1U).satellite.cutoffDeg == 15U);
    setLe32(blob, 310U, 0x80000000U);
    check(decode(blob, 1U).satellite.cutoffDeg == 15U);
}

/** @brief 预存 cfg 键及出厂身份，验证树上原值优先，包括异类型键。 */
void checkPreserve(const Blob &golden)
{
    seed(patched(golden));
    constexpr core::PhonePn kNewPn{'N', 'E', 'W'};
    check(nvs_store_set_sn("NEW-SN") == ESP_OK);
    check(nvs_store_set_pn(kNewPn.data()) == ESP_OK);
    const auto rtk = open(NVS_STORE_CFG_PARTITION, "rtk", NVS_READWRITE);
    check(nvs_set_u8(rtk, "sat_cfg", 4U) == ESP_OK);
    constexpr core::PhoneJsonServerConfig kExistingJson{'n', 'e', 'w'};
    check(nvs_set_blob(rtk, "json_srv", kExistingJson.data(), kExistingJson.size()) == ESP_OK);
    check(nvs_set_u8(rtk, "tb_cfg", 6U) == ESP_OK);
    const auto gnss = open(NVS_STORE_CFG_PARTITION, "gnss", NVS_READWRITE);
    check(nvs_set_u8(gnss, "out", 7U) == ESP_OK);
    boot();
    expectDone("v1");
    expectStats(2280U, {0U, 21U, 3U});
    expectIdentity("NEW-SN", kNewPn);
    uint8_t value = 0U;
    check(nvs_get_u8(rtk, "sat_cfg", &value) == ESP_OK && value == 4U);
    expectBlob(NVS_STORE_CFG_PARTITION, "rtk", "json_srv", kExistingJson);
    check(nvs_get_u8(rtk, "tb_cfg", &value) == ESP_OK && value == 6U);
    check(nvs_get_u8(gnss, "out", &value) == ESP_OK && value == 7U);
    nvs_close(rtk);
    nvs_close(gnss);
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
    seed(patched(golden));
    if (scenario == "write_failure") {
        fake_nvs_fail_key_write("json_srv");
    } else if (scenario == "readback_failure") {
        fake_nvs_corrupt_key_read("json_srv");
    } else if (scenario == "marker_failure") {
        fake_nvs_fail_key_commit("done");
    } else {
        s_scratch_fails = true;
    }
    boot();
    expectMissing("mig", "done");
    check(s_summary_logs == 0U);
    if (scenario != "scratch_failure") {
        expectIdentity("202061126", kGoldenPn);
        const auto handle = open(NVS_STORE_CFG_PARTITION, "gnss", NVS_READWRITE);
        check(nvs_set_u8(handle, "out", 42U) == ESP_OK);
        nvs_close(handle);
    }
    s_scratch_fails = false;
    boot();
    expectDone("v1");
    check(s_summary_logs == 1U && s_scratch_calls == 2U);
    expectIdentity("202061126", kGoldenPn);
    if (scenario != "scratch_failure") {
        const auto handle = open(NVS_STORE_CFG_PARTITION, "gnss", NVS_READONLY);
        uint8_t value = 0U;
        check(nvs_get_u8(handle, "out", &value) == ESP_OK && value == 42U);
        nvs_close(handle);
    }
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
        expectStats(2280U, {0U, 21U, 0U});
    }
    check(s_scratch_calls == (scenario == "unset" ? 1U : 0U));
    expectMissing("fac", "sn");
    expectMissing("rtk", "sat_cfg");
    expectMissing("rtk", "json_srv");
    expectMissing("rtk", "tb_cfg");
    expectMissing("gnss", "out");
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

/** @brief 提供 StoreKeys 写入许可中未启用的关机计时后端。 */
uint32_t rtos::nowMs() noexcept
{
    return 0U;
}

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
    } else {
        const auto golden = loadGolden(args[2U]);
        if (scenario == "fields") {
            checkFields(golden);
        } else if (scenario == "preserve") {
            checkPreserve(golden);
        } else if (scenario.ends_with("_failure")) {
            checkFailure(scenario, golden);
        } else {
            check(scenario == "golden" || scenario == "freset");
            seed(golden);
            boot();
            expectDone("v1");
            expectStats(2280U, {13U, 8U, 0U});
            expectIdentity("202061126", kGoldenPn);
            expectBlob(NVS_STORE_SYSTEM_PARTITION, "eeprom", "eeprom", golden);
            if (scenario == "freset") {
                check(nvs_store_erase_user() == ESP_OK);
                expectNoRepeat();
                expectDone("v1");
                expectIdentity("202061126", kGoldenPn);
                expectBlob(NVS_STORE_SYSTEM_PARTITION, "eeprom", "eeprom", golden);
                expectMissing("gnss", "out");
                expectMissing("rtk", "sat_cfg");
                expectMissing("rtk", "tb_cfg");
            }
            expectNoRepeat();
        }
    }
}
// NOLINTEND(readability-magic-numbers)
