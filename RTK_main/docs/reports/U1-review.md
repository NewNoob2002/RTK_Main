# U1 review · ZED-F9P rover driver

Reviewed `feat/u1-f9p` at HEAD `84025fd`, including the 24 tracked files in `git diff 84025fd` and the three pre-review untracked files. The ESP-IDF/C++ review followed the task card, `AGENTS.md`, `docs/CODING-STYLE.md`, and ADR-0010/0011/0012/0018. U1 corrects the legacy VALSET header and adds output silencing, satellite settings, and receiver serial-number reporting. The cold-start failure and satellite configuration issues below affect the P2 application path.

## Findings

### RU1-01 · blocker · NMEA silencing can prevent rover startup

**Location** `components/gnss/src/drivers/UbloxReceiver.cpp:154`, `components/gnss/src/ControllerConfigure.cpp:85`, `components/gnss/include/gnss/Controller.hpp:55`.

**Failure scenario.** A detected F9P ACKs the basic configuration, then NAKs or fails to answer the new 12-key NMEA-silence VALSET. With the default two retries, the controller sends three copies and enters `kFailed`. It never reaches the output plan or a later scripted baud step. The reviewer host reproduction recorded `outputRuns=0` after both three NAKs and three 500 ms timeouts. This violates U1 §1.5.

**Suggested fix.** Make failure of the F9P silence step observable and nonfatal. Keep the per-attempt deadline, log NAKs with the corresponding keys, and proceed to the next step. Give GLL, VTG, and GSV-disable explicit coverage in the fallback if optional keys can reject the bulk write. Add a controller regression that drives the actual F9P rover script through NAK and timeout into the remaining steps.

### RU1-02 · major · F9P advertises invalid GPS-off configurations

**Location** `components/gnss/src/UbloxExt.cpp:239`, `main/modules/gnss/GnssModuleProbe.cpp:344`, `main/modules/gnss/GnssModulePpp.cpp:94`, `test/gnss/TestGnssUbloxReceiver.cpp:609`.

**Failure scenario.** The web GPS switch, phone `0x06`, and blobs saved by another receiver can request GPS off. The F9P builder emits `GPS_ENA=0` and `QZSS_ENA=0`, including the all-off case, and does not ensure that GPS L1 C/A is enabled. An F9P requires GPS L1 C/A for a valid signal configuration. The current tests bless the generated invalid combination and supply synthetic ACKs in separate tests. A receiver whose GPS L1 C/A or QZSS L1 C/A was changed externally also cannot rely on the assumption that all band keys are at factory defaults.

**Suggested fix.** Encode the F9P GPS L1 constraint in capability reporting, command validation, and replay of older blobs. Reject an invalid F9P request before touching MINELEV and keep UM98x behavior. Ensure the required signal state and supported QZSS L1 C/A state in the combined VALSET, using a firmware-qualified key list. Test GPS-off/all-off rejection, GPS-only, and nondefault band-key state. SparkFun adds supported per-constellation signal keys, including QZSS L1 C/A, in `GNSS_ZED.h:27–36` and `GNSS_ZED.ino:2012–2031`. The GPS L1 constraint is also specified in the review brief and attributed to the vendor CFG-SIGNAL description.

### RU1-03 · major · The SBAS key can reject every constellation update on HPG 1.12

**Location** `components/gnss/src/UbloxExt.cpp:245`, `main/modules/gnss/GnssModuleProbe.cpp:344`, `docs/reports/U1-f9p-driver.md:200`.

**Failure scenario.** A detected HPG 1.12 F9P receives `CFG-SIGNAL-SBAS_ENA` in every constellation batch, even when the caller only changes GLONASS and SBAS remains off. SparkFun's support table excludes SBAS control on 1.12 and its writer tests support before adding an item. If the receiver NAKs this key, the whole constellation batch fails. The preceding cutoff write can already have succeeded, and replay hits the same failure. The report describes this as a warning followed by later steps, but this frame contains every constellation.

**Suggested fix.** Derive SBAS writability from the probed firmware or a confirmed key-capability check. Omit the key when unsupported and mask the capability consistently at the settings and BT boundaries. Define the treatment of a saved SBAS value on downgrade. Add a firmware-specific batch test. Sources are SparkFun `GNSS_ZED.h:36` and `GNSS_ZED.ino:2013–2031`.

