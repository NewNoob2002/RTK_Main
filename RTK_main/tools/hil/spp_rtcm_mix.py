#!/usr/bin/env python3
"""蓝牙 SPP 单流混合负载上板测试工具：RTCM3 + 产品请求并发，串口比对板端计数（ADR-0006）。

主机以 BlueZ RFCOMM 扮演 PHONE 角色连接目标板，按真实基站历元或恒定字节率发送 RTCM3 帧，
并可选交错发送产品协议请求（0x07 设置/查询回显 + 0x01 查询）；测试前后经串口控制台读取
`rtcm:` 与 `stats:` 行并输出人类可读表格与 JSON 摘要。
"""

from __future__ import annotations

import argparse
import json
import random
import re
import select
import socket
import struct
import sys
import threading
import time
import zlib
from collections.abc import Callable, Sequence
from contextlib import suppress
from dataclasses import asdict, dataclass
from typing import Any

try:
    import serial  # type: ignore[import-untyped]
except ImportError:  # 允许无 pyserial 时离线运行 --selftest
    serial = None

# 产品 SPP 协议常量（docs/protocol/phone_protocol.md §1–§4）
PROD_HDR = b"\xAA\x44\x18\x14"
PROD_HEADER_LEN = 20
PROD_CRC_LEN = 4
PROD_MAX_PAYLOAD = 256
PROD_SENDER_APP = 0x00
PROD_SENDER_DEV = 0x01
PROD_TYPE_QUERY = 0x00
PROD_TYPE_QUERY_RES = 0x01
PROD_TYPE_SET = 0x02
PROD_TYPE_SET_RES = 0x03

# RTCM3 常量（ADR-0004 §4）
RTCM3_SYNC = 0xD3
RTCM3_MAX_PAYLOAD = 1023
CRC24Q_POLY = 0x1864CFB
CRC24Q_MASK = 0xFFFFFF

# --scan 速率档位（B/s）：2k / 5k / 10k / 20k / 40k
SCAN_RATES_BPS = (2000, 5000, 10000, 20000, 40000)

# 控制台 ANSI 转义序列清洗与状态行正则（ADR-0006 §2）
ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
RTCM_LINE_RE = re.compile(
    r"rtcm:\s+"
    r"frames=(\d+)\s+"
    r"bytes=(\d+)\s+"
    r"crc32=0x([0-9A-Fa-f]{8})\s+"
    r"last_msg=(\d+)\s+"
    r"rejected_role=(\d+)\s+"
    r"unknown_bytes=(\d+)\s+"
    r"product_rejects=(\d+)\s+"
    r"rtcm_rejects=(\d+)"
)
STATS_LINE_RE = re.compile(
    r"stats:\s+"
    r"frames_rx=(\d+)\s+"
    r"replies_tx=(\d+)\s+"
    r"write_errors=(\d+)\s+"
    r"pushes_tx=(\d+)\s+"
    r"disconnects=(\d+)\s+"
    r"rx_dropped=(\d+)"
)


def _build_crc24q_table() -> tuple[int, ...]:
    """生成 CRC-24Q（多项式 0x1864CFB，非反射，初值 0）256 项查找表。"""
    table: list[int] = []
    for byte_val in range(256):
        crc = byte_val << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= CRC24Q_POLY
        table.append(crc & CRC24Q_MASK)
    return tuple(table)


_CRC24Q_TABLE = _build_crc24q_table()


def crc24q_bitwise(data: bytes, init: int = 0) -> int:
    """逐位参考实现 CRC-24Q（供离线自检核对查找表）。"""
    crc = init & CRC24Q_MASK
    for byte_val in data:
        crc ^= byte_val << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= CRC24Q_POLY
        crc &= CRC24Q_MASK
    return crc


def crc24q(data: bytes, init: int = 0) -> int:
    """查表计算 CRC-24Q，支持分段累计，返回低 24 位无符号整数。"""
    crc = init & CRC24Q_MASK
    table = _CRC24Q_TABLE
    for byte_val in data:
        idx = ((crc >> 16) ^ byte_val) & 0xFF
        crc = ((crc << 8) ^ table[idx]) & CRC24Q_MASK
    return crc


def verify_crc24q_vector() -> None:
    """用标准测试向量 '123456789' -> 0xCDE703 自检 CRC-24Q。"""
    vec = b"123456789"
    got_table = crc24q(vec)
    got_bit = crc24q_bitwise(vec)
    if got_table != 0xCDE703 or got_bit != 0xCDE703:
        raise RuntimeError(
            f"CRC-24Q 自检失败: table=0x{got_table:06X} bit=0x{got_bit:06X} expected=0xCDE703"
        )


def _rand_bytes(rng: random.Random, count: int) -> bytes:
    """生成 count 个随机字节（兼容 Python 3.8+）。"""
    if count <= 0:
        return b""
    if hasattr(rng, "randbytes"):
        return rng.randbytes(count)
    return bytes(rng.getrandbits(8) for _ in range(count))


def build_rtcm3_frame(msg_num: int, payload_len: int, rng: random.Random) -> bytes:
    """构造一条合法 RTCM3 帧：D3 | 00+len(10bit) | payload(前12bit=msg_num) | CRC-24Q(3B大端)。"""
    if not (0 <= msg_num <= 0xFFF):
        raise ValueError(f"RTCM3 消息号超范围: {msg_num}")
    if not (2 <= payload_len <= RTCM3_MAX_PAYLOAD):
        raise ValueError(f"RTCM3 载荷长度须在 2..{RTCM3_MAX_PAYLOAD}: {payload_len}")

    # RTCM3 载荷前 12 bit 为大端消息号 DF002，低 4 bit 为随机填充
    b0 = (msg_num >> 4) & 0xFF
    b1 = ((msg_num & 0x0F) << 4) | (rng.randrange(16) & 0x0F)
    payload = bytes([b0, b1]) + _rand_bytes(rng, payload_len - 2)

    # 帧头第 2 字节高 6 位保留必须为 0
    hdr = bytes([RTCM3_SYNC, (payload_len >> 8) & 0x03, payload_len & 0xFF])
    body = hdr + payload
    crc = crc24q(body)
    trailer = struct.pack(">I", crc)[1:]
    return body + trailer


def build_rtcm_epoch(rng: random.Random) -> list[tuple[int, bytes]]:
    """按真实基站混合构建一个历元的 RTCM3 帧组：1005(19B)、1074/1084/1094/1124(100..400B)、1230(8B)。"""
    specs: list[tuple[int, int]] = [
        (1005, 19),
        (1074, rng.randint(100, 400)),
        (1084, rng.randint(100, 400)),
        (1094, rng.randint(100, 400)),
        (1124, rng.randint(100, 400)),
        (1230, 8),
    ]
    return [(msg_num, build_rtcm3_frame(msg_num, plen, rng)) for msg_num, plen in specs]


