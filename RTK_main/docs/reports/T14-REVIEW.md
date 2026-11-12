# T14 独立复审记录

日期：2026-11-10。范围：T14 NVS、组合根、链路任务、UART0 控制台。profile：esp-idf；依据为项目 sdkconfig、IDF 组件注册及冻结任务卡。实现者为 omp + DeepSeek-V4.1-Flash；本记录由父窗口独立核对生产源码、SDK 源码及目标产物。人工负责人 UNKNOWN（待人工审查签字）。

## 结论

**T14 BLOCKED：冻结的 G7b 全 ELF exceptions/rtti 零符号要求未满足。** 已完成本次实现与可达复审；不能宣称 T14 验收通过或 BATCH-F DONE。T15 未在本次执行。

异常/RTTI 来源可追溯到 IDF NVS 和工具链 libstdc++，自有目标文件未引入堆分配、异常或 RTTI 符号。未修改冻结契约、SDK、工具链或检查脚本，也未添加符号覆盖。

## 实现范围与复审处理

实现文件：`components/rtk_store/rtk_store.c`、该组件 CMake；`main/app_main.cpp`、`app_link.cpp/.hpp`、`app_console.cpp/.hpp`、`Kconfig.projbuild`、主组件 CMake。实现报告见 `T14.md`。

已检查 NVS 参数/长度校验、句柄关闭、提交错误传播；启动失败后控制台可用路径；链路任务对业务对象的单一所有权；快照临界区；命令结果匹配；静态任务、绑核与字节栈；RX 继续读取与轮转预算。

复审期间修复：

1. 命令已入队但等待结果超时时，控制台原先错误显示 NOT executed。现运行中的链路无响应时显示 execution result unknown；命令失败返回非零退出码。
2. 深度 1 的结果队列可能被已超时命令的迟到 ACK 占满，导致下一条命令结果丢失。现保留 seq 匹配，并在队列满时移除旧结果再发布最新结果；生产者限定为单一控制台。
3. 持续非空事件队列可能令 core 1、优先级 10 的链路任务始终可运行，饿死已订阅 TWDT 的 IDLE1。现每轮末尾阻塞 1 tick，保持每轮完成后喂自身 TWDT；实板调度和 TWDT 行为仍需 HIL。
4. RX 和命令处理采用有限预算，RX 预算耗尽保留每会话 drain_pending，不依赖下一个 DATA 事件继续读取；在两个会话间轮转起点。
5. 全局对象显式 constinit；控制台先创建 REPL、再注册命令、最后启动，失败清理检查并记录错误。

本次未修改公共冻结接口或已验收的 rtk_core 实现。GNSS、电台、电池等真实硬件功能不属于 T14；框架迁移未实施。

## 独立执行的验证

每个构建 shell 先执行 `. /home/gtc/esp/esp-idf/export.sh`。该脚本存在 shell 自动补全环境警告（complete command not found），不是编译告警；以下编译没有 compiler warning。

| 命令 / 检查 | 结果 | 证据 |
|---|---|---|
| `sha256sum -c docs/frozen.sha256` | PASS | 清单全部 OK |
| `cmake --build build-host && ctest --test-dir build-host --output-on-failure` | PASS | 8/8 可执行文件通过 |
| `cmake --build build-asan && ctest --test-dir build-asan --output-on-failure` | PASS | 8/8 通过，无 sanitizer 报告 |
| `idf.py -B build-t14-review build`（新目录干净构建） | PASS | 1363 个构建步骤完成，0 compiler warning |
| 同目录最终窄增量构建（加入每轮 delay 后） | PASS | 最终 bin `0x978b0`，620,720 字节，应用分区剩余 64% |
| `idf.py -B build-t14-review size`、`size-components` | PASS | 体积命令成功；随后窄增量的 cpp_cost 记录最终 ELF flash 620,596 字节，低于 1.2 MB |
| `clang-format --style=file --dry-run --Werror main/app_main.cpp main/app_link.cpp main/app_link.hpp main/app_console.cpp main/app_console.hpp components/rtk_store/rtk_store.c` | PASS | 退出码 0 |
| `xtensa-esp32-elf-nm -u -C build-t14-review/esp-idf/main/CMakeFiles/__idf_main.dir/*.obj build-t14-review/esp-idf/rtk_*/CMakeFiles/__idf_rtk_*.dir/*.obj` | PASS | 无堆分配、异常或 RTTI 未定义引用 |
| 最终产物相同对象的完整 `nm -C` 符号表 | PASS | 无 operator new/delete、malloc/free、异常或 RTTI 符号；可见 vTaskDelay 引用 |
| `python3 /home/gtc/.agents/skills/embedded-engineering/scripts/cpp_cost.py --prefix xtensa-esp32-elf- check build-t14-review/rtk_main.elf --tier T2` | FAIL | exceptions=2，rtti=4；整体退出码 1 |
| 权威 astyle_py | NOT_RUN | 项目禁止实现方下载/安装 |
| 实板 HIL、蓝牙兼容/双会话时序、实际堆/栈余量 | NOT_RUN | 未连接串口、未执行烧录或硬件动作 |

