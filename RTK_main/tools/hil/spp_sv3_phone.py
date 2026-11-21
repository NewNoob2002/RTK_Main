#!/usr/bin/env python3
"""SV3 板端 HIL：手机 0x06/0x33/0x24/0x08 经 settings::Service 的延迟应答（SV3.md §7 步骤 2）。

以 PHONE 角色连接 RTK SPP，验证：
  - 0x06 Survey 卫星追踪：查询 12 B；设置为延迟应答 01 01，回读一致（按接收机星座能力屏蔽）
  - 0x33 Survey PPP：查询 4 B；接收机不支持 PPP（CM680）时设置立即回 02 02，支持时延迟 01 01
  - 0x24 天线高：查询 16 B；合法值延迟 01 01 并回读；>10 m 立即 02 02
  - 0x08 Nav SN/PN：查询 48 B；设置延迟 01 01，0x01 设备信息立即反映新 SN
  - 断线：0x06/0x24/0x08 设置发出后立即断开，重复多次（超过 kMaxDeferredReplies=6），
    重连后仍能正常设置（无延迟应答槽/凭据泄漏）
结束时把被测项恢复为原值；--persist 时保留测试值并把期望值写入状态文件，
重启板子后用 --verify <文件> 检查持久化，并恢复原值。
用法：python3 -u -I tools/hil/spp_sv3_phone.py A0:B7:65:30:B5:F2 [--sn 20261121]
      python3 -u -I tools/hil/spp_sv3_phone.py A0:B7:65:30:B5:F2 --persist state.json
      （重启板子）python3 -u -I tools/hil/spp_sv3_phone.py A0:B7:65:30:B5:F2 --verify state.json
      python3 -u -I tools/hil/spp_sv3_phone.py A0:B7:65:30:B5:F2 --wifi
      （0x28：查询 Wi-Fi 状态/IP，非法值被拒，然后开启 → 01 01 → 板子重启进入 Wi-Fi 网页配置）
"""
import argparse
import json
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spp_gnss_sub  # noqa: E402

T_QUERY, T_QUERY_RES, T_SET, T_SET_RES = 0x00, 0x01, 0x02, 0x03
SENDER_NAV, SENDER_SURVEY = 0x00, 0x02  # Nav/Configure App = 0x00，其它 sender（如 0x02）= Survey App
OK, FAIL = b"\x01\x01", b"\x02\x02"
DEFERRED_TIMEOUT_S = 10.0
IMMEDIATE_MAX_S = 0.5
CUTOFF_MAX = 90


def build(cmd, typ, sender, payload=b""):
    """组 PHONE 请求帧（头部 20 B + 负载 + CRC32），sender 决定 Nav/Survey 分支。"""
    raw = bytearray(spp_gnss_sub.build(cmd, typ, 0, payload))
    raw[16] = sender
    body = bytes(raw[:-4])
    return body + struct.pack("<I", spp_gnss_sub.zlib.crc32(body) & 0xFFFFFFFF)


class Phone:
    def __init__(self, addr, ch):
        self.addr, self.ch = addr, ch
        self.sock = self.rx = None
        self.connect()

    def connect(self):
        for attempt in range(5):
            try:
                s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
                s.connect((self.addr, self.ch))
                break
            except OSError as exc:
                s.close()
                if attempt == 4:
                    raise SystemExit(f"connect failed: {exc}")
                time.sleep(1.0)
        s.settimeout(0.2)
        self.sock = s
        self.rx = spp_gnss_sub.MixRx(s)
        self.rx.start()

    def close(self, graceful=True):
        if graceful:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.sock.close()
        self.rx.join(2.0)

    def request(self, cmd, typ, sender, payload=b"", timeout=DEFERRED_TIMEOUT_S):
        """发送一帧并等待同 msg_id 的应答，返回 (耗时 s, type, payload) 或 None。"""
        with self.rx.cv:
            base = len(self.rx.frames)
        t0 = time.perf_counter()
        self.sock.sendall(build(cmd, typ, sender, payload))
        end = t0 + timeout
        with self.rx.cv:
            while True:
                for fr in self.rx.frames[base:]:
                    if fr[1] == cmd:
                        return fr[0] - t0, fr[2], fr[3]
                left = end - time.perf_counter()
                if left <= 0 or self.rx.closed:
                    return None
                self.rx.cv.wait(min(left, spp_gnss_sub.WAIT_SLICE_S))


