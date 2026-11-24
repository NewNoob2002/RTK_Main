# OL1 Cross-Review Report: P2 OLED (SH1106) Driver + WouoUI Read-Only Status Pages

- **Task card**: `docs/tasks/OL1-oled.md`
- **Implementation report**: `docs/reports/OL1-oled.md`
- **ADR draft**: `docs/adr/ADR-0020-oled-display.md`
- **Base commit**: `3bbd6d2` (uncommitted working tree in `RTK_Main-ol1`)
- **Final Verdict**: **FAIL** (0 blocker, 4 major, 3 minor)

---

## 1. Focus-Area Summary & Independent Verification

### Independent Checks Run (prefix `ol1r`)

| Check | Command / Evidence | Result |
| --- | --- | --- |
| Frozen inputs (`G1-frozen`) | `sha256sum -c --quiet docs/frozen.sha256` | PASS |
| Parallel boundary check | `git status --short` — 0 edits under `components/gnss/`, `main/modules/gnss/`, `main/settings/` | PASS |
| Host tests (normal) | `cmake/ninja/ctest` in `builds/ol1r-main_host_check` (129/129, incl. all `charger_*`, `power_*`, `oled_*`, `board_profile_check`, `console_profile_check*`, `radio_mode_check`) | PASS |
| Host tests (ASan/UBSan) | `cmake/ninja/ctest` in `builds/ol1r-main_host_check-asan` (129/129) | PASS |
| Format gate (`G5-format`) | `clang-format --style=file --dry-run --Werror` on all non-excluded C/C++ files | PASS |
| Doc gate (`G6-doc_check`) | `python3 -I tools/doc_check/doc_check.py --base 3bbd6d2` (87 functions checked, 0 failed) | PASS |
| Runtime/heap gate (`G7b`) | Inspected `builds/ol1-logs/G7b-runtime.log` + `rg` on `components/third_party/wououi/`: 0 `malloc`/`calloc`/`realloc`/`free`, 147 objs including `third_party`, 0 exceptions/RTTI/`__cxa_guard` | PASS |
| ELF section/symbol audit | `xtensa-esp32-elf-readelf -S` & `objdump -t` comparing `builds/ol1-base/rtk_main.elf` vs `builds/ol1-idf/rtk_main.elf` | Verified (see §2) |
| WouoUI upstream diff | `diff -u` against upstream `82b985a` (`Csource/` + `LICENSE`) | Exact match except 2 marked edits in `WouoUI_conf.h` |

### Area-by-Area Assessment

1. **Shared I2C Bus (`main/board/I2cBus.*`, `main/board/Charger.cpp`)**:
   - Single owner `app::board::i2c` creates `I2C_NUM_0` once idempotently (`enable_internal_pullup = 1`, `glitch_ignore_cnt = 7`) with a static priority-inheriting mutex (`xSemaphoreCreateMutexStatic`).
   - Init order is sound: `stepPower` → `charger::init` (`0x1A` @ 100 kHz) runs before `stepLink` → `OledModule::init` → `Sh1106::init` (`0x3C` @ 400 kHz), and either device can initialize cleanly if the other is absent.
   - B3 behavior has no regression: single `W` log on missing `0x1A`, REG02 `[7:6]` preserved / `[5:0]` + REG03 verified, 0-tick non-blocking `refreshInputStatus()`, and `scan()` delegates cleanly to `i2c::scanBus()`. All 8 `charger_*` host tests pass.
   - **Defect (`ROL1-02`)**: `Sh1106::writePage()` holds `BusLock` up to 30 ms (2 × 15 ms timeout) vs `kDefaultMutexWaitMs = 20 ms`, does not abort remaining pages on timeout, and neither `I2cBus` nor `Sh1106` invokes `i2c_master_bus_reset()` on bus timeout.
