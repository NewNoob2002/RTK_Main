# RTK_main 编码规范

本规范面向读代码和新增功能的人，依据 ADR-0008 §3、§4、§6–§9；C/C++ 分工沿用 ADR-0003。
硬性门禁、依赖允许表、冻结范围和操作授权见 `AGENTS.md`，告警与格式配置仍由工具维护。
R0 只建立规范：目录迁移由 R1 完成，模块类/跳板由 R2 实现，命名空间与 include 统一由 R3 完成，
文件头清理由 R4 完成，驱动拆分由 R5 完成，模板由 R6 提供。下面的目标示例不表示现有接口已改动。

## 1. 目录与文件

### 1.1 按功能组织，装配入口保持简单

一个功能一个目录，模块实现、命令处理及其私有辅助文件放在一起。
`app_main.cpp` 只做初始化、按清单注册模块、启动执行器。组件依赖以 `AGENTS.md` 的允许表和 CMake
`REQUIRES` 为门禁；纯逻辑组件不得因目录或命名空间统一而引入 IDF/FreeRTOS/产品依赖。

这样写（ADR-0008 §3 的目标结构）：

```text
main/
  app_main.cpp
  app_config.hpp                # 版本、执行器与任务参数
  module_list.inc               # 一行注册一个模块
  def/                          # link_def.hpp、rtcm_def.hpp、system_def.hpp
  modules/
    _template/                  # 模块、命令处理与新增模块说明
    bt_link/                    # bt_link_module.*、bt_link_cmds.cpp、async_tx.*
    rtcm/                       # rtcm_module.*、rtcm_cmds.cpp、rtcm_sink.*
    storage/
    system/
  support/                      # 与具体模块无关的控制台辅助等胶水
components/
  app_fw/  rtos/  cli/            # 框架层
  spp_server/  nvs_store/         # 驱动层
  device_proto/  stream_demux/    # 逻辑层，支持 host 测试
  link_core/  bt_reconnect/
```

不要这样写：

```text
main/
  bt_link_module.cpp            # 同一功能散在根目录
  cmds_link.cpp
  async_tx.cpp
  all_modules.cpp              # 混合多个功能
```

### 1.2 共享定义与实现分离

`def/` 只放跨模块确实共享的类型、枚举和快照，不放驱动操作或模块私有状态；
其他模块通过共享定义或已有队列/Slot 交互。单文件建议不超过 500 行，超过时按职责拆分，避免机械切段。

```cpp
// 这样写：main/def/link_def.hpp
struct LinkSnapshot {
    bool connected = false;
};
// 不要这样写：共享头暴露内部发送循环
void run_bt_tx_forever();
```

### 1.3 注册集中，新增模块从模板开始

新增模块复制 `main/modules/_template/`（详见 [`main/modules/_template/README.md`](../main/modules/_template/README.md)），实现必需操作，再在 `app_config.hpp`、`module_list.inc` 及有命令时的 `app_main.cpp` 完成注册。
不要在入口、多个命令文件中各维护一份模块列表。

```cpp
// 这样写：module_list.inc
MODULE_DEF(BtLink, kLink)
// 不要这样写：在每个调用点重复填写实例和回调表
register_module(&s_bt_link_module, &kHandwrittenBtLinkOps);
```

## 2. 文件头与生成来源

第一行用一句中文说明文件做什么；第二行起说明谁调用、可否阻塞、是否 ISR 安全。
使用堆的文件还须注明第 7 节的档位、容量和失败策略。文件头不写工具/模型或人工负责人。
脚本同样以用途、调用约束开头，本工程通过 `python3` 执行脚本。

```cpp
// 这样写：汇总链路状态并处理链路控制命令。
// init 由启动任务调用；poll/command 由链路执行器串行调用，不阻塞，非 ISR 安全。
// 堆档位：禁止区；本文件不分配堆。

// 不要这样写：把流程信息放在用途说明之前
// Generated-by: 某工具 + 某模型
// 人工负责人：UNKNOWN
```

来源写在 git 提交信息的 trailer，模型标识以任务卡为准；不得添加 `Signed-off-by`。
本条是提交者的记录规则，不授予实现方提交权限。

```text
这样写（提交 trailer）：
Generated-by: pi + codex/gpt-6.1-sol

不要这样写（源文件头）：
// Generated-by: pi + codex/gpt-6.1-sol
```

## 3. 命名与 include

