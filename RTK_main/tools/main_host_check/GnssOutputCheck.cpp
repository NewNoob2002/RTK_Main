// host 验证：GNSS 输出配置（gnss/out blob）、输出计划推导、最新句缓存与超限查询、订阅降采样与断连清空。
// 线程：仅主机单线程测试运行，不依赖 FreeRTOS 或硬件。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

#include "core/services/GnssService.hpp"
#include "data/GnssAppRing.hpp"
#include "data/GnssOutput.hpp"
#include "esp_task_wdt.h"
#include "freertos/task.h"
#include "fw/FanoutRing.hpp"
#include "modules/storage/StoreKeys.hpp"
#include "modules/gnss/GnssConsoleValue.hpp"
#include "modules/gnss/GnssMaintenance.hpp"

/** @brief 供真实存储键表引用的主机错误名。 */
extern "C" const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "OK" : "FAIL";
}

/** @brief 返回测试线程当前绑定的 FreeRTOS 任务句柄。 @retval 当前句柄。 */
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    return fake_current_task;
}

/** @brief 在主机上模拟空闲栈查询。 @param task: 未使用。 @retval 0。 */
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t /*task*/)
{
    return 0U;
}

/** @brief 在主机上模拟看门狗订阅。 @param task: 未使用。 @retval ESP_OK。 */
extern "C" esp_err_t esp_task_wdt_add(TaskHandle_t /*task*/)
{
    return ESP_OK;
}

/** @brief 在主机上模拟喂狗。 @retval ESP_OK。 */
extern "C" esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}

void testGnssFactoryReset();
void testGnssPhonePpp();
void testBaseKeysAndCommandDispatch();
void testGnssTargetBaudPolicy();
void testGgaGateNvsAndModuleFlow();
void testGnssSettingsBusAndGgaPersist();

namespace {

void check_impl(bool cond, const char *expr, int line) noexcept
{
    if (!cond) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        std::abort();
    }
}

#define CHECK(expr) check_impl((expr), #expr, __LINE__)

/** 输出计划由面板配置 + 频率推导（ADR-0012 §2.4）；语句分类改由驱动 classifyFrame() 负责。 */
void testOutputPlan() noexcept
{
    using app::data::GnssOutConfig;
    namespace gn = ::gnss;

    const auto idx = [](gn::OutputMsg msg) {
        return static_cast<size_t>(static_cast<uint8_t>(msg));
    };
    const GnssOutConfig def = GnssOutConfig::defaults();

    // 默认配置、10 Hz、无 HEADINGA 能力：GGA/RMC/GSA/GST 跟频率，SATSINFOA 5 s，HEADINGA 不下发。
    const gn::OutputPlan plan = app::data::makeOutputPlan(def, 10U, true, 2U, 0U);
    CHECK(plan.periodMs[idx(gn::OutputMsg::kGga)] == 100U);
    CHECK(plan.periodMs[idx(gn::OutputMsg::kSatsInfo)] == 5000U);
    CHECK(plan.periodMs[idx(gn::OutputMsg::kHeading)] == 0U);
    CHECK(plan.periodMs[idx(gn::OutputMsg::kGst)] == 100U);
    CHECK(plan.periodMs[idx(gn::OutputMsg::kRmc)] == 100U);
    CHECK(plan.periodMs[idx(gn::OutputMsg::kGsa)] == 100U);
    CHECK(plan.periodMs[idx(gn::OutputMsg::kGsv)] == 5000U);
    CHECK(plan.port == 2U);
    CHECK(plan.rateHz == 10U);

    // 有双天线能力时 HEADINGA 才下发。
    CHECK(app::data::makeOutputPlan(def, 10U, true, 2U, gn::cap::kHeading)
              .periodMs[idx(gn::OutputMsg::kHeading)] == 1000U);

    // 关掉 GSV 就是不下发（周期 0）。
    CHECK(app::data::makeOutputPlan(def, 1U, false, 1U, 0U).periodMs[idx(gn::OutputMsg::kGsv)] ==
          0U);

    // 掩码清零的语句不下发。
    GnssOutConfig noGga = def;
    noGga.mask = static_cast<uint8_t>(def.mask & ~0x01U);
    CHECK(app::data::makeOutputPlan(noGga, 1U, true, 2U, 0U).periodMs[idx(gn::OutputMsg::kGga)] ==
          0U);

    // 自定义间隔：正数是频率（>=50 ms），负数是周期秒，0 是关闭。
    GnssOutConfig custom = def;
    custom.intervals = {2, -2, 1, 4, 0, -5};
    const gn::OutputPlan out = app::data::makeOutputPlan(custom, 1U, true, 2U, gn::cap::kHeading);
    CHECK(out.periodMs[idx(gn::OutputMsg::kGga)] == 500U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kSatsInfo)] == 2000U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kGsv)] == 2000U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kHeading)] == 1000U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kGst)] == 250U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kRmc)] == 0U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kGsa)] == 5000U);

    custom.intervals[0] = -1; // 显式 1 Hz，定位频率 10 Hz 时仍保持一秒。
    CHECK(app::data::makeOutputPlan(custom, 10U, true, 2U, 0U).periodMs[idx(gn::OutputMsg::kGga)] ==
          1000U);
}

