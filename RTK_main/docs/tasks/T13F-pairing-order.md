# T13F：配对事件按到达顺序处理

## 背景
CI run 45311129450 的 `G2b-t13_host_check-asan` 中 `t13_gap_pairing_window` 失败（`tools/t13_host_check/T13HostCheck.c:825-828`）：可信手机的 CFM 先到、他机 PIN 后到，但 PIN 先被处理并占用配对窗口。
根因：`spp_server_gap_cb` 经 `spp_server_ctrl_add`（`components/spp_server/SppServerSession.c:239`）把事件放进固定小表的“第一个空槽”，worker 按槽下标顺序处理，而不是到达顺序。前面的槽刚释放时，后到事件会插到前面先被处理。真机上两台设备几乎同时请求配对时，可能让错误的一方拿到配对窗口。

## 要求
1. 每个待处理条目加单调递增序号（注意 uint32 回绕比较），worker 总是先处理序号最小（最早到达）的条目。改动限于 `components/spp_server`。
2. 新增确定性回归测试：人为制造“前面槽已释放，后到事件进入更靠前的槽”，断言按到达顺序处理。修复前必须失败、修复后通过（报告中写明两次结果）。
3. 检查 `t13_disconnect_reuse_across_slots` 偶发失败是否同一根因（同样涉及槽复用）；如是一并覆盖，否则在报告中写明证据与结论。
4. 删除 `docs/PROGRESS.md` 中关于 t13 偶发超时的记录（若根因已覆盖）。
5. 门禁：`ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix t13f --base <main HEAD>`，并把 `t13_host_check-asan` 单独连跑 20 次，结果写入报告。

## 约束
遵守 AGENTS.md 与 docs/CODING-STYLE.md；不动冻结文件（`sha256sum -c docs/frozen.sha256`）；不提交、不烧录、不碰串口。报告写到 `docs/reports/T13F-pairing-order.md`，含改动、测试、门禁、体积差、未验证项。完成后输出 "T13F DONE"。
