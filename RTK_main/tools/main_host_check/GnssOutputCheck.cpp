// host 验证：GNSS 输出配置（gnss/out blob）、输出计划推导、最新句缓存与超限查询、订阅降采样与断连清空。
// 线程：仅主机单线程测试运行，不依赖 FreeRTOS 或硬件。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

#include "core/services/GnssService.hpp"
#include "data/GnssOutput.hpp"
#include "fw/FanoutRing.hpp"
#include "modules/storage/StoreKeys.hpp"

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
    CHECK(plan.periodMs[idx(gn::OutputMsg::kGsv)] == 1000U);
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
    CHECK(out.periodMs[idx(gn::OutputMsg::kHeading)] == 1000U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kGst)] == 250U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kRmc)] == 0U);
    CHECK(out.periodMs[idx(gn::OutputMsg::kGsa)] == 5000U);
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

} // namespace

int main()
{
    testOutputPlan();
    testOutConfigAndNvsBlob();
    testLatestCacheAndQueryCaps();
    testSubscriptionAndAppRing();
    testPushGate();
    return 0;
}
