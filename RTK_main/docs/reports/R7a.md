# R7a：组件用途命名与命名空间迁移

执行基线：`refactor/xtrack-style`，HEAD `ce62103`；结束时分支与 HEAD 不变。任务依据：`docs/tasks/R7a-component-rename.md`、ADR-0008 §9。未 commit、未烧录、未连接串口、未联网安装。

改名和路径迁移已完成；按审查要求将公共错误码归入 `app_fw` 的 `fw::Status` 后，重新执行自检 1–8，全部通过。自检 6 的三处既有日志/告警文本按用户审查结论接受为例外，保持原文并留待 R7b。

## 1. 改动范围

| 原组件目录 | 新组件目录 | C++ 命名空间 / include 根 |
| --- | --- | --- |
| `components/rtk_bt` | `components/spp_server` | `spp` / `spp/` |
| `components/rtk_proto` | `components/device_proto` | `proto` / `proto/` |
| `components/rtk_store` | `components/nvs_store` | C 接口，文件名及符号前缀保留 |
| `components/rtk_os` | `components/rtos` | `rtos` / `rtos/` |
| `components/rtk_core` | `components/link_core` | `core` / `core/` |
| `components/rtk_console` | `components/cli` | `cli` / `cli/` |

上述目录与 `test/rtk_console → test/cli` 均使用 `git mv`。`app_fw`、`stream_demux`、`bt_reconnect` 保留组件名，其 include 根改为 `fw/`、`demux/`、`reconnect/`；`main` 改用 `app` / `app::data`。所有 C++ 顶层 `rtk::` 已去掉，协议嵌套空间保留。同名冗余 namespace alias 删除。

公共基础错误码头使用 `git mv` 移到 `components/app_fw/include/fw/status.hpp`，实现移到 `components/app_fw/src/status.cpp`，类型与辅助函数为 `fw::Status`、`fw::is_ok`、`fw::to_string`。枚举值、返回值及字符串映射不变；现有 `fw::Result` 保留，辅助函数按参数类型重载。所有基础错误码用法显式限定为 `fw::Status`，头文件未引入 `using`。`proto::alarm::Status` 等协议状态结构保持。`device_proto`、`link_core`、`spp_server` 补齐直接 `REQUIRES app_fw`；`rtos`、`main` 已有该依赖；peer 的 `EXTRA_COMPONENT_DIRS` 补充 `app_fw`，主机 CMake 同步源码与 include 根。`rtk_bt_spp.h`、`rtk_proto_*.h`、`rtk_store.h` 及 C 标识符保持，留待 R7b。

同步文件：

- 全部受影响组件的 `CMakeLists.txt`、公共头和实现；`components/*/.clang-tidy` 随目录移动。
- `main/CMakeLists.txt`、`main/app_config.hpp`、`main/app_context.hpp`、`main/app_main.cpp`、`main/data/*`、`main/def/*`、`main/modules/*`、`main/support/*` 中的 namespace/include 引用。
- `test/host/*`、`test/app_fw/*`、`test/bt_reconnect/*`、`test/stream_demux/*`、`test/stream_demux_dev/h1/*`、`test/stream_demux_dev/h2/*`、`test/services/*`、`test/cli/*` 中的源码引用及 CMake；测试条件与预期值保持。
- `tools/main_host_check/CMakeLists.txt` 及 C++ 检查源码；`tools/t13_host_check/CMakeLists.txt`、`tools/t13_host_check/t13_host_check.c`、`tools/t13_host_check/shim/inc/freertos/FreeRTOS.h`；T13 同步导出编译数据库供 `.clangd` 使用。
- `test_apps/rtk_pure/CMakeLists.txt`、`test_apps/rtk_pure/main/CMakeLists.txt`、`test_apps/rtk_pure/main/test_main.cpp`；`test_apps/rtk_spp_peer/CMakeLists.txt`、`test_apps/rtk_spp_peer/main/CMakeLists.txt`、`test_apps/rtk_spp_peer/main/reconnect_bridge.{cpp,h}`。应用目录名不变。
- `.clangd`、根 `.clang-tidy`、`tools/tidy_check/tidy_check.py`、`tools/g7b_check/g7b_check.py`。检查过 `.clang-tidy-c` 和空的 `tools/g7b_check/heap_allowlist.txt`，无须修改。
- `AGENTS.md`、`docs/CODING-STYLE.md`、`docs/acceptance.md`、`docs/protocol/phone_protocol.md`、`main/modules/_template/README.md`、`docs/frozen.sha256`，以及本报告。