2. **SH1106 Driver (`main/board/Sh1106.*`)**:
   - Init sequence matches the SH1106 datasheet and legacy `P2_V3.1_IDF/main/src/xyz_oled.c`: `0xAE` → col offset 2 (`0x02`, `0x10`) → start line `0x40` → contrast `0x81 0xCF` → 180° flip (`0xA0`/`0xC0`) → mux `0xA8 0x3F` → offset/clock/precharge/COM/VCOMH → DC-DC on (`0xAD 0x8B` while display off) → RAM clear (8 pages) → `0xAF`.
   - `GPIO12` is only configured/driven when `board::hasFeature(profile, Feature::kOled) && profile.oled.i2cPort >= 0` (E1 DevKit never touches `GPIO12` or I2C0), using glitch-free `gpio_set_level` before `gpio_config`.
   - Absent panel path logs one `W` line, pulls `GPIO12` low, and disables display cleanly without failing boot.
   - Shutdown does not extend `PowerModule` stages or timeouts.
   - **Defects (`ROL1-01`, `ROL1-05`, `ROL1-06`)**: `wakeFromPowerOff()` failure leaves `GPIO12` stuck HIGH with `poweredOff_ == true` permanently; `kCutPower` sleeps a full 200 ms before `powerOff()` (can miss clean `0xAE`/`0xAD 0x8A` before fast USB-shutdown `esp_restart()`); `init()` leaks `dev_` handle if `configurePanel()` fails.
3. **OLED Module/Task (`main/modules/oled/`)**:
   - Core 1, priority 1 (below `kLink`=10, `kIo`=9, `console`=2), periodic wait 200 ms (≤ 5 Hz), 64-bit FNV-1a per-page digest skips unchanged pages (0 I2C writes when idle).
   - Reads strictly from existing snapshots (`board::current()`, `systemStatus`, `sessions`, `powerStatus`, `gnssStatus`, `gnssFix`), no new cross-module write APIs, short-key routed via lock-free `UiEventBus`, charge-standby locks to `kChargeStandby` and ignores short keys, zero heap allocation.
   - Console `oled [status]` command is gated to DEV profile per ADR-0016 (`G7c-no-cli-deps` unaffected).
   - **Defects (`ROL1-03`, `ROL1-06`)**: `PowerModule` calls `uiEvents.notifyWake()` before `publishSnapshot()`; `renderPage()` leaves WouoUI's static `p_cur_pen` dangling to a stack local; `sys tasks` does not report `"oled"` stack HWM.
4. **DRAM / IRAM Growth (+4,672 B DRAM, `.bss` +4,048 B, `.data` +624 B; IRAM +0 B)**:
   - IRAM delta confirmed **0 B** (116,139 B unchanged).
   - Exact DRAM symbol accounting verified (see §2 below).
   - **Defect (`ROL1-04`)**: 4,096 B of the +4,672 B DRAM growth comes from unconditional `.bss` globals (`s_oled_stack` 3,072 B + `s_oled_fb` 1,024 B) that bypass `BootArena` (contrary to ADR-0011 §7 and every other `BootTask` user), wasting 4 KB on non-OLED boards (E1) and console-only degraded boot; additionally, 336 B (`s_panel` +232 B, `s_bus` +104 B) land in `.dram0.data` (also growing Flash) solely due to `-1` default member initializers.
5. **WouoUI Vendoring & Gate Tool Changes**:
   - MPL-2.0 compliance is solid: verbatim `LICENSE` in `components/third_party/wououi/LICENSE`, upstream commit `82b985a` documented in `README.md`, 5 upstream files completely unmodified with headers preserved, and only 2 changes in `WouoUI_conf.h` explicitly annotated with `/* RTK_main modification (MPL-2.0): ... */`.
   - Zero `malloc`/`free` verified; only `WouoUI_graph.c` and `WouoUI_font.c` compiled; ELF symbol check confirms `--gc-sections` linked only `Font_6_8` (`F6x8`, 570 B) and `Font_8_16` (`F8X16`, 1,520 B) — unused fonts (`Font_6_12`, `Font_7_12`, `Font_12_24`, CN fonts) are stripped.
   - **Defect (`ROL1-07`)**: Gate exclusions in `doc_check.py` and `tidy_check.py` skip any path segment named `third_party` (and `run_gates.sh` skips all of `components/third_party/`). Acceptable today because only vendored WouoUI files live there, but should be narrowed to vendored subpaths (`components/third_party/wououi/`) so future own-code wrappers cannot silently bypass gates.

