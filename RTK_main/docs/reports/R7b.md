# R7b：C 文件名、符号前缀与日志 tag 迁移

任务卡：`docs/tasks/R7b-c-prefix-rename.md`。基线：`builds/r7b-base`（HEAD `2008da3`）。
结论：8 项自检全部 PASS；无烧录、无串口、无 git 提交。证据与一次性脚本在 `builds/r7b-evidence/`。

## 1. 改动范围

`git diff --cached --shortstat HEAD`：66 个文件、1591 插入 / 1591 删除（纯改名，行数对称）。
13 个文件用 `git mv` 重命名，其余为内容同步。

### 1.1 文件重命名（C 文件名取组件前缀）

| 旧 | 新 |
| --- | --- |
| `components/spp_server/include/rtk_bt_spp.h` | `components/spp_server/include/spp_server.h` |
| `components/spp_server/private_include/rtk_bt_spp_priv.h` | `components/spp_server/private_include/spp_server_priv.h` |
| `components/spp_server/rtk_bt_spp_{init,session,rx,tx,gap}.c` | `components/spp_server/spp_server_{init,session,rx,tx,gap}.c` |
| `components/device_proto/{include/}rtk_proto_{frame,alarm}.h` / `.c` | `components/device_proto/{include/}dev_proto_{frame,alarm}.h` / `.c` |
| `components/nvs_store/include/rtk_store.h`、`components/nvs_store/rtk_store.c` | `components/nvs_store/include/nvs_store.h`、`components/nvs_store/nvs_store.c` |

### 1.2 符号与宏前缀

| 组件 | 公共符号 / 宏前缀 | 备注 |
| --- | --- | --- |
| `spp_server` | `rtk_bt_spp_*` → `spp_server_*`；`RTK_BT_SPP_*` → `SPP_SERVER_*` | `SPP_SERVER_EVT_*`、`spp_server_evt_type_t` 等枚举值名不变 |
| `device_proto` | `rtk_proto_*` → `dev_proto_*`；`RTK_PROTO_*` → `DEV_PROTO_*` | 协议字节、错误码数值与字符串映射不变 |
| `nvs_store` | `rtk_store_*` → `nvs_store_*`；`RTK_STORE_*` → `NVS_STORE_*` | NVS 命名空间 `"rtk"`、键 `"sn"`/`"alarm_wl"` 不变 |

### 1.3 日志 tag 与任务名

| 位置 | 旧 | 新 |
| --- | --- | --- |
| `main/app_main.cpp` | `kTag = "rtk"` | `kTag = "app"` |
| `main/modules/bt_link/bt_link_module.cpp` | `"rtk_app"` | `"bt_link"` |
| `main/modules/bt_link/async_tx.cpp` | 任务名 `"rtk_tx"` | `"async_tx"` |
| `main/app_main.cpp` 执行器任务 | `"rtk_link"` | `"link"` |
| `components/spp_server/spp_server_priv.h` | `"rtk_bt_spp"` | `"spp_server"` |
| `components/spp_server/spp_server_init.c` | 任务名 `"rtk_bt_spp_worker"` | `"spp_worker"` |
| `components/nvs_store/nvs_store.c` | `"rtk_store"`（R7a 残留） | `"nvs_store"` |
| `components/cli/src/console.cpp` | `"rtk_console"`（R7a 残留） | `"cli"` |
| `components/rtos/executor.cpp` | `"executor"` | 不变（已无产品前缀） |

新任务名最长 10 字节（`spp_worker`），低于 `configMAX_TASK_NAME_LEN - 1`。所有 tag 在固件内唯一。

### 1.4 按要求保留

