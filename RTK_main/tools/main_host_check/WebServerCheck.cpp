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
 * @brief 验证协议 §4 全部可改键的翻译、越界 bad_value、未解锁 locked 与未知键 unknown_key
 * @note 主机单线程测试。
 */
void testSetTranslation()
{
    const auto baseOut = data::GnssOutConfig::defaults();

    // gnss.rate
    {
        const auto plan = translateSetRequest("gnss.rate", "10", false, baseOut);
        assert(plan.error == web::ErrorCode::kNone);
        assert(plan.target == CommandTarget::kGnss && plan.waitGnssApply);
        assert(plan.command.target == id(ModuleKind::kGnss));
        assert(plan.command.verb == static_cast<uint8_t>(GnssVerb::kSetRate));
        assert(plan.command.argLen == 1U && plan.command.args[0] == 10U);

        assert(translateSetRequest("gnss.rate", "3", false, baseOut).error ==
               web::ErrorCode::kBadValue);
    }

    // gnss.out.*（5 条语句）
    struct OutCase {
        std::string_view key;
        std::string_view val;
        size_t idx;
        int8_t expectInterval;
        bool expectEnabled;
    };
    const std::array<OutCase, 6U> outCases{{
        {"gnss.out.gga", "100", static_cast<size_t>(::gnss::OutputMsg::kGga), 10, true},
        {"gnss.out.gsa", "1000", static_cast<size_t>(::gnss::OutputMsg::kGsa), -1, true},
        {"gnss.out.gsv", "0", static_cast<size_t>(::gnss::OutputMsg::kSatsInfo), 0, false},
        {"gnss.out.gst", "500", static_cast<size_t>(::gnss::OutputMsg::kGst), 2, true},
        {"gnss.out.rmc", "50", static_cast<size_t>(::gnss::OutputMsg::kRmc), 20, true},
        {"gnss.out.rmc", "200", static_cast<size_t>(::gnss::OutputMsg::kRmc), 5, true},
    }};
    for (const auto &c : outCases) {
        const auto plan = translateSetRequest(c.key, c.val, false, baseOut);
        assert(plan.error == web::ErrorCode::kNone);
        assert(plan.target == CommandTarget::kGnss && plan.waitGnssApply);
        assert(plan.command.verb == static_cast<uint8_t>(GnssSubVerb::kSetOut));
        assert(plan.command.argLen == data::GnssOutConfig::kBlobBytes);
        data::GnssOutConfig decoded{};
        assert(fw::isOk(data::GnssOutConfig::decode(
            std::span<const uint8_t>{plan.command.args}.first(plan.command.argLen), decoded)));
        const bool bitOn = (decoded.mask & (1U << c.idx)) != 0U;
        assert(bitOn == c.expectEnabled);
        assert(decoded.intervals[c.idx] == c.expectInterval);
    }
    assert(translateSetRequest("gnss.out.gga", "150", false, baseOut).error ==
           web::ErrorCode::kBadValue);

    // Q1 GGA 质量门控两键 + 越界/前导零 bad_value
    {
        const auto en = translateSetRequest("gnss.gga_gate.enabled", "1", false, baseOut);
        assert(en.error == web::ErrorCode::kNone && en.target == CommandTarget::kGnss &&
               !en.waitGnssApply);
        assert(en.command.verb == static_cast<uint8_t>(GnssSubVerb::kSetGgaGate));
        assert(en.command.argLen == kGgaGateCmdArgLen);
        assert(en.command.args[0] == kGgaGateFlagEnabled && en.command.args[1] == 1U);

        const auto dis = translateSetRequest("gnss.gga_gate.enabled", "0", false, baseOut);
        assert(dis.error == web::ErrorCode::kNone && dis.command.args[0] == kGgaGateFlagEnabled &&
               dis.command.args[1] == 0U);
        assert(translateSetRequest("gnss.gga_gate.enabled", "2", false, baseOut).error ==
               web::ErrorCode::kBadValue);

        const auto min0 = translateSetRequest("gnss.gga_gate.min_sats", "0", false, baseOut);
        assert(min0.error == web::ErrorCode::kNone && min0.command.args[0] == kGgaGateFlagMinSats &&
               min0.command.args[2] == 0U);
        const auto min40 = translateSetRequest("gnss.gga_gate.min_sats", "40", false, baseOut);
        assert(min40.error == web::ErrorCode::kNone &&
               min40.command.args[0] == kGgaGateFlagMinSats && min40.command.args[2] == 40U);
        assert(translateSetRequest("gnss.gga_gate.min_sats", "41", false, baseOut).error ==
               web::ErrorCode::kBadValue);
        assert(translateSetRequest("gnss.gga_gate.min_sats", "-1", false, baseOut).error ==
               web::ErrorCode::kBadValue);
        assert(translateSetRequest("gnss.gga_gate.min_sats", "08", false, baseOut).error ==
               web::ErrorCode::kBadValue);
    }

    // gnss.const.* -> unsupported
    for (const std::string_view ck :
         {"gnss.const.gps", "gnss.const.bds", "gnss.const.glo", "gnss.const.gal"}) {
        assert(translateSetRequest(ck, "1", false, baseOut).error == web::ErrorCode::kUnsupported);
    }

    // bt.wl.add / bt.wl.del
    {
        const auto add = translateSetRequest("bt.wl.add", "AA:BB:CC:DD:EE:01", false, baseOut);
        assert(add.error == web::ErrorCode::kNone && add.target == CommandTarget::kStorage &&
               add.notifyBtStatus);
        assert(add.command.target == id(ModuleKind::kStorage));
        assert(add.command.verb == verb(StorageVerb::kWhitelistAdd));
        assert(add.command.argLen == core::kAddrLen);
        assert(add.command.args[0] == 0xAAU && add.command.args[5] == 0x01U);

        const auto del = translateSetRequest("bt.wl.del", "aa:bb:cc:dd:ee:01", false, baseOut);
        assert(del.error == web::ErrorCode::kNone && del.target == CommandTarget::kStorage &&
               del.notifyBtStatus);
        assert(del.command.verb == verb(StorageVerb::kWhitelistDel));
        assert(del.command.args[0] == 0xAAU && del.command.args[5] == 0x01U);

        assert(translateSetRequest("bt.wl.add", "AA:BB:CC", false, baseOut).error ==
               web::ErrorCode::kBadValue);
    }

    // bt.pair_next
    {
        const auto pn = translateSetRequest("bt.pair_next", "1", false, baseOut);
        assert(pn.error == web::ErrorCode::kNone && pn.target == CommandTarget::kStorage);
        assert(pn.command.verb == verb(StorageVerb::kSetPairNext));
        assert(pn.command.argLen == 1U && pn.command.args[0] == 1U);
        assert(translateSetRequest("bt.pair_next", "2", false, baseOut).error ==
               web::ErrorCode::kBadValue);
    }

    // factory.sn（锁定 -> locked；解锁后有效 -> kSaveSn；非法 -> bad_value）
    {
        assert(translateSetRequest("factory.sn", "P2A0001", false, baseOut).error ==
               web::ErrorCode::kLocked);
        const auto ok = translateSetRequest("factory.sn", "P2A0001", true, baseOut);
        assert(ok.error == web::ErrorCode::kNone && ok.target == CommandTarget::kStorage);
        assert(ok.command.verb == verb(StorageVerb::kSaveSn));
        assert(ok.command.argLen == 7U);
        assert(std::memcmp(ok.command.args.data(), "P2A0001", 7U) == 0);
        assert(translateSetRequest("factory.sn", "BAD SN!", true, baseOut).error ==
               web::ErrorCode::kBadValue);
    }

    // 未知键
    assert(translateSetRequest("no.such.key", "1", true, baseOut).error ==
           web::ErrorCode::kUnknownKey);
}

