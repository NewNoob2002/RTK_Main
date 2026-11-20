# W2 交叉评审

W2 在 Wi-Fi 配置模式启动 SoftAP 和 WebSocket，把网页修改交给 GNSS 或 storage，并通过 W1 请求重启回蓝牙。审查以当前 `main`（`ca2ae8c5`）为比较基线，覆盖 `feat/w2-web-server`（HEAD `a9e6dc6b`）的工作树差异及未跟踪的 Web 实现和测试。ESP-IDF 生命周期判断依据本机 v6.1 源码。

**结论：修复 P1 后再合入。**

## P1

1. **停止 HTTP 服务时释放静态模块对象**（`main/modules/web/WebServer.cpp:438`、`:477`，ESP-IDF 生命周期）。服务启动后若 URI 注册失败，或启动流程随后进入降级回滚，`httpd_stop()` 会释放 `global_user_ctx`。ESP-IDF v6.1 的 `components/esp_http_server/src/httpd_main.c:612` 在释放回调为空时调用 `free()`，这里传入的是静态 WebModule 地址，回滚可触发堆断言。为借用的上下文注册具名空释放回调，并在停止失败时保留仍在使用的句柄及资源，防止回调访问已解绑的状态。

2. **套接字描述符复用会串会话**（`main/modules/web/WebServer.cpp:259`、`:302`、`:318`，`main/modules/web/WebModule.cpp:489`、`:561`，协议 §3）。连接事件、RX、TX 和 GNSS 在途响应只记录 fd。旧连接在待处理帧或 GNSS 结果仍在途时关闭，新连接复用 fd 后，旧工作可被归到新会话。控制队列满时还会静默丢弃关闭或连接事件，旧解锁状态可能残留。给每次升级绑定会话代号，所有跨线程事件和在途响应都携带并核对 fd 与会话代号。控制队列满时拒绝升级或使用可对账的会话表，并保证失效事件无法丢失。

3. **GNSS 应用结果可能归到另一条命令**（`main/modules/web/WebModule.cpp:464`、`:535`，`main/modules/gnss/GnssModuleCmds.cpp:179`、`:203`，任务卡 §1.6）。kLink 投递前读取全局 `applySeq`。若控制台在此之前已有一条应用在途，且它在 kIo 消费网页命令前完成，网页命令可被正常接受，但 kLink 将前一条命令推进的序号当作本次完成。浏览器可能收到成功和 config，而本次应用随后失败或回滚。让 kIo 在受理时为请求绑定应用结果，持久化或失败时按对应的 web 请求 seq 回传。新增与控制台交错的测试。

4. **GGA 门控已确认成功，退出仍可能丢配置**（`main/modules/gnss/GnssModuleCmds.cpp:337`、`:345`，`main/modules/web/WebModule.cpp:351`、`:531`，任务卡 §1.6 与 §3）。网页在 RAM 更新后立即收到成功。若首次 NVS 写入失败或写锁返回忙，500 ms 去抖后的首次尝试至少再等 1 s 才重试，退出路径却最多等 1 s 就重启。禁写模式还会直接清除 pending，网页仍已收到成功。让 kIo 回报与命令 seq 对应的持久化结果，并使显式退出在保存失败时返回错误，把设备留在配置模式供用户重试。

## P2

5. **发送背压会丢 ACK、config 和 bye**（`main/modules/web/WebModule.cpp:299`、`:308`、`:323`，`main/modules/web/WebServer.cpp:315`、`:348`，协议 §2）。四槽发送队列可被多个连接的初始同步和状态填满。此时配置已经修改，ACK 却被静默丢弃，浏览器超时并重连。若唯一一次 `httpd_queue_work()` 提交失败，已排入的帧还得等下一次成功提交才有机会发送。重启时队列为空也可能仅表示回调已取出最后一帧，套接字发送仍在进行。为必达响应预留容量或保留待发状态，周期性重试调度，并用 HTTP 线程回写的发送完成状态控制有界重启等待。

6. **超时清除在途状态后，旧 GNSS 请求仍可执行**（`main/modules/web/WebModule.cpp:478`、`:511`，`main/modules/gnss/GnssModule.cpp:432`，任务卡 §1.5）。kIo 延后消费跨过两秒后，浏览器可先收到 timeout，随后 kIo 又执行旧请求。若新请求先到，同一个 Slot 也可覆盖未消费的命令。受理阶段需定义领取与取消，超时后保留背压直到 kIo 确认消费或取消。对于已经开始的操作，按同一 seq 回报最终结果。