void testOutConfigAndNvsBlob() noexcept
{
    using app::data::GnssOutConfig;

    GnssOutConfig cfg = GnssOutConfig::defaults();
    CHECK(cfg.isValid());
    CHECK(cfg.mask == app::data::kGnssOutMaskAll);
    CHECK(fw::isOk(cfg.applyInterval(2U, 5)));    // HEADINGA 200 ms
    CHECK(fw::isOk(cfg.applyInterval(1U, -128))); // -128 -> 128000 ms
    CHECK(core::services::GnssService::intervalToPeriodMs(cfg.intervals[1]) == 128000U);
    CHECK(fw::isOk(cfg.applyInterval(3U, 0)));
    CHECK((cfg.mask & (1U << 3U)) == 0U);
    CHECK(cfg.intervals[3] == 0);
    CHECK(cfg.applyInterval(app::data::kGnssAppMsgCount, 1) == fw::Result::kInvalidArg);

    std::array<uint8_t, GnssOutConfig::kBlobBytes> blob{};
    size_t len = 0U;
    CHECK(fw::isOk(cfg.encode(blob, len)));
    CHECK(len == GnssOutConfig::kBlobBytes);

    GnssOutConfig decoded{};
    CHECK(fw::isOk(GnssOutConfig::decode(blob, decoded)));
    CHECK(decoded.mask == cfg.mask);
    CHECK(decoded.intervals == cfg.intervals);

    // 破坏版本字节或保留位必须拒绝。
    auto bad = blob;
    bad[0] = 0x99U;
    CHECK(GnssOutConfig::decode(bad, decoded) == fw::Result::kInvalidArg);
    bad = blob;
    bad[1] = 0xFFU;
    CHECK(GnssOutConfig::decode(bad, decoded) == fw::Result::kInvalidArg);

    // 通过 store::writeBlob / readBlob 往返 NVS。
    static constexpr fw::StoreKey kOutKey{
        .ns = "gnss",
        .key = "out",
        .type = fw::StoreType::kBlob,
        .maxSize = GnssOutConfig::kBlobBytes,
        .flags = 0U,
        .min = 0,
        .max = 0,
        .productMask = fw::kAllProducts,
        .help = "out",
        .binding = {},
    };
    CHECK(app::store::writeBlob(kOutKey, blob) == ESP_OK);
    std::array<uint8_t, GnssOutConfig::kBlobBytes> readback{};
    size_t readLen = 0U;
    CHECK(app::store::readBlob(kOutKey, readback, readLen) == ESP_OK);
    CHECK(readLen == GnssOutConfig::kBlobBytes);
    CHECK(readback == blob);
}