def build_product_frame(
    cmd: int,
    typ: int,
    payload: bytes = b"",
    sender: int = PROD_SENDER_APP,
    corrupt: bool = False,
) -> bytes:
    """按 phone_protocol.md §1 构造产品 SPP 帧（20B 头 + payload + 4B 小端 IEEE CRC32）。"""
    if len(payload) > PROD_MAX_PAYLOAD:
        raise ValueError(f"产品帧 payload 超限: {len(payload)} > {PROD_MAX_PAYLOAD}")
    hdr = (
        PROD_HDR
        + struct.pack("<H", cmd)
        + b"\x00" * 6
        + struct.pack("<H", len(payload))
        + b"\x00\x00"
        + bytes([sender & 0xFF, typ & 0xFF, 0, 0])
    )
    body = hdr + payload
    crc = zlib.crc32(body) & 0xFFFFFFFF
    if corrupt:
        crc ^= 0xDEADBEEF
    return body + struct.pack("<I", crc)


@dataclass
class ParsedProductFrame:
    rx_time: float
    cmd: int
    sender: int
    typ: int
    payload: bytes


class ProductStreamParser:
    """流式重组产品 SPP 应答帧，规则与固件端完全一致（AA 44 18 14、len<=256、IEEE CRC32）。"""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.frames: list[ParsedProductFrame] = []
        self.crc_bad = 0
        self.resync_bytes = 0
        self.total_bytes = 0

    def feed(self, data: bytes, rx_time: float) -> int:
        """喂入新收到的数据，返回本次新解析出的帧数。"""
        before = len(self.frames)
        self.total_bytes += len(data)
        self.buf.extend(data)
        b = self.buf
        while True:
            idx = b.find(PROD_HDR)
            if idx < 0:
                # 保留末尾至多 3 字节，避免跨分包的 AA 44 18 14 前缀丢失
                keep = min(len(b), 3)
                drop = len(b) - keep
                if drop > 0:
                    self.resync_bytes += drop
                    del b[:drop]
                break
            if idx > 0:
                self.resync_bytes += idx
                del b[:idx]
            if len(b) < PROD_HEADER_LEN:
                break
            plen = struct.unpack_from("<H", b, 12)[0]
            if plen > PROD_MAX_PAYLOAD:
                self.resync_bytes += 1
                del b[:1]
                continue
            total_len = PROD_HEADER_LEN + plen + PROD_CRC_LEN
            if len(b) < total_len:
                break
            crc_got = struct.unpack_from("<I", b, PROD_HEADER_LEN + plen)[0]
            crc_exp = zlib.crc32(bytes(b[: PROD_HEADER_LEN + plen])) & 0xFFFFFFFF
            if crc_got != crc_exp:
                self.crc_bad += 1
                del b[:1]
                continue
            cmd = struct.unpack_from("<H", b, 4)[0]
            sender = b[16]
            typ = b[17]
            payload = bytes(b[PROD_HEADER_LEN : PROD_HEADER_LEN + plen])
            self.frames.append(ParsedProductFrame(rx_time, cmd, sender, typ, payload))
            del b[:total_len]
        return len(self.frames) - before


class RxThread(threading.Thread):
    """独立蓝牙接收线程：使用 select 轮询避免过短 socket 超时影响写侧流控。"""

    def __init__(self, sock: socket.socket) -> None:
        super().__init__(daemon=True)
        self.sock = sock
        self.parser = ProductStreamParser()
        self.cv = threading.Condition()
        self.closed = False
        self._stop_req = False

    def request_stop(self) -> None:
        self._stop_req = True

    def run(self) -> None:
        while not self._stop_req:
            try:
                readable, _, _ = select.select([self.sock], [], [], 0.1)
            except (OSError, ValueError):
                break
            if not readable:
                continue
            try:
                chunk = self.sock.recv(4096)
            except TimeoutError:
                continue
            except OSError:
                break
            if not chunk:
                break
            now = time.perf_counter()
            with self.cv:
                self.parser.feed(chunk, now)
                self.cv.notify_all()
        with self.cv:
            self.closed = True
            self.cv.notify_all()


class BtSession:
    """BlueZ RFCOMM 会话封装（PHONE 角色）。"""

    def __init__(self, addr: str, channel: int) -> None:
        self.addr = addr
        self.channel = channel
        self.sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
        t0 = time.perf_counter()
        self.sock.connect((addr, channel))
        self.connect_ms = (time.perf_counter() - t0) * 1e3
        # 发送超时设大，允许高负载下 RFCOMM 背压正常流控，读线程通过 select 定时唤醒
        self.sock.settimeout(5.0)
        self.rx = RxThread(self.sock)
        self.rx.start()
        self.ref01: bytes | None = None

    def close(self) -> None:
        # 先仅关闭写端并短暂等待，避免直接 SHUT_RDWR 导致 Linux RFCOMM 丢弃发送队列中的末尾数据
        with suppress(OSError):
            self.sock.shutdown(socket.SHUT_WR)
            time.sleep(0.2)
        self.rx.request_stop()
        with suppress(OSError):
            self.sock.shutdown(socket.SHUT_RDWR)
        with suppress(OSError):
            self.sock.close()
        self.rx.join(timeout=1.0)

    def frame_count(self) -> int:
        with self.rx.cv:
            return len(self.rx.parser.frames)

    def wait_frames(self, target_count: int, timeout_s: float) -> int:
        deadline = time.perf_counter() + timeout_s
        with self.rx.cv:
            while len(self.rx.parser.frames) < target_count and not self.rx.closed:
                left = deadline - time.perf_counter()
                if left <= 0:
                    break
                self.rx.cv.wait(left)
            return len(self.rx.parser.frames)

    def learn_ref01(self) -> bytes:
        """发送一次 0x01 查询学习设备信息定值应答，同时验证 PHONE 角色通信正常。"""
        base = self.frame_count()
        self.sock.sendall(build_product_frame(0x01, PROD_TYPE_QUERY))
        if self.wait_frames(base + 1, 2.5) <= base:
            raise RuntimeError("0x01 查询无应答：请确认目标 MAC 为非报警器白名单地址（PHONE 角色）且链路正常")
        with self.rx.cv:
            fr = self.rx.parser.frames[base]
        if fr.cmd != 0x01 or fr.sender != PROD_SENDER_DEV or fr.typ != PROD_TYPE_QUERY_RES:
            raise RuntimeError(f"0x01 应答头异常: cmd=0x{fr.cmd:02X} sender=0x{fr.sender:02X} type=0x{fr.typ:02X}")
        self.ref01 = fr.payload
        return fr.payload