### 3.1 C++ 保持 snake_case

类型用 `PascalCase`（即配置中的 `CamelCase`）；函数、变量、参数用 `snake_case`；
常量和枚举值用 `kName`；私有成员用 `name_`；文件作用域可变静态用 `s_` 并放在匿名命名空间；
宏全大写。不用 `p_`、`b_` 等类型前缀，也不用 `g_` 表示文件静态。

```cpp
// 这样写
struct LinkState { bool connected = false; };
enum class LinkEvent { kConnected, kClosed };
constexpr uint32_t kRetryMs = 1000; // ms
namespace {
constinit LinkState s_link_state{};
}
void publish_link(uint32_t now_ms) noexcept;
// 类内：uint32_t last_tick_ms_ = 0;

// 不要这样写
class link_state;
void PublishLink(uint32_t nowMs);
// 类内：uint32_t _lastTick;
```

### 3.2 统一命名空间与 include 根

ADR-0008 §9 的对照如下。组件按用途命名，统一名称不改变职责与可复用性。
公共基础错误码位于 `app_fw` 的 `fw/status.hpp`，使用 `fw::Status`、`fw::is_ok`、`fw::to_string`，枚举值及字符串映射不变。
C 头文件名与符号前缀取组件前缀（`dev_proto_*`、`spp_server_*`、`nvs_store_*`）；NVS 命名空间 `"rtk"`、键值、蓝牙名与 `RTK_*` Kconfig 选项保持不变。

| 组件目录 | 命名空间 | C++ include 根 |
| --- | --- | --- |
| `rtos` | `rtos` | `rtos/` |
| `app_fw` | `fw` | `fw/` |
| `cli` | `cli` | `cli/` |
| `spp_server` | `spp` | `spp/` |
| `nvs_store` | —（仅 C 接口） | —（C 头平铺） |
| `device_proto` | `proto` | `proto/` |
| `stream_demux` | `demux` | `demux/` |
| `link_core` | `core` | `core/` |
| `bt_reconnect` | `reconnect` | `reconnect/` |
| `main` | `app` | `app/`（对外 C++ 头） |

```cpp
// 这样写（R3 目标）
#include "fw/module.hpp"
namespace app {
// 应用模块
}
// 不要这样写（新增接口缺少组件根）
#include "module.hpp"
namespace unrelated_app {
}
```

### 3.3 命名空间：按组件划分，层次服务于可读性

- 每个组件有自己的命名空间，按上表映射，例如 `fw`、`demux`、`reconnect`。
- 不得引入顶层 `alarm`、`link`、`sync`，避免与 POSIX 函数冲突。
- 子命名空间可以用，前提是读调用点时更清楚：协议常量表 `proto::msg_type::kQuery`、一个解码器一个子空间、`detail`。总深度不超过 `a::b::c` 三层。
- 同一组常量如果本来就是“取值集合”，优先 `enum class`；如果是协议里的一张号码表、需要当整数用，放进命名空间也可以。
- `.cpp` 中为跨组件长名写的 `namespace x = …` 别名允许，但只在 `.cpp` 局部使用，头文件不写；同一个别名在多个文件反复出现，说明命名空间该调整了。
- 头文件不写 `using namespace`。

## 4. 类与函数内容

### 4.1 一个类只做一件事，成员按阅读顺序排列

顺序为 public 类型/常量 → 构造 → 公开方法 → private 方法 → 数据成员。
构造必须 `constexpr`、不失败、不访问硬件/RTOS/日志；初始化工作放在显式 `[[nodiscard]] init()`。
独占资源的类删除拷贝，全局对象 `constinit`。运行期多态仍仅限 `Transport`、`demux::Sink`，
采用受保护非虚析构与 `final` 实现；模块不增加虚函数。

```cpp
// 这样写（公开方法注释见第 6 节）
class LinkModule final {
public:
    using Snapshot = LinkSnapshot;
    constexpr LinkModule() noexcept = default;
    [[nodiscard]] fw::Result init() noexcept;
private:
    void publish() noexcept;
    Snapshot snapshot_{};
};
// 不要这样写：构造时启动 RTOS，数据和操作混排
class MixedModule {
    uint32_t tick_;
public:
    MixedModule() { start_task(); }
    virtual void poll();
};
```

### 4.2 函数保持短小，先检查再走主流程