7. **配对请求清除失败时会跨重启重复开窗**（`main/modules/storage/StorageModule.cpp:461`、`main/modules/bt_link/BtLinkModule.cpp:501`，任务卡 §1.7）。若清零 NVS 失败，当前启动仍开窗且 RAM 显示已消费，下一次蓝牙启动从旧记录再次开窗。先确认清零持久化，再清 RAM 标志并开窗。为清除失败加主机测试，验证标志留待重试且本次不开窗。

8. **重连可落在空闲回落决定之后**（`main/modules/web/WebServer.cpp:218`，`main/modules/web/WebModule.cpp:343`、`:642`，任务卡 §2）。终端在空闲阈值附近重新关联时，kLink 可先读到旧的零计数，后续回落等待也不会再检查关联事件。设备可在重连后重启。用一致快照发布站点状态，并在提交 idle 重启前重新核对当前连接和活动序号。主机测试增加已排定 idle 回落后重新关联的情况。

9. **初始同步与接收处理缺少顺序约束**（`main/modules/web/WebModule.cpp:255`、`:561`、`:638`，协议 §2）。客户端在升级后立即发请求时，同一轮会在 hello 后处理请求，ACK 可能先于初始 config。非最终文本帧也被当成完整请求处理，后续续帧被忽略（`main/modules/web/WebServer.cpp:283`）。在初始 config 排入后再处理会话请求，并明确拒绝协议 v1 不支持的分片数据消息。

## P3

10. **实时路径日志及命名不符合项目规则**（`main/modules/web/WebModule.cpp:144`、`:153`、`:645`，`main/modules/web/WebServer.cpp:332`、`:351`，`main/modules/web/WebModule.hpp:407`，AGENTS.md §2）。kLink 的 poll 调用链格式化连接、空闲和调度失败日志，发送失败又直接丢弃会话关闭返回值。模块新成员使用 snake_case。将实时事件改为 trace，检查 IDF 关闭结果，新 C++ 成员使用 lowerCamelCase 加尾下划线。

## 核对与验证

- RX/TX 环形槽的生产者 release 发布配对消费者 acquire，反向回收同样配对。固定帧和 scratch 分区覆盖正常读写路径。JSON 复用 `web_proto`，普通文本在拷贝前检查 1024 B，超长路径发送 1009。会话身份、分片及背压问题见上文。
- SoftAP 配置 IP 和网关均为 192.168.10.12，掩码为 /24。先停止 DHCP、设置地址，再请求启动。IDF v6.1 在 netif 未 up 时将 DHCP 恢复为 INIT，AP 启动时按保存的地址启动服务。实际 DHCP 分配留待 HIL。
- S2 白名单复用 `nvs_store_set_alarms` 与 BT 启动读取的记录，pair_next 为 `rtk/pair_next` U8。正常一次性消费测试通过。两项 GGA 修改走 `kSetGgaGate`，config 读取 `ggaGateStatus`。CM680 星座返回 unsupported 符合任务卡的条件分支。
- **PASS** 定向普通主机构建和测试。以 `cmake -S tools/main_host_check -B builds/w2cr-host -G Ninja` 配置并构建，`ctest --test-dir builds/w2cr-host --output-on-failure -R 'web_server_check|web_s2_storage_bt|radio_mode_check'` 通过 4/4。
- **PASS** 使用同样测试筛选在 `builds/w2cr-asan` 验证 ASan/UBSan，4/4。**BLOCKED** 默认运行的 LeakSanitizer 因环境限制失败，重跑使用 `ASAN_OPTIONS=detect_leaks=0`，仅恢复地址与未定义行为检查。
- **PASS** `git diff --relative --check main --`、对差异中的 C++ 文件及未跟踪 Web 源和测试执行 `clang-format --style=file --dry-run --Werror`、`sha256sum -c docs/frozen.sha256`。
- **PASS** 使用既有产物执行 `python3 -I tools/g7b_check/g7b_check.py --build builds/w2-idf`，128 个目标文件。Web 帧通过现有 BootArena 在 Wi-Fi 启动期获得，审查未发现新的运行期堆调用。
- **PASS** 使用 Xtensa nm 检查既有 `builds/w2-wifi-off/rtk_main.elf`，`esp_wifi_init`、`httpd_start`、`_binary_index_html_gz_start` 均无命中。源码同时按开关排除 Web 模块及页面。
- **BLOCKED** `python3 tools/web_mock/web_mock.py --selftest` 在创建回环套接字时收到 EPERM。
- **NOT_RUN** 全量门禁、新 ESP-IDF 构建、P2 实机和浏览器端到端验证。主机测试覆盖邮箱、状态编码、空闲计时组件与 S2 正常路径。上述设备侧交错和 SDK 回滚发现来自源码审查。