---

## 2. DRAM & IRAM Delta Breakdown (Verified from ELF)

Comparison of `builds/ol1-base/rtk_main.elf` vs `builds/ol1-idf/rtk_main.elf`:

| Section | Base | OL1 | Delta | Main Symbols |
| --- | ---: | ---: | ---: | --- |
| `.iram0.text` | 115,111 B | 115,111 B | **+0 B** | None |
| `.dram0.bss` | 44,752 B | 48,800 B | **+4,048 B** | `s_oled_stack` (+3,072 B), `s_oled_fb` (+1,024 B), `s_page_sm` (+20 B), `cur_screen` (+8 B), `p_cur_pen` (+4 B), `s_oled_task_handle` (+4 B), `s_force_next_flush` (+1 B), `s_charger` (-88 B), alignment (+3 B) |
| `.dram0.data` | 52,011 B | 52,635 B | **+624 B** | `app::s_app` (+272 B: `OledModule`/`BootTask` TCB + `Slot<OledStatus>` + `UiEventBus` + 2 `fw::Command` slots), `s_panel` (+232 B), `s_bus` (+104 B), `s_last_flush` (+1 B), alignment (+15 B) |
| **Total Static DRAM** | **96,763 B** | **101,435 B** | **+4,672 B** | Remaining static DRAM budget: 27,817 B → 23,145 B (81.42% used) |

---

## 3. Detailed Findings

### ROL1-01 — `wakeFromPowerOff()` failure strands `GPIO12` HIGH with display permanently disabled
- **Severity**: `major`
- **Location**: `main/board/Sh1106.cpp:282-305`, `main/modules/oled/OledModule.cpp:219-230`, `main/modules/oled/OledUi.cpp:382-387`
- **Failure Scenario**:
  After a 30 s shutdown cut-power timeout returns `shutdownStage` to `kIdle`, `PageSm::step()` (`OledUi.cpp:384-386`) clears `poweredOff_ = false` and emits `out.shouldWake = true` **only once**. `OledModule::refreshOnce()` calls `s_panel.wakeFromPowerOff()`. Inside `Sh1106::wakeFromPowerOff()`:
  1. `drivePowerPin(true)` drives `GPIO12` HIGH and sleeps 100 ms (`Sh1106.cpp:290-296`).
  2. If `configurePanel()` fails (e.g., transient `kBusy` during `power i2c scan` / `power charger`, or transient `kTimeout`), line 300 returns the error **without** calling `drivePowerPin(false)` and while `s_panel.poweredOff_` remains `true`.
  3. On all subsequent ticks, `PageSm` never emits `shouldWake` again, and `OledModule::refreshOnce()` early-returns at `if (s_panel.poweredOff()) return;` (`OledModule.cpp:228-230`). The OLED remains powered (wasting current via `GPIO12=HIGH`) yet black and permanently unrecoverable until reboot.
- **Suggested Fix**:
  1. In `Sh1106::wakeFromPowerOff()`, call `static_cast<void>(drivePowerPin(false));` before returning error when `configurePanel()` fails.
  2. In `OledModule::refreshOnce()`, either retry wake (with bounded backoff) whenever `!dec.shouldPowerOff && (vm.shutdownStage == ShutdownStage::kIdle) && s_panel.poweredOff()`, or pass actual panel power state back to `PageSm` so `poweredOff_` is only cleared once `wakeFromPowerOff()` succeeds. Add a host test in `OledDriverCheck.cpp` injecting a transient `configurePanel()` failure during `wakeFromPowerOff()`.

---