class Report:
    def __init__(self):
        self.passed = self.failed = 0

    def check(self, name, cond, detail=""):
        if cond:
            self.passed += 1
            print(f"  PASS {name} {detail}")
        else:
            self.failed += 1
            print(f"  FAIL {name} {detail}")
        return cond


def query(ph, cmd, sender, expect_len):
    r = ph.request(cmd, T_QUERY, sender, timeout=3.0)
    if r is None or r[1] != T_QUERY_RES or len(r[2]) != expect_len:
        raise SystemExit(f"0x{cmd:02X} query failed: {r!r}")
    return r[2]


def set_expect(rep, ph, name, cmd, sender, payload, expect, deferred):
    """发送设置并检查应答内容；deferred=None 不检查时延，True 要求延迟应答，False 要求立即应答。"""
    r = ph.request(cmd, T_SET, sender, payload)
    if not rep.check(f"{name} reply", r is not None and r[1] == T_SET_RES and r[2] == expect,
                     f"got={None if r is None else (hex(r[1]), r[2].hex())} want={expect.hex()}"):
        return False
    if deferred is False:
        rep.check(f"{name} immediate", r[0] < IMMEDIATE_MAX_S, f"{r[0] * 1e3:.0f} ms")
    else:
        print(f"       {name} latency {r[0] * 1e3:.0f} ms")
    return True


def sat_payload(cfg):
    p = bytearray(12)
    p[1] = cfg["cutoff"]
    p[4:9] = bytes([cfg["gps"], cfg["glo"], cfg["bds"], cfg["gal"], cfg["sbas"]])
    return bytes(p)


def sat_decode(b):
    return {"cutoff": b[1], "gps": b[4], "glo": b[5], "bds": b[6], "gal": b[7], "sbas": b[8]}


def height_bytes(m):
    return struct.pack("<d", m)


def snpn_payload(sn, pn):
    return sn.encode().ljust(16, b"\0")[:16] + pn.encode().ljust(16, b"\0")[:16] + bytes(16)


def snpn_decode(b):
    return b[:16].rstrip(b"\0").decode(errors="replace"), b[16:32].rstrip(b"\0").decode(errors="replace")


def dev_sn(ph):
    return query(ph, 0x01, SENDER_NAV, 136)[8:24].rstrip(b"\0").decode(errors="replace")


def read_state(ph):
    sat = sat_decode(query(ph, 0x06, SENDER_SURVEY, 12))
    ppp = query(ph, 0x33, SENDER_SURVEY, 4)[0]
    height = struct.unpack_from("<d", query(ph, 0x24, SENDER_NAV, 16))[0]
    sn, pn = snpn_decode(query(ph, 0x08, SENDER_NAV, 48))
    return {"sat": sat, "ppp": ppp, "height": height, "sn": sn, "pn": pn, "dev_sn": dev_sn(ph)}


def test_sat(rep, ph, orig):
    print("[0x06 Survey sat track]")
    # 截止角翻到另一个值，星座位取反后由能力掩码屏蔽；回读与“设置值 & 能力”一致。
    want = dict(orig, cutoff=15 if orig["cutoff"] != 15 else 10, sbas=1 - orig["sbas"])
    if not set_expect(rep, ph, "set", 0x06, SENDER_SURVEY, sat_payload(want), OK, True):
        return None
    got = sat_decode(query(ph, 0x06, SENDER_SURVEY, 12))
    rep.check("readback cutoff", got["cutoff"] == want["cutoff"], f"{got}")
    masked_ok = all(got[k] in (0, want[k]) for k in ("gps", "glo", "bds", "gal", "sbas"))
    rep.check("readback constellations (caps-masked)", masked_ok, f"want={want}")
    bad = dict(want, cutoff=CUTOFF_MAX + 1)
    set_expect(rep, ph, "set cutoff=91 rejected", 0x06, SENDER_SURVEY, sat_payload(bad), FAIL, False)
    return got