### RU1-04 · major · An ACK releases the UART before the signal restart has settled

**Location** `components/gnss/src/PppSequence.cpp:160`, `main/modules/gnss/GnssModule.cpp:304`, `main/modules/gnss/GnssModulePpp.cpp:155`.

**Failure scenario.** After the signal VALSET ACK, the next poll completes the ticket and releases the owner. A second request can start while the roughly 0.5 s signal restart is still in progress, if the ACK comes before the restart finishes. The reviewer host reproduction completed the first ticket 1 ms after ACK and emitted the next VALSET 2 ms after ACK. The second command can be lost or time out on a receiver that is still restarting. The 1 s ACK timer places a maximum on waiting for a reply and supplies no minimum settling time.

**Suggested fix.** Keep following configuration writes gated through a nonblocking settling phase after changing F9P signal configuration, or use a verified receiver-ready indication. Budget that phase alongside the per-command ACK deadlines and the web caller's 2 s timeout. Test rapid consecutive settings and boot replay with an early ACK and delayed receiver readiness. The approximate restart duration comes from the review brief and remains for HIL to measure.

### RU1-05 · major · Late VALSET replies can advance the wrong command

**Location** `components/gnss/src/drivers/UbloxReceiver.cpp:304`, `components/gnss/src/ControllerConfigure.cpp:85`, `components/gnss/src/PppSequence.cpp:167`, `main/modules/gnss/GnssModuleProbe.cpp:507`, `main/modules/gnss/GnssModuleFrames.cpp:172`.

**Failure scenario.** An ACK identifies the acknowledged class and message ID, so the basic config, silence, output, and satellite commands all receive identical ACK bytes. After a retry, a late ACK to the original basic write can advance to silence, and the retry's ACK can then be consumed as the silence ACK. Silence may be reported as successful even if its own NAK comes later. A late NAK can log the next frame's key list because the log state stores only the most recent TX keys. The UART has one task owner, which does not resolve reply identity across command boundaries.

**Suggested fix.** Track the in-flight command through its full reply lifetime and resynchronize after a timeout or retry before trusting another ambiguous ACK. Use targeted VALGET verification where a late reply cannot be safely excluded. Attribute logs only to a known in-flight request and handle unmatched replies explicitly. Add tests with delayed and duplicated ACK/NAK across step boundaries. The ACK payload shape is consistent with the vendor UBX-ACK description and legacy `user_f9p.c:96–166`.

### RU1-06 · major · Failed settings can leave receiver configuration ahead of NVS and the UI

**Location** `main/modules/gnss/GnssModulePpp.cpp:92`, `main/modules/gnss/GnssModulePpp.cpp:131`, `main/modules/gnss/GnssModulePpp.cpp:203`, `main/app/SettingsBackend.cpp:165`.

**Failure scenario.** MINELEV can be ACKed and written to RAM+BBR before a constellation NAK. Conversely the constellation batch can succeed after a cutoff timeout. If both ACK and the NVS write fails, the entire requested change remains active at the receiver. In all these cases web and BT retain the old snapshot or defaults. With no saved blob, a later boot has nothing to replay and can retain the BBR changes. A first web toggle on a receiver previously configured through USB also starts from application defaults and can rewrite other constellations and the cutoff that the user never selected.

**Suggested fix.** Establish a confirmed baseline before the first partial update, or send only the requested keys. Arrange for one validated receiver update per logical change where supported, and reconcile receiver state on ambiguous or partial failure and on NVS commit failure. The existing six-byte blob can remain the durable host format. Test both step failures, NVS failure, and reboot with the same receiver and BBR retained. This is the two-phase settings and snapshot contract in ADR-0018 §2.2–2.4.

### RU1-07 · major · Runtime satellite settings pause and can split the RTCM stream

**Location** `main/modules/gnss/GnssModule.cpp:304`, `main/modules/gnss/GnssModule.cpp:337`, `main/modules/gnss/RtcmPipe.hpp:121`, `main/modules/gnss/GnssModule.hpp:42`.

