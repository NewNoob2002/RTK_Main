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
    unsigned init_calls = 0;
    unsigned start_calls = 0;
    uint32_t poll_now_ms = 0;
    const fw::Command *command = nullptr;
    fw::Result init() noexcept
    {
        ++init_calls;
        return fw::Result::kIoError;
    }
    fw::Result start() noexcept
    {
        ++start_calls;
        return fw::Result::kBusy;
    }
    bool poll(uint32_t now_ms) noexcept
    {
        poll_now_ms = now_ms;
        return now_ms == kPollNowMs;
    }
    void on_command(const fw::Command &cmd, fw::CommandResult &result) noexcept
    {
        command = &cmd;
        result.seq = cmd.seq;
        result.status = fw::Result::kTimeout;
    }
};
struct PollOnlyModule {
    uint32_t now_ms = 0;
    bool poll(uint32_t now) noexcept
    {
        now_ms = now;
        return false;
    }
};
struct EmptyModule {};
constexpr auto kFullOps = fw::make_ops<FullModule>();
constexpr auto kPartialOps = fw::make_ops<PollOnlyModule>();
constexpr auto kEmptyOps = fw::make_ops<EmptyModule>();
consteval bool has_all_callbacks(const fw::ModuleOps &ops) noexcept
{
    return ops.init != nullptr && ops.start != nullptr && ops.poll != nullptr &&
           ops.on_command != nullptr;
}
static_assert(has_all_callbacks(kFullOps));
static_assert(kPartialOps.init == nullptr && kPartialOps.start == nullptr &&
              kPartialOps.on_command == nullptr);
static_assert(kEmptyOps.init == nullptr && kEmptyOps.start == nullptr &&
              kEmptyOps.poll == nullptr && kEmptyOps.on_command == nullptr);

void test_make_ops_full_forwarding()
{
    FullModule module{};
    TEST_ASSERT_TRUE(kFullOps.init(&module) == fw::Result::kIoError);
    TEST_ASSERT_TRUE(kFullOps.start(&module) == fw::Result::kBusy);
    TEST_ASSERT_EQUAL_UINT32(1U, module.init_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, module.start_calls);
    TEST_ASSERT_TRUE(kFullOps.poll(&module, kPollNowMs));
    TEST_ASSERT_EQUAL_UINT32(kPollNowMs, module.poll_now_ms);
    TEST_ASSERT_FALSE(kFullOps.poll(&module, 0));
    TEST_ASSERT_EQUAL_UINT32(0U, module.poll_now_ms);
    fw::Command command{};
    command.seq = kCommandSequence;
    fw::CommandResult result{};
    kFullOps.on_command(&module, command, result);
    TEST_ASSERT_EQUAL_PTR(&command, module.command);
    TEST_ASSERT_EQUAL_UINT32(kCommandSequence, result.seq);
    TEST_ASSERT_TRUE(result.status == fw::Result::kTimeout);
}

void test_make_ops_partial_forwarding()
{
    PollOnlyModule module{};
    TEST_ASSERT_FALSE(kPartialOps.poll(&module, kPollNowMs));
    TEST_ASSERT_EQUAL_UINT32(kPollNowMs, module.now_ms);
    TEST_ASSERT_TRUE(kPartialOps.init == nullptr && kPartialOps.start == nullptr &&
                     kPartialOps.on_command == nullptr);
}

void test_make_ops_empty_table()
{
    TEST_ASSERT_TRUE(kEmptyOps.init == nullptr && kEmptyOps.start == nullptr &&
                     kEmptyOps.poll == nullptr && kEmptyOps.on_command == nullptr);
}

// --------------------------------------------------------------------------
// Result 编译期 + 运行期检查
// --------------------------------------------------------------------------
static_assert(fw::is_ok(fw::Result::kOk));
static_assert(!fw::is_ok(fw::Result::kInvalidArg));
static_assert(!fw::is_ok(fw::Result::kNoSpace));
static_assert(!fw::is_ok(fw::Result::kTimeout));
static_assert(!fw::is_ok(fw::Result::kNotFound));
static_assert(!fw::is_ok(fw::Result::kIoError));
static_assert(!fw::is_ok(fw::Result::kNotSupported));
static_assert(!fw::is_ok(fw::Result::kBusy));
static_assert(!fw::is_ok(fw::Result::kInvalidState));

