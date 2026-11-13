#!/usr/bin/env python3
"""RTK_main SPP 压测：主机 BlueZ RFCOMM 扮演 PHONE，按协议 v1 发帧并按序校验应答。

协议没有逐请求序号（msg_id 即命令 ID），RTK 对单会话按序应答，因此用“期望应答序列”顺序比对：
  请求循环 = [0x07 设置(44B: seq+随机), 0x07 查询(回显 48B, 前 44B 必须等于刚设置的值), 0x01 查询(136B 定值)]
这样上行 payload 与下行 payload 都被端到端校验，丢失/乱序/篡改都能检出。

模式：
  rtt   窗口 1 串行，测往返时延分布
  win   窗口 W 流水线，测应答速率、上下行吞吐、丢失与内容错误
  up    上行洪泛：连续发 0x07 设置(payload 256B，帧 280B)不等应答，测上行极限与 rx_dropped
  bad   好帧之间混入坏 CRC/垃圾字节，验证解析器重同步且好帧应答不丢
"""
import argparse
import os
import socket
import struct
import threading
import time
import zlib

# 实测（Python 3.14 + BlueZ RFCOMM，2026-11-13）：主线程单次长 Condition.wait 期间接收线程收不到数据，
# 应答恰在 wait 超时返回时才到达，伪造出“idle 2s / 断开”；切成短片轮询后 win16×3000 0 丢失。
WAIT_SLICE_S = 0.05

HDR = b"\xAA\x44\x18\x14"


def build(cmd, typ, payload=b"", corrupt=False):
    h = HDR + struct.pack("<H", cmd) + b"\0" * 6 + struct.pack("<H", len(payload)) + b"\0\0"
    h += bytes([0x00, typ, 0, 0]) + payload
    crc = zlib.crc32(h) & 0xFFFFFFFF
    if corrupt:
        crc ^= 0xDEADBEEF
    return h + struct.pack("<I", crc)


class Rx(threading.Thread):
    """接收线程：流式重组帧（与固件同规则：找头、长度上限、CRC），记录到达时间。"""

    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock, self.buf = sock, bytearray()
        self.frames = []  # (t, cmd, sender, type, payload)
        self.crc_bad = self.resync_bytes = self.bytes = 0
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
            t = time.perf_counter()
            with self.cv:
                self.bytes += len(d)
                self.buf += d
                self._parse(t)
                self.cv.notify_all()
        with self.cv:
            self.closed = True
            self.cv.notify_all()

    def _parse(self, t):
        b = self.buf
        while True:
            i = b.find(HDR)
            if i < 0:
                keep = max(0, len(b) - 3)
                self.resync_bytes += keep
                del b[:keep]
                return
            if i:
                self.resync_bytes += i
                del b[:i]
            if len(b) < 20:
                return
            n = struct.unpack_from("<H", b, 12)[0]
            if n > 256:
                self.resync_bytes += 1
                del b[:1]
                continue
            if len(b) < 24 + n:
                return
            crc = struct.unpack_from("<I", b, 20 + n)[0]
            if zlib.crc32(bytes(b[: 20 + n])) & 0xFFFFFFFF != crc:
                self.crc_bad += 1
                del b[:1]
                continue
            self.frames.append((t, struct.unpack_from("<H", b, 4)[0], b[16], b[17], bytes(b[20 : 20 + n])))
            del b[: 24 + n]


class Session:
    def __init__(self, addr, ch):
        self.sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        t = time.perf_counter()
        self.sock.connect((addr, ch))
        self.connect_ms = (time.perf_counter() - t) * 1e3
        self.sock.settimeout(0.2)
        self.rx = Rx(self.sock)
        self.rx.start()
        self.ref01 = None

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def wait_frames(self, count, timeout):
        end = time.perf_counter() + timeout
        with self.rx.cv:
            while len(self.rx.frames) < count and not self.rx.closed:
                left = end - time.perf_counter()
                if left <= 0:
                    break
                self.rx.cv.wait(min(left, WAIT_SLICE_S))
            return len(self.rx.frames)

    def learn_ref(self):
        base = len(self.rx.frames)
        self.sock.sendall(build(0x01, 0x00))
        if self.wait_frames(base + 1, 2.0) <= base:
            raise SystemExit("no reply to 0x01 query: link not working as PHONE")
        fr = self.rx.frames[base]
        self.ref01 = fr[4]
        print(f"0x01 reply: sender={fr[2]:#x} type={fr[3]:#x} len={len(fr[4])} name={fr[4][:8].rstrip(bytes(1))!r} "
              f"sn={fr[4][8:24].rstrip(bytes(1))!r} fw={fr[4][32:48].rstrip(bytes(1))!r}")


