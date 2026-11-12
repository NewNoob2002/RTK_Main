// 存储模块控制台命令表：sn。
//
// Generated-by: pi + antigravity/gemini-3.8-flash
// 人工负责人：UNKNOWN（待人工审查签字）

#include "support/console_support.hpp"
#include "modules/storage/storage_module.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

#include "rtk_store.h"

namespace rtk::app {

namespace {

// NOLINTNEXTLINE(misc-const-correctness)
int cmd_sn(void * /*ctx*/, int argc, char **argv) noexcept
{
    const std::span<char *> args(argv, static_cast<size_t>(argc));

    if (args.size() != 2U) {
        printf("usage: sn <string>\n");
        return 1;
    }
    const size_t len = strlen(args[1]);
    if ((len == 0U) || (len > RTK_STORE_SN_MAX) || (strpbrk(args[1], "\r\n") != nullptr)) {
        printf("invalid sn (1..%u bytes, no newline)\n", static_cast<unsigned>(RTK_STORE_SN_MAX));
        return 1;
    }

    fw::Command command{};
    command.target = id(ModuleKind::kStorage);
    command.verb = verb(StorageVerb::kSaveSn);
    // len 已验证为 1..16，收窄到 uint8_t 无损。
    command.arg_len = static_cast<uint8_t>(len);
    std::memcpy(command.args.data(), args[1], len);
    if (!submit(command, "sn")) {
        return 1;
    }
    printf("sn saved; reboot to apply\n");
    return 0;
}

constexpr std::array<rtk::console::Cmd, 1> kStorageCmds{{
    {"sn", "sn <string>: save SN (reboot to apply)", nullptr, cmd_sn, nullptr},
}};

} // namespace

std::span<const rtk::console::Cmd> storage_cmds() noexcept
{
    return kStorageCmds;
}

} // namespace rtk::app
