#!/usr/bin/env python3
"""K4 板端 HIL：PHONE GNSS 订阅/查询（0x04/0x05/0x1F/0x21/0x22/0x23）与 0x02 GNSS 字节。

连接 RTK 的 SPP（PHONE 角色），同一路流上既有协议帧又有原始语句（$..*hh\\r\\n / #..\\r\\n），
本脚本逐字节分拣两者，验证：
  - 0x02 字节 2 = 0x81（GNSS running）
  - interval=0 查询最近句；无缓存/超长回 02 02
  - interval=1 订阅 → 01 01，随后约 1 Hz 原始句；interval=-5 → 约 5 s 一次
  - interval=0 停止后不再推送；断开重连后订阅清空
  - 订阅期间并发 win16 请求 0 丢、帧不被语句截断（crc_bad=0、未识别字节=0）
用法：python3 -u -I tools/hil/spp_gnss_sub.py F8:B3:B7:25:D8:B2
"""
import argparse
import os
import socket
import struct
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spp_stress  # noqa: E402

HDR = spp_stress.HDR
WAIT_SLICE_S = spp_stress.WAIT_SLICE_S
T_QUERY, T_REPLY, T_SET = 0x00, 0x01, 0x02
SUB_IDS = {0x04: "GGA", 0x05: "SATS", 0x1F: "HEADING", 0x21: "GST", 0x22: "RMC", 0x23: "GSA"}


def build(cmd, typ, interval=0, payload=b""):
    h = HDR + struct.pack("<H", cmd) + b"\0" * 6 + struct.pack("<H", len(payload)) + b"\0\0"
    h += bytes([0x00, typ, 0, interval & 0xFF]) + payload
    return h + struct.pack("<I", zlib.crc32(h) & 0xFFFFFFFF)


class MixRx(threading.Thread):
    """分拣协议帧与原始语句；未识别字节计入 junk（任何帧被语句截断都会表现为 crc_bad/junk）。"""

    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock, self.buf = sock, bytearray()
        self.frames, self.lines = [], []  # (t, cmd, type, payload) / (t, bytes)
        self.crc_bad = self.junk = 0
        self.cv = threading.Condition()
        self.closed = False

    def run(self):
        while True:
            try:
                d = self.sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not d:
                break
            with self.cv:
                self.buf += d
                self._parse(time.perf_counter())
                self.cv.notify_all()
        with self.cv:
            self.closed = True
            self.cv.notify_all()

    def _parse(self, t):
        b = self.buf
        while b:
            if b[0] == 0xAA:
                if len(b) < 20:
                    if HDR.startswith(bytes(b[:4])):
                        return
                elif bytes(b[:4]) == HDR:
                    n = struct.unpack_from("<H", b, 12)[0]
                    if len(b) < 24 + n:
                        return
                    crc = struct.unpack_from("<I", b, 20 + n)[0]
                    if zlib.crc32(bytes(b[: 20 + n])) & 0xFFFFFFFF == crc:
                        self.frames.append((t, struct.unpack_from("<H", b, 4)[0], b[17], bytes(b[20 : 20 + n])))
                        del b[: 24 + n]
                        continue
                    self.crc_bad += 1
                self.junk += 1
                del b[:1]
            elif b[0] in (0x24, 0x23):  # '$' / '#'
                e = b.find(b"\r\n")
                if e < 0:
                    if len(b) > 8192:
                        self.junk += len(b)
                        del b[:]
                    return
                self.lines.append((t, bytes(b[: e + 2])))
                del b[: e + 2]
            else:
                self.junk += 1
                del b[:1]


class Phone:
    def __init__(self, addr, ch):
        self.sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        self.sock.connect((addr, ch))
        self.sock.settimeout(0.2)
        self.rx = MixRx(self.sock)
        self.rx.start()

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()
        self.rx.join(2)

    def send(self, cmd, typ, interval=0, payload=b""):
        self.sock.sendall(build(cmd, typ, interval, payload))

    def request(self, cmd, typ, interval=0, timeout=2.0):
        """发送后等待同 cmd 的应答帧（分片等待，避免长 wait 饿死 RX 线程）。"""
        with self.rx.cv:
            start = len(self.rx.frames)
        self.send(cmd, typ, interval)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.rx.cv:
                for f in self.rx.frames[start:]:
                    if f[1] == cmd:
                        return f
                self.rx.cv.wait(WAIT_SLICE_S)
        return None

    def sleep(self, s):
        end = time.monotonic() + s
        while time.monotonic() < end:
            with self.rx.cv:
                self.rx.cv.wait(WAIT_SLICE_S)

    def lines_since(self, t0, tag):
        with self.rx.cv:
            return [(t, ln) for t, ln in self.rx.lines if t >= t0 and tag in ln[:12]]


RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)


def periods(ts):
    return [round((b - a) * 1000) for a, b in zip(ts, ts[1:])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr")
    ap.add_argument("--ch", type=int, default=1)
    a = ap.parse_args()

    p = Phone(a.addr, a.ch)
    try:
        f = p.request(0x02, T_QUERY)
        check("0x02 gnss byte=0x81", f is not None and len(f[3]) > 2 and f[3][2] == 0x81,
              f"byte2={f[3][2]:#04x}" if f else "no reply")

        for cmd, name in SUB_IDS.items():
            f = p.request(cmd, T_QUERY, 0)
            if f is None:
                check(f"query {name}", False, "no reply")
            elif f[3] == b"\x02\x02":
                check(f"query {name}", cmd == 0x1F, "02 02 (no cache / >256B / no cap)")
            else:
                check(f"query {name}", f[3][:1] in (b"$", b"#") and len(f[3]) <= 256,
                      f"{len(f[3])}B {f[3][:20]!r}")

        # GGA 1 Hz
        t0 = time.perf_counter()
        f = p.request(0x04, T_SET, 1)
        check("sub GGA interval=1 -> 01 01", f is not None and f[3] == b"\x01\x01", repr(f[3] if f else None))
        p.sleep(6.5)
        gga = p.lines_since(t0, b"GGA")
        per = periods([t for t, _ in gga])
        check("GGA ~1Hz", 5 <= len(gga) <= 8 and all(700 <= x <= 1300 for x in per), f"n={len(gga)} periods={per}")

        # GSV/SATS 每 5 s
        t1 = time.perf_counter()
        f = p.request(0x05, T_SET, -5)
        check("sub SATS interval=-5 -> 01 01", f is not None and f[3] == b"\x01\x01", repr(f[3] if f else None))
        p.sleep(16)
        with p.rx.cv:
            sats = [(t, ln) for t, ln in p.rx.lines if t >= t1 and b"GGA" not in ln[:8]
                    and (b"GSV" in ln[:8] or b"SATSINFO" in ln[:12])]
        bursts = []
        for t, _ in sats:
            if not bursts or t - bursts[-1] > 1.0:
                bursts.append(t)
        check("SATS every 5s", 3 <= len(bursts) <= 4 and all(4000 <= x <= 6000 for x in periods(bursts)),
              f"lines={len(sats)} bursts={len(bursts)} periods={periods(bursts)}")

        # 并发：订阅期间 win16 请求不得丢、帧不得被语句截断
        for cmd in (0x21, 0x22, 0x23):
            p.request(cmd, T_SET, 10)
        with p.rx.cv:
            start = len(p.rx.frames)
        n, window, sent, t2 = 1500, 16, 0, time.perf_counter()
        while True:
            with p.rx.cv:
                got = sum(1 for fr in p.rx.frames[start:] if fr[1] == 0x01)
            if got >= n or time.perf_counter() - t2 > 60:
                break
            while sent < n and sent - got < window:
                p.send(0x01, T_QUERY)
                sent += 1
            p.sleep(0.005)
        dur = time.perf_counter() - t2
        raw = p.lines_since(t2, b"")
        check("concurrent win16 x1500 + 5 subs", got == n and p.rx.crc_bad == 0 and p.rx.junk == 0,
              f"replies={got}/{n} dur={dur:.2f}s rate={got / dur:.0f}/s raw_lines={len(raw)} "
              f"crc_bad={p.rx.crc_bad} junk={p.rx.junk}")

        # 停止：interval=0 查询并停推
        for cmd in SUB_IDS:
            if cmd != 0x1F:
                p.request(cmd, T_QUERY, 0)
        p.sleep(0.5)
        t3 = time.perf_counter()
        p.sleep(3.0)
        left = p.lines_since(t3, b"")
        check("stop all -> no raw lines", not left, f"n={len(left)}")

        # 断开前重新订阅 GGA，验证重连后已清空
        p.request(0x04, T_SET, 1)
        p.sleep(1.5)
    finally:
        p.close()
    time.sleep(1.0)
    p2 = Phone(a.addr, a.ch)
    try:
        t4 = time.perf_counter()
        p2.sleep(3.5)
        left = p2.lines_since(t4, b"")
        check("reconnect -> subscriptions cleared", not left, f"n={len(left)}")
        check("stream integrity", p2.rx.crc_bad == 0 and p2.rx.junk == 0, f"crc_bad={p2.rx.crc_bad} junk={p2.rx.junk}")
    finally:
        p2.close()

    bad = [n for n, ok in RESULTS if not ok]
    print(f"TOTAL pass={len(RESULTS) - len(bad)} fail={len(bad)} {bad}", flush=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