def make_requests(n, set_len=44, seed=0):
    """生成 n 个请求及期望应答谓词。"""
    reqs = []
    last_set = None
    for i in range(n):
        k = i % 3
        if k == 0:
            pl = struct.pack("<I", seed + i) + os.urandom(set_len - 4)
            last_set = pl[:44]
            reqs.append((build(0x07, 0x02, pl), (0x07, 0x03, ("eq", b"\x01\x01"))))
        elif k == 1:
            reqs.append((build(0x07, 0x00), (0x07, 0x01, ("prefix", last_set))))
        else:
            reqs.append((build(0x01, 0x00), (0x01, 0x01, ("ref01", None))))
    return reqs


def match(fr, exp, ref01):
    _, cmd, snd, typ, pl = fr
    ecmd, etyp, (kind, val) = exp
    if cmd != ecmd or typ != etyp or snd != 0x01:
        return False
    if kind == "eq":
        return pl == val
    if kind == "prefix":
        return len(pl) == 48 and pl[:44] == val
    return pl == ref01


def evaluate(s, reqs, sent_t, frames):
    """顺序对齐：期望指针 k，回复与 exp[k] 匹配则前进；否则在前方 8 个内找，跳过的计为丢失。"""
    k = ok = bad = 0
    lost = 0
    lat = []
    for fr in frames:
        found = None
        for j in range(k, min(k + 8, len(reqs))):
            if match(fr, reqs[j][1], s.ref01):
                found = j
                break
        if found is None:
            bad += 1
            continue
        lost += found - k
        lat.append((fr[0] - sent_t[found]) * 1e3)
        ok += 1
        k = found + 1
    lost += len(sent_t) - k
    return ok, lost, bad, lat


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else float("nan")


def report(name, s, reqs, sent_t, t0, t1, up, down, start_idx):
    frames = s.rx.frames[start_idx:]
    ok, lost, bad, lat = evaluate(s, reqs, sent_t, frames)
    dt = t1 - t0
    print(f"[{name}] sent={len(sent_t)} replies={len(frames)} ok={ok} lost={lost} content_err={bad} "
          f"dur={dt:.2f}s rate={ok / dt:.1f} rep/s up={up / dt / 1024:.2f} KiB/s down={down / dt / 1024:.2f} KiB/s "
          f"lat_ms p50={pct(lat, .5):.1f} p95={pct(lat, .95):.1f} p99={pct(lat, .99):.1f} max={max(lat) if lat else 0:.1f} "
          f"host_crc_bad={s.rx.crc_bad} host_resync={s.rx.resync_bytes}")
    return dict(ok=ok, lost=lost, bad=bad)


def pipeline(s, n, window, set_len=44, name=None):
    reqs = make_requests(n, set_len, seed=int(time.time() * 1000) & 0xFFFF)
    start_idx = len(s.rx.frames)
    sent_t, up = [], 0
    rx0 = s.rx.bytes
    t0 = time.perf_counter()
    while True:
        with s.rx.cv:
            got = len(s.rx.frames) - start_idx
            if s.rx.closed:
                print("!! link closed by peer")
                break
        if len(sent_t) >= n and got >= n:
            break
        if len(sent_t) < n and len(sent_t) - got < window:
            f = reqs[len(sent_t)][0]
            sent_t.append(time.perf_counter())
            s.sock.sendall(f)
            up += len(f)
            continue
        with s.rx.cv:
            idle_t0 = time.perf_counter()
            while len(s.rx.frames) - start_idx == got and not s.rx.closed and time.perf_counter() - idle_t0 < 2.0:
                s.rx.cv.wait(WAIT_SLICE_S)
            if len(s.rx.frames) - start_idx == got:
                print(f"!! idle 2s with {len(sent_t) - got} outstanding; stop")
                break
    t1 = time.perf_counter()
    return report(name or f"win{window}", s, reqs, sent_t, t0, t1, up, s.rx.bytes - rx0, start_idx)


