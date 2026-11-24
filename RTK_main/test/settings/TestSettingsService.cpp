// settings::Service host 单元测试（ADR-0018 §2.3/§2.4/§6 Q2）。
// 覆盖查键、权限（含 MQTT 全拒绝与工厂解锁）、动态能力域、编码、异步 post/poll/cancel、get 与 forEach。
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "settings/Service.hpp"
#include "unity.h"

namespace {

/** 主机桩后端状态：记录最后一次 post 的命令与模拟响应。 */
struct MockBackendState final {
    settings::GnssCaps caps{};
    bool gnssEnabled = true;
    fw::Result nextPostResult = fw::Result::kOk;
    settings::PollState nextPollState = settings::PollState::kDone;
    fw::CommandResult nextPollResult{};

    int postCalls = 0;
    int cancelCalls = 0;
    settings::Owner lastOwner = settings::Owner::kNone;
    settings::Caller lastCaller = settings::Caller::kConsole;
    fw::Command lastCmd{};
    uint32_t nextSeq = 1U;
    settings::Ticket lastCancelled{};

    int32_t curRateHz = 10;
    bool curPairNext = true;
    std::string_view curSn = "P2A0001";
};

/**
 * @brief  构造 CM680 10 Hz 测试能力快照
 * @retval 填充好的 GnssCaps
 */
settings::GnssCaps makeCm680Caps() noexcept
{
    settings::GnssCaps caps{};
    caps.valid = true;
    caps.navRateHz = 10U;
    caps.navRatesHz = {1, 2, 5, 10};
    caps.navRateCount = 4U;
    const settings::PeriodDomain std10{{0, 100, 200, 500, 1000}, 5U, true};
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGga)] = std10;
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGsa)] = {{0, 1000}, 2U, true};
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGsv)] = {{0, 1000, 5000}, 3U, true};
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGst)] = std10;
    caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kRmc)] = std10;
    caps.constMask = 0U;
    return caps;
}

/**
 * @brief  把 MockBackendState 绑定到 settings::Service
 * @param  st:  桩状态
 * @param  svc: 待绑定服务
 */
void bindMock(MockBackendState &st, settings::Service &svc) noexcept
{
    settings::Backend b{};
    b.ctx = &st;
    b.post = [](void *ctx, settings::Owner owner, const fw::Command &cmd, settings::Caller caller,
                settings::Ticket &out) noexcept -> fw::Result {
        auto &s = *static_cast<MockBackendState *>(ctx);
        ++s.postCalls;
        s.lastOwner = owner;
        s.lastCaller = caller;
        s.lastCmd = cmd;
        if (!fw::isOk(s.nextPostResult)) {
            out = settings::Ticket{};
            return s.nextPostResult;
        }
        out = settings::Ticket{.seq = s.nextSeq++, .slot = 0U, .owner = owner};
        return fw::Result::kOk;
    };
    b.poll = [](void *ctx, const settings::Ticket &t,
                fw::CommandResult &out) noexcept -> settings::PollState {
        const auto &s = *static_cast<const MockBackendState *>(ctx);
        if (s.nextPollState == settings::PollState::kDone) {
            out = s.nextPollResult;
            out.seq = t.seq;
        }
        return s.nextPollState;
    };
    b.cancel = [](void *ctx, const settings::Ticket &t) noexcept {
        auto &s = *static_cast<MockBackendState *>(ctx);
        ++s.cancelCalls;
        s.lastCancelled = t;
    };
    b.get = [](void *ctx, settings::KeyId id, settings::Value &out) noexcept -> bool {
        const auto &s = *static_cast<const MockBackendState *>(ctx);
        switch (id) {
        case settings::KeyId::kGnssRate:
            out = settings::Value{.kind = settings::ValueKind::kInt, .number = s.curRateHz};
            return true;
        case settings::KeyId::kBtPairNext:
            out = settings::Value{.kind = settings::ValueKind::kBool, .boolVal = s.curPairNext};
            return true;
        case settings::KeyId::kFactorySn:
            out = settings::Value{.kind = settings::ValueKind::kString};
            std::memcpy(out.text.data(), s.curSn.data(), s.curSn.size());
            out.textLen = static_cast<uint8_t>(s.curSn.size());
            return true;
        default:
            return false;
        }
    };
    b.gnssCaps = [](void *ctx) noexcept -> settings::GnssCaps {
        return static_cast<const MockBackendState *>(ctx)->caps;
    };
    b.gnssEnabled = [](void *ctx) noexcept -> bool {
        return static_cast<const MockBackendState *>(ctx)->gnssEnabled;
    };
    svc.bindBackend(b);
}

