// app_fw 基础组件 host 单元测试：Result、LockGuard、Slot<T>、StoreKey（含 constexpr static_assert）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "fw/Lock.hpp"
#include "fw/Module.hpp"
#include "fw/Result.hpp"
#include "fw/Slot.hpp"
#include "fw/StoreKey.hpp"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr uint32_t kPollNowMs = 1234;
constexpr uint32_t kCommandSequence = 42;
struct FullModule {
    unsigned initCalls = 0;
    unsigned startCalls = 0;
    uint32_t pollNowMs = 0;
    const fw::Command *command = nullptr;
    fw::Result init() noexcept
    {
        ++initCalls;
        return fw::Result::kIoError;
    }
    fw::Result start() noexcept
    {
        ++startCalls;
        return fw::Result::kBusy;
    }
    bool poll(uint32_t nowMs) noexcept
    {
        pollNowMs = nowMs;
        return nowMs == kPollNowMs;
    }
    void onCommand(const fw::Command &cmd, fw::CommandResult &result) noexcept
    {
        command = &cmd;
        result.seq = cmd.seq;
        result.status = fw::Result::kTimeout;
    }
};
struct PollOnlyModule {
    uint32_t nowMs = 0;
    bool poll(uint32_t now) noexcept
    {
        nowMs = now;
        return false;
    }
};
struct EmptyModule {};
constexpr auto kFullOps = fw::makeOps<FullModule>();
constexpr auto kPartialOps = fw::makeOps<PollOnlyModule>();
constexpr auto kEmptyOps = fw::makeOps<EmptyModule>();
consteval bool hasAllCallbacks(const fw::ModuleOps &ops) noexcept
{
    return ops.init != nullptr && ops.start != nullptr && ops.poll != nullptr &&
           ops.onCommand != nullptr;
}
static_assert(hasAllCallbacks(kFullOps));
static_assert(kPartialOps.init == nullptr && kPartialOps.start == nullptr &&
              kPartialOps.onCommand == nullptr);
static_assert(kEmptyOps.init == nullptr && kEmptyOps.start == nullptr &&
              kEmptyOps.poll == nullptr && kEmptyOps.onCommand == nullptr);

void testMakeOpsFullForwarding()
{
    FullModule module{};
    TEST_ASSERT_TRUE(kFullOps.init(&module) == fw::Result::kIoError);
    TEST_ASSERT_TRUE(kFullOps.start(&module) == fw::Result::kBusy);
    TEST_ASSERT_EQUAL_UINT32(1U, module.initCalls);
    TEST_ASSERT_EQUAL_UINT32(1U, module.startCalls);
    TEST_ASSERT_TRUE(kFullOps.poll(&module, kPollNowMs));
    TEST_ASSERT_EQUAL_UINT32(kPollNowMs, module.pollNowMs);
    TEST_ASSERT_FALSE(kFullOps.poll(&module, 0));
    TEST_ASSERT_EQUAL_UINT32(0U, module.pollNowMs);
    fw::Command command{};
    command.seq = kCommandSequence;
    fw::CommandResult result{};
    kFullOps.onCommand(&module, command, result);
    TEST_ASSERT_EQUAL_PTR(&command, module.command);
    TEST_ASSERT_EQUAL_UINT32(kCommandSequence, result.seq);
    TEST_ASSERT_TRUE(result.status == fw::Result::kTimeout);
}

void testMakeOpsPartialForwarding()
{
    PollOnlyModule module{};
    TEST_ASSERT_FALSE(kPartialOps.poll(&module, kPollNowMs));
    TEST_ASSERT_EQUAL_UINT32(kPollNowMs, module.nowMs);
    TEST_ASSERT_TRUE(kPartialOps.init == nullptr && kPartialOps.start == nullptr &&
                     kPartialOps.onCommand == nullptr);
}

void testMakeOpsEmptyTable()
{
    TEST_ASSERT_TRUE(kEmptyOps.init == nullptr && kEmptyOps.start == nullptr &&
                     kEmptyOps.poll == nullptr && kEmptyOps.onCommand == nullptr);
}

