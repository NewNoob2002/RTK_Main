# W0：Wi-Fi 资源探测（spike）

依据：ADR-0017 §3、§6。
基线：main（ADR-0017 提交之后）。分支：`spike/w0-wifi-budget`（worktree，探测代码**不合入** main，只合入报告与 ADR §3 数据）。
实现：codex；交叉评审：agy。Architect（Claude）负责 P2 台架实测、验收、提交、按需改 `sdkconfig.defaults` 并重算冻结清单。

## 1 目标

回答 ADR-0017 §3 的验收门槛：链入 Wi-Fi 之后，蓝牙模式和网页配置模式的内存是否够用。

1. 在 spike 分支临时打开 Wi-Fi、`esp_http_server`、`CONFIG_HTTPD_WS_SUPPORT`（spike 分支允许改 `sdkconfig.defaults`，
   改动列入报告；main 上的冻结文件不动）。
2. 临时 Kconfig `RTK_W0_RADIO`：`BT`（现状）/ `WIFI`（不初始化蓝牙，`esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)`，
   起 SoftAP + httpd，`GET /` 返回 20 KB 固定内容，`/ws` 回显）/ `BOTH`（只作对照）。
3. 每种组合记录：`idf.py size` 的 IRAM / DRAM / flash，镜像字节数；启动后 `heap_caps_get_free_size(MALLOC_CAP_8BIT)` 与
   `heap_caps_get_largest_free_block`；WIFI/BOTH 再记一个终端连上并打开页面、WS 每秒收发 1 KB 持续 60 s 后的同两项与最低水位。
   测量用控制台 `sys` 现有输出 + 临时日志，不加新的分配。
4. 若不达标，按顺序尝试并记录效果：`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` 降到 4、`DYNAMIC_RX/TX` 降到 8、关闭 AMPDU RX/TX、
   `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`/`WND` 降到 5744、httpd 栈 4096。
5. 用控制台 `gnss send` 在 CM680 上验证星座开关命令（协议 PDF `CM680系列高精度模组产品数据接口协议V1.2.8.pdf` 中的
   `mask`/`unmask`），记录应答原文。

## 2 测试

- 构建：三种 `RTK_W0_RADIO` 各一个 build 目录 `builds/w0-<mode>/`，0 warning。
- 现有门禁照跑 `tools/ci/run_gates.sh`，记录结果（spike 改动预期会让 G7b 堆白名单失败的，写清楚原因）。
- 台架（Architect）：P2 v1.41，手机/电脑连热点，按 §1.3 记录。

## 3 交付

- `docs/reports/W0.md`：§1.3 的表、§1.4 的调参效果、§1.5 的 CM680 应答、建议的 `sdkconfig.defaults` Wi-Fi 配置项清单。
- 不 commit；结束打印 `W0 DONE`。
