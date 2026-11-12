# R2 模块类化 + 注册清单（ADR-0008 §4、§5 R2、§6）

执行方：pi + codex/gpt-6.1-sol。先完整阅读 `AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0008-code-organization.md`、
`components/app_fw/`（全部）、`main/`（全部）、`tools/main_host_check/`（全部）、`test/` 下与 app_fw 相关的测试。

## 目标
现在每个模块是“自由函数 + `void *self` + 手写 `ModuleOps` 表 + 全局 `s_xxx_module` 结构”，`app_main.cpp` 里手写 `kModules[]`。改为：

1. **`components/app_fw/include/app/module.hpp` 增加 `make_ops<T>()`**（允许修改此冻结头，验收方重算哈希）：
   ```cpp
   template <typename T>
   [[nodiscard]] consteval ModuleOps make_ops() noexcept;  // 用 requires 检测 T::init()/start()/poll(now_ms)/on_command(cmd,result)，缺省成员填 nullptr
   ```
   跳板是无捕获 lambda 或静态函数模板，内部 `static_cast<T *>(self)`；这是全工程唯一允许 `void *` → 对象转换的地方。
   `ModuleOps`/`Module` 结构与执行器行为不变。在 `test/` 已有的 app_fw 主机测试中加用例：成员齐全/部分缺失/全缺三种类型生成的表正确、跳板转发正确。
2. **四个模块改成 `final` 类**（`BtLinkModule`、`RtcmModule`、`StorageModule`、`SystemModule`）：公开方法 `init/start/poll/on_command`（只实现原来有的），状态为私有成员 `xxx_`；控制台命令表函数 `xxx_cmds()` 保留为自由函数。实例仍是 `constinit` 静态对象，不用堆。
   命令处理中需要访问模块内部状态的地方，通过公开的窄接口（如 `stats()`）而不是公开成员。
3. **`main/module_list.inc`**：一行一个模块，例如
   ```cpp
   // MODULE_DEF(类型, 实例, 名称, ModuleKind, ExecKind, poll_period_ms)
   MODULE_DEF(BtLinkModule, s_bt_link_module, "bt_link", kBtLink, kLink, 0)
   ```
   `app_main.cpp` 用它展开 `kModules[]`（顺序、名称、id、执行器、周期与现在完全一致）。新增模块只改这个文件 + 模块目录。
   注意 `storage` 现在在 `app_main` 中被提前单独 `storage_init` —— 保持这个启动顺序语义，并在 module_list.inc 旁注释说明。
4. **按 `docs/CODING-STYLE.md` 整理这四个模块的实现**：函数 ≤ 60 行、嵌套 ≤ 3 层、guard clause、`switch` 分支调用具名函数、魔数命名并标单位、类成员顺序。`bt_link_module.cpp`（约 480 行）重点拆分长函数。
   **行为不变**：日志文本、控制台输出、协议字节、时序常量、DataCenter 写入顺序都不能变。

不做的事：命名空间与 include 根统一（R3）、文件头/注释清理（R4，但你新建的文件第一行写用途）、组件内部重构。

## 自检（写入 `docs/reports/R2.md`，逐项 PASS/FAIL 与证据）
1. `idf.py -B builds/r2 build` 0 warning；`idf.py -B builds/r2 size` 与基线（`builds/r2-base` 用 HEAD 构建）对比 DRAM/IRAM/Flash，解释差异（预期 DRAM 0 差异，Flash 小幅变化）。
2. `tools/main_host_check`：普通 `builds/r2-mh` 与 ASan/UBSan `builds/r2-mh-asan` 均 29/29（或更多，若你新增场景）。
3. `test/host`（`builds/r2-host`）全部通过，含新增 make_ops 用例；`test/services`、`test/rtk_console` 若受影响也跑。
4. `python3 -I tools/g7b_check/g7b_check.py --build builds/r2` PASS（不得新增堆符号）。
5. 控制台命令名/help 文本、ESP_LOG 格式串：新旧源码提取对比逐字一致（附脚本与输出）。
6. 函数规模：列出 main/modules 下所有函数行数与最大嵌套，超过 60 行/3 层的逐一说明理由。
7. clang-format `--dry-run --Werror`、`tools/tidy_check/tidy_check.py`（如适用于 main）对改动文件无新增问题。

## 约束
只用 `builds/r2*`，不用 `build/`。不得修改除 `components/app_fw/include/app/module.hpp` 外的冻结文件；不改 `components/` 其他组件。
禁止烧录、连接串口、联网安装、git commit。完成后报告最后一行写 `R2 DONE` 或 `R2 BLOCKED: 原因`。

