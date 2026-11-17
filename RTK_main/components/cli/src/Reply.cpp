// 控制台回执与子命令分派实现（ADR-0009 §2.5）：fail()、dispatch()、printUsage()、分节输出。
// 只依赖 C 标准库，不含 IDF 头，便于 host 测试直接编译 src/Reply.cpp；输出全部经 cli/Out.hpp 整行写出。
// 线程：只在 console REPL 任务（命令串行执行）或 host 测试主线程调用；非 ISR 安全；不分配堆。

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

#include "cli/Console.hpp"
#include "cli/Out.hpp"
#include "ReplyState.hpp"

namespace cli {

namespace {

constexpr int kNameWidth = 10;       // 动词名列最小宽度（表内更长时按最长项加宽）
constexpr int kUsageWidth = 14;      // 参数用法列最小宽度（同上）
constexpr size_t kColumnMax = 32U;   // 列宽上限，防止异常长的用法撑坏排版
constexpr int kKeyWidth = 14;        // printKv 的键列宽度
constexpr size_t kVerbNameMax = 24U; // 默认动词复制到栈上的名称缓冲（含结尾 NUL）
constexpr size_t kValueMax = 160U;   // fail 原因 / printKv 值先格式化到栈上，再与前缀拼成一行输出

bool s_failed = false;

bool sameText(const char *lhs, const char *rhs) noexcept
{
    if ((lhs == nullptr) || (rhs == nullptr)) {
        return false;
    }
    return std::string_view{lhs} == std::string_view{rhs};
}

/** 把 esp_console 的 (argc, argv) 包成 span，避免在分派逻辑里做裸指针运算。 */
std::span<char *const> argSpan(int argc, char **argv) noexcept
{
    if ((argv == nullptr) || (argc <= 0)) {
        return {};
    }
    // argc > 0 已校验，转 size_t 无损。
    return {argv, static_cast<size_t>(argc)};
}

/** handler 的签名固定为 char **（冻结接口）；span::data() 给 char *const *，这里只去掉指针本身的 const。 */
char **mutableArgs(std::span<char *const> args, size_t offset) noexcept
{
    return const_cast<char **>(args.subspan(offset).data());
}

const Verb *findVerb(std::span<const Verb> verbs, const char *name) noexcept
{
    if (name == nullptr) {
        return nullptr;
    }
    for (const Verb &verb : verbs) {
        if (sameText(verb.name, name)) {
            return &verb;
        }
    }
    return nullptr;
}

} // namespace

namespace detail {

void replyReset() noexcept
{
    s_failed = false;
}

bool replyFailed() noexcept
{
    return s_failed;
}

} // namespace detail

int fail(const char *fmt, ...) noexcept
{
    // 先置位再打印：handler 若在打印途中被观测，也应视为“已给出原因”。
    s_failed = true;
    std::array<char, kValueMax> reason{};
    if (fmt != nullptr) {
        va_list args;
        va_start(args, fmt);
        (void)std::vsnprintf(reason.data(), reason.size(), fmt, args); // 超长截断：原因只是一行诊断
        va_end(args);
    }
    outPrintf("ERR %s\n", reason.data());
    return kCmdFail;
}

void printUsage(const char *group, std::span<const Verb> verbs) noexcept
{
    // 按本表最长项定列宽，保证说明列对齐（例如 factory 的 "alarm-set <state> <level>"）。
    size_t nameWidth = static_cast<size_t>(kNameWidth);
    size_t usageWidth = static_cast<size_t>(kUsageWidth);
    for (const Verb &verb : verbs) {
        if (verb.name != nullptr) {
            nameWidth = std::max(nameWidth, std::string_view{verb.name}.size());
        }
        if (verb.usage != nullptr) {
            usageWidth = std::max(usageWidth, std::string_view{verb.usage}.size());
        }
    }
    // 上限 kColumnMax(32)，转换为 int 无损。
    const int nameCol = static_cast<int>(std::min(nameWidth, kColumnMax));
    const int usageCol = static_cast<int>(std::min(usageWidth, kColumnMax));
    outPrintf("usage: %s <verb> [args]\n", (group != nullptr) ? group : "");
    for (const Verb &verb : verbs) {
        const char *name = (verb.name != nullptr) ? verb.name : "?";
        const char *usage = (verb.usage != nullptr) ? verb.usage : "-";
        if (verb.help != nullptr) {
            outPrintf("  %-*s %-*s %s\n", nameCol, name, usageCol, usage, verb.help);
        } else {
            outPrintf("  %-*s %s\n", nameCol, name, usage);
        }
    }
}

int dispatch(std::span<const Verb> verbs, void *ctx, int argc, char **argv,
             const char *defaultVerb) noexcept
{
    const std::span<char *const> args = argSpan(argc, argv);
    const char *group = (args.size() > 0U) ? args[0] : ""; // 组名只用于诊断输出
    if (args.size() > 1U) {
        const Verb *verb = findVerb(verbs, args[1]);
        if (verb == nullptr) {
            printUsage(group, verbs);
            return fail("usage");
        }
        if (verb->handler == nullptr) {
            // 动词表由实现方维护，缺 handler 属表的缺陷，仍按命令失败处理而不是崩溃。
            return fail("%s: no handler", (verb->name != nullptr) ? verb->name : "?");
        }
        return verb->handler(ctx, argc - 1, mutableArgs(args, 1U));
    }
    if (defaultVerb != nullptr) {
        const Verb *verb = findVerb(verbs, defaultVerb);
        if ((verb != nullptr) && (verb->handler != nullptr) && (verb->name != nullptr)) {
            // handler 约定 argv[0] 为动词本身；Verb::name 是 const，复制到栈上可写缓冲再传入，
            // 这样不必对冻结签名里的 char ** 做 const_cast。
            const std::string_view name{verb->name};
            if (name.size() >= kVerbNameMax) {
                return fail("verb name too long");
            }
            std::array<char, kVerbNameMax> nameBuf{};
            std::copy(name.begin(), name.end(), nameBuf.begin());
            std::array<char *, 1> single{nameBuf.data()};
            return verb->handler(ctx, 1, single.data());
        }
    }
    printUsage(group, verbs);
    return fail("usage");
}

void printSection(const char *title) noexcept
{
    outPrintf("[%s]\n", (title != nullptr) ? title : "");
}

void printKv(const char *key, const char *fmt, ...) noexcept
{
    std::array<char, kValueMax> value{};
    if (fmt != nullptr) {
        va_list args;
        va_start(args, fmt);
        (void)std::vsnprintf(value.data(), value.size(), fmt, args); // 超长截断：一行一个值
        va_end(args);
    }
    outPrintf("  %-*s %s\n", kKeyWidth, (key != nullptr) ? key : "", value.data());
}

} // namespace cli