def up_flood(s, n, payload_len=256):
    """不等应答连续发送大帧；设置命令都回 01 01，最后发一个 0x07 查询验证最后一次设置值。"""
    start_idx = len(s.rx.frames)
    t0 = time.perf_counter()
    last = None
    up = 0
    for i in range(n):
        pl = struct.pack("<I", i) + os.urandom(payload_len - 4)
        last = pl[:44]
        f = build(0x07, 0x02, pl)
        s.sock.sendall(f)
        up += len(f)
    t_sent = time.perf_counter()
    s.sock.sendall(build(0x07, 0x00))
    s.wait_frames(start_idx + n + 1, 10.0)
    time.sleep(0.5)
    t1 = time.perf_counter()
    frames = s.rx.frames[start_idx:]
    acks = sum(1 for f in frames if f[1] == 0x07 and f[3] == 0x03 and f[4] == b"\x01\x01")
    q = [f for f in frames if f[1] == 0x07 and f[3] == 0x01]
    echo_ok = bool(q) and q[-1][4][:44] == last
    print(f"[up{payload_len}] frames_sent={n} frame_len={len(f)} host_send_time={t_sent - t0:.2f}s "
          f"acks={acks}/{n} lost={n - acks} final_echo_ok={echo_ok} up={up / (t1 - t0) / 1024:.2f} KiB/s(overall) "
          f"up_sendall={up / max(t_sent - t0, 1e-6) / 1024:.2f} KiB/s")
    return n - acks


def bad_mix(s, n):
    """好帧(0x01 查询)与坏 CRC 帧、垃圾字节、超长声明帧交替，好帧应答必须一个不少。"""
    start_idx = len(s.rx.frames)
    good = 0
    for i in range(n):
        r = i % 4
        if r == 0:
            s.sock.sendall(build(0x01, 0x00, corrupt=True))
        elif r == 1:
            s.sock.sendall(os.urandom(37))
        elif r == 2:
            junk = HDR + b"\0\0" + b"\0" * 6 + struct.pack("<H", 0x0400) + b"\0" * 6
            s.sock.sendall(junk)
        s.sock.sendall(build(0x01, 0x00))
        good += 1
        if i % 8 == 7:
            s.wait_frames(start_idx + good, 2.0)
    s.wait_frames(start_idx + good, 5.0)
    frames = s.rx.frames[start_idx:]
    okc = sum(1 for f in frames if f[1] == 0x01 and f[4] == s.ref01)
    print(f"[bad] good_sent={good} good_replies_ok={okc} lost={good - okc} other_replies={len(frames) - okc}")
    return good - okc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addr")
    ap.add_argument("--ch", type=int, default=1)
    ap.add_argument("--mode", default="all")
    ap.add_argument("-n", type=int, default=600)
    ap.add_argument("--windows", default="1,2,4,8,16")
    ap.add_argument("--flood", type=int, default=200)
    ap.add_argument("--duration", type=float, default=0, help="soak: repeat win8 until this many seconds")
    a = ap.parse_args()

    s = Session(a.addr, a.ch)
    print(f"connected in {s.connect_ms:.0f} ms")
    time.sleep(0.5)
    s.learn_ref()
    total_lost = total_bad = 0
    if a.mode in ("all", "rtt"):
        r = pipeline(s, 300, 1, name="rtt")
        total_lost += r["lost"]; total_bad += r["bad"]
    if a.mode in ("all", "win"):
        for w in map(int, a.windows.split(",")):
            r = pipeline(s, a.n, w)
            total_lost += r["lost"]; total_bad += r["bad"]
    if a.mode in ("all", "up"):
        total_lost += up_flood(s, a.flood, 256)
    if a.mode in ("all", "bad"):
        total_lost += bad_mix(s, 200)
    if a.duration > 0:
        end = time.time() + a.duration
        k = 0
        while time.time() < end:
            r = pipeline(s, 900, 8, name=f"soak{k}")
            total_lost += r["lost"]; total_bad += r["bad"]; k += 1
    print(f"TOTAL lost={total_lost} content_err={total_bad} host_crc_bad={s.rx.crc_bad}")
    s.close()


if __name__ == "__main__":
    main()
