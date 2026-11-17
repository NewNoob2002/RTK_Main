// host 验证：K4 GNSS 输出配置（gnss/out blob）、输出计划推导、最新句缓存与超限查询、订阅降采样与断连清空。
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

/** P3b：输出计划由面板配置 + 频率推导（ADR-0012 §2.4）；语句分类改由驱动 classify_frame() 负责。 */
void test_output_plan() noexcept
{
    using app::data::GnssOutConfig;
    namespace gn = ::gnss;

    const auto idx = [](gn::OutputMsg msg) {
        return static_cast<size_t>(static_cast<uint8_t>(msg));
    };
    const GnssOutConfig def = GnssOutConfig::defaults();

    // 默认配置、10 Hz、无 HEADINGA 能力：GGA/RMC/GSA/GST 跟频率，SATSINFOA 5 s，HEADINGA 不下发。
    const gn::OutputPlan plan = app::data::make_output_plan(def, 10U, true, 2U, 0U);
    CHECK(plan.period_ms[idx(gn::OutputMsg::kGga)] == 100U);
    CHECK(plan.period_ms[idx(gn::OutputMsg::kSatsInfo)] == 5000U);
    CHECK(plan.period_ms[idx(gn::OutputMsg::kHeading)] == 0U);
    CHECK(plan.period_ms[idx(gn::OutputMsg::kGst)] == 100U);
    CHECK(plan.period_ms[idx(gn::OutputMsg::kRmc)] == 100U);
    CHECK(plan.period_ms[idx(gn::OutputMsg::kGsa)] == 100U);
    CHECK(plan.period_ms[idx(gn::OutputMsg::kGsv)] == 1000U);
    CHECK(plan.port == 2U);
    CHECK(plan.rate_hz == 10U);

    // 有双天线能力时 HEADINGA 才下发。
    CHECK(app::data::make_output_plan(def, 10U, true, 2U, gn::cap::kHeading)
              .period_ms[idx(gn::OutputMsg::kHeading)] == 1000U);

    // 关掉 GSV 就是不下发（周期 0）。
    CHECK(app::data::make_output_plan(def, 1U, false, 1U, 0U).period_ms[idx(gn::OutputMsg::kGsv)] ==
          0U);

    // 掩码清零的语句不下发。
    GnssOutConfig no_gga = def;
    no_gga.mask = static_cast<uint8_t>(def.mask & ~0x01U);
    CHECK(
        app::data::make_output_plan(no_gga, 1U, true, 2U, 0U).period_ms[idx(gn::OutputMsg::kGga)] ==
        0U);

    // 自定义间隔：正数是频率（>=50 ms），负数是周期秒，0 是关闭。
    GnssOutConfig custom = def;
    custom.intervals = {2, -2, 1, 4, 0, -5};
    const gn::OutputPlan out = app::data::make_output_plan(custom, 1U, true, 2U, gn::cap::kHeading);
    CHECK(out.period_ms[idx(gn::OutputMsg::kGga)] == 500U);
    CHECK(out.period_ms[idx(gn::OutputMsg::kSatsInfo)] == 2000U);
    CHECK(out.period_ms[idx(gn::OutputMsg::kHeading)] == 1000U);
    CHECK(out.period_ms[idx(gn::OutputMsg::kGst)] == 250U);
    CHECK(out.period_ms[idx(gn::OutputMsg::kRmc)] == 0U);
    CHECK(out.period_ms[idx(gn::OutputMsg::kGsa)] == 5000U);
}

void test_out_config_and_nvs_blob() noexcept
{
    using app::data::GnssOutConfig;

    GnssOutConfig cfg = GnssOutConfig::defaults();
    CHECK(cfg.is_valid());
    CHECK(cfg.mask == app::data::kGnssOutMaskAll);
    CHECK(fw::isOk(cfg.apply_interval(2U, 5)));    // HEADINGA 200 ms
    CHECK(fw::isOk(cfg.apply_interval(1U, -128))); // -128 -> 128000 ms
    CHECK(core::services::GnssService::intervalToPeriodMs(cfg.intervals[1]) == 128000U);
    CHECK(fw::isOk(cfg.apply_interval(3U, 0)));
    CHECK((cfg.mask & (1U << 3U)) == 0U);
    CHECK(cfg.intervals[3] == 0);
    CHECK(cfg.apply_interval(app::data::kGnssAppMsgCount, 1) == fw::Result::kInvalidArg);

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

    // 通过 store::write_blob / read_blob 往返 NVS。
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
    CHECK(app::store::write_blob(kOutKey, blob) == ESP_OK);
    std::array<uint8_t, GnssOutConfig::kBlobBytes> readback{};
    size_t read_len = 0U;
    CHECK(app::store::read_blob(kOutKey, readback, read_len) == ESP_OK);
    CHECK(read_len == GnssOutConfig::kBlobBytes);
    CHECK(readback == blob);
}