- Kconfig 选项与菜单：`RTK_CONSOLE_*`、`RTK_BT_SPP_RX_BUF_SIZE`、`RTK_LINK_POLICY*`（`CONFIG_RTK_*` 改名会让现有 `sdkconfig` 失效），仅改引用它们的本地宏名（`SPP_SERVER_RX_BUF_SIZE`）。
- 产品 / 工程名：`project(rtk_main)`、`rtk_main.bin/.elf/.map`、启动日志文本 `RTK_main`。
- 运行时契约：NVS 命名空间 `"rtk"`、NVS 键、蓝牙名 `E1-Lite-<SN>` / `RTK-HIL-Peer` / `RTK_T13`、CLI 提示符 `rtk> `（`tools/hil/spp_rtcm_mix.py` 依赖）、协议字节。
- `test_apps/rtk_pure`、`test_apps/rtk_spp_peer` 目录名及其 `project()` 名、`[rtk_pure]` unity tag、`tools/tidy_check` 的默认 `builds/rtk_pure-esp32`。
- `reference/**`、`docs/adr/**`、`docs/tasks/**`、`docs/reports/**` 未改（仅新增本报告）。

### 1.5 同步文件

- `components/{spp_server,device_proto,nvs_store,cli,bt_reconnect}/**`：头文件、实现、`CMakeLists.txt`、`Kconfig`。
- `main/`：`app_main.cpp`、`def/{link_def,system_def}.hpp`、`modules/bt_link/*`、`modules/storage/*`、`modules/system/*`。
- `test/host/*`、`test/cli/CMakeLists.txt`、`test/services/CMakeLists.txt`、`test/stream_demux/*`、`test/stream_demux_dev/h2/*`、`test/app_fw/CMakeLists.txt`、`test/bt_reconnect/CMakeLists.txt`。
- `tools/main_host_check/*`、`tools/t13_host_check/*`、`tools/g7b_check/g7b_check.py`。
- `test_apps/rtk_pure/main/test_proto_c.c`、`test_apps/rtk_spp_peer/main/peer.c`。
- `AGENTS.md`（§组件命名尾句改为现行 C 前缀规则）、`docs/CODING-STYLE.md`（§3.2、§8.1）、`docs/frozen.sha256`。

## 2. 自检 1：固件构建与大小对比 — PASS

```sh
. ~/esp/esp-idf/export.sh >/dev/null
IDF_COMPONENT_MANAGER=0 idf.py -B builds/r7b-base build  # 改前（HEAD）
IDF_COMPONENT_MANAGER=0 idf.py -B builds/r7b build       # 改后
idf.py -B builds/r7b size
```

| 指标 | 基线 `builds/r7b-base` | 改后 `builds/r7b` | 差 |
| --- | --- | --- | --- |
| warnings / errors | 0 / 0 | 0 / 0 | — |
| Flash Code | 415900 | 415900 | 0 |
| Flash Data | 118880 | 118848 | −32 |
| DRAM | 83797 | 83797 | 0 |
| IRAM | 82779 | 82779 | 0 |
| Total image | 662260 | 662228 | −32 |
| `rtk_main.bin` | 0xa1b70 | 0xa1b50 | −32 |

`.flash.rodata` 由 `0x1cf50` 变为 `0x1cf30`（−0x20），与 Flash Data −32 完全一致：差异只来自标签 / 任务名字符串变短（`rtk_console`→`cli` −8、`rtk_bt_spp_worker`→`spp_worker` −6、`rtk_link`→`link` −4、`rtk_tx`→`async_tx` +2，其余等长），不含代码或栈变化。日志：`builds/r7b-evidence/{build3,size3}.log`。

## 3. 自检 2：SPP peer 构建 — PASS

```sh
IDF_COMPONENT_MANAGER=0 idf.py -C test_apps/rtk_spp_peer -B "$PWD/builds/r7b-peer" build
```

`PEER_EXIT=0`，0 warning / 0 error（`builds/r7b-evidence/peer-build.log`）。

## 4. 自检 3：全部主机测试 — PASS

`builds/r7b-evidence/host_checks.sh`（cmake 配置 + 构建 + `ctest --output-on-failure`），每个工程独立 `builds/r7b-*` 目录：

