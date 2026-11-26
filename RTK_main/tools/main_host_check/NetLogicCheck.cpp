// 4G 联网纯逻辑回归（N1 复审 #7/#8/#9/#13）：控制台 AT 邮箱的拒绝表/归属/过期结算与低电量门控迟滞。
// 主机可测：NetMailbox/NetPowerGate 无 IDF 依赖。
#include <cstdio>
#include <cstdlib>
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
    // 允许：普通查询/设置。
    CHECK(!NetMailbox::forbidden("AT"));
    CHECK(!NetMailbox::forbidden("AT+CSQ"));
    CHECK(!NetMailbox::forbidden("at+cfun?"));
    CHECK(!NetMailbox::forbidden("AT+CFUN=1"));
    CHECK(!NetMailbox::forbidden("AT+CFUN=0"));
    CHECK(!NetMailbox::forbidden("AT+CFUN=4"));
    CHECK(!NetMailbox::forbidden("AT+QIOPEN=1,0,\"TCP\",\"1.2.3.4\",80,0,0"));
    // 拒绝：非 AT、回显、波特率、模式切换、APN 配置（含凭据）。
    CHECK(NetMailbox::forbidden(""));
    CHECK(NetMailbox::forbidden("A"));
    CHECK(NetMailbox::forbidden("HELLO"));
    CHECK(NetMailbox::forbidden("ATE0"));
    CHECK(NetMailbox::forbidden("ate1"));
    CHECK(NetMailbox::forbidden("AT+IPR=9600"));
    CHECK(NetMailbox::forbidden("AT+IPR?"));
    CHECK(NetMailbox::forbidden("AT+QISWTMD=1,1,2"));
    CHECK(NetMailbox::forbidden("AT+QICSGP=1"));
    CHECK(NetMailbox::forbidden("AT+QICSGP=1,1,\"apn\",\"user\",\"pass\",1"));
    // 拒绝：命令串联与控制字符。
    CHECK(NetMailbox::forbidden("AT+CSQ;+IPR=9600"));
    CHECK(NetMailbox::forbidden("AT+CSQ\r"));
    CHECK(NetMailbox::forbidden("AT+CSQ\n+IPR=1"));
    // 拒绝：CFUN 其它模式（0/1/4 之外），含无参数形式。
    CHECK(NetMailbox::forbidden("AT+CFUN=2"));
    CHECK(NetMailbox::forbidden("AT+CFUN="));
    // 拒绝：进入数据提示符的命令（裸发数据不经 payload 通道）。
    CHECK(NetMailbox::forbidden("AT+QISEND=0,10"));
    CHECK(NetMailbox::forbidden("AT+QISENDEX=0,\"AB\""));
    CHECK(NetMailbox::forbidden("AT+CMGS=\"123\""));
    // 拒绝：QIOPEN 透传/直吐（access_mode≠0）。
    CHECK(NetMailbox::forbidden("AT+QIOPEN=1,0,\"TCP\",\"1.2.3.4\",80,0,1"));
    CHECK(NetMailbox::forbidden("AT+QIOPEN=1,0,\"TCP\",\"1.2.3.4\",80,0,2"));
}

void testMailboxLifecycle()
{
    NetMailbox box;
    CHECK(box.state() == NetMailbox::State::kIdle);
    CHECK(box.submit("AT+CSQ") == fw::Status::kOk);
    CHECK(box.state() == NetMailbox::State::kPending);
    CHECK(box.submit("AT") == fw::Status::kBusy); // 未释放不得覆盖
    std::string_view cmd;
    uint32_t token = 0U;
    CHECK(box.take(cmd, token));
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
    CHECK(box.take(cmd, token));
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
    uint32_t first = 0U;
    uint32_t second = 0U;
    CHECK(box.submit("AT+CSQ") == fw::Status::kOk);
    CHECK(box.take(cmd, first));
    box.release(); // REPL 超时放弃：kRunning 只标记放弃，缓冲不得复用
    CHECK(box.state() == NetMailbox::State::kRunning);
    CHECK(box.submit("AT+CREG?") == fw::Status::kBusy);
    box.onDone(done(first, AtResult::kTimeout)); // 旧请求终结 → 回空闲
    CHECK(box.state() == NetMailbox::State::kIdle);
    CHECK(box.submit("AT+CREG?") == fw::Status::kOk);
    CHECK(box.take(cmd, second));
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
    uint32_t token = 0U;
    CHECK(box.submit("AT+QIACT?") == fw::Status::kOk);
    CHECK(box.take(cmd, token));
    box.untake(); // 引擎队列满：退回 kPending
    CHECK(box.state() == NetMailbox::State::kPending);
    uint32_t again = 0U;
    CHECK(box.take(cmd, again));
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
    testMailboxLifecycle();
    testMailboxStaleToken();
    testMailboxUntakeAndRedaction();
    testPowerGate();
    std::puts("PASS net_logic_check");
    return 0;
}