// --------------------------------------------------------------------------
// Result 编译期 + 运行期检查
// --------------------------------------------------------------------------
static_assert(fw::isOk(fw::Result::kOk));
static_assert(!fw::isOk(fw::Result::kInvalidArg));
static_assert(!fw::isOk(fw::Result::kNoSpace));
static_assert(!fw::isOk(fw::Result::kTimeout));
static_assert(!fw::isOk(fw::Result::kNotFound));
static_assert(!fw::isOk(fw::Result::kIoError));
static_assert(!fw::isOk(fw::Result::kNotSupported));
static_assert(!fw::isOk(fw::Result::kBusy));
static_assert(!fw::isOk(fw::Result::kInvalidState));

// --------------------------------------------------------------------------
// StoreKey::valid / uniqueKeys 编译期 static_assert 用例
// --------------------------------------------------------------------------
constexpr uint16_t kTestBlobSize = 24U;
constexpr uint16_t kTestStrSize = 16U;
constexpr int32_t kSignedMin = -100;
constexpr int32_t kSignedMax = 100;
constexpr int32_t kU16MaxVal = 65535;
constexpr int32_t kBaudMax = 1000000;
constexpr uint8_t kUnknownResultRaw = 250U;
constexpr uint8_t kUnknownStoreTypeRaw = 99U;

consteval fw::StoreKey makeKey(std::string_view ns, std::string_view key, fw::StoreType type,
                               uint16_t maxSize, uint8_t flags = 0U, int32_t min = 0,
                               int32_t max = 0) noexcept
{
    return fw::StoreKey{
        ns, key, type, maxSize, flags, min, max, fw::kAllProducts, "", fw::StoreBinding{}};
}

constexpr fw::StoreKey kValidU8 =
    makeKey("mod_a", "flag", fw::StoreType::kU8, 1U, fw::store_flag::kConsole, 0, 1);
constexpr fw::StoreKey kValidU16 =
    makeKey("mod_a", "port", fw::StoreType::kU16, 2U, 0U, 1, kU16MaxVal);
constexpr fw::StoreKey kValidU32 =
    makeKey("mod_a", "baud", fw::StoreType::kU32, 4U, 0U, 0, kBaudMax);
constexpr fw::StoreKey kValidI32 =
    makeKey("mod_a", "offs", fw::StoreType::kI32, 4U, 0U, kSignedMin, kSignedMax);
constexpr fw::StoreKey kValidStr = makeKey("mod_b", "sn_key_12345678", fw::StoreType::kStr,
                                           kTestStrSize, fw::store_flag::kReadOnly);
constexpr fw::StoreKey kValidBlob = makeKey("123456789012345", "blob", fw::StoreType::kBlob,
                                            kTestBlobSize, fw::store_flag::kExport);

static_assert(fw::valid(kValidU8));
static_assert(fw::valid(kValidU16));
static_assert(fw::valid(kValidU32));
static_assert(fw::valid(kValidI32));
static_assert(fw::valid(kValidStr));
static_assert(fw::valid(kValidBlob));

// 非法用例：空 ns/key、超长 (> 15)、数值 maxSize 不匹配、min > max、串/块 maxSize == 0、未知类型
constexpr fw::StoreKey kBadEmptyNs = makeKey("", "k", fw::StoreType::kU8, 1U);
constexpr fw::StoreKey kBadEmptyKey = makeKey("ns", "", fw::StoreType::kU8, 1U);
constexpr fw::StoreKey kBadLongNs = makeKey("1234567890123456", "k", fw::StoreType::kU8, 1U);
constexpr fw::StoreKey kBadLongKey = makeKey("ns", "1234567890123456", fw::StoreType::kU8, 1U);
constexpr fw::StoreKey kBadU8Size = makeKey("ns", "k", fw::StoreType::kU8, 2U, 0U, 0, 1);
constexpr fw::StoreKey kBadU16Size = makeKey("ns", "k", fw::StoreType::kU16, 1U, 0U, 0, 1);
constexpr fw::StoreKey kBadU32Size = makeKey("ns", "k", fw::StoreType::kU32, 2U, 0U, 0, 1);
constexpr fw::StoreKey kBadI32Size = makeKey("ns", "k", fw::StoreType::kI32, 8U, 0U, 0, 1);
constexpr fw::StoreKey kBadRange = makeKey("ns", "k", fw::StoreType::kI32, 4U, 0U, 10, -10);
constexpr fw::StoreKey kBadStrZero = makeKey("ns", "k", fw::StoreType::kStr, 0U);
constexpr fw::StoreKey kBadBlobZero = makeKey("ns", "k", fw::StoreType::kBlob, 0U);
constexpr fw::StoreKey kBadType =
    makeKey("ns", "k", static_cast<fw::StoreType>(kUnknownStoreTypeRaw), 4U);