**Failure scenario.** The new F9P runtime and replay path uses a sequence that returns before `pumpRtcmPipe()` while it waits for ACKs. The two waits can consume nearly 2 s, during which the 4096-byte input pipe can overflow if there is a burst or backlog. The pipe also drains raw 512-byte chunks. If a chunk ends inside an RTCM frame and a satellite command begins next, UBX can be inserted ahead of the remaining RTCM bytes. That frame fails CRC, and the receiver may consume some or all of the UBX frame as RTCM payload. The single kIo writer prevents simultaneous UART calls and still permits this ordering.

**Suggested fix.** Hand off between RTCM and configuration writes at complete RTCM boundaries and keep RTCM draining when the receiver can accept it during reply waits. Exercise a frame crossing the 512-byte boundary and a loaded pipe during F9P ACK/timeout handling. This is the U1 §4.3 RTCM requirement and ADR-0011 data path contract.

### RU1-08 · major · The portal lacks a cutoff setting

**Location** `components/settings/src/Registry.cpp:819`, `components/settings/src/Registry.cpp:844`, `main/web/ui/app.js:251`, `docs/tasks/U1-f9p-driver.md:23`.

**Failure scenario.** The four registered web switches route correctly as constellation patches. A portal user cannot set 15° or any other cutoff. The only setting carrying the cutoff is `gnss.sat_track`, which uses `kPhoneAndLocal` and returns locked for a web request. This leaves the web part of the U1 §1.3/§4.2 requirement incomplete.

**Suggested fix.** Provide the cutoff through an approved existing web setting path with a 0–90° range, or record an Architect-approved change to the frozen web contract if a dedicated key is needed. Exercise value serialization, the actual kIo command, and NVS restoration. The web v1 document currently lists the four switches only.

### RU1-09 · minor · The silence list omits supported out-of-plan NMEA messages

**Location** `components/gnss/src/drivers/UbloxWire.hpp:92`, `test/gnss/TestGnssUbloxReceiver.cpp:556`.

**Failure scenario.** A previously configured receiver can retain VLW, which SparkFun lists for HPG 1.12, or RLM on 1.13+. U1 clears neither. With one of these outputs enabled on the MCU-facing UART, the board still emits an out-of-plan NMEA sentence after startup. The test asserts only the same 12 items. This is separate from the default GLL/VTG/GSV coverage.

**Suggested fix.** Include supported plan-external keys using firmware-aware filtering, or narrow the documented guarantee to factory-default outputs with an explicit deviation. Verify VLW and version-gated RLM using the UART1/UART2 tables and test a receiver carrying saved extra output. See SparkFun `GNSS_ZED.h:160–188`.

### RU1-10 · minor · VALSET header and UBX framing have two implementations

**Location** `components/gnss/src/drivers/UbloxUbx.cpp:95`, `components/gnss/src/drivers/UbloxUbx.cpp:230`, `components/gnss/src/UbloxExt.cpp:49`, `components/gnss/src/UbloxExt.cpp:81`.

**Failure scenario.** The old overloads are gone, but the new satellite and silence writers use a second header, item writer, and checksum encoder. A correction to the shared frame layout can land in one path and leave the other with the original encoding error. U1 §1.1 explicitly calls for a single implementation.

**Suggested fix.** Share the existing bounded writer and frame builder through one GNSS-private implementation. Keep common UBX details at the component boundary where possible, consistent with ADR-0012 §2.1. App-level ownership of the serial transaction and shared state can stay on kIo.

### RU1-11 · minor · Checked-in protocol descriptions and HIL instructions need reconciliation

**Location** `docs/protocol/phone_protocol.md:46`, `docs/protocol/phone_protocol.md:51`, `components/link_core/services/PhoneCompatService.cpp:746`, `docs/reports/U1-f9p-driver.md:65`, `docs/reports/U1-f9p-driver.md:203`.

**Failure scenario.** A phone client following the frozen table treats `0x06` as an unconditional placeholder. The existing implementation now reaches the F9P and uses a 12-byte query with cutoff at offset 1 and flags at offsets 4–8, a minimum 9-byte set, and deferred results. The six-byte value in the implementer report is the internal command/NVS layout. `0x01` keeps its 136-byte layout and carries the F9P serial in the existing bytes 64–79. The HIL instruction to watch for zero GSV also needs a plan with GSV disabled. With GSV configured on, startup intentionally re-enables it.