完整差异清单保存在本地 `builds/r7a-evidence/changed-files.tsv`，可重跑 `git diff --relative --name-status HEAD` 查看。所有构建与一次性核对脚本位于被忽略的 `builds/r7a*` 下。

### 只读 golden 参考的适配

`test/services/CMakeLists.txt` 原本直接编译 `reference/cpp-v3-dispatcher/command_dispatcher.cpp`，其旧头路径与 namespace 无法直接链接迁移后的实现。现在配置阶段读取原文件和参考头，在当前二进制目录 `golden/` 下生成仅替换 namespace/include 根与基础错误码限定名的副本，删除迁移后自指的 `namespace proto = proto;`，将两处 `const Status` 改为 `const fw::Status`。比较算法、遍历范围、预期值不变；原参考输入逐字节保持。配置依赖跟踪原始参考文件。

## 2. 自检 1：固件构建与大小对比 — PASS

所有 IDF shell 先执行：

```sh
. ~/esp/esp-idf/export.sh >/dev/null
# 改前执行；保留构建与 size 日志后才迁移源码
IDF_COMPONENT_MANAGER=0 idf.py -B builds/r7a-base build
IDF_COMPONENT_MANAGER=0 idf.py -B builds/r7a-base size
# 改后执行，格式化后再次复验
IDF_COMPONENT_MANAGER=0 idf.py -B builds/r7a build
IDF_COMPONENT_MANAGER=0 idf.py -B builds/r7a size
```

改前、改后及 `fw::Status` 迁移并格式化后的最终复验均退出 0，构建日志无 `warning:` / `error:`。基线日志：`builds/r7a-evidence/base-build.log`、`base-size.log`；最终日志：`fw-final-build.log`、`fw-final-size.log`。

| 项目（字节） | 改前 | 改后最终 | 差值 |
| --- | ---: | ---: | ---: |
| DRAM | 83797 | 83797 | 0 |
| IRAM | 82779 | 82779 | 0 |
| Flash Code | 415900 | 415900 | 0 |
| Flash Data | 118880 | 118880 | 0 |
| Total image size | 662260 | 662260 | 0 |
| `rtk_main.bin` 文件大小 | 662384 (`0xa1b70`) | 662384 (`0xa1b70`) | 0 |

段大小无差异；不将大小相等解释为二进制逐字节相同，C++ 符号与构建元数据允许随迁移变化。

首次后台启动曾因 shell 分组不当出现 `idf.py: command not found`，修正分组并重新 source 后完成上述基线。此启动失败发生在源码迁移之前。

## 3. 自检 2：SPP peer 构建 — PASS

```sh
# 使用绝对 -B，确保 -C 后仍写在主工程 builds/r7a-peer
IDF_COMPONENT_MANAGER=0 idf.py -C test_apps/rtk_spp_peer -B "$PWD/builds/r7a-peer" build
```

初次迁移及 `fw::Status` 复验的最终构建均退出 0，0 warning。日志：`builds/r7a-evidence/peer-build.log`、`fw-final-peer.log`。错误码迁入 `app_fw` 后，首次 peer 复验曾因组件发现列表缺少 `app_fw` 失败（`Failed to resolve component 'app_fw' required by component 'device_proto': unknown name.`）；补齐 `EXTRA_COMPONENT_DIRS` 后重新配置、构建通过。

## 4. 自检 3：全部主机测试 — PASS

普通配置、构建、运行命令模式如下，每行目录均实际执行：

```sh
cmake -S <源目录> -B <构建目录>
cmake --build <构建目录> -j 4
ctest --test-dir <构建目录> --output-on-failure
```

