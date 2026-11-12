// 控制台回执与子命令分派实现（ADR-0009 §2.5）：fail()、dispatch()、print_usage()、分节输出。
// 只依赖 C 标准库，不含 IDF 头，便于 host 测试直接编译 src/reply.cpp。
// 线程：只在 console REPL 任务（命令串行执行）或 host 测试主线程调用；非 ISR 安全；不分配堆。

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

#include "cli/console.hpp"
#include "reply_state.hpp"

namespace cli {

namespace {

constexpr int kNameWidth = 10;       // 动词名左对齐宽度
constexpr int kUsageWidth = 14;      // 参数用法列左对齐宽度
constexpr int kKeyWidth = 14;        // print_kv 的键列宽度
constexpr size_t kVerbNameMax = 24U; // 默认动词复制到栈上的名称缓冲（含结尾 NUL）

bool s_failed = false;

bool same_text(const char *lhs, const char *rhs) noexcept
{
    if ((lhs == nullptr) || (rhs == nullptr)) {
        return false;
    }
    return std::string_view{lhs} == std::string_view{rhs};
}

/** 把 esp_console 的 (argc, argv) 包成 span，避免在分派逻辑里做裸指针运算。 */
std::span<char *const> arg_span(int argc, char **argv) noexcept
{
    if ((argv == nullptr) || (argc <= 0)) {
        return {};
    }
    // argc > 0 已校验，转 size_t 无损。
    return {argv, static_cast<size_t>(argc)};
}

/** handler 的签名固定为 char **（冻结接口）；span::data() 给 char *const *，这里只去掉指针本身的 const。 */
char **mutable_args(std::span<char *const> args, size_t offset) noexcept
{
    return const_cast<char **>(args.subspan(offset).data());
}

const Verb *find_verb(std::span<const Verb> verbs, const char *name) noexcept
{
    if (name == nullptr) {
        return nullptr;
    }
    for (const Verb &verb : verbs) {
        if (same_text(verb.name, name)) {
            return &verb;
        }
    }
    return nullptr;
}

} // namespace

namespace detail {

void reply_reset() noexcept
{
    s_failed = false;
}

bool reply_failed() noexcept
{
    return s_failed;
}

} // namespace detail

int fail(const char *fmt, ...) noexcept
{
    // 先置位再打印：handler 若在打印途中被观测，也应视为“已给出原因”。
    s_failed = true;
    std::printf("ERR ");
    if (fmt != nullptr) {
        va_list args;
        va_start(args, fmt);
        std::vprintf(fmt, args);
        va_end(args);
    }
    std::printf("\n");
    return kCmdFail;
}

void print_usage(const char *group, std::span<const Verb> verbs) noexcept
{
    std::printf("usage: %s <verb> [args]\n", (group != nullptr) ? group : "");
    for (const Verb &verb : verbs) {
        const char *name = (verb.name != nullptr) ? verb.name : "?";
        const char *usage = (verb.usage != nullptr) ? verb.usage : "-";
        if (verb.help != nullptr) {
            std::printf("  %-*s %-*s %s\n", kNameWidth, name, kUsageWidth, usage, verb.help);
        } else {
            std::printf("  %-*s %s\n", kNameWidth, name, usage);
        }
    }
}

int dispatch(std::span<const Verb> verbs, void *ctx, int argc, char **argv, const char *default_verb) noexcept
{
    const std::span<char *const> args = arg_span(argc, argv);
    const char *group = (args.size() > 0U) ? args[0] : ""; // 组名只用于诊断输出
    if (args.size() > 1U) {
        const Verb *verb = find_verb(verbs, args[1]);
        if (verb == nullptr) {
            print_usage(group, verbs);
            return fail("usage");
        }
        if (verb->handler == nullptr) {
            // 动词表由实现方维护，缺 handler 属表的缺陷，仍按命令失败处理而不是崩溃。
            return fail("%s: no handler", (verb->name != nullptr) ? verb->name : "?");
        }
        return verb->handler(ctx, argc - 1, mutable_args(args, 1U));
    }
    if (default_verb != nullptr) {
        const Verb *verb = find_verb(verbs, default_verb);
        if ((verb != nullptr) && (verb->handler != nullptr) && (verb->name != nullptr)) {
            // handler 约定 argv[0] 为动词本身；Verb::name 是 const，复制到栈上可写缓冲再传入，
            // 这样不必对冻结签名里的 char ** 做 const_cast。
            const std::string_view name{verb->name};
            if (name.size() >= kVerbNameMax) {
                return fail("verb name too long");
            }
            std::array<char, kVerbNameMax> name_buf{};
            std::copy(name.begin(), name.end(), name_buf.begin());
            std::array<char *, 1> single{name_buf.data()};
            return verb->handler(ctx, 1, single.data());
        }
    }
    print_usage(group, verbs);
    return fail("usage");
}

void print_section(const char *title) noexcept
{
    std::printf("[%s]\n", (title != nullptr) ? title : "");
}

void print_kv(const char *key, const char *fmt, ...) noexcept
{
    std::printf("  %-*s ", kKeyWidth, (key != nullptr) ? key : "");
    if (fmt != nullptr) {
        va_list args;
        va_start(args, fmt);
        std::vprintf(fmt, args);
        va_end(args);
    }
    std::printf("\n");
}

} // namespace cli