目标构建日志见本次会话 `artifact://114`；首次 size/runtime 检查见 `artifact://116`。最终 ELF 和 map 位于 `build-t14-review/`。

## 实现方行为验证证据

实现方报告临时 smoke 5/5 PASS：store 参数及提交错误、控制台 commit 失败、迟到 ACK、持续 RX 下命令和周期任务推进、BT 初始化失败后的控制台。报告给出实际命令、场景及耗时，见 `T14.md` 的临时 smoke 一节。该临时 harness 已删除；父窗口未独立复跑这 5 项，不把它们列为父窗口独立验证。

## G7b 根因与剩余裁决

`build-t14-review/rtk_main.map` 的 1296–1329 行给出链接依赖：

- IDF `nvs_encrypted_partition.cpp.obj` 引用 nothrow 数组分配，拉入 `libstdc++.a(new_opvnt.o)`。
- `new_opvnt.o` 引用 `__cxa_begin_catch`，拉入 `eh_catch.o`，最终 ELF 含 `__cxa_begin_catch` 和 `__cxa_end_catch`。
- 分配实现进一步拉入 `new_op.o`，其 `std::bad_alloc`、`std::exception` 类型信息及名称保留在最终 ELF，共 4 个 RTTI 符号。

配置证据：sdkconfig 的 `CONFIG_COMPILER_CXX_EXCEPTIONS`、`CONFIG_COMPILER_CXX_RTTI` 均未启用；冻结 sdkconfig.defaults 同样明确为 n。IDF NVS CMake 将 `src/nvs_encrypted_partition.cpp` 列入常规源文件；不能仅由文件名推断本项目启用了 NVS 加密。

SDK cxx 组件在禁用异常时提供部分 unwind/throw 的 abort 包装，但未消除上述 catch 与 RTTI 符号。当前任务要求使用 NVS，同时要求全 ELF 两类符号均为零；现有证据表明常规源码实现无法单独满足该门禁。需要架构/验收方裁决依赖来源的门禁范围，或提供获准的 SDK/工具链处理方案；本次未擅自放宽门禁。

## 后续框架适用性讨论

已只读检查 GNSS_Panel 的 DataBroker、DataProc、DeviceManager、ResourceManager 与架构文档。建议 RTK 后续借鉴领域状态单一所有者、类型化命令/状态和 HAL 边界。其 DataBroker 同步、单线程执行；跨任务事件必须先投递给所有者。DataNode 定时器与 provider 目前有动态分配，直接搬入会违反 RTK 规则。ResourceManager 当前用于字体/图片等静态查找，不负责 UART/DMA/蓝牙会话资源仲裁。本次没有进行框架迁移，也没有修改 GNSS_Panel。

## 补记（2026-11-11）：G7b 裁决

用户选择方案 1，G7b 改为“自有目标文件零符号 + 全 ELF 精确白名单 + 固定拉入链”，冻结文本与清单已按授权更新，详见 `G7B-DECISION.md`。对本报告的最终产物执行 `python3 -I tools/g7b_check/g7b_check.py --build build-t14-review`：PASS。按修订后的门禁，T14 不再因 G7b BLOCKED；实板 HIL、权威 astyle_py 仍为 NOT_RUN。