static_assert(!fw::valid(kBadEmptyNs));
static_assert(!fw::valid(kBadEmptyKey));
static_assert(!fw::valid(kBadLongNs));
static_assert(!fw::valid(kBadLongKey));
static_assert(!fw::valid(kBadU8Size));
static_assert(!fw::valid(kBadU16Size));
static_assert(!fw::valid(kBadU32Size));
static_assert(!fw::valid(kBadI32Size));
static_assert(!fw::valid(kBadRange));
static_assert(!fw::valid(kBadStrZero));
static_assert(!fw::valid(kBadBlobZero));
static_assert(!fw::valid(kBadType));

// uniqueKeys 编译期检查
constexpr std::array<fw::StoreKey, 3> kUniqueTable{{
    makeKey("ns1", "same_key", fw::StoreType::kU8, 1U),
    makeKey("ns2", "same_key", fw::StoreType::kU8, 1U),
    makeKey("ns1", "other_key", fw::StoreType::kU16, 2U),
}};
constexpr std::array<fw::StoreKey, 3> kDupTable{{
    makeKey("ns1", "k1", fw::StoreType::kU8, 1U),
    makeKey("ns2", "k2", fw::StoreType::kU8, 1U),
    makeKey("ns1", "k1", fw::StoreType::kU32, 4U),
}};
static_assert(fw::uniqueKeys(std::span<const fw::StoreKey>{}));
static_assert(fw::uniqueKeys(kUniqueTable));
static_assert(!fw::uniqueKeys(kDupTable));

// --------------------------------------------------------------------------
// LockOps 间谍
// --------------------------------------------------------------------------
struct LockSpy {
    uint32_t lockCalls = 0U;
    uint32_t unlockCalls = 0U;
    int32_t depth = 0;
    int32_t maxDepth = 0;
    bool underflow = false;
};

void spyLock(void *ctx) noexcept
{
    auto *spy = static_cast<LockSpy *>(ctx);
    ++spy->lockCalls;
    ++spy->depth;
    if (spy->depth > spy->maxDepth) {
        spy->maxDepth = spy->depth;
    }
}

void spyUnlock(void *ctx) noexcept
{
    auto *spy = static_cast<LockSpy *>(ctx);
    ++spy->unlockCalls;
    --spy->depth;
    if (spy->depth < 0) {
        spy->underflow = true;
    }
}

fw::LockOps makeSpyLock(LockSpy &spy) noexcept
{
    return fw::LockOps{&spyLock, &spyUnlock, &spy};
}

// --------------------------------------------------------------------------
// Slot<T> 测试负载
// --------------------------------------------------------------------------
constexpr size_t kSampleTagLen = 5U;
constexpr uint32_t kInitVerSentinel = 99U;

struct SamplePayload {
    uint32_t seq = 0U;
    int16_t temp = 0;
    uint8_t flags = 0U;
    std::array<uint8_t, kSampleTagLen> tag{};
};

struct MaxSlotPayload {
    std::array<uint8_t, fw::kMaxSlotBytes> data{};
};

constinit fw::Slot<SamplePayload> s_static_slot{};