| 源目录 | 改后构建目录 | 改前 → 改后结果 |
| --- | --- | --- |
| `tools/main_host_check` | `builds/r7a-main` | 30/30 → 30/30 |
| `tools/t13_host_check` | `builds/r7a-t13` | 29/29 → 29/29 |
| `test/host` | `builds/r7a-host` | 7/7 → 7/7 |
| `test/app_fw` | `builds/r7a-fw` | 2/2 → 2/2 |
| `test/stream_demux` | `builds/r7a-demux` | 4/4 → 4/4 |
| `test/bt_reconnect` | `builds/r7a-reconnect` | 1/1 → 1/1 |
| `test/services` | `builds/r7a-services` | 2/2 → 2/2 |
| `test/cli`（改前 `test/rtk_console`） | `builds/r7a-cli` | 1/1 → 1/1 |

改前构建目录分别为 `builds/r7a-base-<同组后缀>`。格式化后全部再次通过。ASan/UBSan 实际命令：

```sh
cmake -S tools/main_host_check -B builds/r7a-main-asan \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build builds/r7a-main-asan -j 4
ctest --test-dir builds/r7a-main-asan --output-on-failure
```

ASan/UBSan 为 30/30，通过，无 sanitizer 报错。未跳过或删减测试。

`test/stream_demux_dev` 自带程序无 CTest 注册，实际单独运行：

```sh
g++ -std=c++20 -Wall -Wextra -Werror -I components/stream_demux/include \
  test/stream_demux_dev/h1/selftest.cpp components/stream_demux/src/demux.cpp \
  -o builds/r7a-evidence/h1-fw-final
builds/r7a-evidence/h1-fw-final
cmake -S test/stream_demux_dev/h2 -B builds/r7a-h2
cmake --build builds/r7a-h2 -j 4
builds/r7a-h2/h2_check
```

H1 改前/改后均输出 `H1 selftest PASS: API, replay, abort, reentry, invalid need, batching, 1000 streams x 3 chunk modes`；H2 均为 `H2 self-check: 5875 passed, 0 failed`。H1 未定义 `NDEBUG`。

日志：`builds/r7a-evidence/base-*.log`、`fw-final-{main,t13,host,fw,demux,h2,reconnect,services,cli,main-asan,h1}.log`；逐条命令及退出码由 `host_checks.py`、`fw-final-host-results.json` 记录。

## 5. 自检 4：G7b — PASS

```sh
python3 -I tools/g7b_check/g7b_check.py --build builds/r7a
```

实际结果：自有目标文件 50 个（含增量构建目录保留的旧 `device_proto/status.cpp.obj`，当前编译数据库使用 `app_fw/src/status.cpp`）；堆白名单 0 个源文件、放行 0 个堆符号；ELF 异常/RTTI 6/6，均在既有白名单内；`G7b PASS`。日志：`builds/r7a-evidence/fw-final-g7b.log`。

## 6. 自检 5：格式与空白 — PASS

用 `git diff --relative --name-only --diff-filter=ACMR HEAD` 筛选当前存在的 `.c/.h/.cpp/.hpp`，共 146 个，含仅移动的 C 文件/头；文件清单为 `builds/r7a-evidence/format-files.txt`。

```sh
xargs -d '\n' clang-format --style=file --dry-run --Werror \
  < builds/r7a-evidence/format-files.txt
git diff --check HEAD
```

首次 dry-run 检出 namespace 缩短后的换行/对齐及原 C 头宏对齐，按同一 `.clang-format` 格式化后重新检查全部通过；最终日志 `builds/r7a-evidence/fw-status-format.log` 为空。随后重跑固件、peer、全部主机测试。未下载或执行权威联网格式门禁。

## 7. 自检 6：残留搜索 — PASS（3 处已批准日志例外）

实际命令（同时包含隐藏配置）：

```sh
rg -n 'rtk::|namespace rtk|include/rtk/|"rtk/|components/rtk_|rtk_os|rtk_core|rtk_console|rtk_proto[^_.]|rtk_bt[^_]|rtk_store[^_.]' \
  --hidden --glob '!reference/**' --glob '!docs/adr/**' \
  --glob '!docs/reports/**' --glob '!docs/tasks/**'
```