### ROL1-02 — I2C lock hold exceeds mutex wait timeout; no early abort or bus reset on timeout
- **Severity**: `major`
- **Location**: `main/board/Sh1106.cpp:95-114, 241-254`, `main/board/I2cBus.hpp:19`, `main/board/I2cBus.cpp:87-150`
- **Failure Scenario**:
  1. `Sh1106::writePage()` (`Sh1106.cpp:105-113`) holds `i2c::BusLock` across two sequential `i2c_master_transmit()` calls, each with `kI2cXferTimeoutMs = 15 ms` (`Sh1106.hpp:33`), allowing up to **30 ms** lock hold — exceeding `i2c::kDefaultMutexWaitMs = 20 ms` (`I2cBus.hpp:19`). Under a stalled I2C transfer, concurrent `charger::readReg()`, `charger::dumpRegs()`, or `i2c::scanBus()` will time out with `kBusy`.
  2. In `Sh1106::flush()` (`Sh1106.cpp:241-254`), when a page write fails with `kTimeout` or `kBusy`, the loop does **not** break; it keeps attempting all remaining dirty pages (up to 8 pages × 15–30 ms = 120–240 ms per 200 ms cycle) without committing hashes, repeating every 200 ms indefinitely.
  3. Neither `I2cBus` nor `Sh1106` exposes/calls `i2c_master_bus_reset()` on `ESP_ERR_TIMEOUT`. If ESD or a panel glitch leaves SDA held low, the shared I2C0 bus stays wedged forever and continuously starves `Charger` polling and `power i2c scan`.
- **Suggested Fix**:
  1. Add `i2c::resetBus()` wrapping `i2c_master_bus_reset(s_bus.bus)` under `BusLock` (rate-limited/bounded), and call it on `kTimeout`.
  2. In `Sh1106::flush()`, break out of the page loop immediately on `kTimeout`/`kBusy` (or any bus error) instead of hammering all 8 pages back-to-back, and add a brief retry backoff when consecutive errors occur.
  3. Keep single `BusLock` hold strictly below `kDefaultMutexWaitMs` (e.g., reduce `kI2cXferTimeoutMs` to 8–10 ms — 130 B @ 400 kHz is ~3 ms — or raise `kDefaultMutexWaitMs` above 2 × `kI2cXferTimeoutMs` + margin, or acquire lock per transmit if page/column addressing allows). Add a timeout + recovery scenario to `OledDriverCheck.cpp`.

---

### ROL1-03 — `PowerModule` notifies `UiEventBus` before publishing `powerStatus` snapshot
- **Severity**: `major`
- **Location**: `main/modules/power/PowerModule.cpp:560, 571, 575, 600, 715-717`
- **Failure Scenario**:
  `PowerModule::stepShutdown()` calls `ctx().data.uiEvents.notifyWake()` inside `handleShutdownStageEntry()` (lines 560, 571, 575) and `handleShutdownCutPower()` (line 600), **before** `PowerModule::poll()` calls `publishSnapshot(pins, false)` at line 717 to write the updated `shutdownStage` into `ctx().data.powerStatus`.
  Today this is accidentally masked on ESP32 only because `kLink` (prio 10) and `oled` (prio 1) are pinned to the same Core 1 so `oled` cannot run until `kLink` yields after line 717. If `oled` is ever moved to Core 0 or run on a host SMP scheduler, `oled` wakes immediately on `notifyWake()`, reads the **stale** `powerStatus.shutdownStage` (`kIdle` or previous stage), does not draw the shutdown notice / does not power off, and sleeps for 200 ms.
- **Suggested Fix**:
  Remove the premature `notifyWake()` calls inside `handleShutdownStageEntry()`/`handleShutdownCutPower()` and instead call `ctx().data.uiEvents.notifyWake()` **after** `ctx().data.powerStatus.write(next)` in `publishSnapshot()` whenever `next.shutdownStage != lastSnapshot_.shutdownStage` (or track a `bool wakeUi` flag in `poll()` and invoke `notifyWake()` right after `publishSnapshot()`).

---