@dataclass
class ExpectedReply:
    cmd: int
    typ: int
    kind: str  # "eq" | "prefix" | "ref01"
    expected_bytes: bytes | None


def make_cycle_request(
    seq: int,
    rng: random.Random,
    last_set_ref: list[bytes | None],
) -> tuple[bytes, ExpectedReply]:
    """按 [0x07 SET(44B), 0x07 QUERY(回显48B), 0x01 QUERY(136B)] 循环生成下一条产品请求。"""
    step = seq % 3
    if step == 0:
        payload = struct.pack("<I", seq) + _rand_bytes(rng, 40)
        last_set_ref[0] = payload
        return (
            build_product_frame(0x07, PROD_TYPE_SET, payload),
            ExpectedReply(0x07, PROD_TYPE_SET_RES, "eq", b"\x01\x01"),
        )
    if step == 1:
        if last_set_ref[0] is None:
            raise RuntimeError("内部状态错误：0x07 查询前未执行 0x07 设置")
        return (
            build_product_frame(0x07, PROD_TYPE_QUERY),
            ExpectedReply(0x07, PROD_TYPE_QUERY_RES, "prefix", last_set_ref[0]),
        )
    return (
        build_product_frame(0x01, PROD_TYPE_QUERY),
        ExpectedReply(0x01, PROD_TYPE_QUERY_RES, "ref01", None),
    )


def match_product_reply(fr: ParsedProductFrame, exp: ExpectedReply, ref01: bytes | None) -> bool:
    """校验单条产品应答是否匹配期望。"""
    if fr.cmd != exp.cmd or fr.typ != exp.typ or fr.sender != PROD_SENDER_DEV:
        return False
    if exp.kind == "eq":
        return fr.payload == exp.expected_bytes
    if exp.kind == "prefix":
        return exp.expected_bytes is not None and len(fr.payload) == 48 and fr.payload[:44] == exp.expected_bytes
    if exp.kind == "ref01":
        return ref01 is not None and fr.payload == ref01
    return False


def evaluate_product_replies(
    expected_list: Sequence[ExpectedReply],
    sent_times: Sequence[float],
    rx_frames: Sequence[ParsedProductFrame],
    ref01: bytes | None,
) -> dict[str, Any]:
    """按顺序对齐匹配产品应答，统计 RTT p50/p95/p99、丢失与内容错误。"""
    cursor = 0
    ok = 0
    lost = 0
    content_err = 0
    lat_ms: list[float] = []

    for fr in rx_frames:
        found: int | None = None
        for j in range(cursor, min(cursor + 8, len(expected_list))):
            if match_product_reply(fr, expected_list[j], ref01):
                found = j
                break
        if found is None:
            content_err += 1
            continue
        lost += found - cursor
        lat_ms.append((fr.rx_time - sent_times[found]) * 1e3)
        ok += 1
        cursor = found + 1

    lost += len(sent_times) - cursor
    return {
        "sent": len(sent_times),
        "replies": len(rx_frames),
        "ok": ok,
        "lost": lost,
        "content_err": content_err,
        "rtt_p50_ms": percentile(lat_ms, 0.50),
        "rtt_p95_ms": percentile(lat_ms, 0.95),
        "rtt_p99_ms": percentile(lat_ms, 0.99),
        "rtt_max_ms": max(lat_ms) if lat_ms else None,
    }


def percentile(values: Sequence[float], q: float) -> float | None:
    if not values:
        return None
    s = sorted(values)
    idx = min(len(s) - 1, int(len(s) * q))
    return s[idx]


@dataclass
class RtcmConsoleStats:
    frames: int
    bytes: int
    crc32: int
    last_msg: int
    rejected_role: int
    unknown_bytes: int
    product_rejects: int
    rtcm_rejects: int


@dataclass
class LinkConsoleStats:
    frames_rx: int
    replies_tx: int
    write_errors: int
    pushes_tx: int
    disconnects: int
    rx_dropped: int


@dataclass
class ConsoleSnapshot:
    rtcm: RtcmConsoleStats
    stats: LinkConsoleStats
    raw_text: str


def strip_ansi(text: str) -> str:
    return ANSI_ESCAPE_RE.sub("", text)


def parse_console_status(text: str) -> ConsoleSnapshot:
    """从控制台 `status` 输出中解析最后一组 `rtcm:` 与 `stats:` 行（ADR-0006 §2）。"""
    clean = strip_ansi(text)
    rtcm_matches = list(RTCM_LINE_RE.finditer(clean))
    stats_matches = list(STATS_LINE_RE.finditer(clean))

    if not rtcm_matches:
        raise ValueError(f"控制台输出缺少符合 ADR-0006 §2 格式的 'rtcm:' 行；原始片段: {clean[-300:]!r}")
    if not stats_matches:
        raise ValueError(f"控制台输出缺少 'stats:' 行；原始片段: {clean[-300:]!r}")

    rm = rtcm_matches[-1]
    sm = stats_matches[-1]
    rtcm = RtcmConsoleStats(
        frames=int(rm.group(1)),
        bytes=int(rm.group(2)),
        crc32=int(rm.group(3), 16),
        last_msg=int(rm.group(4)),
        rejected_role=int(rm.group(5)),
        unknown_bytes=int(rm.group(6)),
        product_rejects=int(rm.group(7)),
        rtcm_rejects=int(rm.group(8)),
    )
    stats = LinkConsoleStats(
        frames_rx=int(sm.group(1)),
        replies_tx=int(sm.group(2)),
        write_errors=int(sm.group(3)),
        pushes_tx=int(sm.group(4)),
        disconnects=int(sm.group(5)),
        rx_dropped=int(sm.group(6)),
    )
    return ConsoleSnapshot(rtcm=rtcm, stats=stats, raw_text=clean)


