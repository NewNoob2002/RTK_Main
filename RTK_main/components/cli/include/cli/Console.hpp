// 控制台组件：esp_console REPL + 分组命令表注册 + 参数解析辅助（冻结接口，ADR-0007 §3.3）。
// 线程：add()/addVerbs()/init()/run() 只在启动任务各调一次；handler 在 console 任务串行执行，可阻塞，非 ISR 安全。
// 堆：只保存静态表指针，不分配堆；REPL 任务栈与命令行缓冲由 IDF 分配。

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_err.h"

namespace cli {

/** 返回值即 esp_console 的命令返回码：0 成功，非 0 由 REPL 打印 "Command returned non-zero error code"。 */
using Handler = int (*)(void *ctx, int argc, char **argv) noexcept;

/** 一条控制台命令的定义；name/help/hint 必须指向静态存储（命令表只保存指针）。 */
struct Cmd {
    const char *name = nullptr; // 命令名（分组名），例如 "status"、"alarm"、"wl"；静态字符串
    const char *help = nullptr; // 多行帮助，静态字符串
    const char *hint = nullptr; // 参数提示，可为 nullptr
    Handler handler = nullptr;
    void *ctx = nullptr; // 传给 handler，通常是模块实例
    bool hidden = false; // true：不在 `help` 列表中出现（工厂命令等），`help <name>` 仍可查看
};

inline constexpr size_t kMaxCmds = 24; // 全部表合计上限
inline constexpr size_t kMacLen = 6;

/**
 * @brief  登记一张命令表
 * @param  cmds: 命令表；只保存 span（指针+长度），不拷贝条目，表必须是静态存储
 * @retval ESP_OK 登记成功（空表也返回它，且不占用名额）
 * @retval ESP_ERR_INVALID_ARG 任一项 name/help/handler 为空
 * @retval ESP_ERR_INVALID_STATE 与已登记名称重复，或 init/start 之后调用
 * @retval ESP_ERR_NO_MEM 合计超过 kMaxCmds
 * @note   只在启动任务、init/start 之前调用；失败时不登记该表的任何一项。
 */
[[nodiscard]] esp_err_t add(std::span<const Cmd> cmds) noexcept;

/**
 * @brief  建立控制台：创建 REPL（UART/USB-CDC 按 sdkconfig 选择）、装好输出通道、注册 help 与全部已登记命令。
 *         REPL 任务创建后阻塞等待 run()，此时不打印提示符、不读输入；cli::out_* 与 ESP_LOG 已走统一出口。
 * @param  prompt: 提示符，静态字符串
 * @retval ESP_OK 成功；重复调用 → ESP_ERR_INVALID_STATE；其余为 IDF 错误码（失败时已释放 REPL）
 * @note   启动任务调用一次；须在其他任务启动前调用（会重装控制台 UART 驱动）。
 */
[[nodiscard]] esp_err_t init(const char *prompt) noexcept;

/**
 * @brief  放行 init() 创建的 REPL 任务：打印 IDF 提示语与 prompt 并开始读命令。启动流程的最后一步调用。
 * @retval ESP_OK 成功；未 init 或已运行 → ESP_ERR_INVALID_STATE
 */
[[nodiscard]] esp_err_t run() noexcept;

/**
 * @brief  init(prompt) + run() 的组合
 * @param  prompt: 提示符，静态字符串
 * @retval ESP_OK 成功
 * @retval 其他 init() 或 run() 的错误码
 * @note   测试应用等无需分阶段的场景使用；只在启动任务调用一次。
 */
[[nodiscard]] esp_err_t start(const char *prompt) noexcept;

/* ---- 参数解析辅助（纯函数，失败返回 false 且不改 out；任意任务可调用，不阻塞、不分配堆） ---- */
/**
 * @brief  解析无符号 32 位整数
 * @param  s: 十进制或 0x/0X 前缀十六进制，NUL 结尾
 * @param[out] out: 解析结果；失败时保持原值
 * @retval true  解析成功
 * @retval false s 为 nullptr、空串、含非法字符或超出 uint32_t
 */
[[nodiscard]] bool parseU32(const char *s, uint32_t &out) noexcept;
/**
 * @brief  解析 0..255 的整数，语法同 parseU32
 * @param  s: 十进制或 0x/0X 前缀十六进制，NUL 结尾
 * @param[out] out: 解析结果；失败时保持原值
 * @retval true  解析成功
 * @retval false 语法错误或数值超过 255
 */
[[nodiscard]] bool parseU8(const char *s, uint8_t &out) noexcept;
/**
 * @brief  解析 MAC 地址
 * @param  s: 必须恰好是 "AA:BB:CC:DD:EE:FF"（大小写均可，17 个字符）
 * @param[out] out: 6 字节 MAC；失败时保持原值
 * @retval true  解析成功
 * @retval false s 为 nullptr、长度不是 17、分隔符不是 ':' 或含非十六进制字符
 */
[[nodiscard]] bool parseMac(const char *s, std::span<uint8_t, kMacLen> out) noexcept;
/**
 * @brief  把偶数个十六进制字符解析成字节
 * @param  s: 十六进制串，NUL 结尾，长度必须为偶数
 * @param[out] out: 输出缓冲；失败时保持原值
 * @param[out] written: 写入的字节数；失败时保持原值
 * @retval true  解析成功，written == s 长度的一半
 * @retval false s 为 nullptr、长度为奇数、含非法字符或字节数超过 out.size()
 */
[[nodiscard]] bool parseHex(const char *s, std::span<uint8_t> out, size_t &written) noexcept;

/* ---- 统一回执与子命令（ADR-0009 §2.5）----
 * init() 为每条命令注册一个 trampoline：handler 返回 0 → 打印 "OK"；返回非 0 且本次调用过 fail() →
 * fail() 已打印 "ERR <原因>"；返回非 0 但没调用 fail() → 打印 "ERR <返回值>"。trampoline 总是向 esp_console
 * 返回 0。回执状态是 console 任务私有的（命令只在 REPL 任务中串行执行）。
 */

inline constexpr int kCmdFail = 1; // fail() 的返回值，handler 直接 return

/**
 * @brief  打印 "ERR <fmt…>" 并标记本次命令已给出原因
 * @param  fmt: printf 格式串，非空
 * @retval kCmdFail 总是返回它，handler 直接 return
 * @note   只在命令 handler 中调用（console REPL 任务串行执行命令）；经 outPrintf 输出，可能阻塞，不分配堆。
 */
int fail(const char *fmt, ...) noexcept __attribute__((format(printf, 1, 2)));

/** 一个子命令（动词）；name/usage/help 指向静态存储。 */
struct Verb {
    const char *name = nullptr;  // 动词，例如 "list"
    const char *usage = nullptr; // 参数用法，例如 "<MAC>"；无参数为 nullptr
    const char *help = nullptr;  // 一行说明
    Handler handler = nullptr;   // argv[0] 是动词本身
};

/**
 * @brief  子命令分派
 * @param  verbs: 本组动词表
 * @param  ctx: 传给动词 handler 的上下文，通常是模块实例
 * @param  argc: esp_console 传入的参数个数，argv[0] 为组名
 * @param  argv: 参数数组，argv[1] 为动词
 * @param  defaultVerb: 没有动词时执行的动词名；nullptr 表示必须显式给动词
 * @retval 动词 handler 的返回值（有动词，或以 defaultVerb 执行时）
 * @retval kCmdFail 未知动词、动词缺 handler、没有动词且无 defaultVerb：已打印本组用法并 fail("usage")
 * @note   找到动词时以 (argc-1, argv+1) 调用其 handler；只在命令 handler 内（REPL 任务）调用；
 *         输出可能阻塞，不分配堆。
 */
int dispatch(std::span<const Verb> verbs, void *ctx, int argc, char **argv,
             const char *defaultVerb = nullptr) noexcept;

/**
 * @brief  打印一组的用法
 * @param  group: 组名，用于 "usage: <组> <verb> [args]"；nullptr 显示为空
 * @param  verbs: 动词表
 * @note   `help <组>` 与 dispatch 失败时使用；列宽按本表最长项加宽，上限 32。
 */
void printUsage(const char *group, std::span<const Verb> verbs) noexcept;

/**
 * @brief  登记一组的动词表，供 `help` 与 `help <组>` 显示
 * @param  group: 组名；必须已经或将要经 add() 登记，静态字符串
 * @param  summary: 组的一行摘要，help 列表里使用，静态字符串
 * @param  verbs: 动词表；只保存 span（指针+长度），必须静态存储
 * @retval ESP_OK 登记成功
 * @retval ESP_ERR_INVALID_ARG group/summary 为空，verbs 为空表，或某项 name/handler 为空
 * @retval ESP_ERR_NO_MEM 已登记的动词表数达到 kMaxCmds
 * @retval ESP_ERR_INVALID_STATE init/start 之后调用
 * @note   只在启动任务、init/start 之前调用。
 */
[[nodiscard]] esp_err_t addVerbs(const char *group, const char *summary,
                                 std::span<const Verb> verbs) noexcept;

/* ---- 分节输出辅助（status、sys 等使用，统一对齐与单位）---- */

/**
 * @brief  打印节标题，例如 "[link]"
 * @param  title: 标题文本；nullptr 打印空标题
 * @note   只在命令 handler 内调用；经 outPrintf 输出，可能阻塞。
 */
void printSection(const char *title) noexcept;
/**
 * @brief  打印一行键值
 * @param  key: 键名，左对齐到固定宽度；nullptr 显示为空
 * @param  fmt: printf 格式串，非空
 * @note   值先格式化到 kValueMax 字节的栈缓冲（超长截断），再与前缀拼成一行输出；
 *         只在命令 handler 内调用，可能阻塞，不分配堆。
 */
void printKv(const char *key, const char *fmt, ...) noexcept __attribute__((format(printf, 2, 3)));

} // namespace cli