void testResultStrings()
{
    TEST_ASSERT_EQUAL_STRING("ok", fw::toString(fw::Result::kOk));
    TEST_ASSERT_EQUAL_STRING("invalid_arg", fw::toString(fw::Result::kInvalidArg));
    TEST_ASSERT_EQUAL_STRING("no_space", fw::toString(fw::Result::kNoSpace));
    TEST_ASSERT_EQUAL_STRING("timeout", fw::toString(fw::Result::kTimeout));
    TEST_ASSERT_EQUAL_STRING("not_found", fw::toString(fw::Result::kNotFound));
    TEST_ASSERT_EQUAL_STRING("io_error", fw::toString(fw::Result::kIoError));
    TEST_ASSERT_EQUAL_STRING("not_supported", fw::toString(fw::Result::kNotSupported));
    TEST_ASSERT_EQUAL_STRING("busy", fw::toString(fw::Result::kBusy));
    TEST_ASSERT_EQUAL_STRING("invalid_state", fw::toString(fw::Result::kInvalidState));
    TEST_ASSERT_EQUAL_STRING("unknown", fw::toString(static_cast<fw::Result>(kUnknownResultRaw)));
}

void testLockGuardPairingAndNolock()
{
    {
        const fw::LockGuard noop(fw::kNoLock);
    }

    LockSpy spy{};
    const fw::LockOps ops = makeSpyLock(spy);
    {
        const fw::LockGuard outer(ops);
        TEST_ASSERT_EQUAL_UINT32(1U, spy.lockCalls);
        TEST_ASSERT_EQUAL_UINT32(0U, spy.unlockCalls);
        TEST_ASSERT_EQUAL_INT32(1, spy.depth);
    }
    TEST_ASSERT_EQUAL_UINT32(1U, spy.lockCalls);
    TEST_ASSERT_EQUAL_UINT32(1U, spy.unlockCalls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);
    TEST_ASSERT_EQUAL_INT32(1, spy.maxDepth);
    TEST_ASSERT_FALSE(spy.underflow);
}

void testSlotReadWriteVersionAndLockPairing()
{
    LockSpy spy{};
    s_static_slot.bindLock(makeSpyLock(spy));

    // 初始版本号为 0，读一次触发 1 次配对锁
    uint32_t ver = kInitVerSentinel;
    const SamplePayload initVal = s_static_slot.read(ver);
    TEST_ASSERT_EQUAL_UINT32(0U, ver);
    TEST_ASSERT_EQUAL_UINT32(0U, initVal.seq);
    TEST_ASSERT_EQUAL_UINT32(1U, spy.lockCalls);
    TEST_ASSERT_EQUAL_UINT32(1U, spy.unlockCalls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);

    // 写两次后读（无参 read 与带 version read），检查锁次数与版本号
    constexpr uint32_t kSeq1 = 101U;
    constexpr int16_t kTemp1 = -25;
    constexpr uint8_t kFlags1 = 0xA5U;
    const SamplePayload p1{kSeq1, kTemp1, kFlags1, {1U, 2U, 3U, 4U, 5U}};
    s_static_slot.write(p1);
    TEST_ASSERT_EQUAL_UINT32(2U, spy.lockCalls);
    TEST_ASSERT_EQUAL_UINT32(2U, spy.unlockCalls);

    constexpr uint32_t kSeq2 = 202U;
    constexpr int16_t kTemp2 = 37;
    constexpr uint8_t kFlags2 = 0x5AU;
    const SamplePayload p2{kSeq2, kTemp2, kFlags2, {9U, 8U, 7U, 6U, 5U}};
    s_static_slot.write(p2);
    TEST_ASSERT_EQUAL_UINT32(3U, spy.lockCalls);
    TEST_ASSERT_EQUAL_UINT32(3U, spy.unlockCalls);

    const SamplePayload gotPlain = s_static_slot.read();
    TEST_ASSERT_EQUAL_UINT32(p2.seq, gotPlain.seq);
    TEST_ASSERT_EQUAL_INT16(p2.temp, gotPlain.temp);
    TEST_ASSERT_EQUAL_UINT8(p2.flags, gotPlain.flags);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(p2.tag.data(), gotPlain.tag.data(), p2.tag.size());
    TEST_ASSERT_EQUAL_UINT32(4U, spy.lockCalls);
    TEST_ASSERT_EQUAL_UINT32(4U, spy.unlockCalls);

    const SamplePayload gotVer = s_static_slot.read(ver);
    TEST_ASSERT_EQUAL_UINT32(2U, ver);
    TEST_ASSERT_EQUAL_UINT32(p2.seq, gotVer.seq);
    TEST_ASSERT_EQUAL_UINT32(5U, spy.lockCalls);
    TEST_ASSERT_EQUAL_UINT32(5U, spy.unlockCalls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);
    TEST_ASSERT_EQUAL_INT32(1, spy.maxDepth);
    TEST_ASSERT_FALSE(spy.underflow);

    // 最大允许尺寸 Slot<MaxSlotPayload>（512 B）读写正确
    fw::Slot<MaxSlotPayload> bigSlot;
    bigSlot.bindLock(fw::kNoLock);
    MaxSlotPayload inBig{};
    constexpr uint8_t kBigFill = 0x5CU;
    constexpr uint8_t kBigHead = 0x11U;
    constexpr uint8_t kBigTail = 0x99U;
    inBig.data.fill(kBigFill);
    inBig.data[0] = kBigHead;
    inBig.data[fw::kMaxSlotBytes - 1U] = kBigTail;
    bigSlot.write(inBig);
    uint32_t bigVer = 0U;
    const MaxSlotPayload outBig = bigSlot.read(bigVer);
    TEST_ASSERT_EQUAL_UINT32(1U, bigVer);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(inBig.data.data(), outBig.data.data(), fw::kMaxSlotBytes);
}