def test_ppp(rep, ph, orig, ppp_supported):
    print("[0x33 Survey PPP]")
    if ppp_supported:
        want = 1 - orig
        set_expect(rep, ph, "set", 0x33, SENDER_SURVEY, bytes([want]), OK, True)
        rep.check("readback", query(ph, 0x33, SENDER_SURVEY, 4)[0] == want)
        set_expect(rep, ph, "restore", 0x33, SENDER_SURVEY, bytes([orig]), OK, True)
    else:
        rep.check("query unsupported returns 0", orig == 0, f"ppp={orig}")
        set_expect(rep, ph, "set on unsupported", 0x33, SENDER_SURVEY, b"\x01", FAIL, False)
        rep.check("still 0 after set", query(ph, 0x33, SENDER_SURVEY, 4)[0] == 0)
    set_expect(rep, ph, "set value 2 rejected", 0x33, SENDER_SURVEY, b"\x02", FAIL, False)


def test_height(rep, ph, orig):
    print("[0x24 antenna height]")
    want = 1.234 if abs(orig - 1.234) > 1e-9 else 1.5
    if not set_expect(rep, ph, "set", 0x24, SENDER_NAV, height_bytes(want), OK, True):
        return None
    got = struct.unpack_from("<d", query(ph, 0x24, SENDER_NAV, 16))[0]
    rep.check("readback", abs(got - want) < 1e-9, f"{got}")
    set_expect(rep, ph, "set 10.5 m rejected", 0x24, SENDER_NAV, height_bytes(10.5), FAIL, False)
    set_expect(rep, ph, "set NaN rejected", 0x24, SENDER_NAV, height_bytes(float("nan")), FAIL, False)
    return got


def test_snpn(rep, ph, orig_sn, orig_pn, final_sn):
    print("[0x08 Nav SN/PN]")
    tmp_sn, tmp_pn = final_sn + "-T", "PN-SV3HIL"
    if set_expect(rep, ph, "set temp", 0x08, SENDER_NAV, snpn_payload(tmp_sn, tmp_pn), OK, True):
        rep.check("readback temp", snpn_decode(query(ph, 0x08, SENDER_NAV, 48)) == (tmp_sn, tmp_pn))
        rep.check("0x01 reflects new SN immediately", dev_sn(ph) == tmp_sn, dev_sn(ph))
    set_expect(rep, ph, "set SN with control char rejected", 0x08, SENDER_NAV,
               snpn_payload("BAD\rSN", tmp_pn), FAIL, None)
    if set_expect(rep, ph, "set final", 0x08, SENDER_NAV, snpn_payload(final_sn, orig_pn), OK, True):
        rep.check("readback final", snpn_decode(query(ph, 0x08, SENDER_NAV, 48)) == (final_sn, orig_pn))
        rep.check("0x01 final SN", dev_sn(ph) == final_sn, dev_sn(ph))


def test_disconnect(rep, ph, rounds, sat_cfg, height, sn, pn):
    """延迟设置发出后立刻断开；多轮之后重连，延迟设置仍能完成。"""
    print(f"[disconnect during deferred set x{rounds}]")
    reqs = [
        (0x06, SENDER_SURVEY, sat_payload(sat_cfg)),
        (0x24, SENDER_NAV, height_bytes(height)),
        (0x08, SENDER_NAV, snpn_payload(sn, pn)),
    ]
    for i in range(rounds):
        cmd, snd, pl = reqs[i % len(reqs)]
        ph.sock.sendall(build(cmd, T_SET, snd, pl))
        time.sleep(0.005)
        ph.close(graceful=(i % 2 == 0))
        time.sleep(0.6)
        ph.connect()
    for cmd, snd, pl in reqs:
        set_expect(rep, ph, f"0x{cmd:02X} set after reconnects", cmd, snd, pl, OK, True)
    ok = query(ph, 0x01, SENDER_NAV, 136) is not None
    rep.check("link healthy after disconnect rounds", ok)