void testLatestCacheAndQueryCaps() noexcept
{
    app::data::GnssLatestCache cache{};
    cache.bindLock(fw::kNoLock);
    std::array<uint8_t, app::data::kGnssLatestTotalBytes> cacheBuf{};
    CHECK(fw::isOk(cache.init(cacheBuf)));

    std::array<uint8_t, 256U> queryBuf{};
    // 无缓存时不命中且不计 oversize。
    auto q0 = cache.query(0U, queryBuf);
    CHECK(!q0.ok && !q0.oversize && (q0.len == 0U));

    // GGA 上限 256 B：256 B 成功，257 B 丢弃并计 oversizeDrops。
    std::array<uint8_t, 257U> gga{};
    gga.fill(static_cast<uint8_t>('G'));
    CHECK(cache.update(0U, std::span<const uint8_t>{gga}.first(256U)));
    q0 = cache.query(0U, queryBuf);
    CHECK(q0.ok && !q0.oversize && (q0.len == 256U));
    CHECK(cache.snapshot()[0].oversizeDrops == 0U);

    CHECK(!cache.update(0U, gga));
    CHECK(cache.snapshot()[0].oversizeDrops == 1U);

    // HEADINGA（1024 B 上限）与 SATSINFOA（4096 B 上限）：更新 >256 B 后查询不截断，返回 oversize=true。
    std::array<uint8_t, 300U> heading{};
    heading.fill(static_cast<uint8_t>('H'));
    CHECK(cache.update(2U, heading));
    const auto qh = cache.query(2U, queryBuf);
    CHECK(!qh.ok && qh.oversize && (qh.len == 0U));

    std::array<uint8_t, 4096U> sats{};
    sats.fill(static_cast<uint8_t>('S'));
    CHECK(cache.update(1U, sats));
    CHECK(cache.snapshot()[1].oversizeDrops == 0U);
    const auto qs = cache.query(1U, queryBuf);
    CHECK(!qs.ok && qs.oversize && (qs.len == 0U));
    CHECK(cache.snapshot()[1].queryOversize == 1U);

    std::array<uint8_t, 4097U> satsOver{};
    satsOver.fill(static_cast<uint8_t>('S'));
    CHECK(!cache.update(1U, satsOver));
    CHECK(cache.snapshot()[1].oversizeDrops == 1U);
}

void testSubscriptionAndAppRing() noexcept
{
    app::data::GnssAppSubState sub{};
    sub.bindLock(fw::kNoLock);

    std::array<uint32_t, app::data::kGnssAppMsgCount> periods{1000U, 5000U, 0U, 0U, 0U, 0U};
    sub.setSubscription((1U << 0U) | (1U << 1U), periods);
    uint32_t mask = 0U;
    std::array<uint32_t, app::data::kGnssAppMsgCount> readPeriods{};
    sub.readSubscription(mask, readPeriods);
    CHECK((mask == ((1U << 0U) | (1U << 1U))) && (readPeriods[0] == 1000U) &&
          (readPeriods[1] == 5000U));

    sub.addTxSent(0U);
    sub.addSkipped(0U);
    sub.addRingOversize(1U);
    sub.setQueryOversizeTotal(2U);

    // App FanoutRing（4096 B，maxRecord=4093）：4093 B 可写入读出，4094 B 触发 oversizeDrop。
    std::array<uint8_t, 4096U> backing{};
    fw::FanoutRing ring{};
    const size_t maxPayload = 4096U - fw::kFanoutTypedHeaderBytes;
    CHECK(fw::isOk(ring.initTyped(backing, maxPayload, fw::kNoLock)));
    uint8_t readerId = 0U;
    CHECK(fw::isOk(ring.attach(0U, readerId)));

    static std::array<uint8_t, 4093U> frame{};
    frame.fill(static_cast<uint8_t>('A'));
    // 未订阅时写侧不入环，记 noSubscriber。
    CHECK(ring.write(0U, std::span<const uint8_t>{frame}.first(32U)) == fw::Result::kOk);
    CHECK(ring.typedWriterStats().noSubscriber == 1U);

    // 订阅后 4093 B 成功，4094 B 触发 oversizeDrop。
    CHECK(fw::isOk(ring.setMask(readerId, (1U << 0U) | (1U << 1U))));
    CHECK(fw::isOk(ring.write(1U, frame)));
    fw::FanoutView view{};
    CHECK(fw::isOk(ring.peek(readerId, view)));
    CHECK((view.type == 1U) && ((view.first.size() + view.second.size()) == 4093U));
    CHECK(fw::isOk(ring.commit(view.lease)));

    static std::array<uint8_t, 4094U> oversize{};
    oversize.fill(static_cast<uint8_t>('B'));
    CHECK(ring.write(1U, oversize) == fw::Result::kInvalidArg);
    CHECK(ring.typedWriterStats().oversizeDrop == 1U);

    // 断连清零：mask=0 后不再有活跃订阅。
    std::array<uint32_t, app::data::kGnssAppMsgCount> zeroPeriods{};
    sub.setSubscription(0U, zeroPeriods);
    const auto snap = sub.snapshot();
    CHECK(snap.activeMask == 0U);
    CHECK(snap.txSent[0] == 1U);
    CHECK(snap.skipped[0] == 1U);
    CHECK(snap.ringOversize[1] == 1U);
    CHECK(snap.queryOversizeTotal == 2U);
}