// --------------------------------------------------------------------------
// StoreKey::valid / unique_keys 编译期 static_assert 用例
// --------------------------------------------------------------------------
constexpr uint16_t kTestBlobSize = 24U;
constexpr uint16_t kTestStrSize = 16U;
constexpr int32_t kSignedMin = -100;
constexpr int32_t kSignedMax = 100;
constexpr int32_t kU16MaxVal = 65535;
constexpr int32_t kBaudMax = 1000000;
constexpr uint8_t kUnknownResultRaw = 250U;
constexpr uint8_t kUnknownStoreTypeRaw = 99U;

consteval fw::StoreKey make_key(std::string_view ns, std::string_view key, fw::StoreType type,
                                uint16_t max_size, uint8_t flags = 0U, int32_t min = 0,
                                int32_t max = 0) noexcept
{
    return fw::StoreKey{
        ns, key, type, max_size, flags, min, max, fw::kAllProducts, "", fw::StoreBinding{}};
}

constexpr fw::StoreKey kValidU8 =
    make_key("mod_a", "flag", fw::StoreType::kU8, 1U, fw::store_flag::kConsole, 0, 1);
constexpr fw::StoreKey kValidU16 =
    make_key("mod_a", "port", fw::StoreType::kU16, 2U, 0U, 1, kU16MaxVal);
constexpr fw::StoreKey kValidU32 =
    make_key("mod_a", "baud", fw::StoreType::kU32, 4U, 0U, 0, kBaudMax);
constexpr fw::StoreKey kValidI32 =
    make_key("mod_a", "offs", fw::StoreType::kI32, 4U, 0U, kSignedMin, kSignedMax);
constexpr fw::StoreKey kValidStr = make_key("mod_b", "sn_key_12345678", fw::StoreType::kStr,
                                            kTestStrSize, fw::store_flag::kReadOnly);
constexpr fw::StoreKey kValidBlob = make_key("123456789012345", "blob", fw::StoreType::kBlob,
                                             kTestBlobSize, fw::store_flag::kExport);

static_assert(fw::valid(kValidU8));
static_assert(fw::valid(kValidU16));
static_assert(fw::valid(kValidU32));
static_assert(fw::valid(kValidI32));
static_assert(fw::valid(kValidStr));
static_assert(fw::valid(kValidBlob));

// 非法用例：空 ns/key、超长 (> 15)、数值 max_size 不匹配、min > max、串/块 max_size == 0、未知类型
constexpr fw::StoreKey kBadEmptyNs = make_key("", "k", fw::StoreType::kU8, 1U);
constexpr fw::StoreKey kBadEmptyKey = make_key("ns", "", fw::StoreType::kU8, 1U);
constexpr fw::StoreKey kBadLongNs = make_key("1234567890123456", "k", fw::StoreType::kU8, 1U);
constexpr fw::StoreKey kBadLongKey = make_key("ns", "1234567890123456", fw::StoreType::kU8, 1U);
constexpr fw::StoreKey kBadU8Size = make_key("ns", "k", fw::StoreType::kU8, 2U, 0U, 0, 1);
constexpr fw::StoreKey kBadU16Size = make_key("ns", "k", fw::StoreType::kU16, 1U, 0U, 0, 1);
constexpr fw::StoreKey kBadU32Size = make_key("ns", "k", fw::StoreType::kU32, 2U, 0U, 0, 1);
constexpr fw::StoreKey kBadI32Size = make_key("ns", "k", fw::StoreType::kI32, 8U, 0U, 0, 1);
constexpr fw::StoreKey kBadRange = make_key("ns", "k", fw::StoreType::kI32, 4U, 0U, 10, -10);
constexpr fw::StoreKey kBadStrZero = make_key("ns", "k", fw::StoreType::kStr, 0U);
constexpr fw::StoreKey kBadBlobZero = make_key("ns", "k", fw::StoreType::kBlob, 0U);
constexpr fw::StoreKey kBadType =
    make_key("ns", "k", static_cast<fw::StoreType>(kUnknownStoreTypeRaw), 4U);

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

// unique_keys 编译期检查
constexpr std::array<fw::StoreKey, 3> kUniqueTable{{
    make_key("ns1", "same_key", fw::StoreType::kU8, 1U),
    make_key("ns2", "same_key", fw::StoreType::kU8, 1U),
    make_key("ns1", "other_key", fw::StoreType::kU16, 2U),
}};
constexpr std::array<fw::StoreKey, 3> kDupTable{{
    make_key("ns1", "k1", fw::StoreType::kU8, 1U),
    make_key("ns2", "k2", fw::StoreType::kU8, 1U),
    make_key("ns1", "k1", fw::StoreType::kU32, 4U),
}};
static_assert(fw::unique_keys(std::span<const fw::StoreKey>{}));
static_assert(fw::unique_keys(kUniqueTable));
static_assert(!fw::unique_keys(kDupTable));

