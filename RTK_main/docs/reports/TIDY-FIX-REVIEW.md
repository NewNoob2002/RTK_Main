# TIDY-FIX 独立复审

日期：2026-11-11。复审者：Codex（独立 QA 角色；非本次 TIDY-FIX 的 DeepSeek 实现角色）。人工负责人：UNKNOWN（待人工审查签字）。Profile：esp-idf，C11 / C++20 子集、T2；依据为 AGENTS.md、组件 CMake 与本次实际工具链。

## 建议结论

**PASS（软件复审）**：本轮未发现有具体可达失败场景的阻塞性行为回归；独立干净构建、G5b、G7b、22 项真实驱动 host 回归与格式检查全部通过。上板、控制台运行、NVS 实际持久化与双会话时序没有本轮硬件证据，不能据此声称 HIL 通过。

本工程没有可用 HEAD（`git rev-parse --verify HEAD` 退出 128），因此无法取得可信的 TIDY-FIX 前后逐行 diff。本复审以 `TIDY-FIX.md` 所列变更和当前真实源码为范围，检查调用链及现有契约；“与旧实现完全等价”仍受缺少旧快照的限制。源码哈希见 `logs/TIDY-FIX-REVIEW/sources.sha256`。未修改生产源码、冻结输入、门禁配置或测试。

## 复审范围与代码判断

覆盖 `main/app_console.cpp/.hpp`、`main/app_link.cpp/.hpp`、`main/app_main.cpp`、`components/rtk_bt/rtk_bt_spp.c`、`components/rtk_os/include/rtk/os/static_task.hpp`；另阅读 C 持久化实现、StaticQueue/CriticalSection 契约和受影响调用方。先通过 CodeGraph 定位，再补读其省略段并与磁盘源码核对。

| 变更 | 当前源码核查 |
|---|---|
| 控制台 argv → span | SDK 回调仍保持 `int(int,char **)`；先检查参数数量再访问下标，`alarm` 的短路条件不读取缺失参数；数值范围、SN 长度/换行检查与错误返回保留。span 不逃出回调 |
| SN / 设备名 | MAC 只取第 4/5 字节；高/低半字节均在 0..15，输出四个大写十六进制字符及 NUL。当前所有固定长度 span 来自足长的栈上 std::array。SN 经 NVS 的成功返回保证终止，随后校验；`%.*s` 精度至多 16，32 字节输出足够；view 在同步调用内消费 |
| 命令和快照的 std::array | Command 由队列按值复制，StaticQueue 的 trivially-copyable 断言编译通过；StatusSnapshot 在原有 portMUX 内按值拷贝，锁外才调用堆/栈查询。无新增跨任务 view 或指针所有权 |
| placement new 存储 | std::byte 底层连续存储、大小与 alignas 保留；对象指针直接取 placement new 返回值，不使用旧失效指针；业务对象仍仅由链路任务操作 |
| RX 块零初始化 | 只把实际 got 字节送给同步 on_data；64 字节临时块未逃逸，有界 RX 预算及会话轮转保留 |
| 驱动分支合并 | status 非成功、负长度、长度不符均失败；成功需三条件全部满足。状态字段赋值和唤醒发生点未变，结算仍在 session mux 内；RTOS 操作在出临界区后 |
| PIN / GAP | PIN 循环仍复制固定四位；MODE_CHG 落入无副作用 default；未增加回调阻塞/解析 |
| const 守卫与 task trampoline | const 不改变构造/析构所持锁范围；trampoline 的 const self 仅读取 entry_/arg_ 后调用既有函数指针 |

## 非阻塞疑问与边界