/** 降采样闸门：到达抖动不丢周期、平均速率不偏高、停顿后不补发突发。 */
void testPushGate() noexcept
{
    using app::data::GnssPushGate;
    // 1 Hz 源、1 s 订阅，到达抖动 ±30 ms：每条都应放行（旧逻辑早到即跳过，形成 2 s 间隔）。
    {
        GnssPushGate gate{};
        const std::array<int32_t, 8> jitter{0, -30, 25, -5, 30, -28, 0, 12};
        uint32_t pushed = 0U;
        for (size_t i = 0; i < jitter.size(); ++i) {
            const auto now =
                static_cast<uint32_t>(static_cast<int32_t>(5000U + (i * 1000U)) + jitter[i]);
            if (gate.due(now, 1000U)) {
                gate.onPushed(now, 1000U);
                ++pushed;
            }
        }
        CHECK(pushed == jitter.size());
    }
    // 10 Hz 源、1 s 订阅：10 s 内放行 10 条（±1），slack 不抬高平均速率。
    {
        GnssPushGate gate{};
        uint32_t pushed = 0U;
        for (uint32_t t = 0U; t < 10000U; t += 100U) {
            if (gate.due(t, 1000U)) {
                gate.onPushed(t, 1000U);
                ++pushed;
            }
        }
        CHECK((pushed >= 9U) && (pushed <= 11U));
    }
    // 源停顿 10 s 后恢复：只放行一条，锚点重置，不补发。
    {
        GnssPushGate gate{};
        gate.onPushed(0U, 1000U);
        CHECK(gate.due(10000U, 1000U));
        gate.onPushed(10000U, 1000U);
        CHECK(!gate.due(10100U, 1000U));
        CHECK(gate.due(11000U, 1000U));
    }
    // period=0 每条放行；未 armed 首条放行；reset 后首条立即放行；now 回绕。
    {
        GnssPushGate gate{};
        CHECK(gate.due(123U, 5000U));
        gate.onPushed(0xFFFFFF00U, 5000U);
        CHECK(!gate.due(0x00000100U, 5000U));
        CHECK(gate.due(0xFFFFFF00U + 4950U, 5000U));
        CHECK(gate.due(0x00000100U, 0U));
        gate.reset();
        CHECK(gate.due(0x00000101U, 5000U));
    }
}