**Suggested fix.** Record the wire-format deviations for Architect approval of the frozen protocol update. Correct the report to identify the actual production layers and the GSV-off precondition for the silence capture. Preserve separate tests for a disabled GSV plan and an enabled GSV plan at its intended rate.

### RU1-12 · minor · The retained baseline artifacts cannot substantiate the resource delta

**Location** `docs/reports/U1-f9p-driver.md:136`, `main/modules/gnss/GnssModule.hpp:905`, `main/modules/gnss/GnssModuleFrames.cpp:172`, `main/AppContext.hpp:83`.

**Failure scenario.** Both retained `u1-baseline` and `u1-idf` ELFs contain `GnssModule::handleUbxFrame` and `buildSatConstValset`. Their measured section sizes match and both bins are 1,520,368 B. The report may have used earlier measurements, but the retained artifacts cannot reproduce its stated +112 B DRAM and +3,632 B bin deltas. The new 256 B NAK frame sits on the UART receive call chain and target stack margin is absent from the report.

**Suggested fix.** Retain a same-configuration build from an isolated, verifiable `84025fd` checkout and its size output. Record the kIo low-water margin under an F9P NAK burst and normal RTCM/NMEA load.

## Protocol and port audit

- **UBX frame.** Checked the `B5 62` sync, little-endian U2 payload length and Fletcher CK_A/CK_B over class through the last payload byte. VALSET v0 uses `[0, layers, 0, 0]`, then little-endian U4 keys and key-sized values. The legacy P2 `user_f9p.c:5–45` and SparkFun X20P update sketch `GNSS_ZED.ino:508–542` independently show the checksum algorithm and four-byte header, respectively. Host tests check complete bytes. No CFG-VALGET path changed or exists in this driver. A v0 VALGET poll uses `version, layer selector, position U2`, followed by U4 keys. A readback implementation should use its own version/layer/position semantics. Vendor reference is UBX-22008968, UBX-CFG-VALGET and UBX-CFG-VALSET.
- **Layer choice.** The actual P2 path sets `useConfiguredPort` in `GnssModule.cpp:72–73`. Basic config, the silence write, normal output and configured-port baud use RAM (`0x01`). The compatibility path uses RAM+BBR (`0x03`) and the F9P satellite/cutoff writes use RAM+BBR on both paths. The normal U1 VALSET writes leave the Flash layer bit clear. The existing explicit receiver save remains a distinct maintenance operation. RAM output state is rebuilt at startup. BBR depends on backup power and GNSS settings replay uses the unchanged host NVS blob. See ADR-0010 §2.4 and RU1-06.
- **Ports.** The P2 ESP32 GNSS port is ESP32 UART2 on GPIO16/17 connected to **receiver UART1**, established by `main/board/BoardProfile.cpp:28`, the P2 v1.41 schematic PDF pages 6–7, and legacy `main.c:147` with `user_f9p.c:305–307,346,354,416–418`. The configured-port UART2 driver path shifts MSGOUT keys by one and is host-covered. The schematic shows the USB hub on page 1 and the F9P USB change note on page 3. Receiver UART2 and native USB are outside the ESP32 rover-data path reviewed here. Leaving their NMEA rates alone preserves service access. The actual P2 F9P population and port traffic remain for HIL.
- **Parsers.** MON-VER checks the declared length and checksum, then scans complete 30-byte extensions after the 40-byte fixed fields. Bounded copies, strict model-prefix handling and the SW-version fallback are covered. A trailing partial extension after a valid model is ignored. SEC-UNIQID validates class/ID, exact v1 length and checksum before reading five bytes at offsets 4–8 into an 11-byte minimum destination. The slot carries a bounded 10-character uppercase hexadecimal value; absence leaves the field empty on reprobe. SparkFun polls SEC-UNIQID in `GNSS_ZED.ino:219–222`. The vendor references are UBX-22008968, UBX-MON-VER and UBX-SEC-UNIQID.
- **Receiver regression.** The UM98x/CM680 ASCII sequence still emits six steps and terminates immediately on NAK. K803 remains unsupported for satellite settings. The all-driver suites pass. Enabling the existing web constellation route makes it callable on previously supported UM98x/CM680 as well as F9P. Live phone, portal and receiver integrations remain HIL items.

### Rover key audit