建议函数不超过 60 行、嵌套不超过 3 层；一个函数保持同一抽象层次。
先校验参数与状态，使用提前返回（guard clause），最后保留直线主流程。
60 行是阅读建议，`.clang-tidy` 当前 100 行阈值是另一条机械门禁，不表示鼓励写满。

```cpp
// 这样写
if (!ready_) {
    return fw::Status::kInvalidState;
}
if (payload.empty()) {
    return fw::Status::kInvalidArg;
}
return send_payload(payload);
// 不要这样写
if (ready_) {
    if (!payload.empty()) {
        // 很长的发送流程包在多层条件里
    }
}
```

### 4.3 事件与命令使用 switch 分派

统一入口使用 `switch`，每个分支调用一个具名处理函数，包含 `default`。
处理函数先验证长度、范围和状态，再拷贝或解码；不得把字节缓冲区强转成结构体。

```cpp
// 这样写
switch (verb) {
case LinkVerb::kReset:
    return handle_reset(payload);
default:
    return fw::Result::kNotSupported;
}
// handle_reset 中先检查，再按字段解码
if (payload.size() != kResetPayloadBytes) {
    return fw::Result::kInvalidArg;
}
// 不要这样写
if (verb == LinkVerb::kReset) {
    const auto &request = *reinterpret_cast<const ResetRequest *>(payload.data());
    // 未检查长度、对齐、字节序，且入口里堆叠业务逻辑
}
```

### 4.4 可恢复错误返回错误码，同一错误只记一次日志

可失败函数返回 `fw::Status`/`Result` 并加 `[[nodiscard]]`。错误在发生处记一次日志，上层只传错误码；
纯逻辑层不依赖日志平台，由第一个有日志能力的适配边界记录一次。
IDF 返回值必须检查，`esp_err_t` 只在 `rtos::from_esp_err` 映射；
禁止用 `ESP_ERROR_CHECK` 处理可恢复错误或用 `(void)` 丢弃 `esp_err_t`。
ISR、C 回调蹦床、析构和移动操作标 `noexcept`。失败路径释放已获得的资源，避免部分更新。

```cpp
// 这样写：init 已在有日志能力的边界记录错误
const auto status = transport.init();
if (!is_ok(status)) {
    return status;
}
// 不要这样写：重复记录，或把失败直接忽略
if (!is_ok(status)) {
    ESP_LOGE("link", "link: init_failed_again");
}
(void)esp_some_fallible_call();
```

### 4.5 参数表达所有权与长度，输出放最后

只读大对象传 `const T &`，连续数据传 `std::span<const T>`（轻量视图按值传递）；输出参数放最后。
小标量按值传；业务 API 不用裸 `void *` 传上下文，只有 C/框架跳板可以恢复已知对象类型。
每处 `static_cast` 都需注释说明数值范围或类型恢复为何安全；C++ 不用 C 风格转换。

```cpp
// 这样写
[[nodiscard]] fw::Status decode(std::span<const uint8_t> wire, Reply &reply) noexcept;
[[nodiscard]] fw::Status apply(const Settings &settings) noexcept;
// 不要这样写：长度和类型都不明确，输出在前
fw::Status decode(Reply *reply, void *data);
fw::Status apply(Settings settings); // 复制大对象
```

## 5. 日志、常量与并发

### 5.1 日志记录状态变化、错误与周期统计

格式统一为 `模块: 事件 key=value`，不逐次打印函数进入/退出，不打印蓝牙数据内容。
统计要有周期，避免在实时每轮路径制造日志负担。

```cpp
// 这样写
ESP_LOGI("link", "link: connected handle=%" PRIu32, handle);
// 不要这样写
ESP_LOGI("link", "begin poll"); // 每轮噪声
```

### 5.2 常量按用途分组并注明单位

协议字段、容量、时间等有业务含义的数字必须命名，同组常量放在一起；单位写在名称及行尾注释。
0、1 等直接表达空值、计数起点的数字无需套上无意义的名字。

```cpp
// 这样写
constexpr uint32_t kRetryDelayMs = 1000; // ms
constexpr uint32_t kConnectTimeoutMs = 5000; // ms
constexpr size_t kTxCapacityBytes = 512; // B
// 不要这样写
if (now_ms - last_ms > 5000) {
    retry_after(1000);
}
```

### 5.3 明确线程所有权与实时边界