## 补充（2026-11-12，用户反馈“文件内仍滥用全局变量”后追加，优先级高于上文）
先读 `docs/CODING-STYLE.md` 新增的 §3.3、§5.4。R2 范围扩大为：
5. **`bt_link_module.cpp` 的 21 个文件作用域对象全部收进 `BtLinkModule` 私有成员**：transport、async_tx、link_manager、alarm_source、四个 service（去掉 `Storage<…>` + 全局指针 + placement new 的写法；能 constexpr 构造直接做成员，否则用类成员 `std::optional`/延迟构造包装）、router、link_service、sessions、rx_cursor、rtcm 批缓冲与计数、retired_demux。DRAM 允许 ±256 B 内变化，需解释。
6. **`main/data/app_data.*`**：13 个 `extern Slot` + 13 个 `inline constexpr auto &kX` 别名合并为一个 `struct DataCenter { fw类型::Slot<LinkStatus> link_status; … }` 和一个实例访问方式（如 `data::center()` 或 `extern DataCenter g_data`，选一种并在报告说明理由）；所有读写点改过来。
7. 其他 main 文件中不属于 §5.4 允许类别的文件作用域可变对象，同样收进所属类；报告列出改造前后全局对象清单（预期改造后只剩：4 个模块实例、执行器 + 栈存储、DataCenter 实例、DataCenter 的锁）。
8. 不在本阶段改命名空间名称（R3），但 main 内部不再新增 `namespace xx = …` 别名。

## 补充 2（用户贴出 X-TRACK `App.cpp`、`DataNode.cpp` 作为目标风格，优先级最高）
参照 X-TRACK `App.cpp`：`struct AppContext { DataBroker*; Global_Helper*; PageManager*; StatusBar*; }`，
`App_CreateContext()` 按步骤创建、每步一行注释；运行循环只调用 context 的方法；文件里没有其他全局。我们的对应写法（静态分配，不用 new）：

```cpp
// main/app_context.hpp —— 应用的全部顶层对象
struct AppContext {
    data::DataCenter data;                 // 原 13 个 Slot
    BtLinkModule bt_link;                  // 由 module_list.inc 展开成员
    RtcmModule rtcm;
    StorageModule storage;
    SystemModule system;
    os::Executor link_executor;
    os::ExecutorStorage<kLinkStackBytes, 4> link_storage;
};
[[nodiscard]] AppContext &app_context() noexcept;   // 控制台命令等只能经此访问
```
- `app_main.cpp` 里唯一的全局是 `constinit AppContext s_app{}`（放匿名命名空间）；`app_main()` 写成 `create/init → start` 几个短步骤，每步一行注释，像 `App_CreateContext`。
- 补充第 7 条的“改造后全局清单”目标改为：main 中只剩 `s_app`（以及确有必要、需在报告说明的锁）。模块之间、命令与模块之间通过 `AppContext` 成员引用或构造参数连接，不通过全局。
- 学 `DataNode.cpp` 的函数写法：每个函数先做参数/状态检查并提前返回，主流程每个步骤前一行 `/* 做什么 */` 注释，函数短小、一个函数一件事。
- 不学的部分：`new/delete`、`std::list`、字符串 ID 查找、到处 `LOG_INFO`。

## 补充 3（2026-11-12 用户澄清，优先级最高，覆盖“补充 2”中“main 只剩 s_app”的硬性目标）
用户原话：“不需要完全跟这参考的一样，根据现在的编程规划和可读性进行优化，不是说不能使用全局变量和命名空间”。
先重读已修订的 `docs/CODING-STYLE.md` §3.3、§5.4。据此调整：
- **目标是可读性**，不是全局变量数量。不再要求 `AppContext` 单一全局；如已做了 `AppContext` 且读起来清楚，可以保留，否则用文件作用域的模块实例（`constinit`、匿名命名空间）也可以。
- 仍然要消除的：`Storage<…>` + 全局指针 + placement new 成对写法；`extern Slot` + `inline constexpr auto &kX` 换名二次暴露（合并为一个 `DataCenter` 或访问函数）；只被单个函数使用却放在文件作用域的状态；同一概念拆成多个并列全局（RTCM 批缓冲/长度/计数等合并成结构体或成员）。
- 已在做的“收进 `BtLinkModule` 成员”如果让代码更清楚就继续；如果需要大量 `optional`/两段式构造反而更难读，可保留少量文件静态对象，在报告里逐个说明理由。
- 命名空间仍不在 R2 改名。
- 报告的“全局清单前后对比”保留，每个保留下来的全局写一句理由（归谁、何时初始化、谁写）。