/** @brief 验证 Hz 文本、协议间隔和命令保护的边界。 */
void testConsoleRates()
{
    using app::detail::formatGnssHz;
    using app::detail::parseGnssHz;
    int8_t interval = 0;
    CHECK(parseGnssHz("0.2", interval) && interval == -5);
    CHECK(parseGnssHz("1/127", interval) && interval == -127);
    CHECK(parseGnssHz("20", interval) && interval == 20);
    CHECK(parseGnssHz("off", interval) && interval == 0);
    std::array<char, app::detail::kGnssRateTextBytes> display{};
    CHECK(parseGnssHz("0.2", interval));
    formatGnssHz(interval, display);
    CHECK(std::string_view{display.data()} == "1/5 Hz");
    formatGnssHz(-3, display);
    CHECK(std::string_view{display.data()} == "1/3 Hz");
    formatGnssHz(-1, display);
    CHECK(std::string_view{display.data()} == "1 Hz");
    formatGnssHz(1, display);
    CHECK(std::string_view{display.data()} == "1 Hz");
    formatGnssHz(0, display);
    CHECK(std::string_view{display.data()} == "off");
    for (const auto text :
         {"-1", "0.3", "0.7", "1/128", "1/0", "1/1", "NaN", "3", "0.001", "0.200x"}) {
        CHECK(!parseGnssHz(text, interval));
    }
    for (int period = 2; period <= 127; ++period) {
        std::array<char, app::detail::kGnssRateTextBytes> text{};
        formatGnssHz(static_cast<int8_t>(-period), text);
        std::string_view value{text.data()};
        CHECK(value.ends_with(" Hz"));
        value.remove_suffix(3U);
        CHECK(parseGnssHz(value, interval) && interval == -period);
    }

    using app::detail::changesConnectedGnssBaud;
    CHECK(changesConnectedGnssBaud("CoNfIg\tCOM2 460800 8 n 1", 2U));
    CHECK(changesConnectedGnssBaud("  COM com2 115200", 2U));
    CHECK(changesConnectedGnssBaud("COM 115200", 2U));
    CHECK(changesConnectedGnssBaud("$PUBX,41,2,0007,0003,19200,0*26", 2U));
    CHECK(!changesConnectedGnssBaud("$PUBX,41,1,0007,0003,19200,0*25", 2U));
    CHECK(!changesConnectedGnssBaud("CONFIG COM1 460800", 2U));
    CHECK(!changesConnectedGnssBaud("COM COM3 115200", 2U));
    CHECK(!changesConnectedGnssBaud("CONFIG COM2", 2U));
}

