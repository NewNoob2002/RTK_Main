# SP1 SPP init 失败路径数据竞争修复

- 分支 / 工作树：`feat/sp1-spp-race`，`../RTK_Main-sp1`，基于 main `c349747`
- 报告：`docs/reports/SP1-spp-init-fail-race.md`；完成后最后一行单独输出 `SP1-FINISHED`
- 门禁：`export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env && tools/ci/run_gates.sh --prefix sp1 --base c349747`
- 来源：MT1 报告 TSan 记录（docs/reports/MT1-rate-save-t13-sync.md:44），既有问题。

## 问题
`components/spp_server/SppServerInit.c` 的 `spp_server_objects_delete()`（init 失败/停止路径）在 `s_spp_mux` 之外复位 `s_spp.sessions[]` 与 `s_spp.ctrl[]`；而保留存活的 `spp_worker` 任务在 `SppServerWorker.c:126`（`spp_server_disconnect_process`）持锁读取同一字段。TSan 在 `t13_init_start_timeout` 报 `SppServerInit.c:41` ↔ `SppServerWorker.c:126` 数据竞争。

## 要求
1. 会话/控制表的状态字段复位放进 `s_spp_mux` 临界区；`rx` StreamBuffer 句柄在临界区内摘下（置 NULL），`vStreamBufferDelete` 在临界区外执行（临界区内不得调用 RTOS 删除/阻塞 API）。
2. 确认 worker 在复位后读取不会使用已删除的 `rx`（持锁检查 `in_use`/`rx` 非空后才用）；如现有 worker 路径有此缺口一并修复，范围仅限此竞争。
3. 不改公共头/冻结文件、不改 SPP 行为与时序；不新增堆分配，DRAM 增量 0。
4. 验证：`-fsanitize=thread -g -O1` 构建 `tools/t13_host_check`，全部用例（含 `t13_init_start_timeout`）TSan 0 报告，报告附命令与输出摘要，并记录基线 c349747 上同一命令失败；普通 / ASan 构建 t13 全过；门禁全过。

## 不做
不重构 SPP 服务器结构、不改 worker 生命周期（仍保留给下次 init 复用）。不访问 /dev/ttyACM0、不烧录（板子在做其它 HIL）。不 push 不合并，不改 `docs/frozen.sha256`。