class SerialConsole:
    """ESP-IDF UART0 控制台接口（2 Mbaud，ADR-0009 §2.6）。"""

    def __init__(self, port: str, baud: int = 2000000) -> None:
        if serial is None:
            raise RuntimeError(
                "未安装 pyserial；请使用 ESP-IDF Python 环境运行（如 ~/.espressif/python_env/idf*_env/bin/python）"
            )
        self.port = port
        try:
            self.ser = serial.Serial(port, baudrate=baud, timeout=0.1, write_timeout=1.0)
        except Exception as exc:
            raise RuntimeError(
                f"串口 {port} 打开失败（可能被 idf.py monitor / 其他进程占用，或无设备权限）: {exc}"
            ) from exc

    def close(self) -> None:
        with suppress(OSError):
            self.ser.close()

    def _drain_input(self) -> None:
        with suppress(OSError):
            self.ser.reset_input_buffer()
        deadline = time.monotonic() + 0.08
        while time.monotonic() < deadline:
            waiting = getattr(self.ser, "in_waiting", 0)
            if waiting <= 0:
                break
            _ = self.ser.read(waiting)

    def _read_until(self, done_fn: Callable[[str], bool], timeout_s: float) -> str:
        buf = bytearray()
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            waiting = getattr(self.ser, "in_waiting", 0)
            chunk = self.ser.read(waiting if waiting > 0 else 1)
            if chunk:
                buf.extend(chunk)
                text = strip_ansi(buf.decode("utf-8", errors="replace"))
                if done_fn(text):
                    return text
        return strip_ansi(buf.decode("utf-8", errors="replace"))

    def reset_rtcm(self) -> None:
        """发送 `rtcm reset\\r` 并等待 `OK` 与控制台回到提示符。"""
        self._drain_input()
        self.ser.write(b"rtcm reset\r")
        self.ser.flush()
        text = self._read_until(
            lambda t: bool(re.search(r"(?m)^(?:OK|ERR\b[^\r\n]*)\r?$", t) and ("console>" in t)),
            timeout_s=1.5,
        )
        if not re.search(r"(?m)^OK\r?$", text):
            raise RuntimeError(f"rtcm reset 未收到 OK: {text!r}")

    def read_status(self) -> ConsoleSnapshot:
        """发送 `status rtcm\\r` 并解析 `rtcm:` 与 `stats:` 行。"""
        self._drain_input()
        self.ser.write(b"status rtcm\r")
        self.ser.flush()
        text = self._read_until(
            lambda t: bool(RTCM_LINE_RE.search(t) and STATS_LINE_RE.search(t) and ("console>" in t)),
            timeout_s=2.5,
        )
        return parse_console_status(text)


def evaluate_stage_result(
    stage_name: str,
    target_rate_bps: int | None,
    epoch_hz: float,
    burst: bool,
    product_mode: str,
    tx_elapsed_s: float,
    host_rtcm_frames: int,
    host_rtcm_bytes: int,
    host_rtcm_crc32: int,
    host_product_tx_bytes: int,
    prod_eval: dict[str, Any],
    host_crc_bad: int,
    host_resync: int,
    before: ConsoleSnapshot | None,
    after: ConsoleSnapshot | None,
) -> dict[str, Any]:
    """整合发送端与板端指标，给出明确 PASS / OVERFLOW_DROPPED / FAIL 判定。"""
    dur = max(tx_elapsed_s, 1e-6)
    actual_rtcm_tx_bps = host_rtcm_bytes / dur
    actual_total_tx_bps = (host_rtcm_bytes + host_product_tx_bytes) / dur

    product_ok = prod_eval["lost"] == 0 and prod_eval["content_err"] == 0 and host_crc_bad == 0

    board_dict: dict[str, Any] | None = None
    rtcm_frames_match: bool | None = None
    rtcm_bytes_match: bool | None = None
    rtcm_crc32_match: bool | None = None
    rx_dropped_delta: int | None = None
    board_rtcm_rx_bps: float | None = None
    verdict_note = ""

    if before is not None and after is not None:
        rx_dropped_delta = after.stats.rx_dropped - before.stats.rx_dropped
        frames_rx_delta = after.stats.frames_rx - before.stats.frames_rx
        replies_tx_delta = after.stats.replies_tx - before.stats.replies_tx

        rtcm_frames_match = after.rtcm.frames == host_rtcm_frames
        rtcm_bytes_match = after.rtcm.bytes == host_rtcm_bytes
        rtcm_crc32_match = after.rtcm.crc32 == host_rtcm_crc32
        rtcm_all_match = rtcm_frames_match and rtcm_bytes_match and rtcm_crc32_match
        board_rtcm_rx_bps = after.rtcm.bytes / dur

        board_dict = {
            "before_rtcm": asdict(before.rtcm),
            "after_rtcm": asdict(after.rtcm),
            "after_rtcm_crc32_hex": f"0x{after.rtcm.crc32:08X}",
            "before_stats": asdict(before.stats),
            "after_stats": asdict(after.stats),
            "rx_dropped_delta": rx_dropped_delta,
            "frames_rx_delta": frames_rx_delta,
            "replies_tx_delta": replies_tx_delta,
            "rtcm_rx_bps": round(board_rtcm_rx_bps, 2),
        }

        if rx_dropped_delta > 0:
            if not rtcm_all_match:
                verdict_note = "检测到板端 rx_dropped > 0（RX 溢出丢字节），rtcm 计数/crc32 不匹配属于预期"
            else:
                verdict_note = "检测到板端 rx_dropped > 0，但本次观测周期内 rtcm 计数仍一致"
            verdict = "OVERFLOW_DROPPED" if product_ok else "FAIL"
            if not product_ok:
                verdict_note += "；但产品请求出现丢失或内容错误"
        else:
            if rtcm_all_match and product_ok and after.rtcm.rejected_role == 0:
                verdict = "PASS"
                verdict_note = "无 rx_dropped，RTCM 帧数/字节/CRC32 与发送端完全一致，产品请求 0 丢 0 错"
            else:
                verdict = "FAIL"
                reasons: list[str] = []
                if not rtcm_all_match:
                    reasons.append("rx_dropped==0 但 RTCM 帧数/字节/CRC32 不匹配")
                if not product_ok:
                    reasons.append(
                        f"产品异常(lost={prod_eval['lost']}, err={prod_eval['content_err']}, "
                        f"host_crc_bad={host_crc_bad})"
                    )
                if after.rtcm.rejected_role != 0:
                    reasons.append(f"PHONE 角色出现 rejected_role={after.rtcm.rejected_role}")
                verdict_note = "；".join(reasons)
    else:
        verdict = "PASS_NO_SERIAL" if product_ok else "FAIL"
        verdict_note = "未接入串口，仅核对蓝牙侧发送与产品应答"

    return {
        "stage": stage_name,
        "config": {
            "target_rtcm_rate_bps": target_rate_bps,
            "epoch_hz": epoch_hz if target_rate_bps is None else None,
            "burst": burst if target_rate_bps is None else False,
            "product_mode": product_mode,
        },
        "elapsed_s": round(tx_elapsed_s, 3),
        "host_rtcm": {
            "frames": host_rtcm_frames,
            "bytes": host_rtcm_bytes,
            "crc32": host_rtcm_crc32,
            "crc32_hex": f"0x{host_rtcm_crc32:08X}",
            "tx_bps": round(actual_rtcm_tx_bps, 2),
        },
        "host_total_tx_bps": round(actual_total_tx_bps, 2),
        "host_rx": {
            "crc_bad": host_crc_bad,
            "resync_bytes": host_resync,
        },
        "product": prod_eval,
        "match": {
            "rtcm_frames_match": rtcm_frames_match,
            "rtcm_bytes_match": rtcm_bytes_match,
            "rtcm_crc32_match": rtcm_crc32_match,
        },
        "board": board_dict,
        "verdict": verdict,
        "note": verdict_note,
    }


