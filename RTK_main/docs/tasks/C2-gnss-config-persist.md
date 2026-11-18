# C2：接收机配置持久化、SAVECONFIG、gnss freset、统一 gnss out（ADR-0014 §2.4–2.5）

状态：ISSUED 2026-11-18，基线 `feat/gnss` HEAD（本卡所在提交；C1 `e400e74`，分区放大 `422a43a`，hint 修复 `df668f2`）。
实施：codex + gpt-6.2-sol；审查/验收/提交/烧录：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0014-runtime-config.md` §2.4–2.5、ADR-0010 §2.4/§2.10、
ADR-0012（Receiver/Controller 契约，尤其 §2.4 输出计划、§2.7 读回、§2.8 型号子类）、`docs/acceptance.md`、
`main/modules/gnss/**`、`main/data/GnssOutput.hpp`、`components/gnss/**`。

## 1 要做的

### 1.1 接收机一次性命令：保存与出厂复位（gnss 组件）

- `gnss::Receiver` 新增两个虚函数（缺省实现返回“不支持”），按 ADR-0012 现有 step/应答模式实现：
  - `saveStep(...)`：Unicore `SAVECONFIG`；u-blox 用 CFG-VALSET 把当前 RAM 层同时写 BBR+Flash 层（或 CFG-CFG save，选一种，报告说明）；
    ComNav 按其手册（`SAVECONFIG`），无资料就返回不支持并在报告列出。
  - `factoryResetStep(...)`：Unicore `FRESET`；u-blox CFG-CFG clear+load（或 UBX-CFG-RST 冷启动配合 clear）；ComNav 同上规则。
- `gnss::Controller` 新增请求入口（与 `requestReadback` 同类）：`requestSave()`、`requestFactoryReset()`，只能在 `kRunning` 发起；
  结果放 stats（seq + ok/nack/timeout/unsupported），main 侧按 seq 判断“我请求的那次完成了”。`FRESET` 之后接收机会重启并恢复缺省
  波特率：控制器完成后直接 `requestReprobe`。
- 冻结头授权：本卡可修改 `components/gnss/include/gnss/Receiver.hpp`、`Controller.hpp` 及三个家族/叶子驱动头，仅限新增成员；
  已有签名不变。golden 字节（`test/gnss/golden/*.inc`）必须不变；新命令另加单元测试（每家族字节级断言）。

### 1.2 `gnss save`（main）

> 2026-11-18 用户修订：out/rate/baud 等修改**不再自动 SAVECONFIG**，只写 NVS；自动保存只在工作模式切换（C5a）时做，本卡保留可复用的保存请求接口但不在任何现有命令后自动触发。开机探测到的波特率与 NVS `gnss/baud` 不同时自动切到 NVS 值。

- 用户改动（`gnss out`、`gnss rate`、本卡新增的 `gnss baud`）**成功应用到接收机**并写 NVS 后，自动 `requestSave()` 一次；失败只记 W
  `<cfg> Receiver save failed (<原因>)`，不回滚已生效的设置。启动配置流程**不**发保存。
- 连续改动合并：保存请求在途时再有改动，完成后再补发一次（最多一个在途、一个待发）。
- `gnss save`：手动发一次，打印结果（`saved` / `ERR not supported` / `ERR busy` / `ERR timeout`）。

### 1.3 `gnss freset`

- 需二次确认：`gnss freset yes`；无 `yes` 时打印说明并返回失败。
- 动作：`requestFactoryReset()` → NVS 中 gnss 用户键（`gnss.config` 组 + 本卡新键）恢复缺省并保存 → 清 `gnss.cache` → 重新探测，
  探测完成后按缺省配置正常走配置流程。打印结果；控制台不阻塞超过现有命令等待上限。

### 1.4 主机口波特率 `gnss baud`

- 新键 `gnss/baud`（u32，取值 9600/19200/38400/57600/115200/230400/460800/921600，缺省 = Kconfig 当前值），进 `gnss.config` 组。
- `gnss baud <n>`：接收机运行中 → 发改口波特率命令（Unicore `CONFIG COMx <n>`；u-blox CFG-VALSET UARTx BAUDRATE；ComNav 按手册）→
  主机 UART 切到新波特率 → 在新波特率上重新探测；成功后写 NVS（不自动保存到接收机）；失败（探测不到）→ 切回旧波特率重新探测，NVS 不变，打印失败。
- 启动时探测候选表把 NVS 值排在最前（缓存提示之后）。
- 端口约定（用户 2026-11-18）：`RTK_GNSS_PORT`（COM2）是接收机连到本板 ESP32 的口，保持 Kconfig、不做运行期配置；所有 gnss 命令不带端口时
  都作用于这个口（输出、波特率、保存）。`gnss baud` 只改这个口，并且 ESP32 侧 UART 必须同步切换后再继续通信。
- 其他 COM 口（COM1/COM3 …）不建专门命令：用户用 `gnss send <原始命令>` 经 COM2 透传配置，再 `gnss save` 落盘。
  `gnss send` 若下发的是改 COM2 自身波特率的命令（如 `CONFIG COM2 <n>`），必须拒绝并提示改用 `gnss baud`，防止 ESP32 失联；帮助文本写明这两条。

### 1.5 统一 `gnss out`（单位 Hz）

- 解析与显示改用 Hz：`gnss out gga 1`、`gnss out gsv 0.2`、`gnss out all 1`、`gnss out gga off`（或 `0`）、`gnss out default`、`gnss out`（显示）。
  正数只接受 1/2/5/10/20；<1 Hz 只接受 1/n（0.5、0.2、0.1 … 最小 1/127），其他值报 `ERR invalid rate (Hz: 1/2/5/10/20 or 1/n)`。
- 名称：沿用现有 6 类（gga、sats/gsv 同义、heading、gst、rmc、gsa）；显示表头 `msg  rate  receiver`，rate 列打印 `1 Hz` / `0.2 Hz` / `off`。
- NVS 与蓝牙协议的 int8 编码不变（正 = Hz，负 = 周期秒），`GnssService::intervalToPeriodMs` 不改；只改控制台解析/显示，转换函数放到可主机测试的位置。
- `gnss rate <hz>` 保留，帮助文本改为 `shortcut: position messages rate (see gnss out)`。
- RTCM 报文（`gnss out rtcm ...`）**不在本卡**（C5a）。

## 2 不做

- 工作模式、基站、smartBase（C5）；`freset` 全机恢复出厂、早期加载、启动失败计数（C3/C4）。
- 不提交、不重算 `docs/frozen.sha256`、不烧录（Claude 做）。不用 `uvx`。

## 3 门禁

- `. ~/esp/esp-idf/export.sh; idf.py -B builds/c2 build`：0 warning；记录 bin 大小。
- 主机：test/gnss（普通 + ASan）与全驱动构建（`-DRTK_GNSS_DRV_COMNAV=ON -DRTK_GNSS_DRV_UBLOX=ON`）全过，新测试覆盖：三家族 save/freset/baud 命令字节、
  Controller 新请求的状态机（ok/nack/timeout/非 kRunning 拒绝/FRESET 后重探测）、Hz⇄int8 转换与非法值、保存合并逻辑（若在 main，放 main_host_check）。
- main_host_check、t13_host_check、test/services、test/app_fw、test/host、test/cli、test/bt_reconnect、test/nvs_store 全过。
- G5b：`python3 -I tools/tidy_check/tidy_check.py --host-build <host> --idf-build builds/c2 --gnss-build <gnss-all> --nvs-build <nvs> --test-app-build builds/q4-rtk_pure-esp32` 0 条。
- G7b `--build builds/c2` PASS；clang-format 0；`doc_check --base <基线>` 0 不合格；golden 不变。
- `sha256sum -c docs/frozen.sha256` 只允许 §1.1 授权的 gnss 头失败。

## 4 报告

`docs/reports/C2.md`：改动清单、各家族命令字节与资料出处（手册章节）、不支持项、测试列表、门禁结果、bin 变化、控制台样例（模拟）。
完成后最后单独一行打印 `C2 DONE` 并停下。

## 5 Claude 上板验收（实施方不做）

UM980：`gnss out gsv 0.2` → 重启后保持且模块 `gnss config` 显示已保存；`gnss save`；`gnss baud 460800` → 重探测成功、重启保持，再改回 115200；
`gnss freset yes` → 模块恢复缺省、NVS 缺省、重新探测正常；全程 `nmea bad=0`。
