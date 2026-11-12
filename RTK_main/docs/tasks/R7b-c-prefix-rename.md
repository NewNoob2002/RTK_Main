# R7b C 标识符前缀、日志 tag、任务名（ADR-0008 §9）

执行方：pi + workbuddy/deepseek-v4.1-flash。前置：R7a 已完成（组件已是 `spp_server`、`device_proto`、`nvs_store` 等）。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0008-code-organization.md` §9、`docs/CODING-STYLE.md`、`docs/reports/R7a.md`。

## 目标（只改名，行为不变）
1. C 头文件改名（`git mv`）与前缀替换：
   - `components/spp_server`：`rtk_bt_spp.h → spp_server.h`，`private_include/rtk_bt_spp_priv.h → spp_server_priv.h`，`rtk_bt_spp_{init,session,rx,tx,gap}.c → spp_server_{...}.c`；标识符 `rtk_bt_spp_* → spp_server_*`，`RTK_BT_SPP_* → SPP_SERVER_*`（类型 `rtk_bt_spp_evt_t → spp_server_evt_t` 等）。
   - `components/device_proto`：`rtk_proto_frame.h → dev_proto_frame.h`、`rtk_proto_alarm.h → dev_proto_alarm.h`（及对应 .c）；`rtk_proto_* → dev_proto_*`，`RTK_PROTO_* → DEV_PROTO_*`。
   - `components/nvs_store`：`rtk_store.h/.c → nvs_store.h/.c`；`rtk_store_* → nvs_store_*`，`RTK_STORE_* → NVS_STORE_*`，**但 NVS 命名空间字符串值保持 `"rtk"`**（宏可改名为 `NVS_STORE_NS`，值不变），键 `"sn"`、`"alarm_wl"` 不变。
   - 例外：Kconfig 生成的 `CONFIG_RTK_*` 不改（例如 `CONFIG_RTK_BT_SPP_RX_BUF_SIZE` 仍被引用，只改引用它的本地宏名）。
2. 其他残留的 `rtk_`/`RTK_` 本地标识符（main、link_core、cli、test、tools 中的 C 风格宏/函数/静态变量）按所属组件或用途改名；保留 Kconfig、协议、NVS、蓝牙名相关的。逐个在报告中列出保留项与理由。
3. 日志 tag 与任务名（会改变串口日志前缀，用户已同意）：tag 取组件名 —— `"rtk_bt_spp"→"spp_server"`、`"rtk_store"→"nvs_store"`、`"rtk_console"→"cli"`、`"rtk"`（main/app_main.cpp）→`"app"`、`"rtk_app"`（bt_link_module.cpp）→`"bt_link"`；任务名 `rtk_bt_spp_worker→spp_worker`、`rtk_link→link`、`rtk_tx→async_tx`（≤ configMAX_TASK_NAME_LEN-1）。全量搜索 `tools/`、`test_apps/`、`tools/hil/` 中是否有解析这些字符串的脚本/测试并同步（例如 `tools/g7b_check/g7b_check.py`、`tools/t13_host_check`、`tools/main_host_check`）。
4. 同步测试、工具、`.clang-tidy*`、`tools/tidy_check`、`tools/g7b_check/heap_allowlist.txt`、`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/acceptance.md`、`_template/README.md` 中的文件名与标识符。`reference/**`、`docs/adr|reports|tasks` 历史文件不改。
5. clang-format 后可能因名字长度变化重排行，属正常。

## 冻结文件
冻结头改名后更新 `docs/frozen.sha256` 路径并重算：`sha256sum $(awk '{print $2}' docs/frozen.sha256) > docs/frozen.sha256`，`sha256sum -c` 全 OK。

## 自检（写入 `docs/reports/R7b.md`，逐项 PASS/FAIL，附命令）
1. `IDF_COMPONENT_MANAGER=0 idf.py -B builds/r7b build` 0 warning；与 `builds/r7a` 对比 `idf.py size`：DRAM/IRAM 一致，Flash/镜像只允许因字符串（tag、任务名、`__func__`/assert）变化的小差异，并说明。
2. `idf.py -C test_apps/rtk_spp_peer -B builds/r7b-peer build` 0 warning。
3. 主机测试全部通过且用例数不变：`tools/main_host_check`（普通 + ASan/UBSan，30/30）、`tools/t13_host_check`（29）、`test/host`、`test/app_fw`、`test/stream_demux`、`test/stream_demux_dev`、`test/bt_reconnect`、`test/services`、`test/cli`；构建目录 `builds/r7b-*`。
4. `python3 -I tools/g7b_check/g7b_check.py --build builds/r7b` PASS。
5. clang-format `--dry-run --Werror` 覆盖所有改动文件。
6. 残留搜索：`rg -n '\brtk_|\bRTK_' --glob '!reference/**' --glob '!docs/adr/**' --glob '!docs/reports/**' --glob '!docs/tasks/**' --glob '!builds/**'`，逐条归类为“已改/保留（Kconfig、NVS、test_apps 目录名等）”，无未解释项。
7. `strings builds/r7b/*.elf`（或 .bin）中确认存在 `"rtk"` NVS 命名空间字符串、`E1-Lite-` 名称前缀。
8. 冻结清单重算 + `sha256sum -c` 全 OK。

报告最后一行 `R7b DONE` 或 `R7b BLOCKED: <原因>`。

## 禁止
改变行为、协议、NVS 命名空间/键值、蓝牙名、Kconfig 选项名；修改 `reference/**`、历史报告/ADR/任务卡；烧录、连接串口、联网安装、git commit。只用 `builds/r7b*`。