/**
 * @brief 验证 hello/config/status/ack/bye 帧与 ws-protocol.md 一致，以及 status 分组差分
 * @note 主机单线程测试。
 */
void testJsonFramesAndDiff()
{
    std::array<char, web::kMaxFrameBytes> buf{};

    // hello
    const size_t helloLen = buildHelloFrame({.board = "P2",
                                             .fw = "6.1.0-dev",
                                             .btName = "P2-B5F2",
                                             .console = "dev",
                                             .factoryUnlocked = false},
                                            buf);
    assert(
        std::string_view(buf.data(), helloLen) ==
        R"({"t":"hello","proto":1,"board":"P2","fw":"6.1.0-dev","bt_name":"P2-B5F2","console":"dev","factory":false})");

    // config（含扁平键与嵌套 gga_gate 对象）
    auto outCfg = data::GnssOutConfig::defaults();
    assert(fw::isOk(outCfg.applyInterval(static_cast<size_t>(::gnss::OutputMsg::kSatsInfo), -1)));
    const size_t cfgLen = buildConfigFrame({.gnssRateHz = 10U,
                                            .gnssOut = outCfg,
                                            .ggaGateEnabled = true,
                                            .ggaGateMinSats = 12U,
                                            .btPairNext = true,
                                            .sn = "P2A0001"},
                                           buf);
    assert(
        std::string_view(buf.data(), cfgLen) ==
        R"({"t":"config","gnss.rate":10,"gnss.out.gga":100,"gnss.out.gsa":100,"gnss.out.gsv":1000,"gnss.out.gst":100,"gnss.out.rmc":100,"gga_gate":{"enabled":true,"min_sats":12},"bt.pair_next":1,"factory.sn":"P2A0001"})");

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
    testSetTranslation();
    testJsonFramesAndDiff();
    testIdleTimer();
    std::puts("PASS web_server_check");
    return 0;
}