/** @brief 验证 GnssAppRing 满环淘汰优先级（GSV→GSA→GST→RMC→GGA）、GGA 保留与 gga_drop 统计。 */
void testGnssAppRingPriority() noexcept
{
    constexpr uint8_t kGga = static_cast<uint8_t>(::gnss::OutputMsg::kGga);
    constexpr uint8_t kGst = static_cast<uint8_t>(::gnss::OutputMsg::kGst);
    constexpr uint8_t kRmc = static_cast<uint8_t>(::gnss::OutputMsg::kRmc);
    constexpr uint8_t kGsa = static_cast<uint8_t>(::gnss::OutputMsg::kGsa);
    constexpr uint8_t kGsv = static_cast<uint8_t>(::gnss::OutputMsg::kGsv);
    constexpr fw::FanoutMask kNmeaMask =
        (1U << kGga) | (1U << kGst) | (1U << kRmc) | (1U << kGsa) | (1U << kGsv);

    // 1. 4096 B 满环混合压测：严格按 GSV → GSA → GST → RMC 淘汰，GGA 全部保留且 gga_drop == 0。
    {
        std::array<uint8_t, 4096U> storage{};
        app::data::GnssAppRing ring{};
        const size_t maxPayload = storage.size() - fw::kFanoutTypedHeaderBytes;
        CHECK(fw::isOk(ring.initTyped(storage, maxPayload, fw::kNoLock)));
        uint8_t rid = 0U;
        CHECK(fw::isOk(ring.attach(kNmeaMask, rid)));

        // 先验证单条最大记录（4093 B）可写入、4094 B 拒收并计 oversizeDrop。
        static std::array<uint8_t, 4094U> bigBuf{};
        bigBuf.fill(static_cast<uint8_t>('X'));
        CHECK(ring.write(kGga, std::span<const uint8_t>{bigBuf}.first(4093U)) == fw::Result::kOk);
        fw::FanoutView bigView{};
        CHECK(fw::isOk(ring.peek(rid, bigView)));
        CHECK((bigView.type == kGga) && (bigView.first.size() == 4093U) && bigView.second.empty());
        CHECK(fw::isOk(ring.commit(bigView.lease)));
        CHECK(ring.write(kGga, bigBuf) == fw::Result::kInvalidArg);
        CHECK(ring.typedWriterStats().oversizeDrop == 1U);

        // 连续写入 40 组 (GGA/RMC/GST/GSA/GSV)，每条 80 B（每组 5*83=415 B，40 组约 16.6 KB >> 4 KB）。
        // 40 条 GGA 共 40*83=3320 B < 4096 B，应全部保留，剩余空间留给最新 RMC，GSV/GSA/GST 全被淘汰。
        std::array<uint8_t, 80U> pkt{};
        for (uint8_t seq = 0U; seq < 40U; ++seq) {
            for (const uint8_t type : {kGga, kRmc, kGst, kGsa, kGsv}) {
                pkt.fill(seq);
                pkt[0] = type;
                const fw::Result res = ring.write(type, pkt);
                if (type == kGga) {
                    CHECK(res == fw::Result::kOk);
                } else {
                    CHECK((res == fw::Result::kOk) || (res == fw::Result::kNoSpace));
                }
            }
        }
        CHECK(ring.ggaDropCount() == 0U);
        CHECK(ring.typedReaderStats(rid).base.dropped > 0U);

        uint32_t ggaCount = 0U;
        uint32_t rmcCount = 0U;
        uint32_t gstCount = 0U;
        uint32_t gsaCount = 0U;
        uint32_t gsvCount = 0U;
        uint8_t expectedGgaSeq = 0U;
        fw::FanoutView view{};
        while (fw::isOk(ring.peek(rid, view))) {
            CHECK(view.first.size() == pkt.size());
            if (view.type == kGga) {
                CHECK(view.first[1] == expectedGgaSeq);
                ++expectedGgaSeq;
                ++ggaCount;
            } else if (view.type == kRmc) {
                ++rmcCount;
            } else if (view.type == kGst) {
                ++gstCount;
            } else if (view.type == kGsa) {
                ++gsaCount;
            } else if (view.type == kGsv) {
                ++gsvCount;
            }
            CHECK(fw::isOk(ring.commit(view.lease)));
        }
        CHECK(ggaCount == 40U);
        CHECK(gsvCount == 0U);
        CHECK(gsaCount == 0U);
        CHECK(gstCount == 0U);
        CHECK(rmcCount > 0U);
    }

    // 2. 精确逐步淘汰顺序：5 条记录占满小环，随后每写入一条新 GGA，淘汰顺序严格为 GSV → GSA → GST → RMC → GGA。
    {
        // 每条 20 B 负载 + 3 B 头 = 23 B；5 条共 115 B，总容量 120 B（仅剩 5 B，不够再放第 6 条）。
        std::array<uint8_t, 120U> smallStorage{};
        app::data::GnssAppRing ring{};
        CHECK(fw::isOk(ring.initTyped(
            smallStorage, smallStorage.size() - fw::kFanoutTypedHeaderBytes, fw::kNoLock)));
        uint8_t rid = 0U;
        CHECK(fw::isOk(ring.attach(kNmeaMask, rid)));

        std::array<uint8_t, 20U> rec{};
        const std::array<uint8_t, 5U> initialOrder = {kGga, kRmc, kGst, kGsa, kGsv};
        for (size_t i = 0U; i < initialOrder.size(); ++i) {
            rec.fill(static_cast<uint8_t>(i));
            CHECK(ring.write(initialOrder[i], rec) == fw::Result::kOk);
        }

        // 再写 4 条 GGA：依次淘汰 GSV、GSA、GST、RMC，gga_drop 始终为 0。
        for (uint8_t step = 0U; step < 4U; ++step) {
            rec.fill(static_cast<uint8_t>(10U + step));
            CHECK(ring.write(kGga, rec) == fw::Result::kOk);
            CHECK(ring.ggaDropCount() == 0U);
        }

        // 此时环内 5 条全为 GGA：再尝试写入 GSV/GSA/GST/RMC，必须全部拒收（kNoSpace）且 gga_drop == 0。
        for (const uint8_t lowType : {kGsv, kGsa, kGst, kRmc}) {
            rec.fill(0xEEU);
            CHECK(ring.write(lowType, rec) == fw::Result::kNoSpace);
            CHECK(ring.ggaDropCount() == 0U);
        }

        // 环已全为 GGA 时再写入第 6 条 GGA：淘汰最旧一条 GGA，gga_drop == 1。
        rec.fill(99U);
        CHECK(ring.write(kGga, rec) == fw::Result::kOk);
        CHECK(ring.ggaDropCount() == 1U);

        // 读出剩余 5 条：应为 step=0..3 的 GGA 以及最后的 99（最旧的 0 号 GGA 已被淘汰）。
        const std::array<uint8_t, 5U> expectedSeqs = {10U, 11U, 12U, 13U, 99U};
        for (const uint8_t expected : expectedSeqs) {
            fw::FanoutView view{};
            CHECK(fw::isOk(ring.peek(rid, view)));
            CHECK((view.type == kGga) && (view.first[0] == expected));
            CHECK(fw::isOk(ring.commit(view.lease)));
        }
        fw::FanoutView emptyView{};
        CHECK(ring.peek(rid, emptyView) == fw::Result::kNotFound);
    }

    // 3. 读侧 pin 保护：第一条被 pin 时，满环淘汰只作用于 pin 区之后，不破坏已 pin 字节。
    {
        std::array<uint8_t, 120U> smallStorage{};
        app::data::GnssAppRing ring{};
        CHECK(fw::isOk(ring.initTyped(
            smallStorage, smallStorage.size() - fw::kFanoutTypedHeaderBytes, fw::kNoLock)));
        uint8_t rid = 0U;
        CHECK(fw::isOk(ring.attach(kNmeaMask, rid)));

        std::array<uint8_t, 20U> rec{};
        rec.fill(0xAAU);
        CHECK(ring.write(kGsv, rec) == fw::Result::kOk); // 首条为 GSV，随后 pin 住。
        rec.fill(0xBBU);
        CHECK(ring.write(kGsv, rec) == fw::Result::kOk);
        rec.fill(0xCCU);
        CHECK(ring.write(kGga, rec) == fw::Result::kOk);
        rec.fill(0xDDU);
        CHECK(ring.write(kGga, rec) == fw::Result::kOk);
        rec.fill(0xEEU);
        CHECK(ring.write(kGga, rec) == fw::Result::kOk);

        fw::FanoutView pinned{};
        CHECK(fw::isOk(ring.peek(rid, pinned)));
        CHECK((pinned.type == kGsv) && (pinned.first[0] == 0xAAU));

        // 写入新 GGA：首条 GSV 被 pin 保护，只能淘汰第 2 条 GSV（0xBB），首条字节不受破坏。
        rec.fill(0x55U);
        CHECK(ring.write(kGga, rec) == fw::Result::kOk);
        CHECK(ring.ggaDropCount() == 0U);
        CHECK(pinned.first[0] == 0xAAU);
        CHECK(fw::isOk(ring.commit(pinned.lease)));
    }
}

/** @brief 多次排队只补发一次，清除后可以继续接收下一轮。 */
void testSaveCoalescing()
{
    app::GnssSaveQueue queue;
    CHECK(!queue.take(true));
    queue.request();
    CHECK(queue.take(true));
    queue.request();
    queue.request();
    CHECK(!queue.take(false));
    CHECK(queue.take(true));
    CHECK(!queue.take(true));
    queue.request();
    queue.clear();
    CHECK(!queue.take(true));
}

} // namespace

int main()
{
    testOutputPlan();
    testOutConfigAndNvsBlob();
    testLatestCacheAndQueryCaps();
    testSubscriptionAndAppRing();
    testGnssAppRingPriority();
    testPushGate();
    testConsoleRates();
    testSaveCoalescing();
    testGnssFactoryReset();
    testBaseKeysAndCommandDispatch();
    testGnssPhonePpp();
    testGnssTargetBaudPolicy();
    testGgaGateNvsAndModuleFlow();
    testGnssSettingsBusAndGgaPersist();
    return 0;
}