1. **白名单 C 边界的可移植性疑问**（`main/app_link.cpp:68`、`:159`、`:539`；依据：review-checklist A 的对象布局/别名检查）。`std::array<std::array<uint8_t,6>,4>` 被 reinterpret 为 `uint8_t (*)[6]`。本机 GCC 15.2 的 `<array>` 使用唯一 `_M_elems` 数组成员，本次尺寸断言、构建与静态分析通过；现有 C getter 按 `addrs[idx][byte]` 写入，setter 按字节读取 blob。总大小断言自身不是对所有标准库实现的布局和跨 wrapper 指针运算证明。未找到当前固定工具链上的具体失败场景，按 W05 只记录为疑问，不能伪称已复现 bug。后续有机会调整此边界时，直接保留小型 C 二维数组并做具名 NOLINT，比依赖 wrapper 转换更容易证明；本次只读复审不改写。
2. **固定 span 的保证来自构造前提**（`main/app_link.hpp:144`、`app_link.cpp:506`）。固定 extent 不会替调用者检查指针；从空指针或不足长缓冲区构造 span 本身即违反前提。当前全部调用方使用足长 std::array，未发现可达错误。报告中“类型排除所有空指针”的表述应理解为当前合法调用的约束，不能作为任意外部输入的校验替代。
3. `build_device_name` 的 view 当前只来自终止字符串或常量，不存在内嵌 NUL。若将来允许任意二进制 view，`%.*s` 仍在首个 NUL 截断，需要先定义 SN 的 NUL 规则；不把尚无调用方的情形当作本次运行回归。

没有列出无法给出触发场景的 High/Medium 缺陷；上述疑问保留供人工审查，不替代目标实测。

## 本轮实际执行的命令

构建 shell 先执行 `. ~/esp/esp-idf/export.sh >/dev/null`，IDF 命令设置 `IDF_COMPONENT_MANAGER=0`。所有完整日志在 `docs/reports/logs/TIDY-FIX-REVIEW/`。

| 命令 / 检查 | 状态 | 结果 / 日志 |
|---|---|---|
| `git rev-parse --verify HEAD` | BLOCKED | 无 HEAD，不能逐行比对变更前快照；未进行 git 写操作 |
| `idf.py -B build-tidy-fix build` | PASS | 退出 0，0 warning/error；`build.log` |
| `idf.py -B build-tidy-fix-review build` | PASS | 独立新目录，1363 步完成；四个改动 TU 实际重编，0 warning/error；`build-fresh.log` |
| `python3 -I tools/tidy_check/tidy_check.py --idf-build build-tidy-fix --test-app-build test_apps/rtk_pure/build-accept-esp32` | PASS | 27 文件，0 发现，G5b PASS；`tidy.log` |
| `python3 -I tools/g7b_check/g7b_check.py --build build-tidy-fix` | PASS | 自有 17 个对象零违规，全 ELF 6/6 符号在精确白名单与拉入链内；`g7b.log` |
| `python3 -I tools/g7b_check/g7b_check.py --build build-tidy-fix-review` | PASS | 干净构建同样通过；`g7b-fresh.log` |
| `idf.py -B build-tidy-fix size` | PASS | Total image 620408 B；bin 620528 B，app 分区余 64%；IRAM 82743 B、DRAM 62877 B；`size.log` |
| `cmake -S tools/t13_host_check -B build-t13-tidy-review`、`cmake --build build-t13-tidy-review -j 4` | PASS | 构建真实 rtk_bt_spp.c，0 warning/error；`t13-build.log` |
| `ctest --test-dir build-t13-tidy-review --output-on-failure` | PASS | 22/22，6.53 s；包括完成状态、分块、超时、并发关闭/迟到回调和 GAP PIN；`t13-test.log` |
| `clang-format --style=file --dry-run --Werror main/app_console.cpp main/app_console.hpp main/app_link.cpp main/app_link.hpp main/app_main.cpp components/rtk_bt/rtk_bt_spp.c components/rtk_os/include/rtk/os/static_task.hpp` | PASS | 退出 0；`format.log` |
| main 控制台/SN/持久化 host smoke | NOT_RUN | 既有临时 harness 已删除，本轮未新建框架，不把类型分析当成执行结果 |
| 上板、UART0、NVS 实际掉电保持、双 SPP 会话 | NOT_RUN | 本复审子任务不执行硬件动作；由主任务处理 HIL 前置条件 |
| 权威 astyle_py | NOT_RUN | 禁止安装/下载；未执行 |

host/ASan、冻结清单及 T15 两目标复核由主任务独立执行，结果记录在主任务报告；本报告不把其他执行者的结果算作本复审子任务执行。

## 契约偏离及交接

本次仅新增本报告与检查日志，保留构建目录；没有更改接口、源码、告警或冻结校验。TIDY-FIX 原报告所列“测试目录 15 条 G5b 发现”和“static_task 哈希待更新”已在后续 T15 验收中处理，本轮 G5b 0 发现与之相符。最终人工签字和硬件验收仍待完成。