完整输出：

```text
components/cli/src/console.cpp:19:constexpr const char *kTag = "rtk_console";
components/spp_server/CMakeLists.txt:10:    message(WARNING "rtk_bt: CONFIG_SOC_BT_CLASSIC_SUPPORTED=0，SPP 驱动与适配器未编译")
components/nvs_store/rtk_store.c:15:#define RTK_STORE_TAG     "rtk_store"
```

原始输出保存在 `builds/r7a-evidence/references-after.txt`；输出顺序可能因并行遍历而不同。三处均为迁移前既有的日志标签/构建告警文本。用户审查已明确接受这三处为日志例外，日志 tag 改动留给 R7b。未修改、拆分或通过新排除项隐藏这些命中；除三处已批准例外外，无旧 namespace/include/component 路径残留，本项记为 PASS。

## 8. 自检 7：命名空间清单 — PASS

改前、改后采用相同源码范围，排除历史输入与构建产物：

```sh
rg -o --no-filename '^namespace [a-z_:]+' components main test tools test_apps | sort | uniq -c
```

### 改前原始清单

```text
      3 namespace ac
      1 namespace alarm
      1 namespace app
      2 namespace demux_test
      1 namespace detail
      2 namespace fw
      5 namespace msg_type
      2 namespace mt
      3 namespace proto
      2 namespace rtk
     30 namespace rtk::app
      2 namespace rtk::app::data
      2 namespace rtk::bt
      3 namespace rtk::console
     11 namespace rtk::core
      8 namespace rtk::core::services
      4 namespace rtk::demux
      2 namespace rtk::demux::crc
      2 namespace rtk::demux::nmea
      2 namespace rtk::demux::rtcm
      2 namespace rtk::demux::ubx
      2 namespace rtk::demux::unicore_bin
      2 namespace rtk::demux::unicore_hash
      9 namespace rtk::fw
     13 namespace rtk::os
      4 namespace rtk::proto
      2 namespace rtk::proto::alarm
      2 namespace rtk::proto::demux_frame
      2 namespace rtk::reconnect
      1 namespace sender
      1 namespace store_flag
      1 namespace svc
      1 namespace test
```

### 改后原始清单

```text
      3 namespace ac
      1 namespace alarm
     30 namespace app
      2 namespace app::data
      3 namespace cli
     11 namespace core
      8 namespace core::services
      4 namespace demux
      2 namespace demux::crc
      2 namespace demux::nmea
      2 namespace demux::rtcm
      2 namespace demux_test
      2 namespace demux::ubx
      2 namespace demux::unicore_bin
      2 namespace demux::unicore_hash
      1 namespace detail
     11 namespace fw
      5 namespace msg_type
      2 namespace mt
      4 namespace proto
      2 namespace proto::alarm
      2 namespace proto::demux_frame
      2 namespace reconnect
     13 namespace rtos
      1 namespace sender
      2 namespace spp
      1 namespace store_flag
      1 namespace svc
      1 namespace test
```

命令也匹配 namespace alias：原来的 `app`、`fw`、`proto` 同名 alias 经迁移后删除，因此相应计数减少。`namespace alarm = proto::alarm;` 位于 `core::services` 的匿名命名空间内，不是顶层 namespace；未新增顶层 `alarm`、`link`、`sync`。公共错误码的两个裸 `namespace rtk` 包装改为 `namespace fw`，故改后 `fw` 定义计数为 11。

## 9. 自检 8：冻结清单 — PASS

以 HEAD 中原始 163 个条目逐项映射自有组件路径，保留 `reference/` 路径，然后重算：

```sh
sha256sum $(awk '{print $2}' docs/frozen.sha256) > builds/r7a-evidence/fw-frozen-next.sha256
mv builds/r7a-evidence/fw-frozen-next.sha256 docs/frozen.sha256
sha256sum -c docs/frozen.sha256
wc -l docs/frozen.sha256
```