/**
 * @brief  校验权限规则：MQTT 对全部现有键返回 kLocked，factory.sn 未解锁返回 kLocked
 */
void testPermissionsAndFactoryLock()
{
    MockBackendState st{};
    st.caps = makeCm680Caps();
    settings::Service svc{};
    bindMock(st, svc);

    settings::Ticket t{};
    // ADR-0018 §6 Q2：MQTT 对 GNSS/门控/蓝牙/工厂全部拒绝（kLocked）。
    for (const auto &spec : settings::keyTable()) {
        TEST_ASSERT_EQUAL(
            static_cast<int>(settings::Error::kLocked),
            static_cast<int>(svc.set(spec.name, "1", settings::Caller::kMqtt, t, true)));
        TEST_ASSERT_EQUAL_UINT32(0U, t.seq);
    }
    TEST_ASSERT_EQUAL_INT(0, st.postCalls);

    // factory.sn 未解锁拒绝，解锁后成功 post。
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kLocked),
        static_cast<int>(svc.set("factory.sn", "P2B0001", settings::Caller::kWeb, t, false)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(svc.set("factory.sn", "P2B0001", settings::Caller::kWeb, t, true)));
    TEST_ASSERT_EQUAL_UINT32(1U, t.seq);
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Owner::kStorage), static_cast<int>(t.owner));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbStorageSaveSn, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(7U, st.lastCmd.argLen);
    TEST_ASSERT_EQUAL_MEMORY("P2B0001", st.lastCmd.args.data(), 7U);
}

/**
 * @brief  校验未知键、域外值 bad_value、未支持键 unsupported、队列满 busy 与编码正确性
 */