The offline SparkFun checkout uses symbolic configuration IDs and has no copy of its imported `SparkFun_u-blox_GNSS_v3` key definitions. The vendor PDF UBX-22008968 was unavailable in the accessible local references. The table checks the emitted widths against the wire key size codes (L/U1/I1 occupy 1 byte, U2 occupies 2, U4 occupies 4), named keys against SparkFun where available, and numbers against the implementer’s vendor cross-reference. **Independent vendor confirmation of the numeric IDs remains open** except where the SparkFun source supplies the literal. The vendor reference groups are CFG-RATE, CFG-UART1/2 and UART protocol groups, CFG-MSGOUT, CFG-NAVSPG and CFG-SIGNAL. Base-mode TMODE and RTCM MSGOUT key definitions were unchanged and outside U1 §2.

| Key | UART1 or global ID | UART2 ID where used | Type and programmed value | Local evidence |
| --- | --- | --- | --- | --- |
| CFG-RATE-MEAS | `0x30210001` | — | U2, milliseconds per measurement | SparkFun `GNSS_ZED.ino:568,2556`; legacy `user_f9p.c:440–449` uses CFG-RATE |
| UARTxINPROT-RTCM3X | `0x10730004` | `0x10750004` | L, 1 | SparkFun `GNSS_ZED.ino:383,2068` |
| UARTxOUTPROT-NMEA | `0x10740002` | `0x10760002` | L, 1 | SparkFun `GNSS_ZED.ino:378,401` |
| UARTx-BAUDRATE | `0x40520001` | `0x40530001` | U4, bits/s | SparkFun `GNSS_ZED.ino:924,1965,1976`; X20P update sketch `GNSS_ZED.ino:35,533–540` provides the UART1 literal and width |
| UART2OUTPROT-UBX | — | `0x10760001` | L, 1 in existing factory-reset probe | `UbloxUbx.cpp:450–459`; implementer vendor cross-reference, independent numeric check open |
| UARTxOUTPROT-RTCM3X | `0x10740004` | `0x10760004` | L, existing base mode | SparkFun `GNSS_ZED.ino:379–380,401–403` |
| MSGOUT-NMEA_ID_DTM | `0x209100A7` | `0x209100A8` | U1, 0 | SparkFun `GNSS_ZED.h:161` |
| MSGOUT-NMEA_ID_RMC | `0x209100AC` | `0x209100AD` | U1, 0 or interval in epochs | SparkFun `GNSS_ZED.h:181`, `GNSS_ZED.ino:456,464` |
| MSGOUT-NMEA_ID_VTG | `0x209100B1` | `0x209100B2` | U1, 0 | SparkFun `GNSS_ZED.h:185`, `GNSS_ZED.ino:459,467` |
| MSGOUT-NMEA_ID_GNS | `0x209100B6` | `0x209100B7` | U1, 0 | SparkFun `GNSS_ZED.h:169` |
| MSGOUT-NMEA_ID_GGA | `0x209100BB` | `0x209100BC` | U1, 0 or interval in epochs | SparkFun `GNSS_ZED.h:165`, `GNSS_ZED.ino:453,461` |
| MSGOUT-NMEA_ID_GSA | `0x209100C0` | `0x209100C1` | U1, 0 or interval in epochs | SparkFun `GNSS_ZED.h:173`, `GNSS_ZED.ino:454,462` |
| MSGOUT-NMEA_ID_GSV | `0x209100C5` | `0x209100C6` | U1, 0 or interval in epochs | SparkFun `GNSS_ZED.h:177`, `GNSS_ZED.ino:455,463` |
| MSGOUT-NMEA_ID_GLL | `0x209100CA` | `0x209100CB` | U1, 0 | SparkFun `GNSS_ZED.h:167`, `GNSS_ZED.ino:458,466` |
| MSGOUT-NMEA_ID_GRS | `0x209100CF` | `0x209100D0` | U1, 0 | SparkFun `GNSS_ZED.h:171` |
| MSGOUT-NMEA_ID_GST | `0x209100D4` | `0x209100D5` | U1, 0 or interval in epochs | SparkFun `GNSS_ZED.h:175`, `GNSS_ZED.ino:457,465` |
| MSGOUT-NMEA_ID_ZDA | `0x209100D9` | `0x209100DA` | U1, 0 | SparkFun `GNSS_ZED.h:187` |
| MSGOUT-NMEA_ID_GBS | `0x209100DE` | `0x209100DF` | U1, 0 | SparkFun `GNSS_ZED.h:163` |
| NAVSPG-INFIL_MINELEV | `0x201100A4` | — | I1, degrees. U1 allows 0–90; 91 and 255 are rejected | SparkFun `GNSS_ZED.ino:603,2108–2115` |
| SIGNAL-GPS_ENA | `0x1031001F` | — | L, 0/1 in implementation. See RU1-02 | SparkFun `GNSS_ZED.h:32` |
| SIGNAL-SBAS_ENA | `0x10310020` | — | L, 0/1. See RU1-03 | SparkFun `GNSS_ZED.h:36` |
| SIGNAL-GAL_ENA | `0x10310021` | — | L, 0/1 | SparkFun `GNSS_ZED.h:30` |
| SIGNAL-BDS_ENA | `0x10310022` | — | L, 0/1 | SparkFun `GNSS_ZED.h:29` |
| SIGNAL-QZSS_ENA | `0x10310024` | — | L, follows requested GPS bit. See RU1-02 | SparkFun `GNSS_ZED.h:35` |
| SIGNAL-GLO_ENA | `0x10310025` | — | L, 0/1 | SparkFun `GNSS_ZED.h:31` |