| 工程 | 用例 | 结果 |
| --- | --- | --- |
| `tools/main_host_check` | 30 | PASS |
| `tools/main_host_check`（ASan/UBSan） | 30 | PASS |
| `tools/t13_host_check` | 29 | PASS |
| `test/host` | 7 | PASS |
| `test/app_fw` | 2 | PASS |
| `test/stream_demux` | 4 | PASS |
| `test/bt_reconnect` | 1 | PASS |
| `test/services` | 2 | PASS |
| `test/cli` | 1 | PASS |

`test/stream_demux_dev/h1/selftest.cpp`：`H1 selftest PASS: API, replay, abort, reentry, invalid need, batching, 1000 streams x 3 chunk modes`。
`test/stream_demux_dev/h2`：`H2 self-check: 5875 passed, 0 failed`。

用例数量与 R7a 报告一致，未删除或跳过任何用例。

## 5. 自检 4：G7b — PASS

```sh
python3 -I tools/g7b_check/g7b_check.py --build builds/r7b
```

自有目标文件 49 个；堆白名单 0 个源文件；ELF 异常 / RTTI 符号 6/6（均为工具链 `__cxa_begin_catch` 等既有白名单项）；`G7b PASS`（`builds/r7b-evidence/g7b.log`）。

## 6. 自检 5：格式与空白 — PASS

```sh
clang-format --style=file -i  <46 个改动的 .c/.h/.cpp/.hpp>
clang-format --style=file --dry-run --Werror <同一列表>
```

`FORMAT_OK`，无残留空白行尾（受影响文件均整体重写并重新格式化）。文件列表：`builds/r7b-evidence/format-files.txt`。`Kconfig` 属非 C/C++ 输入，未纳入 clang-format 门禁。

## 7. 自检 6：残留搜索 — PASS（58 处，全部为保留类别）

```sh
rg -n '\brtk_|\bRTK_' --glob '!reference/**' --glob '!docs/adr/**' \
   --glob '!docs/reports/**' --glob '!docs/tasks/**' --glob '!builds/**'
```

原始输出：`builds/r7b-evidence/residual.txt`。分类：

| 类别 | 处数 | 说明 |
| --- | --- | --- |
| Kconfig 选项 / 菜单 | 12 | `components/cli/Kconfig`（6）、`main/Kconfig.projbuild`（5）、`components/spp_server/Kconfig`（`RTK_BT_SPP_RX_BUF_SIZE`） |
| 产品 / 工程名 | 9 | `project(rtk_main)`、`sdkconfig.defaults`、`CODING-STYLE.md` 标题、`AGENTS.md` 两处、`RTK_main` 启动日志、`g7b_check.py` 的 `rtk_main.elf/.map`、`_template/README.md`、`frozen.sha256` 中 `reference/c-v1/main/rtk_main.c` |
| `test_apps` 目录名与 tag | 23 | `rtk_pure` / `rtk_spp_peer` 的 `project()`、unity tag、`tidy_check.py`、`acceptance.md`、`reconnect_bridge.*` 注释 |
| 蓝牙名 `RTK_T13` | 6 | `tools/t13_host_check/t13_host_check.c` |
| 文档描述保留项 | 3 | `AGENTS.md:26`、`CODING-STYLE.md:130`、`docs/acceptance.md` |

`components/spp_server/include/spp_server.h` 与 `components/spp_server/Kconfig` 仍引用 `CONFIG_RTK_BT_SPP_RX_BUF_SIZE`（任务卡明确要求不改 Kconfig 生成名）。

## 8. 自检 7：运行时字符串与 Kconfig — PASS

改前后固件 ELF（`strings -n 3` + `grep -x`）：

| 字符串 | 基线 | 改后 |
| --- | --- | --- |
| `rtk_console` | 1 | 0 |
| `rtk_store` | 1 | 0 |
| `rtk_bt_spp` | 1 | 0 |
| `rtk_bt_spp_worker` | 3 | 0 |
| `rtk_app` | 1 | 0 |
| `rtk_link` | 1 | 0 |
| `rtk_tx` | 1 | 0 |
| `spp_server` / `spp_worker` / `nvs_store` / `async_tx` | 0 | 各 1 |
| `rtk`（NVS 命名空间） | 1 | 1 |
| `E1-Lite-`（设备名前缀） | 存在 | 存在 |
| `rtk> `（CLI 提示符） | 存在 | 存在 |