def run_single_stage(
    stage_name: str,
    session: BtSession,
    console: SerialConsole | None,
    duration_s: float,
    epoch_hz: float,
    rtcm_rate_bps: int | None,
    burst: bool,
    product_mode: str,
    product_hz: float,
    product_inflight: int,
    rng: random.Random,
    tail_wait_s: float = 1.5,
) -> dict[str, Any]:
    """执行单轮发送与采集，返回评估摘要字典。"""
    if product_mode == "cycle" and session.ref01 is None:
        # 在串口清零和采集基线前先完成一次 0x01 学习，避免污染测试期内的 frames_rx 增量
        session.learn_ref01()

    before: ConsoleSnapshot | None = None
    if console is not None:
        console.reset_rtcm()
        before = console.read_status()

    rx_start_idx = session.frame_count()
    crc_bad_start = session.rx.parser.crc_bad
    resync_start = session.rx.parser.resync_bytes

    host_rtcm_frames = 0
    host_rtcm_bytes = 0
    host_rtcm_crc32 = 0
    host_product_tx_bytes = 0

    expected_list: list[ExpectedReply] = []
    sent_times: list[float] = []
    last_set_ref: list[bytes | None] = [None]
    prod_seq = 0

    # 预先组装排队函数，确保每一条帧都是完整写出
    pending_epoch: list[bytes] = []

    def next_rtcm_frame() -> bytes:
        if not pending_epoch:
            pending_epoch.extend(fr for _, fr in build_rtcm_epoch(rng))
        return pending_epoch.pop(0)

    t_start = time.perf_counter()
    t_end = t_start + duration_s
    next_rtcm_t = t_start
    next_prod_t = t_start
    prod_period = 1.0 / max(product_hz, 0.1)
    epoch_period = 1.0 / max(epoch_hz, 0.1)

    def maybe_send_product(now_t: float) -> None:
        nonlocal next_prod_t, prod_seq, host_product_tx_bytes
        if product_mode != "cycle":
            return
        if now_t < next_prod_t:
            return
        got = session.frame_count() - rx_start_idx
        inflight = len(sent_times) - got
        if inflight >= product_inflight:
            return
        wire, exp = make_cycle_request(prod_seq, rng, last_set_ref)
        prod_seq += 1
        t_tx = time.perf_counter()
        session.sock.sendall(wire)
        sent_times.append(t_tx)
        expected_list.append(exp)
        host_product_tx_bytes += len(wire)
        # 避免长时间背压后连发补偿过多请求
        next_prod_t = max(next_prod_t + prod_period, t_tx)

    while True:
        now = time.perf_counter()
        if now >= t_end:
            break
        with session.rx.cv:
            if session.rx.closed:
                raise RuntimeError("测试期间蓝牙 RFCOMM 连接被对端断开")

        if rtcm_rate_bps is not None:
            # 恒定字节率模式：按字节数推进目标时间戳，逐条完整 RTCM 帧写出
            if now >= next_rtcm_t:
                frame = next_rtcm_frame()
                session.sock.sendall(frame)
                host_rtcm_frames += 1
                host_rtcm_bytes += len(frame)
                host_rtcm_crc32 = zlib.crc32(frame, host_rtcm_crc32) & 0xFFFFFFFF
                t_after = time.perf_counter()
                next_rtcm_t = max(next_rtcm_t + (len(frame) / float(rtcm_rate_bps)), t_after - 0.05)
                maybe_send_product(t_after)
                continue
            maybe_send_product(now)
        elif burst:
            # 历元突发模式：每个历元一次性写出整组 6 条帧
            if now >= next_rtcm_t:
                epoch_frames = [fr for _, fr in build_rtcm_epoch(rng)]
                blob = b"".join(epoch_frames)
                session.sock.sendall(blob)
                host_rtcm_frames += len(epoch_frames)
                host_rtcm_bytes += len(blob)
                host_rtcm_crc32 = zlib.crc32(blob, host_rtcm_crc32) & 0xFFFFFFFF
                next_rtcm_t += epoch_period
            maybe_send_product(time.perf_counter())
        else:
            # 非突发历元模式：在一个历元周期内均分 6 条帧写出，中间穿插产品请求
            if now >= next_rtcm_t:
                frame = next_rtcm_frame()
                session.sock.sendall(frame)
                host_rtcm_frames += 1
                host_rtcm_bytes += len(frame)
                host_rtcm_crc32 = zlib.crc32(frame, host_rtcm_crc32) & 0xFFFFFFFF
                next_rtcm_t += epoch_period / 6.0
                maybe_send_product(time.perf_counter())
                continue
            maybe_send_product(now)

        wake_t = min(next_rtcm_t, next_prod_t) if product_mode == "cycle" else next_rtcm_t
        sleep_s = min(wake_t, t_end) - time.perf_counter()
        if sleep_s > 0.0005:
            time.sleep(min(sleep_s, 0.02))

    tx_elapsed = time.perf_counter() - t_start

    # 等待在途产品应答回完，同时无论是否有串口或产品负载均至少等待 tail_wait_s，
    # 确保主机内核 RFCOMM 发送队列和板端 RX 缓冲充分排空后再读板端状态或关闭蓝牙
    tail_wait = max(0.0, tail_wait_s)
    drain_start = time.perf_counter()
    if product_mode == "cycle" and expected_list and tail_wait > 0.0:
        session.wait_frames(rx_start_idx + len(expected_list), tail_wait)
    drain_elapsed = time.perf_counter() - drain_start
    if drain_elapsed < tail_wait:
        time.sleep(tail_wait - drain_elapsed)

    after: ConsoleSnapshot | None = None
    if console is not None:
        after = console.read_status()

    with session.rx.cv:
        rx_frames = list(session.rx.parser.frames[rx_start_idx:])
        crc_bad_delta = session.rx.parser.crc_bad - crc_bad_start
        resync_delta = session.rx.parser.resync_bytes - resync_start

    prod_eval = evaluate_product_replies(expected_list, sent_times, rx_frames, session.ref01)
    return evaluate_stage_result(
        stage_name=stage_name,
        target_rate_bps=rtcm_rate_bps,
        epoch_hz=epoch_hz,
        burst=burst,
        product_mode=product_mode,
        tx_elapsed_s=tx_elapsed,
        host_rtcm_frames=host_rtcm_frames,
        host_rtcm_bytes=host_rtcm_bytes,
        host_rtcm_crc32=host_rtcm_crc32,
        host_product_tx_bytes=host_product_tx_bytes,
        prod_eval=prod_eval,
        host_crc_bad=crc_bad_delta,
        host_resync=resync_delta,
        before=before,
        after=after,
    )