改前 163 项全部 OK；改后仍为 163 项，全部 OK，无增删。公共错误码的原始头条目映射为 `components/app_fw/include/fw/status.hpp`。日志：`builds/r7a-evidence/frozen-before.log`、`fw-final-frozen.log`。第一次重算发现参考头路径被机械替换，已从 HEAD 原条目重建清单、只对自有组件路径迁移并重算；未修改参考文件。

## 10. 附加核对、偏离与未验证项

- PASS：`python3 builds/r7a-evidence/verify_invariants.py`。逐个与 HEAD 对比：170 个 C/C++ 文件的运行时字符串/字符字面量相同；43 个 C 文件/头的 C 标识符前缀序列相同；232 个既有 `reference/**`、`docs/adr/**`、`docs/tasks/**`、`docs/reports/**` 输入逐字节相同；3 个 Kconfig 文件逐字节相同。日志：`builds/r7a-evidence/fw-final-invariants.log`。字面量核对不含 `#include` 路径及注释。
- PASS：`git diff --relative --name-only HEAD -- reference docs/adr docs/tasks docs/reports` 在新报告创建前为空；历史报告未改，本报告为新增文件。
- PASS：针对旧级联诊断，`.clangd` 改为本次构建数据库并为两组主机工具选择各自 shim 数据库；T13 开启数据库导出后，直接执行 `clangd --check="$PWD/tools/t13_host_check/shim/fake_rtos.c" --log=info` 和 `clangd --check="$PWD/components/device_proto/rtk_proto_frame.c" --log=info` 均退出 0、`All checks completed, 0 errors`。
- FAIL（附加检查，不属于任务卡强制 clang-tidy 门禁）：直接 `clangd --check` 对 `tools/main_host_check/template_check.cpp`、`tools/main_host_check/executor_check.cpp` 分别报告 151、146 个 C++ 风格项，类型均为既有 CHECK 宏的 do/while、varargs、数组/指针写法和 magic numbers；没有缺失头或未知类型错误。未为改名任务重写测试或降低检查。内置 LSP 会话仍可返回早先的 `fake_rtos.c` 缺失头快照/超时，因此不宣称 LSP 会话缓存全净；直接 clangd 日志保存在 `builds/r7a-evidence/clangd-*.log`。
- PASS：公共错误码最终 `clangd --check` 对 `components/app_fw/src/status.cpp`、`components/device_proto/frame.cpp`、`components/spp_server/spp_transport.cpp` 均退出 0、`All checks completed, 0 errors`，日志为 `builds/r7a-evidence/clangd-fw-status-cpp.log`、`clangd-fw-frame.log`、`clangd-fw-spp_transport.log`。活动 LSP 对 5 个改动路径未报诊断，但均未确认完成（4 个超时、1 个 push-only），不能据此宣称全部干净。直接 clangd 对错误码头及 `command_router.cpp` 的非零退出仅来自 `DefineOutline` / `ExtractFunction` 重构 tweak 的工具自检失败，未见源代码编译诊断；日志为 `clangd-fw-status.log`、`clangd-fw-command_router.log`。
- NOT_RUN：完整 G5b clang-tidy 扫描、硬件/HIL、烧录与串口验证；本卡未要求完整 G5b，且硬件操作在禁止范围内。固件零告警构建、主机执行和静态核对不替代上板验证。
- 契约适配：自检 6 的三处日志例外依据用户审查结论记为 PASS，其日志文本保持至 R7b；公共错误码归属及直接依赖按审查要求更新为 `app_fw` / `fw::Status`。golden 参考通过构建目录机械副本适配，原始 oracle 与输入保持。无未解决阻塞。

R7a DONE

## 附：审查方补充（Claude）
- 删除 git 未跟踪的空目录残留后发现 `components/link_core/CMakeLists.txt` 与 `test/host/CMakeLists.txt` 仍引用不存在的 `private_include`（R3 已把 service_support 内部化，干净克隆会 CMake 失败）；已删除这两处引用并重算 frozen.sha256。
- 复验：idf build 0 warning，DRAM 83,797 / Flash Code 415,900 / image 662,260 与改前一致；peer 0 warning；host 7、app_fw 2、stream_demux 4、bt_reconnect 1、services 2、cli 1、main_host_check 30、t13 29 全通过；G7b PASS；frozen OK。
