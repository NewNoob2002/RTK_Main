// 存储模块控制台命令表（ADR-0009 §2.5）：store show / store sn <value>。
// 线程：仅在 console REPL 任务调用，非 ISR；show 只读 DataCenter，sn 经 Executor::call 投递。

#include "modules/storage/storage_module.hpp"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "esp_err.h"

#include "app_context.hpp"
#include "cli/out.hpp"
#include "support/console_support.hpp"

namespace app {

namespace {

constexpr size_t kSnArgs = 2U;

int cmd_store_show(void * /*ctx*/, int argc, char ** /*argv*/) noexcept
{
    if (argc != 1) {
        return cli::fail("usage: store show");
    }
    const auto &data = app_context().data;
    const auto sys = data.system_status.read();
    const auto store = data.store_status.read();
    const auto link = data.link_status.read();

    cli::print_section("store");
    cli::print_kv("sn", "%s", sys.sn.data());
    cli::print_kv("bt_name", "%s", sys.device_name.data());
    cli::print_kv("sn_err", "%s", esp_err_to_name(store.sn_error));
    // 白名单计数上限为 kMaxAlarms，转 unsigned 无损。
    cli::print_kv("wl_count", "%u", static_cast<unsigned>(link.whitelist_count));
    cli::print_kv("wl_pending", "%s", store.whitelist_pending ? "yes" : "no");
    cli::print_kv("wl_err", "%s", esp_err_to_name(store.whitelist_error));
    cli::print_kv("wl_failures", "%" PRIu32, store.failures);
    cli::print_kv("wl_retry", "%" PRIu32 " ms", store.retry_ms);
    return 0;
}

int cmd_store_sn(void * /*ctx*/, int argc, char **argv) noexcept
{
    if ((argc != static_cast<int>(kSnArgs)) || (argv == nullptr)) {
        return cli::fail("usage: store sn <value>");
    }
    const std::span<char *const> args(argv, kSnArgs);
    const size_t len = std::strlen(args[1]);
    if ((len == 0U) || (len > NVS_STORE_SN_MAX) || (std::strpbrk(args[1], "\r\n") != nullptr)) {
        return cli::fail("bad sn (1-%u bytes)", static_cast<unsigned>(NVS_STORE_SN_MAX));
    }

    fw::Command command{};
    command.target = id(ModuleKind::kStorage);
    command.verb = verb(StorageVerb::kSaveSn);
    // len 已校验在 1..NVS_STORE_SN_MAX(16) 范围，缩窄到 uint8_t 无损。
    command.arg_len = static_cast<uint8_t>(len);
    std::memcpy(command.args.data(), args[1], len);
    if (!submit(command, "store sn")) {
        return cli::kCmdFail;
    }
    cli::out_printf("sn saved; reboot to apply\n");
    return 0;
}

constexpr std::array<cli::Verb, 2U> kStoreVerbs{{
    {"show", nullptr, "show SN, BT name and NVS sync state", cmd_store_show},
    {"sn", "<value>", "save device SN (1-16 chars, reboot to apply)", cmd_store_sn},
}};

int cmd_store(void *ctx, int argc, char **argv) noexcept
{
    return cli::dispatch(kStoreVerbs, ctx, argc, argv);
}

constexpr std::array<cli::Cmd, 1U> kStorageCmds{{
    {"store", "store <show|sn>: NVS persistence inspection and SN update", "<show|sn <value>>", cmd_store, nullptr},
}};

} // namespace

std::span<const cli::Cmd> storage_cmds() noexcept
{
    return kStorageCmds;
}

std::span<const cli::Verb> store_verbs() noexcept
{
    return kStoreVerbs;
}

} // namespace app