Local references are the [SparkFun driver](../../../../SparkFun_RTK_Everywhere_Firmware/Firmware/RTK_Everywhere/GNSS_ZED.ino), [SparkFun key usage tables](../../../../SparkFun_RTK_Everywhere_Firmware/Firmware/RTK_Everywhere/GNSS_ZED.h), [SparkFun X20P update sketch](<../../../../SparkFun_RTK_Everywhere_Firmware/Firmware/Test Sketches/Flash_Update/X20P_Update/GNSS_ZED.ino>), [legacy P2 F9P driver](../../../../P2_V3.1_IDF/main/user_src/user_f9p.c), and [P2 v1.41 schematic](../hardware/P2/schematic/p2v1.41.pdf).

## Verification

| Review check | Result |
| --- | --- |
| `git diff --check 84025fd` and `sha256sum -c docs/frozen.sha256` | PASS |
| `clang-format --style=file --dry-run --Werror` on changed and untracked C/C++ files | PASS |
| `python3 -I tools/doc_check/doc_check.py --base 84025fd` with the cached lizard on PATH | PASS, 51 functions |
| Host CMake/Ninja + CTest under `builds/ru1-*` | PASS, GNSS all drivers 10/10, settings 2/2, main host 103/103 |
| Host ASan/UBSan, same three projects | PASS, 10/10, 2/2, 103/103. Used the repository's `-fno-sanitize=vptr` setting and `UBSAN_OPTIONS=halt_on_error=1`. Initial LeakSanitizer run hit the environment's ptrace restriction. The successful rerun used `ASAN_OPTIONS=detect_leaks=0`. Leak checking remains blocked. |
| Additional host probe against the rebuilt library | Reproduced RU1-01 and the early completion in RU1-04. Output is in `builds/ru1-gnss-all/repro.log`. |
| Existing ESP32 ELF and bin inspection | Current DRAM `96,595 B`, IRAM `116,139 B`, bin `1,520,368 B`. Disassembly shows 256 B own stack frame for `handleUbxFrame`, 224 B for `pollUbloxBoardSn`, 128 B each for the silence and constellation builders, and 560 B for `wait` on a 4096 B kIo stack. These are individual frames. Full call-chain peak, HIL high-water mark, and the baseline delta remain unverified. |
| Full IDF build matrix, live F9P, RTCM fixed solution and power-cycle persistence | NOT_RUN by reviewer. IDF matrix results are implementer evidence. |

The test additions cover full UBX frames, both configured UARTs at the encoder level, MINELEV boundaries, simple ACK/NAK/timeout sequences, MON-VER and SEC-UNIQID bounds, and the settings/web dispatch and serialization seams. The web and settings fixtures do not establish a successful receiver apply, NVS flash commit, or reboot. The most valuable missing regression tests exercise the real F9P startup failure path, invalid signal combinations, delayed ACKs, and persistence with an injected NVS failure.

## Verdict

**REJECT**