def test_wifi(rep, ph):
    print("[0x28 Wi-Fi web config]")
    for name, snd in (("nav", SENDER_NAV), ("survey", SENDER_SURVEY)):
        q = query(ph, 0x28, snd, 8)
        rep.check(f"{name} query off + ip", q == bytes([0, 192, 168, 10, 12, 0, 0, 0]), q.hex())
    set_expect(rep, ph, "set close (noop)", 0x28, SENDER_NAV, b"\x00\x00\x00\x00\x00", OK, False)
    set_expect(rep, ph, "set value 2 rejected", 0x28, SENDER_NAV, b"\x02", FAIL, False)
    set_expect(rep, ph, "set empty rejected", 0x28, SENDER_NAV, b"", FAIL, False)
    if set_expect(rep, ph, "set open", 0x28, SENDER_NAV, bytes([1, 192, 168, 4, 1]), OK, False):
        # 板子复位后 BlueZ 常要等监督超时才报 RFCOMM 断开，所以改为重连探测：
        # 进入 Wi-Fi 模式后蓝牙控制器已释放，新连接应失败。
        ph.close()
        time.sleep(3.0)
        s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        s.settimeout(10.0)
        try:
            s.connect((ph.addr, ph.ch))
            gone = False
        except OSError as exc:
            gone, why = True, str(exc)
        finally:
            s.close()
        rep.check("BT gone after restart (reconnect fails)", gone, why if gone else "reconnected")
        ph.connect = lambda: None  # 调用方 finally 里的 close() 已不需要
        ph.close = lambda graceful=True: None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr")
    ap.add_argument("--ch", type=int, default=1)
    ap.add_argument("--sn", default=None, help="最终保留的 SN（默认保持当前 SN）")
    ap.add_argument("--rounds", type=int, default=9, help="断线轮数（>6 覆盖延迟应答槽耗尽）")
    ap.add_argument("--ppp-supported", action="store_true", help="接收机支持 PPP（UM98x）")
    ap.add_argument("--persist", metavar="STATE", help="保留测试值并写状态文件，重启后用 --verify")
    ap.add_argument("--verify", metavar="STATE", help="重启后检查状态文件中的期望值并恢复原值")
    ap.add_argument("--wifi", action="store_true", help="只测 0x28，最后让板子重启进入 Wi-Fi 网页配置")
    args = ap.parse_args()

    rep = Report()
    ph = Phone(args.addr, args.ch)
    try:
        if args.wifi:
            test_wifi(rep, ph)
            print(f"RESULT {rep.passed} passed, {rep.failed} failed")
            return 1 if rep.failed else 0
        if args.verify:
            with open(args.verify, encoding="utf-8") as f:
                st = json.load(f)
            print("[persist verify after reboot]")
            cur = read_state(ph)
            for key in ("sat", "height", "sn", "pn", "dev_sn"):
                rep.check(f"{key} persisted", cur[key] == st["expect"][key], f"{cur[key]!r}")
            o = st["orig"]
            set_expect(rep, ph, "restore 0x06", 0x06, SENDER_SURVEY, sat_payload(o["sat"]), OK, None)
            set_expect(rep, ph, "restore 0x24", 0x24, SENDER_NAV, height_bytes(o["height"]), OK, None)
        else:
            orig = read_state(ph)
            print(f"orig: {orig}")
            final_sn = args.sn or orig["sn"] or orig["dev_sn"]
            sat_got = test_sat(rep, ph, orig["sat"])
            test_ppp(rep, ph, orig["ppp"], args.ppp_supported)
            h_got = test_height(rep, ph, orig["height"])
            test_snpn(rep, ph, orig["sn"], orig["pn"], final_sn)
            test_disconnect(rep, ph, args.rounds, sat_got or orig["sat"],
                            h_got if h_got is not None else orig["height"], final_sn, orig["pn"])
            if args.persist:
                expect = read_state(ph)
                with open(args.persist, "w", encoding="utf-8") as f:
                    json.dump({"orig": orig, "expect": expect}, f, indent=1)
                print(f"state saved to {args.persist}; reboot the board, then run --verify")
            else:
                set_expect(rep, ph, "restore 0x06", 0x06, SENDER_SURVEY, sat_payload(orig["sat"]), OK, None)
                set_expect(rep, ph, "restore 0x24", 0x24, SENDER_NAV, height_bytes(orig["height"]), OK, None)
        print(f"final: {read_state(ph)}")
    finally:
        ph.close()
    print(f"RESULT {rep.passed} passed, {rep.failed} failed")
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