链路状态由链路任务独占，其他任务通过队列/快照交互。SMP 临界区使用 `portMUX_TYPE` 和守卫，
其间不调用 FreeRTOS API、不阻塞；不得以 `volatile` 同步。任务静态创建、显式绑核并注释原因，
IDF 栈大小单位为字节。蓝牙回调不阻塞、不解析、不打印数据内容。

```cpp
// 这样写：只在短临界区更新共享计数
{
    const rtos::CriticalSection guard(stats_mux_);
    ++stats_.events;
}
// 不要这样写
volatile bool ready = false; // 不能建立跨核同步关系
// 持有 stats_mux_ 时调用 xQueueReceive(..., portMAX_DELAY)
```

### 5.4 全局可变状态：允许，但要有主人、成组、看得出生命周期

全局变量和文件静态变量不是禁止项。单实例的驱动、模块、应用顶层对象放在文件作用域，往往比层层传参更好读（Arduino `BluetoothSerial.cpp` 的驱动状态就是文件静态）。判断标准是读代码的人能否一眼看出：**这份状态归谁、什么时候初始化、谁会改它**。

可以：
- 单实例对象放文件作用域：`constinit` 初始化，放匿名命名空间或 `static`，名字带 `s_` 前缀。例如模块实例、执行器栈、C 驱动的 `s_spp`。
- 跨文件共享的少量对象，用 `extern` 暴露**一个**对象或用访问函数，例如 `data::center()`。
- 一个概念的多个字段收进一个结构体，例如 RTCM 批缓冲 + 长度 + 计数 → `struct RtcmBatch`，不要拆成 5 个并列全局。

避免（读者需要跨文件拼图才能理解的写法）：
- “存储区 + 全局指针 + init 里 placement new 后赋值”成对出现：能 constexpr 构造就直接定义对象；必须延迟构造时用一个 `fw::Lazy<T>`/`std::optional<T>`，不要裸指针。
- `extern` 后再用 `inline constexpr auto &kX = s_x` 换名二次暴露。
- 只被一个函数使用的状态提到文件作用域：应该放进函数或类成员。
- 同一份状态被多个任务写：要么只有一个写者，要么在旁边注明锁或临界区。
- 一个文件里堆积大量互相关联的全局（参考：`BluetoothSerial.cpp` 的 30 多个 static 加上 `#define` 位标志，阅读时很难看出哪些状态成组变化）：先按“对象/概念”合并成结构体或类成员。

是否把模块全局收进类、收进 `AppContext`，按可读性决定，不追求“全工程只剩一个全局”。

## 6. 模块与注释

### 6.1 模块类 + 模板跳板 + 一行清单注册（R2 目标）

模块以类保存状态，按需要提供 `init`、`start`、`poll`、`command`。
`fw::make_ops<T>()` 用 `if constexpr (requires { ... })` 检测成员，未提供的操作填 `nullptr`；
框架内部生成静态跳板，业务方法不暴露 `void *self`。实例静态存储，零虚表、零堆。
以下是目标形态示意，R0 不实现尚未存在的 `make_ops` 接口。

```cpp
// 这样写：命名空间 app 中，fw 指通用 app_fw 组件
class BtLinkModule final {
public:
    constexpr BtLinkModule() noexcept = default;
    [[nodiscard]] fw::Result init() noexcept;
    [[nodiscard]] bool poll(uint32_t now_ms) noexcept;
    void command(const fw::Command &cmd, fw::CommandResult &out) noexcept;
private:
    uint32_t last_tick_ms_ = 0;
};
// 清单的一行展开为静态实例与 fw::make_ops<BtLinkModule>() 跳板表
MODULE_DEF(BtLink, kLink)

// 不要这样写：业务层重复恢复指针并手工维护操作表
void bt_link_command(void *self, const fw::Command &cmd, fw::CommandResult &out);
const fw::ModuleOps kOps{init_trampoline, nullptr, poll_trampoline, bt_link_command};
```

### 6.2 公开 API 用 doxygen 写契约，实现内解释原因

每个公开函数用 `/** */` 说明做什么、谁调用、可否阻塞、线程/ISR 安全性及错误返回。
返回 `void` 或 `bool` 也解释结果含义。注释用中文，保留必要英文术语；实现内只解释为什么、约束与副作用，
不复述语句，不保留注释掉的代码。上面的片段省略重复契约，实际公开头必须补齐。