void test_latest_cache_and_query_caps() noexcept
{
    app::data::GnssLatestCache cache{};
    cache.bind_lock(fw::kNoLock);
    std::array<uint8_t, app::data::kGnssLatestTotalBytes> cache_buf{};
    CHECK(fw::isOk(cache.init(cache_buf)));

    std::array<uint8_t, 256U> query_buf{};
    // 无缓存时不命中且不计 oversize。
    auto q0 = cache.query(0U, query_buf);
    CHECK(!q0.ok && !q0.oversize && (q0.len == 0U));

    // GGA 上限 256 B：256 B 成功，257 B 丢弃并计 oversize_drops。
    std::array<uint8_t, 257U> gga{};
    gga.fill(static_cast<uint8_t>('G'));
    CHECK(cache.update(0U, std::span<const uint8_t>{gga}.first(256U)));
    q0 = cache.query(0U, query_buf);
    CHECK(q0.ok && !q0.oversize && (q0.len == 256U));
    CHECK(cache.snapshot()[0].oversize_drops == 0U);

    CHECK(!cache.update(0U, gga));
    CHECK(cache.snapshot()[0].oversize_drops == 1U);

    // HEADINGA（1024 B 上限）与 SATSINFOA（4096 B 上限）：更新 >256 B 后查询不截断，返回 oversize=true。
    std::array<uint8_t, 300U> heading{};
    heading.fill(static_cast<uint8_t>('H'));
    CHECK(cache.update(2U, heading));
    const auto qh = cache.query(2U, query_buf);
    CHECK(!qh.ok && qh.oversize && (qh.len == 0U));

    std::array<uint8_t, 4096U> sats{};
    sats.fill(static_cast<uint8_t>('S'));
    CHECK(cache.update(1U, sats));
    CHECK(cache.snapshot()[1].oversize_drops == 0U);
    const auto qs = cache.query(1U, query_buf);
    CHECK(!qs.ok && qs.oversize && (qs.len == 0U));
    CHECK(cache.snapshot()[1].query_oversize == 1U);

    std::array<uint8_t, 4097U> sats_over{};
    sats_over.fill(static_cast<uint8_t>('S'));
    CHECK(!cache.update(1U, sats_over));
    CHECK(cache.snapshot()[1].oversize_drops == 1U);
}

void test_subscription_and_app_ring() noexcept
{
    app::data::GnssAppSubState sub{};
    sub.bind_lock(fw::kNoLock);

    std::array<uint32_t, app::data::kGnssAppMsgCount> periods{1000U, 5000U, 0U, 0U, 0U, 0U};
    sub.set_subscription((1U << 0U) | (1U << 1U), periods);
    uint32_t mask = 0U;
    std::array<uint32_t, app::data::kGnssAppMsgCount> read_periods{};
    sub.read_subscription(mask, read_periods);
    CHECK((mask == ((1U << 0U) | (1U << 1U))) && (read_periods[0] == 1000U) &&
          (read_periods[1] == 5000U));

    sub.add_tx_sent(0U);
    sub.add_skipped(0U);
    sub.add_ring_oversize(1U);
    sub.set_query_oversize_total(2U);

    // App FanoutRing（4096 B，max_record=4093）：4093 B 可写入读出，4094 B 触发 oversize_drop。
    std::array<uint8_t, 4096U> backing{};
    fw::FanoutRing ring{};
    const size_t max_payload = 4096U - fw::kFanoutTypedHeaderBytes;
    CHECK(fw::isOk(ring.initTyped(backing, max_payload, fw::kNoLock)));
    uint8_t reader_id = 0U;
    CHECK(fw::isOk(ring.attach(0U, reader_id)));

    static std::array<uint8_t, 4093U> frame{};
    frame.fill(static_cast<uint8_t>('A'));
    // 未订阅时写侧不入环，记 no_subscriber。
    CHECK(ring.write(0U, std::span<const uint8_t>{frame}.first(32U)) == fw::Result::kOk);
    CHECK(ring.typedWriterStats().noSubscriber == 1U);

    // 订阅后 4093 B 成功，4094 B 触发 oversize_drop。
    CHECK(fw::isOk(ring.setMask(reader_id, (1U << 0U) | (1U << 1U))));
    CHECK(fw::isOk(ring.write(1U, frame)));
    fw::FanoutView view{};
    CHECK(fw::isOk(ring.peek(reader_id, view)));
    CHECK((view.type == 1U) && ((view.first.size() + view.second.size()) == 4093U));
    CHECK(fw::isOk(ring.commit(view.lease)));

    static std::array<uint8_t, 4094U> oversize{};
    oversize.fill(static_cast<uint8_t>('B'));
    CHECK(ring.write(1U, oversize) == fw::Result::kInvalidArg);
    CHECK(ring.typedWriterStats().oversizeDrop == 1U);

    // 断连清零：mask=0 后不再有活跃订阅。
    std::array<uint32_t, app::data::kGnssAppMsgCount> zero_periods{};
    sub.set_subscription(0U, zero_periods);
    const auto snap = sub.snapshot();
    CHECK(snap.active_mask == 0U);
    CHECK(snap.tx_sent[0] == 1U);
    CHECK(snap.skipped[0] == 1U);
    CHECK(snap.ring_oversize[1] == 1U);
    CHECK(snap.query_oversize_total == 2U);
}

/** 降采样闸门：到达抖动不丢周期、平均速率不偏高、停顿后不补发突发。 */
void test_push_gate() noexcept
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
                gate.on_pushed(now, 1000U);
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
                gate.on_pushed(t, 1000U);
                ++pushed;
            }
        }
        CHECK((pushed >= 9U) && (pushed <= 11U));
    }
    // 源停顿 10 s 后恢复：只放行一条，锚点重置，不补发。
    {
        GnssPushGate gate{};
        gate.on_pushed(0U, 1000U);
        CHECK(gate.due(10000U, 1000U));
        gate.on_pushed(10000U, 1000U);
        CHECK(!gate.due(10100U, 1000U));
        CHECK(gate.due(11000U, 1000U));
    }
    // period=0 每条放行；未 armed 首条放行；reset 后首条立即放行；now 回绕。
    {
        GnssPushGate gate{};
        CHECK(gate.due(123U, 5000U));
        gate.on_pushed(0xFFFFFF00U, 5000U);
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
    test_output_plan();
    test_out_config_and_nvs_blob();
    test_latest_cache_and_query_caps();
    test_subscription_and_app_ring();
    test_push_gate();
    return 0;
}