### ROL1-04 — +4,672 B DRAM growth: 4 KB unconditional `.bss` bypasses `BootArena` and 336 B lands in `.dram0.data`
- **Severity**: `major`
- **Location**: `main/modules/oled/OledModule.cpp:30-35`, `main/board/I2cBus.cpp:14-23`, `main/app/BootBuffers.cpp:88-111`
- **Failure Scenario**:
  1. **Unconditional `.bss` stack & framebuffer (+4,096 B)**: Every other task using `rtos::BootTask` (`task.link`, `task.io`, `task.spp_worker`, `task.trace_out`) allocates its stack from `BootArena` in `BootBuffers.cpp` (ADR-0011 §7; `BootTask.hpp:1` states *"任务栈由启动期 boot_alloc() 注入"*). `OledModule.cpp:30-31` instead allocates `s_oled_stack` (3,072 B) and `s_oled_fb` (1,024 B) unconditionally in `.bss`. Even on boards without `Feature::kOled` (E1 DevKit) or during console-only degraded boot, 4,096 B of scarce internal DRAM is permanently lost (static DRAM jumps 77.67% → 81.42%) and invisible in `sys mem`'s `BootArena` ledger.
  2. **Avoidable `.dram0.data` (+336 B DRAM & Flash)**: `s_panel` (+232 B, `OledModule.cpp:32`) and `s_bus` (+104 B, `I2cBus.cpp:23`) are pushed out of `.bss` into `.dram0.data` solely because their structs default-initialize GPIO/port fields to `-1` (moving `s_charger`'s 88 B mutex from `.bss` to `.data` and adding 336 B to the Flash image as well).
- **Suggested Fix**:
  1. Allocate `oled.stack` (3,072 B) and `oled.fb` (1,024 B) in `allocateBootBuffers()` (`main/app/BootBuffers.cpp`) gated on `board::hasFeature(board::current(), board::Feature::kOled)` and bind them to `OledModule` (consistent with `DebugModule::bindStorage()`), saving 4 KB on non-OLED/degraded boots and keeping `sys mem` accounting complete. (On P2 with OLED, total DRAM used is unchanged; after HIL measures `stack_hwm` via `ROL1-06`, evaluate tightening the 3,072 B stack if headroom permits.)
  2. Make `Sh1106` and `i2c::BusState` all-zero default-initialized (e.g., `bool initialized_ = false` with `0`-initialized pin structs, populated in `init()`/`ensureInit()`), moving 336 B back from `.dram0.data` to `.dram0.bss`.

---

### ROL1-05 — Fixed 200 ms sleep in `kCutPower` can miss clean `Sh1106::powerOff()` before USB-shutdown restart
- **Severity**: `minor`
- **Location**: `main/modules/oled/OledUi.cpp:374-378`, `main/modules/oled/OledModule.cpp:203-211`, `main/modules/power/PowerModule.cpp:607-612, 679-685`
- **Failure Scenario**:
  When `kAnnounce` → `kSave` → `kQuiesce` → `kCutPower` completes quickly (e.g., ~50–60 ms with no pending NVS writes), `kCutPower` wakes `oled` while `nowMs - shutdownNoticeSinceMs_ < 200 ms`. `PageSm::step()` returns `shouldPowerOff = false`, and `OledModule::run()` unconditionally sleeps for a full `kRefreshPeriodMs = 200 ms` (`OledModule.cpp:209`), delaying `powerOff()` until ~260 ms instead of 200 ms.
  On **USB-connected shutdown**, if U3 (`GPIO25`) drops quickly after `GPIO4` goes high, `PowerModule` sets `restartPending_ = true` and calls `esp_restart()` after `kQuiesceMinWaitMs = 30 ms` (`PowerModule.cpp:681-682`) — potentially before `oled` wakes up to send `0xAE`/`0xAD 0x8A` and pull `GPIO12` low.
- **Suggested Fix**:
  Have `PageDecision` carry an optional `nextWakeMs` (or compute `remaining = kShutdownNoticeMinMs - elapsed` when in `kCutPower && !poweredOff_`) so `OledModule::run()` waits only the remaining milliseconds until `kShutdownNoticeMinMs` expires, rather than a full 200 ms.

---

### ROL1-06 — Minor resource/safety/observability gaps (`dev_` leak on init fail, dangling `p_cur_pen`, missing `sys tasks` row)
- **Severity**: `minor`
- **Location**: `main/board/Sh1106.cpp:210-222`, `main/modules/oled/OledUi.cpp:574-580`, `main/modules/system/SystemCmds.cpp:273-293`
- **Failure Scenario**:
  1. In `Sh1106::init()` (`Sh1106.cpp:216-222`), if `configurePanel()` fails after `probeAndAttach()` already called `i2c::addDevice(..., dev_)`, `dev_` is never detached via `i2c_master_bus_rm_device()` nor nulled (minor one-time IDF heap leak on boot failure).
  2. In `renderPage()` (`OledUi.cpp:576-580`), stack local `Pen pen{...}` is registered with `WouoUI_GraphSetPen(&pen)` and never cleared before returning, leaving WouoUI's static `p_cur_pen` dangling to dead stack memory (safe today only because `WouoUI_BuffSend()` doesn't read `p_cur_pen`, but a latent UAF trap for future callers).
  3. `cmdSysTasks()` (`SystemCmds.cpp:273-293`) lists `link`, `io`, `spp_worker`, `trace_out`, `console`, `idle0`, `idle1`, but omits `"oled"`, so HIL cannot observe `oled`'s `stack_hwm` to validate the 3,072 B stack budget (`CODING-STYLE.md` §10 RTOS-002).
