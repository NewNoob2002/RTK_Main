// 主机回归：验证网页配置模块的 SPSC 帧邮箱、set 命令翻译、JSON 编码/状态差分与空闲回落计时。
// 主机测试（含多线程 SPSC 压力与 ASan/UBSan），非 ISR。
#include "modules/web/WebDns.hpp"
#include "modules/web/WebIdleTimer.hpp"
#include "modules/web/WebLogic.hpp"
#include "modules/web/WebMailbox.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

    // 当 constMask 支持星座开关时，hello.caps 与 config 均输出 gnss.const.*（0/1 整数编码）。
    settings::GnssCaps constCaps = caps;
    constCaps.constMask = settings::const_bit::kGps | settings::const_bit::kBds |
                          settings::const_bit::kGlo | settings::const_bit::kGal;
    settings::Service constService{};
    constService.bindBackend(
        {.ctx = &constCaps, .get = [](void *, settings::KeyId id, settings::Value &out) noexcept {
             switch (id) {
             case settings::KeyId::kGnssConstGps:
                 out = {.kind = settings::ValueKind::kBool, .boolVal = true};
                 return true;
             case settings::KeyId::kGnssConstGlo:
                 out = {.kind = settings::ValueKind::kBool, .boolVal = false};
                 return true;
             default:
                 return false;
             }
         }});
    const size_t constHelloLen = buildHelloFrame({.board = "P2",
                                                  .fw = "6.1.0-dev",
                                                  .btName = "P2-B5F2",
                                                  .console = "dev",
                                                  .factoryUnlocked = false},
                                                 constService, constCaps, true, buf);
    const std::string_view constHello(buf.data(), constHelloLen);
    assert(constHello.find(R"("gnss.const.gps":[0,1])") != std::string_view::npos);
    assert(constHello.find(R"("gnss.const.bds":[0,1])") != std::string_view::npos);
    assert(constHello.find(R"("gnss.const.glo":[0,1])") != std::string_view::npos);
    assert(constHello.find(R"("gnss.const.gal":[0,1])") != std::string_view::npos);
    const size_t constCfgLen = buildConfigFrame(constService, buf);
    assert(std::string_view(buf.data(), constCfgLen) ==
           R"({"t":"config","gnss.const.gps":1,"gnss.const.glo":0})");

    // RU1-02/08：F9P 能力掩码隐藏 gnss.const.gps，但暴露 gnss.const.bds/glo/gal 与 gnss.cutoff（0..90）。
    constexpr uint8_t kGpsRequiredBit = 1U << 7U;
    settings::GnssCaps f9pCaps = caps;
    f9pCaps.constMask = kGpsRequiredBit | settings::const_bit::kBds | settings::const_bit::kGlo |
                        settings::const_bit::kGal;
    const settings::KeySpec *const cutoffSpec = settings::findKey("gnss.cutoff");
    assert(cutoffSpec != nullptr);
    const settings::KeyId cutoffId = cutoffSpec->id;
    settings::Service f9pService{};
    f9pService.bindBackend(
        {.ctx = const_cast<settings::KeyId *>(&cutoffId),
         .get = [](void *ctx, settings::KeyId id, settings::Value &out) noexcept {
             if (id == *static_cast<const settings::KeyId *>(ctx)) {
                 out = {.kind = settings::ValueKind::kInt, .number = 25};
                 return true;
             }
             if (id == settings::KeyId::kGnssConstBds) {
                 out = {.kind = settings::ValueKind::kBool, .boolVal = true};
                 return true;
             }
             return false;
         }});
    const size_t f9pHelloLen = buildHelloFrame({.board = "P2",
                                                .fw = "6.1.0-dev",
                                                .btName = "P2-B5F2",
                                                .console = "dev",
                                                .factoryUnlocked = false},
                                               f9pService, f9pCaps, true, buf);
    const std::string_view f9pHello(buf.data(), f9pHelloLen);
    assert(f9pHello.find("gnss.const.gps") == std::string_view::npos);
    assert(f9pHello.find(R"("gnss.const.bds":[0,1])") != std::string_view::npos);
    assert(f9pHello.find(R"("gnss.cutoff":{"min":0,"max":90})") != std::string_view::npos);
    const size_t f9pCfgLen = buildConfigFrame(f9pService, buf);
    assert(std::string_view(buf.data(), f9pCfgLen) ==
           R"({"t":"config","gnss.const.bds":1,"gnss.cutoff":25})");

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

/**
 * @brief 按 RFC 1035 构造单问句 DNS 查询报文供测试使用
 * @param id: 事务 ID
 * @param flags: 标志位（如 0x0100 表示 RD=1）
 * @param qdCount: QDCOUNT 字段值
 * @param labels: 域名标签列表（空列表表示根域）
 * @param qtype: 查询类型（1=A, 28=AAAA 等）
 * @param qclass: 查询类（1=IN）
 * @retval 构造好的原始字节序列
 * @note 主机测试辅助函数。
 */