void testStoreKeyRuntimeAndBinding()
{
    TEST_ASSERT_TRUE(fw::valid(kValidU8));
    TEST_ASSERT_TRUE(fw::valid(kValidBlob));
    TEST_ASSERT_FALSE(fw::valid(kBadLongKey));
    TEST_ASSERT_FALSE(fw::valid(kBadRange));
    TEST_ASSERT_TRUE(fw::uniqueKeys(kUniqueTable));
    TEST_ASSERT_FALSE(fw::uniqueKeys(kDupTable));

    // StoreBinding 绑定示例
    constexpr uint32_t kInitialCounter = 42U;
    constexpr int32_t kCounterMax = 1000;
    uint32_t storedVal = kInitialCounter;
    const fw::StoreBinding binding{
        &storedVal,
        [](void *obj, std::span<const uint8_t> in) noexcept -> fw::Result {
            if (in.size() != sizeof(uint32_t)) {
                return fw::Result::kInvalidArg;
            }
            uint32_t v = 0U;
            std::memcpy(&v, in.data(), sizeof(v));
            *static_cast<uint32_t *>(obj) = v;
            return fw::Result::kOk;
        },
        [](const void *obj, std::span<uint8_t> out, size_t &len) noexcept -> fw::Result {
            if (out.size() < sizeof(uint32_t)) {
                return fw::Result::kNoSpace;
            }
            const uint32_t v = *static_cast<const uint32_t *>(obj);
            std::memcpy(out.data(), &v, sizeof(v));
            len = sizeof(v);
            return fw::Result::kOk;
        },
    };
    const fw::StoreKey keyWithBind{
        "app", "counter",   fw::StoreType::kU32, sizeof(uint32_t), fw::store_flag::kConsole,
        0,     kCounterMax, fw::kAllProducts,    "counter",        binding};
    TEST_ASSERT_TRUE(fw::valid(keyWithBind));

    std::array<uint8_t, sizeof(uint32_t)> buf{};
    size_t len = 0U;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(fw::Result::kOk),
        static_cast<uint8_t>(keyWithBind.binding.save(keyWithBind.binding.obj, buf, len)));
    TEST_ASSERT_EQUAL_size_t(sizeof(uint32_t), len);

    storedVal = 0U;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(fw::Result::kOk),
        static_cast<uint8_t>(keyWithBind.binding.load(keyWithBind.binding.obj, buf)));
    TEST_ASSERT_EQUAL_UINT32(kInitialCounter, storedVal);
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(testMakeOpsFullForwarding);
    RUN_TEST(testMakeOpsPartialForwarding);
    RUN_TEST(testMakeOpsEmptyTable);
    RUN_TEST(testResultStrings);
    RUN_TEST(testLockGuardPairingAndNolock);
    RUN_TEST(testSlotReadWriteVersionAndLockPairing);
    RUN_TEST(testStoreKeyRuntimeAndBinding);
    return UNITY_END();
}
