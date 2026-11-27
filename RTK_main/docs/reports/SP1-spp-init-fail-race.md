# SP1 SPP init 失败路径数据竞争修复报告

## 改动文件
- `components/spp_server/SppServerInit.c`：`spp_server_objects_delete()` 把 `sessions[]`/`ctrl[]` 状态复位并入 `s_spp_mux` 临界区（原来只有 `inflight`/`stream_owner`/`tx_stats.inflight` 在锁内）；`rx` 句柄在锁内摘下（局部数组 `rx_old[]`，字段置 NULL），`vStreamBufferDelete` 移到锁外。更新函数 `@note`。

## 根因与 worker 路径核查
- 竞争对：`SppServerInit.c:41`（锁外写 `sessions[idx].in_use`）↔ `SppServerWorker.c:126`（`spp_server_disconnect_process` 持锁读 `in_use`）。
- worker 全部访问 `sessions[]`/`ctrl[]` 均在 `s_spp_mux` 内；`rx` 只被 `SppServerRx.c`（BT 回调/读路径，init 失败时 SPP 已关闭、无会话）使用，worker 不读 `rx`，无已删除 `rx` 被使用的缺口，故无需额外修改 worker。
- 无公共头/冻结文件改动，不新增堆/静态对象（仅栈上 `StreamBufferHandle_t[2]`），DRAM 增量 0；SPP 行为与时序不变（复位语句与顺序未变）。

## 验证
| 项 | 结果 |
| --- | --- |
| 基线 c349747，`cmake -S tools/t13_host_check -B <dir> -G Ninja -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1" -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread` + ctest | FAIL：45/46，`t13_init_start_timeout` 失败；TSan `data race`，`spp_server_objects_delete SppServerInit.c:41` ↔ `spp_server_disconnect_process SppServerWorker.c:126` |
| 修复后同一命令（全部 46 用例） | PASS：46/46，0 个 `WARNING: ThreadSanitizer`（含 `t13_init_start_timeout`） |
| t13 普通构建 | PASS 46/46 |
| t13 ASan/UBSan 构建 | PASS 46/46 |
| `tools/ci/run_gates.sh --prefix sp1 --base c349747` | PASS：43 passed, 0 failed |
| `sha256sum -c docs/frozen.sha256` | 无冻结文件改动 |
| 硬件/烧录/串口 | NOT_RUN（任务禁止） |

## 与契约的偏离
无。

## 未解决问题
无。