// --------------------------------------------------------------------------
// LockOps 间谍
// --------------------------------------------------------------------------
struct LockSpy {
    uint32_t lock_calls = 0U;
    uint32_t unlock_calls = 0U;
    int32_t depth = 0;
    int32_t max_depth = 0;
    bool underflow = false;
};

void spy_lock(void *ctx) noexcept
{
    auto *spy = static_cast<LockSpy *>(ctx);
    ++spy->lock_calls;
    ++spy->depth;
    if (spy->depth > spy->max_depth) {
        spy->max_depth = spy->depth;
    }
}

void spy_unlock(void *ctx) noexcept
{
    auto *spy = static_cast<LockSpy *>(ctx);
    ++spy->unlock_calls;
    --spy->depth;
    if (spy->depth < 0) {
        spy->underflow = true;
    }
}

fw::LockOps make_spy_lock(LockSpy &spy) noexcept
{
    return fw::LockOps{&spy_lock, &spy_unlock, &spy};
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

void test_result_strings()
{
    TEST_ASSERT_EQUAL_STRING("ok", fw::to_string(fw::Result::kOk));
    TEST_ASSERT_EQUAL_STRING("invalid_arg", fw::to_string(fw::Result::kInvalidArg));
    TEST_ASSERT_EQUAL_STRING("no_space", fw::to_string(fw::Result::kNoSpace));
    TEST_ASSERT_EQUAL_STRING("timeout", fw::to_string(fw::Result::kTimeout));
    TEST_ASSERT_EQUAL_STRING("not_found", fw::to_string(fw::Result::kNotFound));
    TEST_ASSERT_EQUAL_STRING("io_error", fw::to_string(fw::Result::kIoError));
    TEST_ASSERT_EQUAL_STRING("not_supported", fw::to_string(fw::Result::kNotSupported));
    TEST_ASSERT_EQUAL_STRING("busy", fw::to_string(fw::Result::kBusy));
    TEST_ASSERT_EQUAL_STRING("invalid_state", fw::to_string(fw::Result::kInvalidState));
    TEST_ASSERT_EQUAL_STRING("unknown", fw::to_string(static_cast<fw::Result>(kUnknownResultRaw)));
}

void test_lock_guard_pairing_and_nolock()
{
    {
        const fw::LockGuard noop(fw::kNoLock);
    }

    LockSpy spy{};
    const fw::LockOps ops = make_spy_lock(spy);
    {
        const fw::LockGuard outer(ops);
        TEST_ASSERT_EQUAL_UINT32(1U, spy.lock_calls);
        TEST_ASSERT_EQUAL_UINT32(0U, spy.unlock_calls);
        TEST_ASSERT_EQUAL_INT32(1, spy.depth);
    }
    TEST_ASSERT_EQUAL_UINT32(1U, spy.lock_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, spy.unlock_calls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);
    TEST_ASSERT_EQUAL_INT32(1, spy.max_depth);
    TEST_ASSERT_FALSE(spy.underflow);
}

void test_slot_read_write_version_and_lock_pairing()
{
    LockSpy spy{};
    s_static_slot.bind_lock(make_spy_lock(spy));

    // 初始版本号为 0，读一次触发 1 次配对锁
    uint32_t ver = kInitVerSentinel;
    const SamplePayload init_val = s_static_slot.read(ver);
    TEST_ASSERT_EQUAL_UINT32(0U, ver);
    TEST_ASSERT_EQUAL_UINT32(0U, init_val.seq);
    TEST_ASSERT_EQUAL_UINT32(1U, spy.lock_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, spy.unlock_calls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);

    // 写两次后读（无参 read 与带 version read），检查锁次数与版本号
    constexpr uint32_t kSeq1 = 101U;
    constexpr int16_t kTemp1 = -25;
    constexpr uint8_t kFlags1 = 0xA5U;
    const SamplePayload p1{kSeq1, kTemp1, kFlags1, {1U, 2U, 3U, 4U, 5U}};
    s_static_slot.write(p1);
    TEST_ASSERT_EQUAL_UINT32(2U, spy.lock_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, spy.unlock_calls);

    constexpr uint32_t kSeq2 = 202U;
    constexpr int16_t kTemp2 = 37;
    constexpr uint8_t kFlags2 = 0x5AU;
    const SamplePayload p2{kSeq2, kTemp2, kFlags2, {9U, 8U, 7U, 6U, 5U}};
    s_static_slot.write(p2);
    TEST_ASSERT_EQUAL_UINT32(3U, spy.lock_calls);
    TEST_ASSERT_EQUAL_UINT32(3U, spy.unlock_calls);

    const SamplePayload got_plain = s_static_slot.read();
    TEST_ASSERT_EQUAL_UINT32(p2.seq, got_plain.seq);
    TEST_ASSERT_EQUAL_INT16(p2.temp, got_plain.temp);
    TEST_ASSERT_EQUAL_UINT8(p2.flags, got_plain.flags);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(p2.tag.data(), got_plain.tag.data(), p2.tag.size());
    TEST_ASSERT_EQUAL_UINT32(4U, spy.lock_calls);
    TEST_ASSERT_EQUAL_UINT32(4U, spy.unlock_calls);

    const SamplePayload got_ver = s_static_slot.read(ver);
    TEST_ASSERT_EQUAL_UINT32(2U, ver);
    TEST_ASSERT_EQUAL_UINT32(p2.seq, got_ver.seq);
    TEST_ASSERT_EQUAL_UINT32(5U, spy.lock_calls);
    TEST_ASSERT_EQUAL_UINT32(5U, spy.unlock_calls);
    TEST_ASSERT_EQUAL_INT32(0, spy.depth);
    TEST_ASSERT_EQUAL_INT32(1, spy.max_depth);
    TEST_ASSERT_FALSE(spy.underflow);

    // 最大允许尺寸 Slot<MaxSlotPayload>（512 B）读写正确
    fw::Slot<MaxSlotPayload> big_slot;
    big_slot.bind_lock(fw::kNoLock);
    MaxSlotPayload in_big{};
    constexpr uint8_t kBigFill = 0x5CU;
    constexpr uint8_t kBigHead = 0x11U;
    constexpr uint8_t kBigTail = 0x99U;
    in_big.data.fill(kBigFill);
    in_big.data[0] = kBigHead;
    in_big.data[fw::kMaxSlotBytes - 1U] = kBigTail;
    big_slot.write(in_big);
    uint32_t big_ver = 0U;
    const MaxSlotPayload out_big = big_slot.read(big_ver);
    TEST_ASSERT_EQUAL_UINT32(1U, big_ver);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in_big.data.data(), out_big.data.data(), fw::kMaxSlotBytes);
}

