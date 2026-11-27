// 4G 联网纯逻辑回归（N1 复审 #7/#8/#9/#13，r3 I2/I7/I8）：控制台 AT 邮箱的白名单/归属/过期结算/并发
// 取消与完成竞态、应答前缀推导，以及低电量门控迟滞。
// 主机可测：NetMailbox/NetPowerGate 无 IDF 依赖。
#include <cstdio>
#include <array>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <string>
#include <string_view>

#include "modules/net/NetMailbox.hpp"
#include "modules/net/NetPolicy.hpp"

namespace {

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

using app::NetMailbox;
using cellular::AtResponse;
using cellular::AtResult;

AtResponse done(uint32_t token, AtResult result = AtResult::kOk, int32_t code = 0)
{
    AtResponse rsp;
    rsp.result = result;
    rsp.errorCode = code;
    rsp.token = token;
    return rsp;
}

void testForbiddenList()
{
    // 白名单（含大小写不敏感）：已批准的诊断命令。
    for (const char *ok : {"AT",
                           "ATI",
                           "AT+QGMR",
                           "AT+GSN",
                           "AT+QCCID",
                           "AT+CSQ",
                           "AT+CREG?",
                           "AT+CEREG?",
                           "AT+CGREG?",
                           "AT+COPS?",
                           "AT+CPIN?",
                           "AT+QNWINFO",
                           "AT+QENG=\"servingcell\"",
                           "AT+CGDCONT?",
                           "AT+QIACT?",
                           "AT+QISTATE?",
                           "at+cfun?",
                           "AT+CFUN=0",
                           "AT+CFUN=1",
                           "AT+CFUN=4",
                           "at+csq"}) {
        if (NetMailbox::forbidden(ok)) {
            std::fprintf(stderr, "whitelist rejected: %s\n", ok);
            std::abort();
        }
    }
    // 未识别/危险命令一律拒绝（I8）：复位、关机、AT+CMEE 改模式、波特率、APN/凭据、数据提示符、QIOPEN。
    for (const char *bad : {"",
                            "A",
                            "HELLO",
                            "ATE0",
                            "ate1",
                            "AT+IPR=9600",
                            "AT+IPR?",
                            "AT+QISWTMD=1,1,2",
                            "AT+QICSGP=1",
                            "AT+QICSGP=1,1,\"apn\",\"user\",\"pass\",1",
                            "AT+CFUN=1,1",
                            "AT+CFUN=2",
                            "AT+CFUN=",
                            "AT+CFUN=1,0",
                            "AT+QPOWD=1",
                            "AT+QPOWD=0",
                            "AT+CMEE=2",
                            "AT+CMEE?",
                            "AT+CSQ=1",
                            "AT+CPIN=\"1234\"",
                            "AT+COPS=0",
                            "AT+CGDCONT=1,\"IP\",\"apn\"",
                            "AT+QIACT=1",
                            "AT+QIDEACT=1",
                            "AT+QISEND=0,10",
                            "AT+QISENDEX=0,\"AB\"",
                            "AT+CMGS=\"123\"",
                            "AT+QIOPEN=1,0,\"TCP\",\"1.2.3.4\",80,0,0",
                            "AT+QIOPEN=1,0,\"TCP\",\"1.2.3.4\",80,0,1",
                            "AT+QENG=\"neighbourcell\"",
                            "AT+QGMR=1",
                            "ATD123;",
                            "ATH",
                            "AT&F",
                            "AT+CRESET",
                            "AT+QRST=1"}) {
        if (!NetMailbox::forbidden(bad)) {
            std::fprintf(stderr, "whitelist accepted: %s\n", bad);
            std::abort();
        }
    }
    // 拒绝：命令串联与控制字符。
    CHECK(NetMailbox::forbidden("AT+CSQ;+IPR=9600"));
    CHECK(NetMailbox::forbidden("AT+CSQ\r"));
    CHECK(NetMailbox::forbidden("AT+CSQ\n+IPR=1"));
    CHECK(NetMailbox::forbidden("AT+CSQ\x01"));
}

void testReplyPrefix()
{
    std::array<char, 16> buf{};
    CHECK(NetMailbox::replyPrefix("AT+CPIN?", buf) == "+CPIN:");
    CHECK(NetMailbox::replyPrefix("at+cereg?", buf) == "+CEREG:");
    CHECK(NetMailbox::replyPrefix("AT+CFUN=1", buf) == "+CFUN:");
    CHECK(NetMailbox::replyPrefix("AT+QENG=\"servingcell\"", buf) == "+QENG:");
    CHECK(NetMailbox::replyPrefix("AT", buf).empty());
    CHECK(NetMailbox::replyPrefix("ATI", buf).empty());
    // take() 把推导出的前缀交给引擎（同前缀 URC 与应答行的归属依据）。
    NetMailbox box;
    CHECK(box.submit("AT+CEREG?") == fw::Status::kOk);
    std::string_view cmd;
    std::string_view prefix;
    uint32_t token = 0U;
    CHECK(box.take(cmd, token, prefix));
    CHECK(prefix == "+CEREG:");
}

void testMailboxLifecycle()
{
    NetMailbox box;
    CHECK(box.state() == NetMailbox::State::kIdle);
    CHECK(box.submit("AT+CSQ") == fw::Status::kOk);
    CHECK(box.state() == NetMailbox::State::kPending);
    CHECK(box.submit("AT") == fw::Status::kBusy); // 未释放不得覆盖
    std::string_view cmd;
    std::string_view prefix;
    uint32_t token = 0U;
    CHECK(box.take(cmd, token, prefix));
    CHECK(cmd == "AT+CSQ");
    CHECK(token != 0U);
    CHECK(box.state() == NetMailbox::State::kRunning);
    box.onLine("+CSQ: 20,99");
    box.onDone(done(token));
    CHECK(box.state() == NetMailbox::State::kDone);
    bool ok = false;
    CHECK(box.response(ok) == "+CSQ: 20,99\n");
    CHECK(ok);
    box.release();
    CHECK(box.state() == NetMailbox::State::kIdle);
    // 失败终结：响应里追加结果文本。
    CHECK(box.submit("AT+CPIN?") == fw::Status::kOk);
    CHECK(box.take(cmd, token, prefix));
    box.onDone(done(token, AtResult::kCmeError, 10));
    CHECK(box.state() == NetMailbox::State::kDone);
    const std::string failed(box.response(ok));
    CHECK(!ok);
    CHECK(failed.find("CME ERROR 10") != std::string::npos);
    box.release();
}

void testMailboxStaleToken()
{
    NetMailbox box;
    std::string_view cmd;
    std::string_view prefix;
    uint32_t first = 0U;
    uint32_t second = 0U;
    CHECK(box.submit("AT+CSQ") == fw::Status::kOk);
    CHECK(box.take(cmd, first, prefix));
    box.release(); // REPL 超时放弃：kRunning 改为 kAbandoned，缓冲不得复用
    CHECK(box.state() == NetMailbox::State::kAbandoned);
    CHECK(box.submit("AT+CREG?") == fw::Status::kBusy);
    box.onDone(done(first, AtResult::kTimeout)); // 旧请求终结 → 回空闲
    CHECK(box.state() == NetMailbox::State::kIdle);
    CHECK(box.submit("AT+CREG?") == fw::Status::kOk);
    CHECK(box.take(cmd, second, prefix));
    CHECK(second != first);
    // 迟到的旧回调（同一个旧 token）既不能结算新请求，也不能污染新响应。
    box.onDone(done(first, AtResult::kOk));
    CHECK(box.state() == NetMailbox::State::kRunning);
    box.onLine("+CREG: 0,1");
    box.onDone(done(second));
    bool ok = false;
    CHECK(box.state() == NetMailbox::State::kDone);
    CHECK(box.response(ok) == "+CREG: 0,1\n");
    box.release();
}

void testMailboxUntakeAndRedaction()
{
    NetMailbox box;
    std::string_view cmd;
    std::string_view prefix;
    uint32_t token = 0U;
    CHECK(box.submit("AT+QIACT?") == fw::Status::kOk);
    CHECK(box.take(cmd, token, prefix));
    box.reject(); // 引擎队列满：退回 kPending
    CHECK(box.state() == NetMailbox::State::kPending);
    uint32_t again = 0U;
    CHECK(box.take(cmd, again, prefix));
    box.onLine("+QICSGP: 1,1,\"apn\",\"user\",\"secret\",1"); // 凭据行必须脱敏
    box.onDone(done(again));
    bool ok = false;
    const std::string rsp(box.response(ok));
    CHECK(rsp.find("secret") == std::string::npos);
    CHECK(rsp.find("user") == std::string::npos);
    box.release();
    // Pending 状态 release 立即回空闲（尚未交给引擎）。
    CHECK(box.submit("AT") == fw::Status::kOk);
    box.release();
    CHECK(box.state() == NetMailbox::State::kIdle);
    // r4 R6：REPL 在命令已取走（Running）时放弃，随后引擎队列满触发 reject()：
    // 状态须收敛到 Idle，不得把已放弃的请求退回 Pending（旧实现无条件 store(kPending)）。
    CHECK(box.submit("AT+CSQ") == fw::Status::kOk);
    CHECK(box.take(cmd, token, prefix));
    box.release();
    box.reject();
    CHECK(box.state() == NetMailbox::State::kIdle);
    CHECK(box.submit("AT") == fw::Status::kOk); // 之后可立即复用
    box.release();
}

/**
 * I7：REPL 的放弃（release）与 kNet 的完成（onDone）竞态——两线程交替 20 万轮，
 * 任何时刻都不得残留 Done/卡死 Busy；旧实现（abandon 标志 + 非原子结算）会在此处残留 Done。
 */
void testMailboxCancelCompleteRace()
{
    NetMailbox box;
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> processed{0U};
    std::thread net([&box, &stop, &processed]() {
        std::string_view cmd;
        std::string_view prefix;
        uint32_t token = 0U;
        for (;;) {
            if (box.take(cmd, token, prefix)) {
                box.onLine("+CSQ: 20,99");
                box.onDone(done(token));
                processed.fetch_add(1U, std::memory_order_relaxed);
            } else if (stop.load(std::memory_order_acquire)) {
                return;
            }
        }
    });
    constexpr uint32_t kRounds = 200000U;
    uint32_t stuck = 0U;
    for (uint32_t i = 0U; i < kRounds; ++i) {
        uint64_t spins = 0U;
        while (box.submit("AT+CSQ") != fw::Status::kOk) {
            if (++spins > 200000000ULL) {
                stuck = i + 1U; // 残留 Done/Abandoned 永不回收：永久 Busy
                break;
            }
        }
        if (stuck != 0U) {
            break;
        }
        // 交替“等到完成”与“立刻放弃”，让放弃落在 Running 的各个时刻。
        if ((i & 1U) == 0U) {
            for (uint32_t w = 0U; w < (i % 97U); ++w) {
                if (box.state() == NetMailbox::State::kDone) {
                    break;
                }
            }
        }
        box.release();
    }
    stop.store(true, std::memory_order_release);
    net.join();
    CHECK(stuck == 0U);
    // 线程结束后邮箱必须可复用（无残留 Done/Abandoned）。
    const NetMailbox::State end = box.state();
    CHECK((end == NetMailbox::State::kIdle) || (end == NetMailbox::State::kPending));
}

void testPowerGate()
{
    app::NetPowerGate gate;
    app::NetGateInput in;
    in.batteryPct = 50U;
    auto d = gate.update(in);
    CHECK(d.powerAllowed && d.startAllowed);
    // 配网：整体断电，不影响低电量迟滞。
    in.wifiConfig = true;
    d = gate.update(in);
    CHECK(!d.powerAllowed && d.startAllowed);
    in.wifiConfig = false;
    // <10%：只禁止新上电，不再要求已在线模块断电（ADR-0021 §2.9）。
    in.batteryPct = 9U;
    d = gate.update(in);
    CHECK(d.powerAllowed);
    CHECK(!d.startAllowed);
    // 迟滞：回升到 10..14% 仍禁止，≥15% 才恢复。
    in.batteryPct = 14U;
    d = gate.update(in);
    CHECK(!d.startAllowed);
    in.batteryPct = 15U;
    d = gate.update(in);
    CHECK(d.startAllowed);
    // 未知电量不改变迟滞状态。
    in.batteryPct = 5U;
    CHECK(!gate.update(in).startAllowed);
    in.batteryPct = app::board::kBatteryPctUnknown;
    CHECK(!gate.update(in).startAllowed);
    // 关机：整体断电。
    in.batteryPct = 80U;
    in.shutdown = true;
    CHECK(!gate.update(in).powerAllowed);
}

} // namespace

int main()
{
    testForbiddenList();
    testReplyPrefix();
    testMailboxCancelCompleteRace();
    testMailboxLifecycle();
    testMailboxStaleToken();
    testMailboxUntakeAndRedaction();
    testPowerGate();
    std::puts("PASS net_logic_check");
    return 0;
}