std::vector<uint8_t> makeDnsQuery(uint16_t id, uint16_t flags, uint16_t qdCount,
                                  std::span<const std::string_view> labels, uint16_t qtype,
                                  uint16_t qclass)
{
    std::vector<uint8_t> pkt(12U, 0U);
    pkt[0] = static_cast<uint8_t>((id >> 8U) & 0xFFU);
    pkt[1] = static_cast<uint8_t>(id & 0xFFU);
    pkt[2] = static_cast<uint8_t>((flags >> 8U) & 0xFFU);
    pkt[3] = static_cast<uint8_t>(flags & 0xFFU);
    pkt[4] = static_cast<uint8_t>((qdCount >> 8U) & 0xFFU);
    pkt[5] = static_cast<uint8_t>(qdCount & 0xFFU);
    for (const auto label : labels) {
        pkt.push_back(static_cast<uint8_t>(label.size()));
        for (const char ch : label) {
            pkt.push_back(static_cast<uint8_t>(ch));
        }
    }
    pkt.push_back(0U);
    pkt.push_back(static_cast<uint8_t>((qtype >> 8U) & 0xFFU));
    pkt.push_back(static_cast<uint8_t>(qtype & 0xFFU));
    pkt.push_back(static_cast<uint8_t>((qclass >> 8U) & 0xFFU));
    pkt.push_back(static_cast<uint8_t>(qclass & 0xFFU));
    return pkt;
}

/**
 * @brief 验证强制门户 DNS 编解码：A 应答、AAAA/其他 NOERROR 空应答与各类畸形报文丢弃
 * @note 主机单线程测试，配合 ASan/UBSan 覆盖边界检查。
 */