- **Suggested Fix**:
  1. Expose `i2c::removeDevice(i2c_master_dev_handle_t &dev)` and call it on `Sh1106::init()` `configurePanel()` failure.
  2. Use a `static constinit Pen` or call `WouoUI_GraphSetPen(nullptr)` at the end of `renderPage()`.
  3. Add `printTaskRow("oled", xTaskGetHandle("oled"));` to `cmdSysTasks()` in `main/modules/system/SystemCmds.cpp`.

---

### ROL1-07 — Gate exclusions for `third_party` match overly broad directory segments
- **Severity**: `minor`
- **Location**: `tools/doc_check/doc_check.py:34`, `tools/tidy_check/tidy_check.py:71`, `tools/ci/run_gates.sh:79`
- **Failure Scenario**:
  `doc_check.py:34` excludes any path containing `"/third_party/"`, `tidy_check.py:71` skips any path with segment `"third_party"` under `components/`, `main/`, or `test/nvs_store/`, and `run_gates.sh:79` skips all of `^components/third_party/`. Today only vendored upstream WouoUI files exist under `components/third_party/wououi/`, so no own code is currently bypassed. However, if any own glue/wrapper code is later placed under `components/third_party/` (outside `wououi/`) or a subdirectory named `third_party/` elsewhere, it would silently skip format, Doxygen, and clang-tidy gates.
- **Suggested Fix**:
  Restrict the exclusions in `run_gates.sh`, `doc_check.py`, and `tidy_check.py` to explicit vendored prefixes (e.g., `components/third_party/wououi/`) — keeping a single documented vendored-paths list — so only verified vendored upstream directories are exempted.

---

## 4. Verdict

**FAIL** — Core architecture, SH1106 protocol, B3 I2C compatibility, WouoUI MPL-2.0 vendoring, zero-heap enforcement, and IRAM neutrality are well executed, and all 129 host tests (normal + ASan) pass. However, four `major` findings (`ROL1-01` permanent display lockup with `GPIO12` stuck HIGH on wake error, `ROL1-02` I2C lock timeout mismatch / missing bus reset & timeout storm, `ROL1-03` shutdown `notifyWake` before `publishSnapshot`, and `ROL1-04` 4 KB unconditional `.bss` bypassing `BootArena` + 336 B `.dram0.data` bloat) must be fixed and covered by regression tests before merge.