void test_store_key_runtime_and_binding()
{
    TEST_ASSERT_TRUE(fw::valid(kValidU8));
    TEST_ASSERT_TRUE(fw::valid(kValidBlob));
    TEST_ASSERT_FALSE(fw::valid(kBadLongKey));
    TEST_ASSERT_FALSE(fw::valid(kBadRange));
    TEST_ASSERT_TRUE(fw::unique_keys(kUniqueTable));
    TEST_ASSERT_FALSE(fw::unique_keys(kDupTable));

    // StoreBinding 绑定示例
    constexpr uint32_t kInitialCounter = 42U;
    constexpr int32_t kCounterMax = 1000;
    uint32_t stored_val = kInitialCounter;
    const fw::StoreBinding binding{
        &stored_val,
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
    const fw::StoreKey key_with_bind{
        "app", "counter",   fw::StoreType::kU32, sizeof(uint32_t), fw::store_flag::kConsole,
        0,     kCounterMax, fw::kAllProducts,    "counter",        binding};
    TEST_ASSERT_TRUE(fw::valid(key_with_bind));

    std::array<uint8_t, sizeof(uint32_t)> buf{};
    size_t len = 0U;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(fw::Result::kOk),
        static_cast<uint8_t>(key_with_bind.binding.save(key_with_bind.binding.obj, buf, len)));
    TEST_ASSERT_EQUAL_size_t(sizeof(uint32_t), len);

    stored_val = 0U;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(fw::Result::kOk),
        static_cast<uint8_t>(key_with_bind.binding.load(key_with_bind.binding.obj, buf)));
    TEST_ASSERT_EQUAL_UINT32(kInitialCounter, stored_val);
}

} // namespace

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_make_ops_full_forwarding);
    RUN_TEST(test_make_ops_partial_forwarding);
    RUN_TEST(test_make_ops_empty_table);
    RUN_TEST(test_result_strings);
    RUN_TEST(test_lock_guard_pairing_and_nolock);
    RUN_TEST(test_slot_read_write_version_and_lock_pairing);
    RUN_TEST(test_store_key_runtime_and_binding);
    return UNITY_END();
}