```cpp
// 这样写
/** 初始化链路；仅启动任务调用一次，不阻塞，非线程/ISR 安全。
 * @return kOk 成功；kBusy 已初始化；kInvalidArg 配置非法。
 */
[[nodiscard]] fw::Status init() noexcept;
// 队列仅存值副本，调用结束后不持有调用者缓冲区。

// 不要这样写
// 初始化
fw::Status init();
// count 加一
// ++old_count;  // 历史代码应交给版本控制
```

## 7. 堆内存分级（ADR-0008 §8，2026-11-12 任务卡确认采用）

### 7.1 先判断调用路径，再选择分配方式

| 档位 | 路径 | 规则 |
| --- | --- | --- |
| 禁止区 | ISR、蓝牙回调、链路任务每轮路径、发送任务 | 禁止分配堆；使用静态存储、固定容量缓冲区或调用者存储；避免释放造成不可预测耗时 |
| 启动期一次性 | 任务启动前的初始化 | 允许一次分配，之后不释放；规定容量与失败处理，初始化成功后只用既有存储 |
| 非实时限量 | 控制台、配置导入导出、OTA、文件解析等非实时路径 | 允许有明确上限的分配，检查失败、明确所有者并释放；不能因入口叫“控制台”就豁免其在链路任务执行的处理函数 |

禁止区检查沿调用链生效；文件登记白名单不能让其中的实时函数分配。
C 文件继续遵守既有 `malloc`/`free` 禁令；下列 C++ 分级不是修改 C 协议/驱动内存语义的授权。
其余被禁容器和特性不因本节解禁。

```cpp
// 这样写：实时路径使用固定容量
std::array<uint8_t, kTxCapacityBytes> tx{};
// 不要这样写：即使源文件已登记，发送任务仍禁止分配
std::vector<uint8_t> tx(payload.begin(), payload.end());
```

### 7.2 异常关闭时显式处理分配失败

`std::vector`/`std::string` 分配失败会走 `abort` 并重启，不能靠 `try/catch` 恢复。
必须处理失败的地方使用 `new (std::nothrow)` 并检查空指针、返回错误码。
也可在允许档位预先 `reserve`，使用时限制长度不超过已保留容量，避免随后增长分配；
**`reserve` 自身仍可能 abort，它不是失败处理机制**。若初次分配失败也必须恢复，应选择 nothrow 存储方案。
`new`/`delete`、`malloc`/`free`、`<vector>`、`<string>` 均须符合档位与白名单要求；
placement new 只构造于已有存储，不分配堆，仍须保证容量、对齐和生命周期。

```cpp
// 这样写：仅用于已登记的非实时限量路径；kImportMaxBytes 为固定上限
if (size > kImportMaxBytes) {
    return fw::Status::kInvalidArg;
}
auto *buffer = new (std::nothrow) uint8_t[size];
if (buffer == nullptr) {
    return fw::Status::kNoSpace;
}
const auto status = load_and_parse_import({buffer, size});
delete[] buffer;
return status;
// 不要这样写：关闭异常后无法可靠返回内存不足
std::string buffer(size, '\0');
```

### 7.3 按精确源文件登记，不按目录放宽

使用堆的文件必须在文件头标档位，并在 `tools/g7b_check/heap_allowlist.txt` 登记仓库相对路径，
行内 `#` 写档位与理由、上限及失败策略。初始白名单为空，不预先豁免未来文件。
G7b 从 `compile_commands.json` 映射目标文件到源文件，逐项列出放行的源文件与堆符号；
未登记则 FAIL，无法确认映射不放行。该门禁不能证明调用路径属于正确档位，仍需审查。
异常/RTTI 禁令和全 ELF 精确白名单/固定拉入链独立生效。

```text
# 这样写（示例路径，只有对应文件实际存在且审查通过时才可登记）
main/modules/storage/config_import.cpp # 非实时限量；配置导入最多 4096 B，nothrow 失败返回 kNoSpace
# 不要这样写
main/modules/storage/* # 任意文件与任意调用路径
```

## 8. C 文件规则（整理既有规则，不改变语义）

### 8.1 命名与 C/C++ 边界

C11 子集；公共符号/宏带组件前缀（`dev_proto_*`、`DEV_PROTO_*`、`spp_server_*`、`nvs_store_*`）；
文件静态变量 `s_`，类型 `_t`；不用 `p_`/`g_`/`b_`。
头文件 `#pragma once`，用 `__cplusplus` 保护 `extern "C"`，只含 C 构造；C 不调用 C++。
布局、错误检查与并发规则同样适用；C 中不用 `malloc`/`free`、不用 `goto`，内部函数 `static`。