void testValidationEncodingAndErrors()
{
    MockBackendState st{};
    st.caps = makeCm680Caps();
    settings::Service svc{};
    bindMock(st, svc);

    settings::Ticket t{};
    // 未知键
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kUnknownKey),
                      static_cast<int>(svc.set("no.such.key", "1", settings::Caller::kWeb, t)));
    // 星座 v1 未支持
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kUnsupported),
                      static_cast<int>(svc.set("gnss.const.gps", "1", settings::Caller::kWeb, t)));
    // CM680 GSA 域外值 -> bad_value
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(svc.set("gnss.out.gsa", "100", settings::Caller::kWeb, t)));
    // CM680 GSV 5000 ms (0.2 Hz) -> 合法并编码为 -5
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.out.gsv", "5000", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Owner::kGnss), static_cast<int>(st.lastOwner));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbGnssSetOut, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(2U, st.lastCmd.argLen);
    TEST_ASSERT_EQUAL_UINT8(1U, st.lastCmd.args[0]);
    TEST_ASSERT_EQUAL_INT8(-5, static_cast<int8_t>(st.lastCmd.args[1])); // index 1 (kSatsInfo)

    // 后端返回 kBusy -> Error::kBusy 且票据保持无效
    st.nextPostResult = fw::Result::kBusy;
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBusy),
                      static_cast<int>(svc.set("gnss.rate", "5", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL_UINT32(0U, t.seq);

    // poll / cancel 透传
    st.nextPostResult = fw::Result::kOk;
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.rate", "10", settings::Caller::kWeb, t)));
    fw::CommandResult res{};
    st.nextPollState = settings::PollState::kDone;
    st.nextPollResult = {.seq = 0U, .status = fw::Result::kOk, .detail = 0};
    TEST_ASSERT_EQUAL(static_cast<int>(settings::PollState::kDone),
                      static_cast<int>(svc.poll(t, res)));
    TEST_ASSERT_EQUAL_UINT32(t.seq, res.seq);

    svc.cancel(t);
    TEST_ASSERT_EQUAL_INT(1, st.cancelCalls);
    TEST_ASSERT_EQUAL_UINT32(t.seq, st.lastCancelled.seq);
}

/**
 * @brief  校验 get()、domain()、forEach() 与未探测/禁用 GNSS 时隐藏对应键
 */
void testGetDomainAndForEach()
{
    MockBackendState st{};
    st.caps = makeCm680Caps();
    settings::Service svc{};
    bindMock(st, svc);

    settings::Value v{};
    TEST_ASSERT_TRUE(svc.get("gnss.rate", v));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::ValueKind::kInt), static_cast<int>(v.kind));
    TEST_ASSERT_EQUAL_INT32(10, v.number);
    TEST_ASSERT_TRUE(svc.get("bt.pair_next", v));
    TEST_ASSERT_TRUE(v.boolVal);
    TEST_ASSERT_TRUE(svc.get("factory.sn", v));
    TEST_ASSERT_TRUE(v.textView() == "P2A0001");
    // 只写键不返回快照
    TEST_ASSERT_FALSE(svc.get("bt.wl.add", v));
    TEST_ASSERT_FALSE(svc.get("bt.wl.del", v));

    settings::Domain d{};
    TEST_ASSERT_TRUE(svc.domain("gnss.out.gsv", d));
    TEST_ASSERT_TRUE(d.supported);
    TEST_ASSERT_EQUAL_size_t(3U, d.values().size());
    TEST_ASSERT_EQUAL_INT32(5000, d.values()[2]);

    // 未识别 GNSS 型号时，gnss.rate 与 gnss.out.* 的 domain.supported 必须为 false。
    st.caps = settings::GnssCaps{};
    TEST_ASSERT_TRUE(svc.domain("gnss.rate", d));
    TEST_ASSERT_FALSE(d.supported);
    TEST_ASSERT_TRUE(svc.domain("gnss.gga_gate.min_sats", d));
    TEST_ASSERT_TRUE(d.supported);
    TEST_ASSERT_EQUAL_INT32(0, d.minValue);
    TEST_ASSERT_EQUAL_INT32(40, d.maxValue);

    // 禁用 GNSS 时，GGA 门控与预探测静态回退均变为不支持。
    st.gnssEnabled = false;
    TEST_ASSERT_TRUE(svc.domain("gnss.gga_gate.enabled", d));
    TEST_ASSERT_FALSE(d.supported);
    settings::Ticket t{};
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kUnsupported),
                      static_cast<int>(svc.set("gnss.rate", "5", settings::Caller::kConsole, t)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kUnsupported),
        static_cast<int>(svc.set("gnss.out.gga", "1000", settings::Caller::kConsole, t)));

    // 探测未完成（gnssEnabled && !caps.valid）时：domain 仍为 false（隐藏网页 caps），
    // 但 set() 对 gnss.rate / gnss.out.* 按静态范围校验放行（P2-1/P2-2），对 gnss.ppp 仍报 unsupported。
    st.gnssEnabled = true;
    st.caps = settings::GnssCaps{};
    TEST_ASSERT_TRUE(svc.domain("gnss.rate", d));
    TEST_ASSERT_FALSE(d.supported);
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.rate", "20", settings::Caller::kConsole, t)));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbGnssSetRate, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(20U, st.lastCmd.args[0]);
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(svc.set("gnss.rate", "3", settings::Caller::kConsole, t)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(svc.set("gnss.out.gga", "5000", settings::Caller::kConsole, t)));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbGnssSetOut, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_INT8(-5, static_cast<int8_t>(st.lastCmd.args[1]));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kBadValue),
        static_cast<int>(svc.set("gnss.out.gga", "150", settings::Caller::kConsole, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kUnsupported),
                      static_cast<int>(svc.set("gnss.ppp", "1", settings::Caller::kPhone, t)));

    size_t count = 0U;
    svc.forEach([&count](const settings::KeySpec &) { ++count; });
    TEST_ASSERT_EQUAL_size_t(21U, count);
}

/**
 * @brief  校验 SV3 手机来源（Caller::kPhone）权限与网页隔离（phone.* 不允许 kWeb）
 */
