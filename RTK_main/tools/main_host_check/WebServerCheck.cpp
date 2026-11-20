// 主机回归：验证网页配置模块的 SPSC 帧邮箱、set 命令翻译、JSON 编码/状态差分与空闲回落计时。
// 主机测试（含多线程 SPSC 压力与 ASan/UBSan），非 ISR。
#include "modules/web/WebIdleTimer.hpp"
#include "modules/web/WebLogic.hpp"
#include "modules/web/WebMailbox.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <thread>

#include "def/GnssDef.hpp"
#include "def/GnssSubDef.hpp"
#include "def/StorageDef.hpp"

namespace {

using namespace app;
using namespace app::web_cfg;

/**
 * @brief 验证 SPSC 帧邮箱的容量上限、超长拒绝、控制邮箱、ID 扫描与双线程并发投递
 * @note 主机测试，使用 std::thread 覆盖并发读写，支持 ASan/UBSan。
 */
void testMailbox()
{
    WebFrameMailbox<2U> box{};
    assert(box.empty());
    assert(box.size() == 0U);

    // 正常入队与满邮箱拒绝（含会话代号 gen 透传）。
    constexpr std::string_view kF1 = R"({"t":"set","id":7,"key":"gnss.rate","value":"10"})";
    constexpr std::string_view kF2 = R"({"t":"cmd","id":8,"name":"save"})";
    assert(box.tryPush(11, 101U, std::span<const char>{kF1.data(), kF1.size()}) ==
           MailboxPush::kOk);
    assert(box.tryPush(12, 102U, std::span<const char>{kF2.data(), kF2.size()}) ==
           MailboxPush::kOk);
    assert(box.size() == 2U);
    assert(box.tryPush(13, 103U, std::span<const char>{kF1.data(), kF1.size()}) ==
           MailboxPush::kFull);

    // 恰好 1024 B 接受，1025 B 报超长。
    std::array<char, web::kMaxFrameBytes + 1U> oversize{};
    oversize.fill('a');
    assert(box.tryPush(14, 104U, std::span<const char>{oversize}) == MailboxPush::kTooLong);

    WebFrame out{};
    assert(box.tryPop(out));
    assert(out.fd == 11 && out.gen == 101U && std::string_view(out.data.data(), out.len) == kF1);
    assert(box.tryPop(out));
    assert(out.fd == 12 && out.gen == 102U && std::string_view(out.data.data(), out.len) == kF2);
    assert(!box.tryPop(out));

    assert(box.tryPush(15, 105U, std::span<const char>{oversize}.first(web::kMaxFrameBytes)) ==
           MailboxPush::kOk);
    assert(box.tryPop(out));
    assert(out.fd == 15 && out.gen == 105U && out.len == web::kMaxFrameBytes);

    // 轻量 ID 扫描用于满邮箱时回 busy。
    assert(extractRequestId(std::span<const char>{kF1.data(), kF1.size()}) == 7U);
    constexpr std::string_view kSpaced = R"({"t":"set", "id" : 65535 })";
    assert(extractRequestId(std::span<const char>{kSpaced.data(), kSpaced.size()}) == 65535U);
    constexpr std::string_view kBadId = R"({"t":"set","id":65536})";
    assert(extractRequestId(std::span<const char>{kBadId.data(), kBadId.size()}) == 0U);
    constexpr std::string_view kNoId = R"({"t":"set"})";
    assert(extractRequestId(std::span<const char>{kNoId.data(), kNoId.size()}) == 0U);

    // 控制事件邮箱（含会话代号与空闲槽位统计）。
    WebCtrlMailbox<4U> ctrl{};
    assert(ctrl.freeSlots() == 4U);
    assert(ctrl.tryPush({WebCtrlEvent::Kind::kConnect, 5, 1U}));
    assert(ctrl.freeSlots() == 3U);
    assert(ctrl.tryPush({WebCtrlEvent::Kind::kDisconnect, 5, 1U}));
    assert(ctrl.freeSlots() == 2U);
    WebCtrlEvent ce{};
    assert(ctrl.tryPop(ce) && ce.kind == WebCtrlEvent::Kind::kConnect && ce.fd == 5 &&
           ce.gen == 1U);
    assert(ctrl.tryPop(ce) && ce.kind == WebCtrlEvent::Kind::kDisconnect && ce.fd == 5 &&
           ce.gen == 1U);
    assert(ctrl.freeSlots() == 4U);
    assert(!ctrl.tryPop(ce));

    // 并发 SPSC 压力测试：生产者线程与消费者主线程无锁交接 4000 帧（含外供存储视图版）。
    std::array<WebFrame, 4U> extSlots{};
    WebFrameMailboxView conc{};
    assert(!conc.bind({}));
    assert(!conc.bind(std::span<WebFrame>{extSlots}.first(3U)));
    assert(conc.tryPush(1, 1U, std::span<const char>{kF1.data(), kF1.size()}) ==
           MailboxPush::kFull);
    assert(!conc.tryPop(out));
    assert(conc.bind(extSlots));
    assert(conc.capacity() == 4U && conc.freeSlots() == 4U);
    assert(conc.tryPush(1, 1U, std::span<const char>{oversize}) == MailboxPush::kTooLong);
    constexpr uint32_t kTotal = 4000U;
    std::atomic<uint32_t> fullRetries{0U};
    std::thread producer([&]() {
        std::array<char, 32U> msg{};
        for (uint32_t i = 0U; i < kTotal; ++i) {
            const int n = std::snprintf(msg.data(), msg.size(), "seq=%u", static_cast<unsigned>(i));
            assert(n > 0);
            const auto span = std::span<const char>{msg.data(), static_cast<size_t>(n)};
            while (conc.tryPush(static_cast<int>(i % 7U), i + 1U, span) == MailboxPush::kFull) {
                fullRetries.fetch_add(1U, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
    });
    std::array<char, 32U> expected{};
    for (uint32_t i = 0U; i < kTotal; ++i) {
        while (!conc.tryPop(out)) {
            std::this_thread::yield();
        }
        const int n =
            std::snprintf(expected.data(), expected.size(), "seq=%u", static_cast<unsigned>(i));
        assert(n > 0);
        assert(out.fd == static_cast<int>(i % 7U));
        assert(out.gen == (i + 1U));
        assert(std::string_view(out.data.data(), out.len) ==
               std::string_view(expected.data(), static_cast<size_t>(n)));
    }
    producer.join();
    assert(conc.empty() && conc.freeSlots() == 4U);
    conc.unbind();
    assert(conc.tryPush(1, 1U, std::span<const char>{kF1.data(), kF1.size()}) ==
           MailboxPush::kFull);
}

/**
 * @brief 验证 hello/config/status/ack/bye 帧与 ws-protocol.md 一致，以及 status 分组差分
 * @note 主机单线程测试。
 */
void testJsonFramesAndDiff()
{
    std::array<char, web::kMaxFrameBytes> buf{};

    settings::Service service{};
    service.bindBackend({.get = [](void *, settings::KeyId id, settings::Value &out) noexcept {
        switch (id) {
        case settings::KeyId::kGnssRate:
            out = {.number = 10};
            return true;
        case settings::KeyId::kGnssOutGsv:
            out = {.number = 5000};
            return true;
        case settings::KeyId::kBtPairNext:
            out = {.kind = settings::ValueKind::kBool, .boolVal = true};
            return true;
        case settings::KeyId::kGnssGgaGateEnabled:
            out = {.kind = settings::ValueKind::kBool, .boolVal = true};
            return true;
        case settings::KeyId::kGnssGgaGateMinSats:
            out = {.number = 12};
            return true;
        default:
            return false;
        }
    }});
    settings::GnssCaps caps{.valid = true,
                            .enabled = true,
                            .navRateHz = 10U,
                            .navRatesHz = {1U, 2U, 5U, 10U},
                            .navRateCount = 4U};
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGsv)] = {
        {0U, 1000U, 5000U}, 3U, true};

    // hello
    const size_t helloLen = buildHelloFrame({.board = "P2",
                                             .fw = "6.1.0-dev",
                                             .btName = "P2-B5F2",
                                             .console = "dev",
                                             .factoryUnlocked = false},
                                            service, caps, true, buf);
    const std::string_view hello(buf.data(), helloLen);
    assert(hello.starts_with(
        R"({"t":"hello","proto":1,"board":"P2","fw":"6.1.0-dev","bt_name":"P2-B5F2","console":"dev","factory":false,"caps":{)"));
    assert(hello.find(R"("gnss.out.gsv":[0,1000,5000])") != std::string_view::npos);
    assert(hello.find(R"("gnss.gga_gate.min_sats":{"min":0,"max":40})") != std::string_view::npos);
    assert(hello.find("gnss.const") == std::string_view::npos);

    const size_t cfgLen = buildConfigFrame(service, buf);
    assert(
        std::string_view(buf.data(), cfgLen) ==
        R"({"t":"config","gnss.rate":10,"gnss.out.gsv":5000,"gga_gate":{"enabled":true,"min_sats":12},"bt.pair_next":1})");

    // status 全量 + 差分（只发变化组）+ 可选字段省略
    StatusSnapshot s1{};
    s1.dev = {.uptimeS = 42, .heapFree = 65536, .heapMin = 50000, .reset = "software"};
    s1.gnss.state = "running";
    std::memcpy(s1.gnss.model.data(), "CM680", 6U);
    s1.gnss.hasModel = true;
    std::memcpy(s1.gnss.fw.data(), "1.2.3", 6U);
    s1.gnss.hasFw = true;
    s1.gnss.fix = "fixed";
    s1.gnss.sats = 24;
    s1.gnss.hasSats = true;
    s1.gnss.haccMm = 14;
    s1.gnss.hasHacc = true;
    s1.gnss.hz = 10;
    s1.gnss.hasHz = true;
    s1.bt.whitelistCount = 1U;
    s1.bt.whitelist[0] = {{0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0x01U}};

    const size_t fullLen = buildStatusFrame(s1, nullptr, buf);
    assert(
        std::string_view(buf.data(), fullLen) ==
        R"({"t":"status","dev":{"uptime_s":42,"heap_free":65536,"heap_min":50000,"reset":"software"},"gnss":{"state":"running","model":"CM680","fw":"1.2.3","fix":"fixed","sats":24,"hacc_mm":14,"hz":10},"bt":{"whitelist":["AA:BB:CC:DD:EE:01"]}})");

    // 无变化返回 0。
    assert(buildStatusFrame(s1, &s1, buf) == 0U);

    // 仅 dev 变化 -> 只发 dev。
    StatusSnapshot s2 = s1;
    s2.dev.uptimeS = 43;
    const size_t devOnly = buildStatusFrame(s2, &s1, buf);
    assert(
        std::string_view(buf.data(), devOnly) ==
        R"({"t":"status","dev":{"uptime_s":43,"heap_free":65536,"heap_min":50000,"reset":"software"}})");

    // 仅 bt 变化 -> 只发 bt。
    StatusSnapshot s3 = s2;
    s3.bt.whitelistCount = 0U;
    const size_t btOnly = buildStatusFrame(s3, &s2, buf);
    assert(std::string_view(buf.data(), btOnly) == R"({"t":"status","bt":{"whitelist":[]}})");

    // 仅 gnss 变化且数值不可得 -> 省略 model/fw/sats/hacc_mm/hz。
    StatusSnapshot s4 = s3;
    s4.gnss = GnssWebStatus{};
    s4.gnss.state = "probing";
    s4.gnss.fix = "none";
    const size_t gnssOnly = buildStatusFrame(s4, &s3, buf);
    assert(std::string_view(buf.data(), gnssOnly) ==
           R"({"t":"status","gnss":{"state":"probing","fix":"none"}})");

    // ack 与 bye
    assert(std::string_view(buf.data(), buildAckFrame(7U, web::ErrorCode::kNone, buf)) ==
           R"({"t":"ack","id":7,"ok":true})");
    assert(std::string_view(buf.data(), buildAckFrame(8U, web::ErrorCode::kLocked, buf)) ==
           R"({"t":"ack","id":8,"ok":false,"err":"locked"})");
    assert(std::string_view(buf.data(), buildByeFrame("idle", buf)) ==
           R"({"t":"bye","reason":"idle"})");
}

/**
 * @brief 验证空闲回落计时：无终端、最后终端断开、期间重连取消及毫秒回绕
 * @note 主机单线程测试。
 */
void testIdleTimer()
{
    constexpr uint32_t kTimeoutMs = 300000U;

    // 场景 1：从未有终端连上 -> 满 300 s 到期（含 uint32 回绕）。
    WebIdleTimer timer{};
    const uint32_t wrapStart = UINT32_MAX - 100000U;
    timer.start(wrapStart, kTimeoutMs);
    assert(timer.armed());
    assert(!timer.tick(0U, 0U, wrapStart + kTimeoutMs - 1U));
    assert(timer.tick(0U, 0U, wrapStart + kTimeoutMs));

    // 场景 2：终端一直连接不触发；最后终端断开后重新计满 300 s 才触发。
    timer.start(1000U, kTimeoutMs);
    assert(!timer.tick(1U, 1U, 1000U + kTimeoutMs * 2U));
    assert(!timer.armed());
    // 两台终端在线，断开一台仍不计时。
    assert(!timer.tick(2U, 2U, 700000U));
    assert(!timer.tick(1U, 3U, 800000U));
    // 最后一台断开：从 900000 开始计时。
    assert(!timer.tick(0U, 4U, 900000U));
    assert(timer.armed());
    assert(!timer.tick(0U, 4U, 900000U + kTimeoutMs - 1U));
    assert(timer.tick(0U, 4U, 900000U + kTimeoutMs));

    // 场景 3：断开后计时中途重连取消，再次断开重头计满 300 s。
    timer.start(0U, kTimeoutMs);
    assert(!timer.tick(0U, 0U, 200000U));
    assert(!timer.tick(1U, 1U, 250000U));
    assert(!timer.armed());
    assert(!timer.tick(0U, 2U, 260000U));
    assert(!timer.tick(0U, 2U, 260000U + kTimeoutMs - 1U));
    assert(timer.tick(0U, 2U, 260000U + kTimeoutMs));

    // 场景 4：两次 poll 之间发生短连又断开（activitySeq 变化），计时重置。
    timer.start(0U, kTimeoutMs);
    assert(!timer.tick(0U, 0U, 290000U));
    assert(!timer.tick(0U, 2U, 295000U));
    assert(!timer.tick(0U, 2U, 295000U + kTimeoutMs - 1U));
    assert(timer.tick(0U, 2U, 295000U + kTimeoutMs));

    // 场景 5：一致快照打包与空闲重启提交前复核（P2 #8）。
    const uint32_t idleSnap = packStaState(0U, 7U);
    assert(staStateCount(idleSnap) == 0U && staStateSeq(idleSnap) == 7U);
    assert(canCommitIdleExit(idleSnap, 7U));
    // 已排定 idle 重启后终端重连（count=1, seq=8）→ 必须中止提交。
    assert(!canCommitIdleExit(packStaState(1U, 8U), 7U));
    // 期间短连又断开（count=0, seq=9）→ 序号变化也必须中止提交。
    assert(!canCommitIdleExit(packStaState(0U, 9U), 7U));
}

} // namespace

/**
 * @brief 运行全部网页配置纯逻辑主机测试
 * @retval 0 全部通过
 * @note 主机测试入口。
 */
int main()
{
    testMailbox();
    testJsonFramesAndDiff();
    testIdleTimer();
    std::puts("PASS web_server_check");
    return 0;
}