def format_summary_table(results: Sequence[dict[str, Any]]) -> str:
    """将单轮或 --scan 结果格式化为人类可读表格。"""
    headers = (
        "stage",
        "tx_B/s",
        "rx_B/s",
        "rtcm_match(F/B/C)",
        "rx_drop",
        "prod_ok/sent",
        "lost",
        "rtt_p50/p99(ms)",
        "verdict",
    )
    rows: list[tuple[str, ...]] = [headers]
    for r in results:
        b = r["board"]
        m = r["match"]
        p = r["product"]
        if b is not None:
            rx_bps = f"{b['rtcm_rx_bps']:.0f}"
            match_str = (
                f"{'Y' if m['rtcm_frames_match'] else 'N'}/"
                f"{'Y' if m['rtcm_bytes_match'] else 'N'}/"
                f"{'Y' if m['rtcm_crc32_match'] else 'N'}"
            )
            drop_str = str(b["rx_dropped_delta"])
        else:
            rx_bps = "-"
            match_str = "-/-/-"
            drop_str = "-"
        p50 = f"{p['rtt_p50_ms']:.1f}" if p["rtt_p50_ms"] is not None else "-"
        p99 = f"{p['rtt_p99_ms']:.1f}" if p["rtt_p99_ms"] is not None else "-"
        rows.append(
            (
                str(r["stage"]),
                f"{r['host_rtcm']['tx_bps']:.0f}",
                rx_bps,
                match_str,
                drop_str,
                f"{p['ok']}/{p['sent']}",
                str(p["lost"]),
                f"{p50}/{p99}",
                str(r["verdict"]),
            )
        )

    widths = [max(len(row[i]) for row in rows) for i in range(len(headers))]
    lines: list[str] = []
    for idx, row in enumerate(rows):
        lines.append("  ".join(row[i].ljust(widths[i]) for i in range(len(headers))))
        if idx == 0:
            lines.append("  ".join("-" * w for w in widths))
    for r in results:
        lines.append(f"[{r['stage']}] {r['note']}")
    return "\n".join(lines)