```c
/* 这样写：C 公共头（省略具体类型定义） */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
dev_proto_err_t dev_proto_decode(const uint8_t *data, size_t len);
#ifdef __cplusplus
}
#endif
/* 实现内部：static uint32_t s_frame_count = 0U; */
/* 不要这样写：公共 C 头包含 C++ 构造 */
/* class Decoder;  std::span<const uint8_t> decode(); */
```

### 8.2 保持 BARR-C 安全核心

每个 `switch` 有 `default`；不混用有/无符号；窄化转换写范围注释；变量使用前初始化；
不将缓冲区强转为结构体指针；多字节字段逐字节小端读写；循环有界。
纯 C 错误返回本模块枚举，依赖 IDF 的 C 模块检查并传播 `esp_err_t`，不在 C 中调用 C++ 映射器。

```c
/* 这样写：调用方已检查 data 至少有两个字节 */
uint16_t value = 0U;
/* 两个 uint8_t 拼成 0..65535，窄化到 uint16_t 安全。 */
value = (uint16_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8U));
/* 不要这样写：布局、对齐和字节序均未经保证 */
/* const frame_t *frame = (const frame_t *)data; */
```

## 9. 格式与静态检查交给工具

布局不在此重复维护：日常用仓库 `.clang-format`，C++ 静态检查用 `.clang-tidy`，C 用 `.clang-tidy-c`；
权威格式门禁是 astyle_py 3.4.7 + IDF 规则文件，实现方不得联网下载/安装。
格式工具不能替代线程契约、命名语义、内存档位及错误路径审查；不得通过放宽告警或改冻结配置消除发现。

```sh
# 这样写：检查实际改动文件
clang-format --style=file --dry-run --Werror path/to/changed.cpp
clang-tidy -p builds/host path/to/pure.cpp
clang-tidy -p builds/host --config-file=.clang-tidy-c path/to/pure.c
# 不要这样写：用忽略退出状态隐藏发现
# clang-tidy path/to/pure.cpp || true
```

## 附录：从 X-TRACK 学了什么、改了什么

来源为 ADR-0008 §2；参考 `main/Software/X-Track/USER/App` 与
`dev_lvgl_v9/Firmware/X-TRACK`（本地参考目录名 `v9`），新分支为主要结构参考。

| X-TRACK 做法 | 本工程采用或调整 |
| --- | --- |
| 一个功能一个目录/文件 | 采用功能目录，超 500 行按职责拆分 |
| `DataProc/Def` 分离共享类型 | 使用 `main/def/`，实现与私有状态留在模块目录 |
| `DataProc_NodeList.inc` 一行注册 | 使用 `module_list.inc`，避免多份注册表 |
| `_Template` 最小骨架 | 提供 `_template/`，新增模块从最小必需操作开始 |
| `onEvent` + switch + 长度检查 | 采用统一入口与先校验后解码，每分支调用具名函数 |
| 类 + 静态回调跳转 | 采用模块类与 `fw::make_ops<T>()`，业务方法隐藏 `void *` |
| 固定分层 | 保留允许依赖表与 host 可测的纯组件边界 |
| 成组常量/单位、公开 API doxygen | 采用；实现内注释解释原因 |
| lowerCamel/PascalCase 方法、前缀私有成员 | 改为 snake_case 方法、`name_` 私有成员 |
| 字符串 ID 订阅、通用堆容器 | 用枚举/constexpr 表、静态存储；堆仅在允许档位按文件登记 |
| 单线程无锁 DataBroker | 保留 Slot/FanoutRing/Executor 和双核同步契约 |
| 每个设备虚函数 | 模块用模板跳板；多态仅限既有 Transport/Sink |
| 双下划线 include guard | 使用 `#pragma once`，避免保留标识符 |
| 函数进入日志 | 只记录状态变化、错误与周期统计 |

```cpp
// 这样写：借鉴统一入口，保持本工程命名和类型 ID
switch (event) {
case LinkEvent::kClosed:
    handle_closed();
    break;
default:
    break;
}
// 不要这样写：把参考工程的字符串订阅与命名一并照搬
// node->subscribe("Power");  void onEvent(void *param);
```