void testDnsCodec()
{
    constexpr std::array<uint8_t, 4U> kPortalIp = {192U, 168U, 10U, 12U};
    std::array<uint8_t, kDnsMaxPacketBytes> reply{};

    // 1. 正常 A 查询（多标签域名，RD=1）→ 应答 192.168.10.12，TTL 60 s，NOERROR。
    const std::array<std::string_view, 3U> hostLabels = {"connectivitycheck", "gstatic", "com"};
    const auto aQuery = makeDnsQuery(0x1234U, 0x0100U, 1U, hostLabels, 1U, 1U);
    const size_t aLen = buildDnsReply(aQuery, kPortalIp, reply);
    assert(aLen == (aQuery.size() + 16U));
    assert((reply[0] == 0x12U) && (reply[1] == 0x34U));   // ID
    assert((reply[2] == 0x85U) && (reply[3] == 0x00U));   // QR=1, AA=1, RD=1, RCODE=0
    assert((reply[4] == 0x00U) && (reply[5] == 0x01U));   // QDCOUNT=1
    assert((reply[6] == 0x00U) && (reply[7] == 0x01U));   // ANCOUNT=1
    assert((reply[8] == 0x00U) && (reply[9] == 0x00U));   // NSCOUNT=0
    assert((reply[10] == 0x00U) && (reply[11] == 0x00U)); // ARCOUNT=0
    const size_t ans = aQuery.size();
    assert((reply[ans] == 0xC0U) && (reply[ans + 1U] == 0x0CU));      // 指针指向偏移 12
    assert((reply[ans + 2U] == 0x00U) && (reply[ans + 3U] == 0x01U)); // TYPE=A
    assert((reply[ans + 4U] == 0x00U) && (reply[ans + 5U] == 0x01U)); // CLASS=IN
    assert((reply[ans + 6U] == 0x00U) && (reply[ans + 7U] == 0x00U) && (reply[ans + 8U] == 0x00U) &&
           (reply[ans + 9U] == 60U));                                   // TTL=60
    assert((reply[ans + 10U] == 0x00U) && (reply[ans + 11U] == 0x04U)); // RDLENGTH=4
    assert((reply[ans + 12U] == 192U) && (reply[ans + 13U] == 168U) && (reply[ans + 14U] == 10U) &&
           (reply[ans + 15U] == 12U));

    // 1b. 带合法 EDNS0 OPT 附加节的 A 查询（ARCOUNT=1）应回带最小 OPT（根名、type 41、UDP 512、flags 0、RDLEN 0）且 ARCOUNT=1。
    auto ednsQuery = makeDnsQuery(0x2222U, 0x0000U, 1U, hostLabels, 1U, 1U);
    ednsQuery[11] = 1U; // ARCOUNT=1
    const std::array<uint8_t, 11U> optRec = {0U, 0U, 41U, 0x04U, 0xD0U, 0U, 0U, 0U, 0U, 0U, 0U};
    ednsQuery.insert(ednsQuery.end(), optRec.begin(), optRec.end());
    const size_t ednsLen = buildDnsReply(ednsQuery, kPortalIp, reply);
    assert(ednsLen == (aQuery.size() + 16U + 11U));
    assert((reply[2] == 0x84U) && (reply[3] == 0x00U));   // RD=0 保留
    assert((reply[6] == 0x00U) && (reply[7] == 0x01U));   // ANCOUNT=1
    assert((reply[10] == 0x00U) && (reply[11] == 0x01U)); // ARCOUNT=1
    const size_t optOff = aQuery.size() + 16U;
    assert(reply[optOff] == 0x00U);                                         // 根名
    assert((reply[optOff + 1U] == 0x00U) && (reply[optOff + 2U] == 41U));   // TYPE=41 OPT
    assert((reply[optOff + 3U] == 0x02U) && (reply[optOff + 4U] == 0x00U)); // UDP=512
    assert((reply[optOff + 5U] == 0x00U) && (reply[optOff + 6U] == 0x00U) &&
           (reply[optOff + 7U] == 0x00U) && (reply[optOff + 8U] == 0x00U));  // ext/ver/flags=0
    assert((reply[optOff + 9U] == 0x00U) && (reply[optOff + 10U] == 0x00U)); // RDLENGTH=0

    // 1c. 带 RDATA 的合法 OPT（含 4 字节选项载荷）与 AAAA + OPT 均合法且应答回最小 OPT。
    auto ednsWithData = makeDnsQuery(0x2223U, 0x0100U, 1U, hostLabels, 28U, 1U);
    ednsWithData[11] = 1U;
    const std::array<uint8_t, 15U> optWithData = {0U, 0U, 41U, 0x05U, 0xC0U, 0U,    0U,   0U,
                                                  0U, 0U, 4U,  0xAAU, 0xBBU, 0xCCU, 0xDDU};
    ednsWithData.insert(ednsWithData.end(), optWithData.begin(), optWithData.end());
    const size_t aaaaOptLen = buildDnsReply(ednsWithData, kPortalIp, reply);
    assert(aaaaOptLen == (aQuery.size() + 11U));
    assert((reply[6] == 0x00U) && (reply[7] == 0x00U));   // ANCOUNT=0
    assert((reply[10] == 0x00U) && (reply[11] == 0x01U)); // ARCOUNT=1

    // 1d. 255 B 合法域名边界（64+64+64+62+1=255 B）通过，256 B 域名（64+64+64+63+1=256 B）拒绝。
    const std::string lbl63(63U, 'a');
    const std::string lbl61(61U, 'b');
    const std::string lbl62(62U, 'c');
    const std::array<std::string_view, 4U> maxName255 = {lbl63, lbl63, lbl63, lbl61};
    const auto q255 = makeDnsQuery(0x3333U, 0x0100U, 1U, maxName255, 1U, 1U);
    assert(buildDnsReply(q255, kPortalIp, reply) == (q255.size() + 16U));
    const std::array<std::string_view, 4U> overName256 = {lbl63, lbl63, lbl63, lbl62};
    const auto q256 = makeDnsQuery(0x3334U, 0x0100U, 1U, overName256, 1U, 1U);
    assert(buildDnsReply(q256, kPortalIp, reply) == 0U);

    // 2. AAAA（28）与其他类型（HTTPS=65, TXT=16）→ NOERROR 空应答（ANCOUNT=0）。
    for (const uint16_t qtype : std::array<uint16_t, 3U>{28U, 65U, 16U}) {
        const auto q = makeDnsQuery(0x5678U, 0x0100U, 1U, hostLabels, qtype, 1U);
        const size_t len = buildDnsReply(q, kPortalIp, reply);
        assert(len == q.size());
        assert((reply[2] == 0x85U) && (reply[3] == 0x00U)); // NOERROR
        assert((reply[6] == 0x00U) && (reply[7] == 0x00U)); // ANCOUNT=0
    }

    // 3. 畸形报文全部丢弃（返回 0）且无越界：
    // 3a. 过短 / 截断报文。
    assert(buildDnsReply({}, kPortalIp, reply) == 0U);
    assert(buildDnsReply(std::span<const uint8_t>{aQuery}.first(11U), kPortalIp, reply) == 0U);
    assert(buildDnsReply(std::span<const uint8_t>{aQuery}.first(16U), kPortalIp, reply) == 0U);
    assert(buildDnsReply(std::span<const uint8_t>{aQuery}.first(aQuery.size() - 1U), kPortalIp,
                         reply) == 0U);
    assert(buildDnsReply(std::span<const uint8_t>{aQuery}.first(aQuery.size() - 4U), kPortalIp,
                         reply) == 0U);

    // 3b. QDCOUNT != 1（0 或 2）。
    assert(buildDnsReply(makeDnsQuery(1U, 0x0100U, 0U, hostLabels, 1U, 1U), kPortalIp, reply) ==
           0U);
    assert(buildDnsReply(makeDnsQuery(1U, 0x0100U, 2U, hostLabels, 1U, 1U), kPortalIp, reply) ==
           0U);

    // 3c. QR=1（已是应答）、OPCODE!=0、TC=1、ANCOUNT!=0、NSCOUNT!=0。
    assert(buildDnsReply(makeDnsQuery(1U, 0x8100U, 1U, hostLabels, 1U, 1U), kPortalIp, reply) ==
           0U);
    assert(buildDnsReply(makeDnsQuery(1U, 0x0800U, 1U, hostLabels, 1U, 1U), kPortalIp, reply) ==
           0U);
    assert(buildDnsReply(makeDnsQuery(1U, 0x0200U, 1U, hostLabels, 1U, 1U), kPortalIp, reply) ==
           0U);
    auto badAn = aQuery;
    badAn[7] = 1U;
    assert(buildDnsReply(badAn, kPortalIp, reply) == 0U);
    auto badNs = aQuery;
    badNs[9] = 1U;
    assert(buildDnsReply(badNs, kPortalIp, reply) == 0U);

    // 3d. 附加段严格校验（F4）：尾部垃圾、ARCOUNT 缺记录、ARCOUNT=2、OPT 截断、非根名/非 OPT、RDLENGTH 越界。
    auto trailingGarbage = aQuery;
    trailingGarbage.push_back(0x00U); // ARCOUNT=0 但有尾部垃圾
    assert(buildDnsReply(trailingGarbage, kPortalIp, reply) == 0U);

    auto arMissing = aQuery;
    arMissing[11] = 1U; // ARCOUNT=1 但缺少 OPT 记录
    assert(buildDnsReply(arMissing, kPortalIp, reply) == 0U);

    auto arTwo = ednsQuery;
    arTwo[11] = 2U; // ARCOUNT=2 不允许
    assert(buildDnsReply(arTwo, kPortalIp, reply) == 0U);

    auto optTrunc = ednsQuery;
    optTrunc.pop_back(); // OPT 仅 10 B（截断）
    assert(buildDnsReply(optTrunc, kPortalIp, reply) == 0U);

    auto optBadName = ednsQuery;
    optBadName[aQuery.size()] = 0x01U; // OPT 非根名
    assert(buildDnsReply(optBadName, kPortalIp, reply) == 0U);

    auto optBadType = ednsQuery;
    optBadType[aQuery.size() + 2U] = 1U; // TYPE=A 而非 41(OPT)
    assert(buildDnsReply(optBadType, kPortalIp, reply) == 0U);

    auto optRdOver = ednsQuery;
    optRdOver[aQuery.size() + 10U] = 1U; // RDLENGTH=1 但无 RDATA（越界）
    assert(buildDnsReply(optRdOver, kPortalIp, reply) == 0U);

    auto optTrailing = ednsQuery;
    optTrailing.push_back(0xFFU); // OPT RDLENGTH=0 但尾部多出垃圾字节
    assert(buildDnsReply(optTrailing, kPortalIp, reply) == 0U);

    // 3e. 标签长度越界（声明长度大于剩余字节）。
    auto badLabelLen = aQuery;
    badLabelLen[12] = 63U;
    assert(buildDnsReply(badLabelLen, kPortalIp, reply) == 0U);

    // 3f. 压缩指针（0xC0 0x0C）与保留标签位（0x40/0x80）一律拒绝不跟随。
    std::vector<uint8_t> ptrPkt(18U, 0U);
    ptrPkt[5] = 1U;
    ptrPkt[12] = 0xC0U;
    ptrPkt[13] = 0x0CU;
    ptrPkt[15] = 1U;
    ptrPkt[17] = 1U;
    assert(buildDnsReply(ptrPkt, kPortalIp, reply) == 0U);
    ptrPkt[12] = 0x40U;
    assert(buildDnsReply(ptrPkt, kPortalIp, reply) == 0U);

    // 3g. 超长报文（> 512 B）与输出缓冲不足。
    std::vector<uint8_t> oversize(kDnsMaxPacketBytes + 1U, 0U);
    oversize[5] = 1U;
    assert(buildDnsReply(oversize, kPortalIp, reply) == 0U);
    assert(buildDnsReply(aQuery, kPortalIp, std::span<uint8_t>{reply}.first(64U)) == 0U);
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
    testDnsCodec();
    std::puts("PASS web_server_check");
    return 0;
}