def _check(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def run_selftest() -> int:
    """无板离线自检：CRC-24Q、RTCM3 帧生成、产品帧编解码与对齐、控制台正则解析、判定逻辑。"""
    # 1. CRC-24Q 标准向量 "123456789" -> 0xCDE703 与逐位参考实现交叉核对
    verify_crc24q_vector()
    _check(crc24q(b"") == 0, "空输入 CRC-24Q 应为 0")
    rng = random.Random(20261111)
    for length in (1, 7, 64, 513):
        sample = _rand_bytes(rng, length)
        _check(crc24q(sample) == crc24q_bitwise(sample), f"CRC-24Q 查表与逐位不一致 len={length}")
        split = length // 2
        _check(crc24q(sample[split:], crc24q(sample[:split])) == crc24q(sample), "CRC-24Q 分段累计不一致")

    # 2. RTCM3 历元生成与格式校验
    epoch = build_rtcm_epoch(rng)
    _check([msg for msg, _ in epoch] == [1005, 1074, 1084, 1094, 1124, 1230], "历元消息号顺序不符")
    expected_lens = {1005: 19, 1230: 8}
    stream_crc_seq = 0
    for msg_num, wire in epoch:
        _check(wire[0] == RTCM3_SYNC, "RTCM3 首字节非 0xD3")
        _check((wire[1] & 0xFC) == 0, "RTCM3 保留位非 0")
        plen = ((wire[1] & 0x03) << 8) | wire[2]
        _check(len(wire) == 6 + plen, "RTCM3 总长与载荷长不符")
        if msg_num in expected_lens:
            _check(plen == expected_lens[msg_num], f"RTCM {msg_num} 长度不符")
        else:
            _check(100 <= plen <= 400, f"MSM {msg_num} 长度越界: {plen}")
        decoded_msg = (wire[3] << 4) | ((wire[4] >> 4) & 0x0F)
        _check(decoded_msg == msg_num, f"RTCM 载荷首 12bit 消息号解码不符: {decoded_msg} != {msg_num}")
        got_crc = (wire[-3] << 16) | (wire[-2] << 8) | wire[-1]
        _check(got_crc == crc24q(wire[:-3]), "RTCM3 CRC-24Q 校验失败")
        stream_crc_seq = zlib.crc32(wire, stream_crc_seq) & 0xFFFFFFFF

    # 拼接整组一次性 zlib.crc32 与逐帧累计一致（供 --burst 与常规核对）
    _check((zlib.crc32(b"".join(w for _, w in epoch)) & 0xFFFFFFFF) == stream_crc_seq, "CRC32 分段累计不一致")

    # 3. 产品帧生成、流式解析器抗干扰（垃圾+坏CRC+任意分片）与顺序匹配
    ref01 = b"RTK-E1\x00\x00" + b"SN0001" + b"\x00" * 122
    _check(len(ref01) == 136, "ref01 测试向量长度应为 136")
    last_set: list[bytes | None] = [None]
    reqs: list[ExpectedReply] = []
    sent_t: list[float] = []
    sim_rx_stream = bytearray()
    for seq in range(6):
        _, exp = make_cycle_request(seq, rng, last_set)
        reqs.append(exp)
        sent_t.append(10.0 + seq * 0.01)
        if exp.kind == "eq":
            sim_rx_stream.extend(build_product_frame(0x07, PROD_TYPE_SET_RES, b"\x01\x01", sender=PROD_SENDER_DEV))
        elif exp.kind == "prefix":
            _check(last_set[0] is not None, "last_set 不应为空")
            assert last_set[0] is not None  # noqa: S101
            sim_rx_stream.extend(
                build_product_frame(0x07, PROD_TYPE_QUERY_RES, last_set[0] + b"\x00\x00\x00\x00", sender=PROD_SENDER_DEV)
            )
        else:
            sim_rx_stream.extend(build_product_frame(0x01, PROD_TYPE_QUERY_RES, ref01, sender=PROD_SENDER_DEV))
        if seq == 2:
            sim_rx_stream.extend(b"\xAA\x44\x99\x00\x01")
            sim_rx_stream.extend(build_product_frame(0x01, PROD_TYPE_QUERY_RES, ref01, sender=PROD_SENDER_DEV, corrupt=True))

    parser = ProductStreamParser()
    pos = 0
    while pos < len(sim_rx_stream):
        step = rng.randint(1, 17)
        parser.feed(bytes(sim_rx_stream[pos : pos + step]), 10.05)
        pos += step
    _check(len(parser.frames) == 6, f"期望解析出 6 帧，实际 {len(parser.frames)}")
    _check(parser.crc_bad == 1, f"期望 crc_bad == 1，实际 {parser.crc_bad}")
    _check(parser.resync_bytes > 0, "resync_bytes 应大于 0")
    ev_ok = evaluate_product_replies(reqs, sent_t, parser.frames, ref01)
    _check(ev_ok["ok"] == 6 and ev_ok["lost"] == 0 and ev_ok["content_err"] == 0, "产品应答全匹配自检失败")

    ev_drop = evaluate_product_replies(reqs, sent_t, parser.frames[:1] + parser.frames[2:], ref01)
    _check(ev_drop["ok"] == 5 and ev_drop["lost"] == 1 and ev_drop["content_err"] == 0, "产品应答丢帧检出失败")

    # 4. 控制台 status rtcm 正则解析与 reset_rtcm 回执检查（含 ANSI、异步日志、kv 节与 OK）
    sample_console = (
        "\x1b[0;32mI (12345) BT_HCI: hcif mode change\x1b[0m\r\n"
        "status rtcm\r\n"
        "[rtcm]\r\n"
        "  frames         60\r\n"
        "  bytes          10630 B (10.3 KB)\r\n"
        "  rate           5315 B/s\r\n"
        "  crc32          0x0ABC1234\r\n"
        "  last_msg       1230\r\n"
        "  rejected_role  0\r\n"
        "  unknown_bytes  0 B\r\n"
        "  prod_rejects   0\r\n"
        "  rtcm_rejects   0\r\n"
        "rtcm: frames=60 bytes=10630 crc32=0x0ABC1234 last_msg=1230 rejected_role=0 "
        "unknown_bytes=0 product_rejects=0 rtcm_rejects=0\r\n"
        "stats: frames_rx=120 replies_tx=120 write_errors=0 pushes_tx=0 disconnects=0 rx_dropped=0\r\n"
        "OK\r\n"
        "console> "
    )
    snap = parse_console_status(sample_console)
    _check(snap.rtcm.frames == 60 and snap.rtcm.bytes == 10630, "rtcm frames/bytes 解析错误")
    _check(snap.rtcm.crc32 == 0x0ABC1234 and snap.rtcm.last_msg == 1230, "rtcm crc32/last_msg 解析错误")
    _check(snap.stats.frames_rx == 120 and snap.stats.replies_tx == 120 and snap.stats.rx_dropped == 0, "stats 解析错误")

    class _DummySer:
        def __init__(self, reply_map: dict[bytes, bytes]) -> None:
            self._reply_map = reply_map
            self._rx = bytearray()
            self.written = bytearray()

        @property
        def in_waiting(self) -> int:
            return len(self._rx)

        def reset_input_buffer(self) -> None:
            self._rx.clear()

        def write(self, data: bytes) -> int:
            self.written.extend(data)
            self._rx.extend(self._reply_map.get(bytes(data), b""))
            return len(data)

        def flush(self) -> None:
            pass

        def read(self, size: int) -> bytes:
            chunk = bytes(self._rx[:size])
            del self._rx[:size]
            return chunk

    dummy_ser = _DummySer({
        b"rtcm reset\r": b"rtcm reset\r\nOK\r\nconsole> ",
        b"status rtcm\r": sample_console.encode("utf-8"),
    })
    dummy_con = SerialConsole.__new__(SerialConsole)
    dummy_con.port = "dummy"
    dummy_con.ser = dummy_ser  # type: ignore[assignment]
    dummy_con.reset_rtcm()
    _check(bytes(dummy_ser.written) == b"rtcm reset\r", "reset_rtcm 发送命令不符")

    snap2 = dummy_con.read_status()
    _check(bytes(dummy_ser.written) == b"rtcm reset\rstatus rtcm\r", "read_status 发送命令不符")
    _check(snap2.rtcm.frames == 60 and snap2.stats.frames_rx == 120, "read_status 解析异常")

    dummy_ser_err = _DummySer({b"rtcm reset\r": b"rtcm reset\r\nERR link not running\r\nconsole> "})
    dummy_con.ser = dummy_ser_err  # type: ignore[assignment]
    got_err = False
    try:
        dummy_con.reset_rtcm()
    except RuntimeError:
        got_err = True
    _check(got_err, "reset_rtcm 收到 ERR 时应抛出 RuntimeError")

    # 5. 判定逻辑（无丢弃匹配 PASS / 无丢弃不匹配 FAIL / 有丢弃 OVERFLOW_DROPPED）
    before0 = ConsoleSnapshot(
        rtcm=RtcmConsoleStats(0, 0, 0, 0, 0, 0, 0, 0),
        stats=LinkConsoleStats(10, 10, 0, 0, 0, 0),
        raw_text="",
    )
    after_pass = ConsoleSnapshot(
        rtcm=RtcmConsoleStats(60, 10630, 0x0ABC1234, 1230, 0, 0, 0, 0),
        stats=LinkConsoleStats(16, 16, 0, 0, 0, 0),
        raw_text="",
    )
    res_pass = evaluate_stage_result(
        "selftest_pass", 5000, 1.0, False, "cycle", 2.0, 60, 10630, 0x0ABC1234, 240, ev_ok, 0, 0, before0, after_pass
    )
    _check(res_pass["verdict"] == "PASS", "PASS 判定异常")

    after_bad_crc = ConsoleSnapshot(
        rtcm=RtcmConsoleStats(60, 10630, 0x11111111, 1230, 0, 0, 0, 0),
        stats=LinkConsoleStats(16, 16, 0, 0, 0, 0),
        raw_text="",
    )
    res_fail = evaluate_stage_result(
        "selftest_fail", 5000, 1.0, False, "cycle", 2.0, 60, 10630, 0x0ABC1234, 240, ev_ok, 0, 0, before0, after_bad_crc
    )
    _check(res_fail["verdict"] == "FAIL", "FAIL 判定异常")

    after_drop = ConsoleSnapshot(
        rtcm=RtcmConsoleStats(55, 9500, 0x22222222, 1230, 0, 0, 0, 0),
        stats=LinkConsoleStats(16, 16, 0, 0, 0, 5),
        raw_text="",
    )
    res_drop = evaluate_stage_result(
        "selftest_drop", 40000, 1.0, False, "cycle", 2.0, 60, 10630, 0x0ABC1234, 240, ev_ok, 0, 0, before0, after_drop
    )
    _check(res_drop["verdict"] == "OVERFLOW_DROPPED", "OVERFLOW_DROPPED 判定异常")

    # 6. CLI --tail-wait 参数默认值及 BtSession.close() 优雅关闭顺序自检
    args_default = parse_args([])
    _check(args_default.tail_wait == 1.5, f"--tail-wait 默认值应为 1.5，实际 {args_default.tail_wait}")
    args_custom = parse_args(["--tail-wait", "2.25", "--serial", "none", "--product", "off"])
    _check(args_custom.tail_wait == 2.25, f"--tail-wait 解析失败: {args_custom.tail_wait}")

    class _DummySock:
        def __init__(self) -> None:
            self.calls: list[tuple[str, Any]] = []

        def fileno(self) -> int:
            return -1

        def shutdown(self, how: int) -> None:
            self.calls.append(("shutdown", how))

        def close(self) -> None:
            self.calls.append(("close", None))

    dummy_sess = BtSession.__new__(BtSession)
    dummy_sock = _DummySock()
    dummy_sess.sock = dummy_sock  # type: ignore[assignment]
    dummy_sess.rx = RxThread(dummy_sock)  # type: ignore[arg-type]
    dummy_sess.rx.start()
    dummy_sess.close()
    _check(
        dummy_sock.calls
        == [
            ("shutdown", socket.SHUT_WR),
            ("shutdown", socket.SHUT_RDWR),
            ("close", None),
        ],
        f"BtSession.close() 关闭顺序异常: {dummy_sock.calls}",
    )
    _check(not dummy_sess.rx.is_alive(), "BtSession.close() 后 RxThread 未退出")

    print(
        json.dumps(
            {
                "selftest": "PASS",
                "crc24q_123456789": "0xCDE703",
                "epoch_frames": len(epoch),
                "sample_table": format_summary_table([res_pass, res_fail, res_drop]),
            },
            indent=2,
            ensure_ascii=False,
        )
    )
    return 0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="ESP32 蓝牙 SPP 单流 RTCM3 + 产品请求混合负载测试工具（ADR-0006）")
    ap.add_argument("--addr", default="F8:B3:B7:25:D8:B2", help="目标板经典蓝牙 MAC 地址（默认 F8:B3:B7:25:D8:B2）")
    ap.add_argument("--channel", type=int, default=1, help="RFCOMM 通道号（默认 1）")
    ap.add_argument("--serial", default="/dev/ttyACM0", help="控制台串口设备（默认 /dev/ttyACM0；传 none 跳过串口）")
    ap.add_argument("--baud", type=int, default=2000000, help="控制台波特率（默认 2000000）")
    ap.add_argument("--duration", type=float, default=10.0, help="单轮发送持续秒数（默认 10.0）")
    ap.add_argument(
        "--tail-wait",
        type=float,
        default=1.5,
        help="发送结束后至少等待排空的秒数（默认 1.5，无论是否有串口或产品负载）",
    )
    ap.add_argument("--epoch-hz", type=float, default=1.0, help="历元发送频率 Hz（默认 1.0）")
    ap.add_argument("--rtcm-rate", type=int, default=None, help="均匀发送模式目标速率 B/s（指定后覆盖历元节奏）")
    ap.add_argument("--burst", action="store_true", help="历元模式下一次性写出整组 RTCM 帧，模拟 NTRIP 突发")
    ap.add_argument("--product", choices=("off", "cycle"), default="cycle", help="产品负载模式：off 或 cycle（默认 cycle）")
    ap.add_argument("--product-hz", type=float, default=20.0, help="产品请求交错频率 Hz（默认 20.0）")
    ap.add_argument("--product-inflight", type=int, default=8, help="最大在途未应答产品请求数（默认 8）")
    ap.add_argument("--scan", action="store_true", help="依次以 2k/5k/10k/20k/40k B/s 各跑一轮，输出吞吐/丢弃表")
    ap.add_argument("--seed", type=int, default=None, help="随机数种子（默认取当前时间）")
    ap.add_argument("--json-out", default=None, help="将 JSON 摘要同时写入指定文件")
    ap.add_argument("--selftest", action="store_true", help="无板离线自检（CRC-24Q/帧生成/解析正则/判定）")
    args = ap.parse_args(argv)
    if args.tail_wait < 0:
        ap.error("--tail-wait 不能为负数")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    verify_crc24q_vector()

    if args.selftest:
        return run_selftest()

    seed = args.seed if args.seed is not None else int(time.time()) & 0xFFFFFFFF
    rng = random.Random(seed)
    use_serial = bool(args.serial) and args.serial.lower() != "none"

    console: SerialConsole | None = None
    session: BtSession | None = None
    try:
        if use_serial:
            console = SerialConsole(args.serial, baud=args.baud)
        session = BtSession(args.addr, args.channel)
        print(f"已连接 RFCOMM {args.addr} ch={args.channel}（耗时 {session.connect_ms:.0f} ms），seed={seed}")

        results: list[dict[str, Any]] = []
        if args.scan:
            for rate in SCAN_RATES_BPS:
                name = f"rate_{rate // 1000}k"
                print(f"开始 {name} ({rate} B/s, duration={args.duration}s, product={args.product})...")
                res = run_single_stage(
                    stage_name=name,
                    session=session,
                    console=console,
                    duration_s=args.duration,
                    epoch_hz=args.epoch_hz,
                    rtcm_rate_bps=rate,
                    burst=False,
                    product_mode=args.product,
                    product_hz=args.product_hz,
                    product_inflight=args.product_inflight,
                    rng=rng,
                    tail_wait_s=args.tail_wait,
                )
                results.append(res)
                # 档位之间稍作停顿让串口与对端流控稳定
                time.sleep(0.5)
        else:
            mode_name = (
                f"rate_{args.rtcm_rate}"
                if args.rtcm_rate is not None
                else f"epoch_{args.epoch_hz}hz_{'burst' if args.burst else 'paced'}"
            )
            res = run_single_stage(
                stage_name=mode_name,
                session=session,
                console=console,
                duration_s=args.duration,
                epoch_hz=args.epoch_hz,
                rtcm_rate_bps=args.rtcm_rate,
                burst=args.burst,
                product_mode=args.product,
                product_hz=args.product_hz,
                product_inflight=args.product_inflight,
                rng=rng,
                tail_wait_s=args.tail_wait,
            )
            results.append(res)

        summary = {
            "addr": args.addr,
            "channel": args.channel,
            "serial": args.serial if use_serial else None,
            "tail_wait_s": args.tail_wait,
            "seed": seed,
            "results": results,
        }
        print("\n=== 汇总表 ===")
        print(format_summary_table(results))
        print("\n=== JSON 摘要 ===")
        json_text = json.dumps(summary, indent=2, ensure_ascii=False)
        print(json_text)

        if args.json_out:
            with open(args.json_out, "w", encoding="utf-8") as fp:
                fp.write(json_text + "\n")

        any_fail = any(r["verdict"] == "FAIL" for r in results)
        return 1 if any_fail else 0
    except Exception as exc:
        print(f"错误: {exc}", file=sys.stderr)
        return 2
    finally:
        if session is not None:
            session.close()
        if console is not None:
            console.close()


if __name__ == "__main__":
    raise SystemExit(main())