void testPhoneCallerPermissionsAndEncoding()
{
    MockBackendState st{};
    st.caps = makeCm680Caps();
    st.caps.pppSupported = true;
    st.caps.constMask = settings::const_bit::kGps | settings::const_bit::kBds |
                        settings::const_bit::kGlo | settings::const_bit::kGal |
                        settings::const_bit::kSbas;
    settings::Service svc{};
    bindMock(st, svc);

    settings::Ticket t{};
    // 网页不能直接写 phone.height / phone.sn_pn / gnss.ppp / gnss.sat_track。
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kLocked),
        static_cast<int>(svc.set("phone.height", "000000000000F83F", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kLocked),
                      static_cast<int>(svc.set("gnss.ppp", "1", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kLocked),
        static_cast<int>(svc.set("gnss.sat_track", "10,1,1,1,1,1", settings::Caller::kWeb, t)));

    // 手机来源允许写 gnss.ppp / gnss.sat_track / phone.height / phone.sn_pn。
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.ppp", "1", settings::Caller::kPhone, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Owner::kGnss), static_cast<int>(st.lastOwner));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbGnssSetPpp, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(1U, st.lastCmd.argLen);
    TEST_ASSERT_EQUAL_UINT8(1U, st.lastCmd.args[0]);

    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(svc.set("gnss.sat_track", "12,1,0,1,1,0", settings::Caller::kPhone, t)));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbGnssSetSatTrack, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(6U, st.lastCmd.argLen);
    TEST_ASSERT_EQUAL_UINT8(12U, st.lastCmd.args[0]);

    // 网页可写单星座键 gnss.const.*（编码为 2 字节 {constBit, 0/1} 复用 kVerbGnssSetSatTrack）。
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.const.glo", "0", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Owner::kGnss), static_cast<int>(st.lastOwner));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbGnssSetSatTrack, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(2U, st.lastCmd.argLen);
    TEST_ASSERT_EQUAL_UINT8(settings::const_bit::kGlo, st.lastCmd.args[0]);
    TEST_ASSERT_EQUAL_UINT8(0U, st.lastCmd.args[1]);

    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.const.gps", "1", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL_UINT8(settings::const_bit::kGps, st.lastCmd.args[0]);
    TEST_ASSERT_EQUAL_UINT8(1U, st.lastCmd.args[1]);

    // RU1-08：网页可写 gnss.cutoff（0..90），编码为 1 字节角度并复用 kVerbGnssSetSatTrack。
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.cutoff", "20", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Owner::kGnss), static_cast<int>(st.lastOwner));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbGnssSetSatTrack, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(1U, st.lastCmd.argLen);
    TEST_ASSERT_EQUAL_UINT8(20U, st.lastCmd.args[0]);
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(svc.set("gnss.cutoff", "91", settings::Caller::kWeb, t)));

    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(svc.set("phone.height", "000000000000F83F", settings::Caller::kPhone, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Owner::kStorage), static_cast<int>(st.lastOwner));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbStorageSetHeight, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(8U, st.lastCmd.argLen);

    const char *snPnHex = "534E3031000000000000000000000000"
                          "504E3031000000000000000000000000";
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(svc.set("phone.sn_pn", snPnHex, settings::Caller::kPhone, t)));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbStorageSetSnPn, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(32U, st.lastCmd.argLen);
}

/**
 * @brief  校验 ZED-F9P 5 Hz 能力域：拒绝 10 Hz、拒绝 500 ms 非整倍周期，放行 0/200/1000 与 GSV=0
 */
void testF9pFiveHzDomainValidation()
{
    MockBackendState st{};
    st.caps.valid = true;
    st.caps.navRateHz = 5U;
    st.caps.navRatesHz = {1U, 2U, 5U};
    st.caps.navRateCount = 3U;
    const settings::PeriodDomain f9p5Hz{{0U, 200U, 1000U}, 3U, true};
    st.caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGga)] = f9p5Hz;
    st.caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGsa)] = {{0U, 1000U}, 2U, true};
    st.caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGsv)] = {
        {0U, 1000U, 5000U}, 3U, true};
    st.caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kGst)] = f9p5Hz;
    st.caps.outPeriods[static_cast<size_t>(settings::OutKeyIndex::kRmc)] = f9p5Hz;
    settings::Service svc{};
    bindMock(st, svc);

    settings::Ticket t{};
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(svc.set("gnss.rate", "10", settings::Caller::kConsole, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kBadValue),
                      static_cast<int>(svc.set("gnss.out.gga", "500", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.out.gga", "200", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL(static_cast<int>(settings::Error::kOk),
                      static_cast<int>(svc.set("gnss.out.gga", "1000", settings::Caller::kWeb, t)));
    TEST_ASSERT_EQUAL(
        static_cast<int>(settings::Error::kOk),
        static_cast<int>(svc.set("gnss.out.gsv", "0", settings::Caller::kConsole, t)));
    TEST_ASSERT_EQUAL_UINT8(settings::kVerbGnssSetOut, st.lastCmd.verb);
    TEST_ASSERT_EQUAL_UINT8(2U, st.lastCmd.argLen);
    TEST_ASSERT_EQUAL_UINT8(0U, st.lastCmd.args[1]);
}

} // namespace

/** @brief Unity 每用例前置钩子。 */
void setUp() {}

/** @brief Unity 每用例后置钩子。 */
void tearDown() {}

/**
 * @brief  运行全部 settings::Service 单元测试
 * @retval 0 全部通过
 */
int main()
{
    UNITY_BEGIN();
    RUN_TEST(testPermissionsAndFactoryLock);
    RUN_TEST(testValidationEncodingAndErrors);
    RUN_TEST(testGetDomainAndForEach);
    RUN_TEST(testPhoneCallerPermissionsAndEncoding);
    RUN_TEST(testF9pFiveHzDomainValidation);
    return UNITY_END();
}