`builds/r7b/config/sdkconfig.h` 仍有 `CONFIG_RTK_BT_SPP_RX_BUF_SIZE 4096`，`SPP_SERVER_RX_BUF_SIZE` 取 4096，2 会话缓冲与基线逐字节同尺寸（DRAM 不变可交叉验证）。

## 9. 自检 8：冻结清单 — PASS

`docs/frozen.sha256` 中 4 个改名头路径已更新（`spp_server.h`、`dev_proto_alarm.h`、`dev_proto_frame.h`、`nvs_store.h`），随后 `sha256sum $(awk '{print $2}' docs/frozen.sha256)` 全量重算，`sha256sum -c` 退出码 0、163/163 `OK`（`builds/r7b-evidence/frozen-check.log`）。被改内容的冻结文件仅 4 个改名头 + `AGENTS.md` + `docs/CODING-STYLE.md`，均为本卡要求。

## 10. 偏离、修正过程与未验证项

- **修正 1（Kconfig 符号被机械规则误改）**：首轮替换把 `components/spp_server/Kconfig` 的 `config RTK_BT_SPP_RX_BUF_SIZE` 改成 `config SPP_SERVER_RX_BUF_SIZE`，违反任务卡“Kconfig 生成的 `CONFIG_RTK_*` 不改”。已把 Kconfig 符号名与 `spp_server.h` 的 `CONFIG_*` 引用改回 `RTK_BT_SPP_RX_BUF_SIZE`，只保留本地宏 `SPP_SERVER_RX_BUF_SIZE`。中间态构建曾出现 DRAM 79701（缓冲回退 2048）；修正后 DRAM 恢复 83797，与基线一致。
- **修正 2**：`test/bt_reconnect/CMakeLists.txt` 的 `project(rtk_bt_reconnect_tests)` 被规则误改为 `spp_server_reconnect_tests`，已改为 `bt_reconnect_tests`；重新配置并跑通（1/1）。
- **修正 3**：`tools/t13_host_check/t13_host_check.c` 首行注释原写 `…/rtk_bt_spp.c`（一直与实际 5 个源文件不符），随改名改为 `…/spp_server_*.c`。
- **说明（Flash Data −32）**：仅来自 tag / 任务名字符串变短，`.flash.rodata` 同步 −0x20；代码、DRAM、IRAM 完全不变。
- **说明（LSP 假阳性）**：内置 LSP 对 `components/bt_reconnect/src/reconnect.cpp` 报 19 条“`reconnect/reconnect.hpp` file not found”级联，对 `tools/t13_host_check/t13_host_check.c` 报 20 条 `atomic_bool` 未知。二者均为探测时缺少编译数据库 / clangd 注入 GCC-16 C++ 头导致的假阳性，非本次改名引入：
  - `clangd --check=components/bt_reconnect/src/reconnect.cpp --compile-commands-dir=builds/r7b-reconnect` → `All checks completed, 0 errors`（`builds/r7b-evidence/clangd-reconnect.log`）。
  - `clang -std=gnu11 -Wall -Wextra -fsyntax-only -I tools/t13_host_check/shim -I tools/t13_host_check/shim/inc -I components/spp_server/include tools/t13_host_check/t13_host_check.c` 退出 0；`g++ -I components/bt_reconnect/include -fsyntax-only` 亦 0。
- **NOT_RUN**：完整 G5b `tools/tidy_check` 扫描（本卡未要求）、权威 astyle 门禁（需联网，实现方禁止）、烧录 / 串口 / HIL 验证（禁止范围）。改名的运行时效果（tag 与任务名）只通过 ELF 字符串与构建等价性核对，未上板验证。

R7b DONE
