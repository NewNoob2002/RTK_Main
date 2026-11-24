#!/usr/bin/env python3
"""Offline mock HTTP + WebSocket server for RTK web UI (stdlib only)."""

import argparse
import base64
import hashlib
import http.client
import http.server
import json
import pathlib
import re
import socket
import socketserver
import struct
import sys
import threading
import time
import urllib.request

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_FRAME_BYTES = 1024
OTA_HDR_BYTES = 288
OTA_IMG_MAGIC = 0xE9
OTA_CHIP_ID_ESP32 = 0x0000
OTA_DESC_MAGIC = 0xABCD5432
OTA_DEFAULT_MAX = 0x1E0000  # partitions.csv ota_0/ota_1 大小
OTA_RECV_TIMEOUT_S = 30.0
OTA_TOTAL_TIMEOUT_S = 150.0
OTA_DRAIN_TIMEOUT_S = 0.25
OTA_ERR_HTTP: dict[str, int] = {
    "busy": 409,
    "state": 409,
    "length": 411,
    "too_large": 413,
    "bad_image": 400,
    "validate": 400,
    "timeout": 408,
    "flash": 500,
    "aborted": 503,
}
MAC_RE = re.compile(r"([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}")
SN_RE = re.compile(r"[0-9A-Za-z-]{1,16}")

VALID_BOOL = {"0", "1"}
NMEA_KEYS = {
    "gnss.out.gga",
    "gnss.out.gsa",
    "gnss.out.gsv",
    "gnss.out.gst",
    "gnss.out.rmc",
}
CONST_KEYS = {
    "gnss.const.gps",
    "gnss.const.bds",
    "gnss.const.glo",
    "gnss.const.gal",
}
CAPTIVE_PROBE_PATHS = (
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/connecttest.txt",
    "/ncsi.txt",
    "/redirect",
)


def parse_esp_app_header(data: bytes) -> tuple[str, bytes] | None:
    """解析 ESP-IDF 镜像头与 esp_app_desc_t，返回 (version, project_name_bytes) 或 None。"""
    if len(data) < OTA_HDR_BYTES or data[0] != OTA_IMG_MAGIC:
        return None
    (chip_id,) = struct.unpack_from("<H", data, 12)
    if chip_id != OTA_CHIP_ID_ESP32:
        return None
    (magic_word,) = struct.unpack_from("<I", data, 32)
    if magic_word != OTA_DESC_MAGIC:
        return None
    ver_bytes = data[48:80].split(b"\x00", 1)[0]
    proj_bytes = data[80:112].split(b"\x00", 1)[0]
    version = ver_bytes.decode("utf-8", errors="replace")
    return version, proj_bytes


def build_mock_firmware(
    version: str,
    project: str = "rtk_main",
    total_size: int = 4096,
    img_magic: int = OTA_IMG_MAGIC,
    desc_magic: int = OTA_DESC_MAGIC,
    chip_id: int = OTA_CHIP_ID_ESP32,
    raw_version: bytes | None = None,
    raw_project: bytes | None = None,
) -> bytes:
    """构造带合法或指定畸形 esp_app_desc_t 头的测试固件字节串。"""
    size = max(total_size, OTA_HDR_BYTES)
    buf = bytearray(size)
    buf[0] = img_magic & 0xFF
    struct.pack_into("<H", buf, 12, chip_id & 0xFFFF)
    struct.pack_into("<I", buf, 32, desc_magic & 0xFFFFFFFF)
    ver_b = (raw_version if raw_version is not None else version.encode("utf-8"))[:32]
    proj_b = (raw_project if raw_project is not None else project.encode("utf-8"))[:32]
    buf[48 : 48 + len(ver_b)] = ver_b
    buf[80 : 80 + len(proj_b)] = proj_b
    return bytes(buf[:total_size])


class MockState:
    """Shared mutable device state for the mock server."""

    def __init__(
        self,
        unlock_token: str = "123456",
        no_const: bool = False,
        caps_cm680: bool = False,
        ota_err: str | None = None,
        ota_slow_kbps: float = 0.0,
        ota_max: int = OTA_DEFAULT_MAX,
        no_ota: bool = False,
    ) -> None:
        self.lock = threading.Lock()
        self.unlock_token = unlock_token
        self.no_const = no_const or caps_cm680
        self.caps_cm680 = caps_cm680
        self.force_err: str | None = None
        self.drop_ack_key: str | None = None
        self.gnss_none: bool = False
        self.wl_version: int = 0
        self.start_time = time.monotonic()
        self.ota_running: str = "0.1.0-mock"
        self.ota_project: str = "rtk_main"
        self.ota_part: str = "ota_0"
        self.ota_state: str = "valid"
        self.ota_other: str | None = None
        self.ota_max: int = ota_max
        self.ota_busy: bool = False
        self.ota_rebooting: bool = False
        self.ota_pending_ver: str | None = None
        self.ota_pending_part: str | None = None
        self.ota_reboot_timer: threading.Timer | None = None
        self.ota_err: str | None = ota_err
        self.ota_slow_kbps: float = ota_slow_kbps
        self.ota_reboot_ms: int = 1500
        self.ota_recv_timeout_s: float = OTA_RECV_TIMEOUT_S
        self.ota_total_timeout_s: float = OTA_TOTAL_TIMEOUT_S
        self.ota_drain_timeout_s: float = OTA_DRAIN_TIMEOUT_S
        self.no_ota: bool = no_ota
        self.ws_clients: set[socket.socket] = set()
        self.whitelist = ["AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"]
        self.config: dict[str, object] = {
            "gnss.rate": 10,
            "gnss.out.gga": 100,
            "gnss.out.gsa": 1000,
            "gnss.out.gsv": 1000,
            "gnss.out.gst": 1000,
            "gnss.out.rmc": 100,
            "gga_gate": {"enabled": False, "min_sats": 8},
            "bt.pair_next": 0,
            "factory.sn": "P2A0001",
        }
        if caps_cm680:
            self.config["gnss.out.gsv"] = 5000
        if not self.no_const:
            self.config.update(
                {
                    "gnss.const.gps": 1,
                    "gnss.const.bds": 1,
                    "gnss.const.glo": 1,
                    "gnss.const.gal": 1,
                    "gnss.cutoff": 15,
                }
            )


    def caps(self) -> dict[str, object]:
        result: dict[str, object] = {
            "bt.wl.add": {"min": 17, "max": 17},
            "bt.wl.del": {"min": 17, "max": 17},
            "bt.pair_next": [0, 1],
            "factory.sn": {"min": 1, "max": 16},
        }
        if self.gnss_none:
            return result
        result.update({
            "gnss.rate": [1, 2, 5, 10] if self.caps_cm680 else [1, 2, 5, 10, 20],
            "gnss.gga_gate.enabled": [0, 1],
            "gnss.gga_gate.min_sats": {"min": 0, "max": 40},
        })
        min_period = 1000 // int(self.config["gnss.rate"])
        for key in NMEA_KEYS:
            candidates = [1000, 500, 200, 100, 50]
            if self.caps_cm680:
                if key == "gnss.out.gsa":
                    candidates = [1000]
                elif key == "gnss.out.gsv":
                    candidates = [1000, 5000]
                else:
                    candidates = [1000, 500, 200, 100]
            result[key] = [0, *(p for p in candidates if p >= min_period)]
        if not self.no_const:
            result.update({key: [0, 1] for key in CONST_KEYS})
            result["gnss.cutoff"] = {"min": 0, "max": 90}
        return result

    def ota_info(self) -> dict[str, object]:
        """构造 GET /ota/info 应答对象（调用方须持锁）。"""
        return {
            "running": self.ota_running,
            "project": self.ota_project,
            "part": self.ota_part,
            "state": "pending_verify" if self.ota_err == "state" else self.ota_state,
            "other": self.ota_other,
            "max": self.ota_max,
            "busy": self.ota_busy,
        }

    def complete_ota_reboot(self) -> None:
        """在唯一一次重启回调中原子切换运行槽快照、释放 busy 并断开所有活跃 WS。"""
        with self.lock:
            if not self.ota_rebooting:
                return
            self.ota_other = self.ota_running
            self.ota_running = "" if self.ota_pending_ver is None else self.ota_pending_ver
            if self.ota_pending_part is not None:
                self.ota_part = self.ota_pending_part
            self.ota_state = "valid"
            self.ota_pending_ver = None
            self.ota_pending_part = None
            self.ota_rebooting = False
            self.ota_busy = False
            self.ota_reboot_timer = None
            clients = list(self.ws_clients)
            self.ws_clients.clear()
        for cli in clients:
            try:
                cli.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                cli.close()
            except OSError:
                pass


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("socket closed")
        buf.extend(chunk)
    return bytes(buf)


def ws_read_frame(sock: socket.socket, require_mask: bool = True) -> tuple[int, bytes]:
    """Read one RFC 6455 frame. Returns (opcode, payload)."""
    hdr = _recv_exact(sock, 2)
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    length = b1 & 0x7F
    if length == 126:
        (length,) = struct.unpack("!H", _recv_exact(sock, 2))
    elif length == 127:
        (length,) = struct.unpack("!Q", _recv_exact(sock, 8))

    if require_mask and length > MAX_FRAME_BYTES:
        raise ValueError("too_long")

    mask_key = _recv_exact(sock, 4) if masked else b""
    payload = _recv_exact(sock, length) if length > 0 else b""
    if masked:
        payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def ws_write_frame(
    sock: socket.socket, opcode: int, payload: bytes, mask: bool = False
) -> None:
    """Write one RFC 6455 frame."""
    header = bytearray([0x80 | (opcode & 0x0F)])
    length = len(payload)
    mask_bit = 0x80 if mask else 0x00
    if length < 126:
        header.append(mask_bit | length)
    elif length < 65536:
        header.append(mask_bit | 126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(mask_bit | 127)
        header.extend(struct.pack("!Q", length))

    if mask:
        key = b"\x12\x34\x56\x78"
        header.extend(key)
        masked_payload = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
        sock.sendall(bytes(header) + masked_payload)
    else:
        sock.sendall(bytes(header) + payload)


def ws_send_json(sock: socket.socket, lock: threading.Lock, obj: dict) -> None:
    data = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(data) > MAX_FRAME_BYTES:
        raise ValueError(f"Outbound frame exceeds {MAX_FRAME_BYTES} B: {len(data)}")
    with lock:
        ws_write_frame(sock, 0x1, data, mask=False)


def _no_dup_pairs(pairs: list[tuple[str, object]]) -> dict:
    out: dict = {}
    for k, v in pairs:
        if k in out:
            raise ValueError("duplicate_key")
        out[k] = v
    return out


def _is_v1_req_value(v: object) -> bool:
    if isinstance(v, bool):
        return True
    if isinstance(v, (str, int)):
        return True
    return False


def ws_send_close(sock: socket.socket, lock: threading.Lock, code: int) -> None:
    with lock:
        ws_write_frame(sock, 0x8, struct.pack("!H", code), mask=False)


class MockHandler(http.server.BaseHTTPRequestHandler):
    """HTTP + WebSocket handler for mock UI."""

    protocol_version = "HTTP/1.1"
    ui_dir: pathlib.Path
    state: MockState
    canonical_host: str = ""

    def log_message(self, fmt: str, *args) -> None:
        pass

    def _portal_location(self) -> str:
        if self.canonical_host:
            return f"http://{self.canonical_host}/"
        host, port = self.server.server_address[:2]
        return f"http://{host}:{port}/"

    def _send_captive_redirect(self) -> None:
        self.send_response(302, "Found")
        self.send_header("Location", self._portal_location())
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _send_json(self, status: int, obj: dict, close: bool = False) -> None:
        body = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        if close:
            self.close_connection = True
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if close:
            self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.wfile.flush()

    def _read_body_chunk(self, want: int, idle_timeout_s: float) -> tuple[bytes, bool]:
        """在空闲时限内读取一段请求体，返回 (chunk, timed_out)。"""
        old_timeout = self.connection.gettimeout()
        deadline = time.monotonic() + idle_timeout_s
        try:
            rem = deadline - time.monotonic()
            if rem <= 0:
                return b"", True
            self.connection.settimeout(rem)
            try:
                return self.rfile.read1(want), False
            except TimeoutError:
                return b"", True
        finally:
            try:
                self.connection.settimeout(old_timeout)
            except OSError:
                pass

    def _drain_small_body(
        self, remaining: int, limit: int = 256 * 1024, timeout_s: float | None = None
    ) -> bool:
        """限时排空小体积拒绝请求体；超时或过大立即放弃，由调用方关闭连接。"""
        if remaining <= 0:
            return True
        if remaining > limit:
            return False
        if timeout_s is None:
            timeout_s = self.state.ota_drain_timeout_s
        old_timeout = self.connection.gettimeout()
        deadline = time.monotonic() + timeout_s
        left = remaining
        try:
            while left > 0:
                rem = deadline - time.monotonic()
                if rem <= 0:
                    return False
                self.connection.settimeout(rem)
                try:
                    chunk = self.rfile.read1(min(4096, left))
                except (TimeoutError, OSError):
                    return False
                if not chunk:
                    return False
                left -= len(chunk)
            return True
        finally:
            try:
                self.connection.settimeout(old_timeout)
            except OSError:
                pass

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/ws":
            self._handle_ws()
            return
        if path == "/ota/info":
            with self.state.lock:
                if self.state.no_ota:
                    self.send_response(404, "Not Found")
                    self.send_header("Content-Length", "0")
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    return
                info = self.state.ota_info()
            self._send_json(200, info)
            return
        if path == "/favicon.ico":
            self.send_response(204, "No Content")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path in CAPTIVE_PROBE_PATHS:
            self._send_captive_redirect()
            return

        routes = {
            "/": ("index.html", "text/html; charset=utf-8"),
            "/app.css": ("app.css", "text/css; charset=utf-8"),
            "/app.js": ("app.js", "application/javascript; charset=utf-8"),
            "/logo.png": ("logo.png", "image/png"),
        }
        if path not in routes:
            self._send_captive_redirect()
            return

        if path == "/" and self.canonical_host:
            req_host = (self.headers.get("Host") or "").strip()
            if req_host != self.canonical_host:
                self._send_captive_redirect()
                return

        fname, ctype = routes[path]
        fpath = self.ui_dir / fname
        if not fpath.is_file():
            self.send_error(404, "Not Found")
            return

        data = fpath.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self) -> None:
        path = self.path.split("?", 1)[0]
        if path != "/ota" or self.state.no_ota:
            self.send_response(404, "Not Found")
            self.send_header("Content-Length", "0")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        self._handle_ota_post()

    def _handle_ota_post(self) -> None:
        cl_hdr = self.headers.get("Content-Length")
        parsed_cl: int | None = None
        if cl_hdr is not None:
            try:
                parsed_cl = int(cl_hdr.strip())
            except ValueError:
                parsed_cl = -1

        already_busy = False
        with self.state.lock:
            if self.state.ota_busy or self.state.ota_rebooting:
                already_busy = True
            else:
                self.state.ota_busy = True
                forced_err = self.state.ota_err
                cur_state = self.state.ota_state
                ota_max = self.state.ota_max
                ota_project_bytes = self.state.ota_project.encode("utf-8")
                slow_kbps = self.state.ota_slow_kbps
                reboot_ms = self.state.ota_reboot_ms
                recv_timeout_s = self.state.ota_recv_timeout_s
                total_timeout_s = self.state.ota_total_timeout_s
                total_deadline = time.monotonic() + total_timeout_s
        if already_busy:
            drained = self._drain_small_body(parsed_cl or 0)
            try:
                self._send_json(OTA_ERR_HTTP["busy"], {"ok": False, "err": "busy"}, close=not drained)
            except OSError:
                self.close_connection = True
            return

        remaining = parsed_cl if (parsed_cl is not None and parsed_cl > 0) else 0
        owns_busy = True

        def release_busy() -> None:
            nonlocal owns_busy
            if owns_busy:
                with self.state.lock:
                    self.state.ota_busy = False
                owns_busy = False

        try:
            def fail(code: str, rem: int, drain: bool = True) -> None:
                release_busy()
                drained = self._drain_small_body(rem) if drain else False
                try:
                    self._send_json(OTA_ERR_HTTP[code], {"ok": False, "err": code}, close=not drained)
                except OSError:
                    self.close_connection = True

            if forced_err in ("busy", "state", "length", "too_large"):
                fail(forced_err, remaining)
                return
            if cur_state == "pending_verify":
                fail("state", remaining)
                return
            if parsed_cl is None or parsed_cl <= 0:
                fail("length", 0, drain=False)
                return
            if parsed_cl > ota_max:
                fail("too_large", remaining)
                return
            if parsed_cl <= OTA_HDR_BYTES:
                fail("bad_image", remaining)
                return

            hdr = bytearray()
            received = 0
            new_ver = ""
            header_checked = False
            chunk_sz = 4096
            if slow_kbps > 0:
                chunk_sz = min(4096, max(512, int(slow_kbps * 1024.0 / 10.0)))
            t0 = time.monotonic()

            while received < parsed_cl:
                total_rem = total_deadline - time.monotonic()
                if total_rem <= 0:
                    fail("timeout", 0, drain=False)
                    return
                want = min(chunk_sz, parsed_cl - received)
                try:
                    chunk, timed_out = self._read_body_chunk(want, min(recv_timeout_s, total_rem))
                except OSError:
                    fail("aborted", 0, drain=False)
                    return
                if timed_out:
                    fail("timeout", 0, drain=False)
                    return
                if not chunk:
                    fail("aborted", 0, drain=False)
                    return
                received += len(chunk)
                if time.monotonic() >= total_deadline:
                    fail("timeout", 0, drain=False)
                    return
                if not header_checked:
                    need = OTA_HDR_BYTES - len(hdr)
                    hdr.extend(chunk[:need])
                    if len(hdr) >= OTA_HDR_BYTES:
                        header_checked = True
                        if forced_err == "bad_image":
                            fail("bad_image", parsed_cl - received)
                            return
                        parsed = parse_esp_app_header(bytes(hdr))
                        if parsed is None or parsed[1] != ota_project_bytes:
                            fail("bad_image", parsed_cl - received)
                            return
                        new_ver = parsed[0]
                if slow_kbps > 0:
                    elapsed = time.monotonic() - t0
                    expected = received / (slow_kbps * 1024.0)
                    if expected > elapsed:
                        sleep_s = expected - elapsed
                        rem_after = total_deadline - time.monotonic()
                        if rem_after <= 0:
                            fail("timeout", 0, drain=False)
                            return
                        if sleep_s >= rem_after:
                            time.sleep(rem_after)
                            fail("timeout", 0, drain=False)
                            return
                        time.sleep(sleep_s)

            if forced_err in ("validate", "timeout", "flash", "aborted"):
                fail(forced_err, 0, drain=True)
                return

            with self.state.lock:
                next_part = "ota_1" if self.state.ota_part == "ota_0" else "ota_0"
                self.state.ota_pending_ver = new_ver
                self.state.ota_pending_part = next_part
                self.state.ota_rebooting = True
                # 重启窗口保持 ota_busy=True，所有权移交给重启定时器
                owns_busy = False
                timer = threading.Timer(reboot_ms / 1000.0, self.state.complete_ota_reboot)
                timer.daemon = True
                self.state.ota_reboot_timer = timer

            try:
                self._send_json(200, {"ok": True, "ver": new_ver, "reboot_ms": reboot_ms})
            except OSError:
                self.close_connection = True
            finally:
                timer.start()
        finally:
            release_busy()

    def _handle_ws(self) -> None:
        self.close_connection = True
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_error(400, "Missing Sec-WebSocket-Key")
            return
        accept = base64.b64encode(
            hashlib.sha1((key.strip() + WS_GUID).encode("ascii")).digest()
        ).decode("ascii")

        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()

        sock: socket.socket = self.connection
        send_lock = threading.Lock()
        stop_ev = threading.Event()
        unlocked = False
        t_thread: threading.Thread | None = None

        with self.state.lock:
            self.state.ws_clients.add(sock)
            hello = {
                "t": "hello",
                "caps": self.state.caps(),
                "proto": 1,
                "board": "P2",
                "fw": self.state.ota_running,
                "bt_name": "P2-A1B2",
                "console": "dev",
                "factory": unlocked,
            }
            cfg = {"t": "config", **self.state.config}
            wl = list(self.state.whitelist)
            last_wl_ver = self.state.wl_version

        try:
            try:
                ws_send_json(sock, send_lock, hello)
                ws_send_json(sock, send_lock, cfg)
                self._send_status(sock, send_lock, 0, include_bt=wl)
            except OSError:
                return

            def ticker() -> None:
                nonlocal last_wl_ver
                tick = 1
                while not stop_ev.wait(1.0):
                    try:
                        bt_wl = None
                        with self.state.lock:
                            if self.state.wl_version != last_wl_ver:
                                last_wl_ver = self.state.wl_version
                                bt_wl = list(self.state.whitelist)
                        self._send_status(sock, send_lock, tick, include_bt=bt_wl)
                        tick += 1
                    except OSError:
                        break

            th = threading.Thread(target=ticker, daemon=True)
            th.start()
            t_thread = th

            while not stop_ev.is_set():
                try:
                    opcode, payload = ws_read_frame(sock, require_mask=True)
                except ValueError:
                    try:
                        ws_send_close(sock, send_lock, 1009)
                    except OSError:
                        pass
                    break
                except (EOFError, OSError):
                    break

                if opcode == 0x8:
                    break
                if opcode == 0x9:
                    try:
                        with send_lock:
                            ws_write_frame(sock, 0xA, payload, mask=False)
                    except OSError:
                        break
                    continue
                if opcode != 0x1:
                    continue

                try:
                    unlocked, should_close = self._on_text(
                        sock, send_lock, payload, unlocked
                    )
                except OSError:
                    break
                if should_close:
                    break
        finally:
            with self.state.lock:
                self.state.ws_clients.discard(sock)
            stop_ev.set()
            if t_thread is not None:
                t_thread.join(timeout=0.5)

    def _send_status(
        self,
        sock: socket.socket,
        send_lock: threading.Lock,
        tick: int,
        include_bt: list[str] | None,
    ) -> None:
        uptime_s = int(time.monotonic() - self.state.start_time)
        with self.state.lock:
            hz = int(self.state.config["gnss.rate"])
            gnss_none = self.state.gnss_none
        gnss_obj: dict = {
            "state": "probing" if gnss_none else "running",
            "model": "CM680",
            "fw": "1.2.3",
            "fix": "none" if gnss_none else "fixed",
        }
        if not gnss_none:
            gnss_obj["sats"] = 22 + (tick % 5)
            gnss_obj["hacc_mm"] = 12 + (tick % 4) * 2
            gnss_obj["hz"] = hz
        msg: dict = {
            "t": "status",
            "dev": {
                "uptime_s": uptime_s,
                "heap_free": 49152 + (tick % 7) * 256,
                "heap_min": 44032,
                "reset": "poweron",
            },
            "gnss": gnss_obj,
        }
        if include_bt is not None:
            msg["bt"] = {"whitelist": include_bt}
        ws_send_json(sock, send_lock, msg)

    def _on_text(
        self,
        sock: socket.socket,
        send_lock: threading.Lock,
        payload: bytes,
        unlocked: bool,
    ) -> tuple[bool, bool]:
        try:
            req = json.loads(payload.decode("utf-8"), object_pairs_hook=_no_dup_pairs)
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": 0, "ok": False, "err": "bad_json"}
            )
            return unlocked, False

        if not isinstance(req, dict) or not all(
            isinstance(k, str) and _is_v1_req_value(v) for k, v in req.items()
        ):
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": 0, "ok": False, "err": "bad_json"}
            )
            return unlocked, False

        rid = req.get("id")
        if isinstance(rid, bool) or not isinstance(rid, int) or rid < 1 or rid > 65535:
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": 0, "ok": False, "err": "bad_json"}
            )
            return unlocked, False

        rtype = req.get("t")
        if not isinstance(rtype, str) or rtype not in ("unlock", "cmd", "set"):
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": "bad_json"}
            )
            return unlocked, False

        if rtype == "unlock" and not isinstance(req.get("token"), str):
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": "bad_value"}
            )
            return unlocked, False
        if rtype == "cmd" and not isinstance(req.get("name"), str):
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": "bad_value"}
            )
            return unlocked, False
        if rtype == "set" and (
            not isinstance(req.get("key"), str) or not isinstance(req.get("value"), str)
        ):
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": "bad_value"}
            )
            return unlocked, False

        if rtype == "unlock":
            tok = req["token"]
            with self.state.lock:
                forced = self.state.force_err
                ok_tok = forced is None and tok == self.state.unlock_token
            if forced is not None:
                ws_send_json(
                    sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": forced}
                )
                return unlocked, False
            if ok_tok:
                ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
                return True, False
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": "bad_value"}
            )
            return unlocked, False

        if rtype == "cmd":
            name = req["name"]
            bye_msg: dict | None = None
            should_close = False
            with self.state.lock:
                if self.state.ota_busy or self.state.ota_rebooting:
                    ack_msg = {"t": "ack", "id": rid, "ok": False, "err": "busy"}
                elif self.state.force_err is not None:
                    ack_msg = {"t": "ack", "id": rid, "ok": False, "err": self.state.force_err}
                elif name == "save":
                    ack_msg = {"t": "ack", "id": rid, "ok": True}
                elif name == "exit":
                    ack_msg = {"t": "ack", "id": rid, "ok": True}
                    bye_msg = {"t": "bye", "reason": "exit"}
                    should_close = True
                elif name == "freset":
                    if not unlocked:
                        ack_msg = {"t": "ack", "id": rid, "ok": False, "err": "locked"}
                    else:
                        ack_msg = {"t": "ack", "id": rid, "ok": True}
                        bye_msg = {"t": "bye", "reason": "freset"}
                        should_close = True
                else:
                    ack_msg = {"t": "ack", "id": rid, "ok": False, "err": "unsupported"}
            ws_send_json(sock, send_lock, ack_msg)
            if bye_msg is not None:
                ws_send_json(sock, send_lock, bye_msg)
            return unlocked, should_close

        if rtype == "set":
            key = req.get("key", "")
            val = req.get("value", "")
            if not isinstance(key, str) or not isinstance(val, str):
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return unlocked, False
            return unlocked, self._handle_set(sock, send_lock, rid, key, val, unlocked)

        ws_send_json(
            sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": "bad_json"}
        )
        return unlocked, False

    def _handle_set(
        self,
        sock: socket.socket,
        send_lock: threading.Lock,
        rid: int,
        key: str,
        val: str,
        unlocked: bool,
    ) -> bool:
        out_msgs: list[dict] = []
        send_bt_wl: list[str] | None = None

        with self.state.lock:
            if self.state.ota_busy or self.state.ota_rebooting:
                out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "busy"})
            elif self.state.force_err is not None:
                out_msgs.append(
                    {"t": "ack", "id": rid, "ok": False, "err": self.state.force_err}
                )
            elif self.state.drop_ack_key is not None and key == self.state.drop_ack_key:
                return False
            elif key == "gnss.rate":
                if val not in {str(v) for v in self.state.caps().get(key, [])}:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    self.state.config[key] = int(val)
                    out_msgs.append({"t": "ack", "id": rid, "ok": True})
                    out_msgs.append({"t": "config", **self.state.config})
                    out_msgs.append({"t": "caps", "caps": self.state.caps()})
            elif key in NMEA_KEYS:
                if val not in {str(v) for v in self.state.caps().get(key, [])}:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    self.state.config[key] = int(val)
                    out_msgs.append({"t": "ack", "id": rid, "ok": True})
                    out_msgs.append({"t": "config", **self.state.config})
            elif key == "gnss.gga_gate.enabled":
                if val not in VALID_BOOL:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    gate = dict(self.state.config["gga_gate"])
                    gate["enabled"] = val == "1"
                    self.state.config["gga_gate"] = gate
                    out_msgs.append({"t": "ack", "id": rid, "ok": True})
                    out_msgs.append({"t": "config", **self.state.config})
            elif key == "gnss.gga_gate.min_sats":
                if not re.fullmatch(r"0|[1-9][0-9]*", val) or int(val) > 40:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    gate = dict(self.state.config["gga_gate"])
                    gate["min_sats"] = int(val)
                    self.state.config["gga_gate"] = gate
                    out_msgs.append({"t": "ack", "id": rid, "ok": True})
                    out_msgs.append({"t": "config", **self.state.config})
            elif key in CONST_KEYS:
                if self.state.no_const:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "unsupported"})
                elif val not in VALID_BOOL:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    self.state.config[key] = int(val)
                    out_msgs.append({"t": "ack", "id": rid, "ok": True})
                    out_msgs.append({"t": "config", **self.state.config})
            elif key == "gnss.cutoff":
                if self.state.no_const:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "unsupported"})
                elif not re.fullmatch(r"0|[1-9][0-9]*", val) or int(val) > 90:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    self.state.config[key] = int(val)
                    out_msgs.append({"t": "ack", "id": rid, "ok": True})
                    out_msgs.append({"t": "config", **self.state.config})
            elif key == "bt.pair_next":
                if val not in VALID_BOOL:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    self.state.config[key] = int(val)
                    out_msgs.append({"t": "ack", "id": rid, "ok": True})
                    out_msgs.append({"t": "config", **self.state.config})
            elif key in ("bt.wl.add", "bt.wl.del"):
                if not MAC_RE.fullmatch(val):
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    mac = val.upper()
                    if key == "bt.wl.add":
                        new_wl = (
                            self.state.whitelist + [mac]
                            if mac not in self.state.whitelist
                            else list(self.state.whitelist)
                        )
                        probe = json.dumps(
                            {"t": "status", "bt": {"whitelist": new_wl}},
                            separators=(",", ":"),
                        ).encode("utf-8")
                        if len(probe) > MAX_FRAME_BYTES:
                            out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "failed"})
                        else:
                            self.state.whitelist = new_wl
                            send_bt_wl = list(self.state.whitelist)
                            self.state.wl_version += 1
                            out_msgs.append({"t": "ack", "id": rid, "ok": True})
                            out_msgs.append({"t": "config", **self.state.config})
                    else:
                        self.state.whitelist = [m for m in self.state.whitelist if m != mac]
                        send_bt_wl = list(self.state.whitelist)
                        self.state.wl_version += 1
                        out_msgs.append({"t": "ack", "id": rid, "ok": True})
                        out_msgs.append({"t": "config", **self.state.config})
            elif key == "factory.sn":
                if not unlocked:
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "locked"})
                elif not SN_RE.fullmatch(val):
                    out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "bad_value"})
                else:
                    self.state.config[key] = val
                    out_msgs.append({"t": "ack", "id": rid, "ok": True})
                    out_msgs.append({"t": "config", **self.state.config})
            else:
                out_msgs.append({"t": "ack", "id": rid, "ok": False, "err": "unknown_key"})

        for msg in out_msgs:
            ws_send_json(sock, send_lock, msg)
        if send_bt_wl is not None:
            self._send_status(sock, send_lock, 0, include_bt=send_bt_wl)
        return False


class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


class _WsClient:
    """Minimal stdlib WebSocket client for --selftest."""

    def __init__(self, host: str, port: int) -> None:
        self.sock = socket.create_connection((host, port), timeout=5.0)
        self._buf = bytearray()
        key = base64.b64encode(b"0123456789abcdef").decode("ascii")
        req = (
            f"GET /ws HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode("ascii"))
        raw = b""
        while b"\r\n\r\n" not in raw:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise EOFError("Handshake closed")
            raw += chunk
        hdr, rest = raw.split(b"\r\n\r\n", 1)
        expected = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
        ).decode("ascii")
        assert b"HTTP/1.1 101 Switching Protocols" in hdr, f"Handshake failed: {hdr!r}"
        assert expected.encode("ascii") in hdr, f"Bad Sec-WebSocket-Accept: {hdr!r}"
        self._buf.extend(rest)

    def _read_exact(self, n: int, deadline: float | None = None) -> bytes:
        while len(self._buf) < n:
            if deadline is not None:
                rem = deadline - time.monotonic()
                if rem <= 0:
                    raise TimeoutError("WS read deadline exceeded")
                self.sock.settimeout(rem)
            chunk = self.sock.recv(n - len(self._buf))
            if not chunk:
                raise EOFError("socket closed")
            self._buf.extend(chunk)
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def send_text(self, text: str) -> None:
        ws_write_frame(self.sock, 0x1, text.encode("utf-8"), mask=True)

    def send_json(self, obj: dict) -> None:
        self.send_text(json.dumps(obj, separators=(",", ":")))

    def recv_next(self, deadline: float | None = None) -> tuple[int, bytes]:
        hdr = self._read_exact(2, deadline=deadline)
        b0, b1 = hdr[0], hdr[1]
        opcode = b0 & 0x0F
        length = b1 & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._read_exact(2, deadline=deadline))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._read_exact(8, deadline=deadline))
        payload = self._read_exact(length, deadline=deadline) if length > 0 else b""
        return opcode, payload

    def recv_until(self, t_name: str) -> dict:
        for _ in range(20):
            op, data = self.recv_next()
            assert op == 0x1, f"Unexpected opcode {op}"
            msg = json.loads(data.decode("utf-8"))
            if msg.get("t") == t_name:
                return msg
        raise AssertionError(f"Did not receive t={t_name}")

    def close(self) -> None:
        self.sock.close()


def _wait_ws_closed(client: _WsClient, timeout_s: float) -> bool:
    """在绝对截止时间内等待 WS 客户端收到关闭帧或 EOF；持续收到普通帧超时后返回 False。"""
    deadline = time.monotonic() + timeout_s
    try:
        while True:
            if time.monotonic() >= deadline:
                return False
            op, _pl = client.recv_next(deadline=deadline)
            if op == 0x8:
                return True
    except TimeoutError:
        return False
    except (EOFError, ConnectionError):
        return True


def resolve_canonical_host(bind_host: str, actual_port: int, canonical_host: str | None) -> str:
    """根据绑定地址、实际端口与显式参数确定强制门户规范 Host。"""
    if canonical_host:
        return canonical_host
    host = "127.0.0.1" if bind_host in ("", "0.0.0.0") else bind_host
    return f"{host}:{actual_port}"


def run_selftest(ui_dir: pathlib.Path, caps_cm680: bool = False) -> int:
    """Run automated protocol + HTTP self-tests."""
    # 验证 0.0.0.0 回退与显式 --canonical-host 逻辑（与 CLI 共用同一函数）。
    assert resolve_canonical_host("0.0.0.0", 8080, None) == "127.0.0.1:8080"
    assert resolve_canonical_host("0.0.0.0", 8080, "192.168.10.12") == "192.168.10.12"

    state = MockState(unlock_token="123456", caps_cm680=caps_cm680)

    class Handler(MockHandler):
        pass

    Handler.ui_dir = ui_dir
    Handler.state = state

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    Handler.canonical_host = resolve_canonical_host("127.0.0.1", port, None)
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()

    try:
        # 1. HTTP 静态资源与强制门户 302 重定向校验
        for path, code in [("/", 200), ("/app.css", 200), ("/app.js", 200), ("/logo.png", 200)]:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}") as resp:
                assert resp.status == code
                assert len(resp.read()) > 0

        expected_loc = f"http://{Handler.canonical_host}/"
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5.0)
        try:
            conn.request("GET", "/favicon.ico", headers={"Host": Handler.canonical_host})
            resp = conn.getresponse()
            assert resp.status == 204 and resp.read() == b""

            for probe_path in (*CAPTIVE_PROBE_PATHS, "/nonexistent"):
                conn.request("GET", probe_path, headers={"Host": "connectivitycheck.gstatic.com"})
                resp = conn.getresponse()
                body = resp.read()
                assert resp.status == 302, f"{probe_path}: got {resp.status}"
                assert resp.getheader("Location") == expected_loc, f"{probe_path}: bad Location"
                assert resp.getheader("Cache-Control") == "no-store", f"{probe_path}: bad Cache-Control"
                assert body == b"", f"{probe_path}: non-empty body"

            # GET / 携带非规范 Host 时应 302 重定向到规范根页
            conn.request("GET", "/", headers={"Host": "captive.apple.com"})
            resp = conn.getresponse()
            assert resp.status == 302 and resp.getheader("Location") == expected_loc
            assert resp.getheader("Cache-Control") == "no-store" and resp.read() == b""
        finally:
            conn.close()

        # 2. Initial hello / config / status
        c = _WsClient("127.0.0.1", port)
        hello = c.recv_until("hello")
        assert hello["proto"] == 1 and hello["factory"] is False
        cfg = c.recv_until("config")
        assert "gnss.rate" in cfg
        st = c.recv_until("status")
        assert "dev" in st and "gnss" in st and "bt" in st
        if caps_cm680:
            assert hello["caps"]["gnss.out.gsv"] == [0, 1000, 5000]
            assert cfg["gnss.out.gsv"] == 5000
            c.send_json({"t": "set", "id": 2, "key": "gnss.out.gsa", "value": "100"})
            assert c.recv_until("ack") == {"t": "ack", "id": 2, "ok": False, "err": "bad_value"}
            c.send_json({"t": "set", "id": 3, "key": "gnss.rate", "value": "1"})
            assert c.recv_until("ack") == {"t": "ack", "id": 3, "ok": True}
            assert c.recv_until("config")["gnss.rate"] == 1
            assert c.recv_until("caps")["caps"]["gnss.out.gga"] == [0, 1000]

        # 3. ws-protocol.md §6 example 1: set gnss.rate=10
        c.send_json({"t": "set", "id": 7, "key": "gnss.rate", "value": "10"})
        ack = c.recv_until("ack")
        assert ack == {"t": "ack", "id": 7, "ok": True}
        cfg = c.recv_until("config")
        assert cfg["gnss.rate"] == 10

        # 4. ws-protocol.md §6 example 2: set factory.sn while locked -> locked
        c.send_json({"t": "set", "id": 8, "key": "factory.sn", "value": "P2A0001"})
        ack = c.recv_until("ack")
        assert ack == {"t": "ack", "id": 8, "ok": False, "err": "locked"}

        # 5. All §5 error codes:
        # bad_json (malformed, bool id, out-of-range id, missing id, dup key, float, null)
        c.send_text("{bad json")
        ack = c.recv_until("ack")
        assert ack["ok"] is False and ack["err"] == "bad_json"

        for bad_req in (
            '{"t":"set","id":true,"key":"gnss.rate","value":"5"}',
            '{"t":"set","id":0,"key":"gnss.rate","value":"5"}',
            '{"t":"set","id":65536,"key":"gnss.rate","value":"5"}',
            '{"t":"set","key":"gnss.rate","value":"5"}',
            '{"t":"set","id":7,"id":8,"key":"gnss.rate","value":"5"}',
            '{"t":"set","id":7,"key":"gnss.rate","value":"5","extra":1.5}',
            '{"t":"set","id":7,"key":"gnss.rate","value":"5","extra":null}',
        ):
            c.send_text(bad_req)
            ack = c.recv_until("ack")
            assert ack == {"t": "ack", "id": 0, "ok": False, "err": "bad_json"}, bad_req
        with state.lock:
            assert state.config["gnss.rate"] == 10, "Invalid req mutated config"

        # unknown_key
        c.send_json({"t": "set", "id": 9, "key": "no.such.key", "value": "1"})
        ack = c.recv_until("ack")
        assert ack == {"t": "ack", "id": 9, "ok": False, "err": "unknown_key"}

        # bad_value (including trailing LF on MAC/SN)
        c.send_json({"t": "set", "id": 10, "key": "gnss.rate", "value": "99"})
        ack = c.recv_until("ack")
        assert ack == {"t": "ack", "id": 10, "ok": False, "err": "bad_value"}
        c.send_json({"t": "set", "id": 17, "key": "bt.wl.add", "value": "11:22:33:44:55:66\n"})
        assert c.recv_until("ack") == {"t": "ack", "id": 17, "ok": False, "err": "bad_value"}

        # unsupported
        c.send_json({"t": "cmd", "id": 11, "name": "invalid_cmd"})
        ack = c.recv_until("ack")
        assert ack == {"t": "ack", "id": 11, "ok": False, "err": "unsupported"}

        # busy / failed / timeout via forced error injection
        for idx, err_code in enumerate(("busy", "failed", "timeout"), start=12):
            with state.lock:
                state.force_err = err_code
            c.send_json({"t": "cmd", "id": idx, "name": "save"})
            ack = c.recv_until("ack")
            assert ack == {"t": "ack", "id": idx, "ok": False, "err": err_code}
        with state.lock:
            state.force_err = None
            state.drop_ack_key = "gnss.rate"
        c.send_json({"t": "set", "id": 15, "key": "gnss.rate", "value": "5"})
        c.sock.settimeout(0.35)
        try:
            while True:
                op, data = c.recv_next()
                if op == 0x1:
                    msg = json.loads(data.decode("utf-8"))
                    assert msg.get("t") != "ack", f"Unexpected ack when drop_ack active: {msg}"
        except TimeoutError:
            pass
        finally:
            c.sock.settimeout(5.0)
        with state.lock:
            state.drop_ack_key = None
            assert state.config["gnss.rate"] == 10
        c.close()

        # Reconnect after drop-ack: verify config re-synced and next save works
        c = _WsClient("127.0.0.1", port)
        c.recv_until("hello")
        assert c.recv_until("config")["gnss.rate"] == 10
        c.send_json({"t": "cmd", "id": 18, "name": "save"})
        assert c.recv_until("ack") == {"t": "ack", "id": 18, "ok": True}

        # 6. Unlock + factory.sn + whitelist (with config) + gnss_none + exit -> bye
        c.send_json({"t": "unlock", "id": 20, "token": "123456"})
        assert c.recv_until("ack") == {"t": "ack", "id": 20, "ok": True}

        c.send_json({"t": "set", "id": 19, "key": "factory.sn", "value": "P2B0042\n"})
        assert c.recv_until("ack") == {"t": "ack", "id": 19, "ok": False, "err": "bad_value"}

        c.send_json({"t": "set", "id": 21, "key": "factory.sn", "value": "P2B0042"})
        assert c.recv_until("ack") == {"t": "ack", "id": 21, "ok": True}
        assert c.recv_until("config")["factory.sn"] == "P2B0042"

        c.send_json({"t": "set", "id": 22, "key": "bt.wl.add", "value": "11:22:33:44:55:66"})
        assert c.recv_until("ack") == {"t": "ack", "id": 22, "ok": True}
        assert c.recv_until("config")["gnss.rate"] == 10
        st_bt = c.recv_until("status")
        assert "11:22:33:44:55:66" in st_bt["bt"]["whitelist"]

        c.send_json({"t": "set", "id": 23, "key": "bt.wl.del", "value": "11:22:33:44:55:66"})
        assert c.recv_until("ack") == {"t": "ack", "id": 23, "ok": True}
        assert c.recv_until("config")["gnss.rate"] == 10
        st_bt = c.recv_until("status")
        assert "11:22:33:44:55:66" not in st_bt["bt"]["whitelist"]

        with state.lock:
            state.gnss_none = True
        st_none = c.recv_until("status")
        while st_none["gnss"]["fix"] != "none":
            st_none = c.recv_until("status")
        assert "hacc_mm" not in st_none["gnss"] and "sats" not in st_none["gnss"]
        with state.lock:
            state.gnss_none = False

        c.send_json({"t": "cmd", "id": 24, "name": "save"})
        assert c.recv_until("ack") == {"t": "ack", "id": 24, "ok": True}

        # Q1 GGA quality gate config & validation
        c.send_json({"t": "set", "id": 26, "key": "gnss.gga_gate.enabled", "value": "1"})
        assert c.recv_until("ack") == {"t": "ack", "id": 26, "ok": True}
        assert c.recv_until("config")["gga_gate"] == {"enabled": True, "min_sats": 8}

        c.send_json({"t": "set", "id": 27, "key": "gnss.gga_gate.min_sats", "value": "12"})
        assert c.recv_until("ack") == {"t": "ack", "id": 27, "ok": True}
        assert c.recv_until("config")["gga_gate"] == {"enabled": True, "min_sats": 12}

        for bad_idx, bad_val in enumerate(("-1", "41", "08", "abc"), start=28):
            c.send_json(
                {"t": "set", "id": bad_idx, "key": "gnss.gga_gate.min_sats", "value": bad_val}
            )
            assert c.recv_until("ack") == {
                "t": "ack",
                "id": bad_idx,
                "ok": False,
                "err": "bad_value",
            }

        c.send_json({"t": "cmd", "id": 25, "name": "exit"})
        assert c.recv_until("ack") == {"t": "ack", "id": 25, "ok": True}
        assert c.recv_until("bye") == {"t": "bye", "reason": "exit"}
        c.close()

        # 7. too_long (> 1024 B) -> close code 1009
        c2 = _WsClient("127.0.0.1", port)
        c2.recv_until("hello")
        c2.send_text("x" * 1025)
        for _ in range(10):
            op, payload = c2.recv_next()
            if op == 0x8:
                (code,) = struct.unpack("!H", payload[:2])
                assert code == 1009, f"Expected 1009, got {code}"
                break
        else:
            raise AssertionError("Expected close frame 1009 for too_long")
        c2.close()

        # 8. OTA endpoints (ADR-0019 §2.3)
        def http_req(
            method: str,
            path: str,
            body: bytes | None = None,
            headers: dict[str, str] | None = None,
        ) -> tuple[int, str, bytes]:
            hc = http.client.HTTPConnection("127.0.0.1", port, timeout=5.0)
            try:
                hc.request(method, path, body=body, headers=headers or {})
                r = hc.getresponse()
                return r.status, r.getheader("Content-Type") or "", r.read()
            finally:
                hc.close()

        # 8a. GET /ota/info contract
        st_code, ctype, raw_body = http_req("GET", "/ota/info")
        assert st_code == 200 and "application/json" in ctype
        info = json.loads(raw_body.decode("utf-8"))
        assert info == {
            "running": "0.1.0-mock",
            "project": "rtk_main",
            "part": "ota_0",
            "state": "valid",
            "other": None,
            "max": OTA_DEFAULT_MAX,
            "busy": False,
        }, f"Unexpected /ota/info: {info}"

        valid_bin = build_mock_firmware("6.2.0-test", "rtk_main", total_size=4096)

        # 8b. Missing Content-Length (raw HTTP) and Content-Length: 0 -> 411 length
        raw_sock = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        try:
            raw_sock.sendall(
                b"POST /ota HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                b"Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n"
            )
            raw_resp = b""
            while True:
                chunk = raw_sock.recv(4096)
                if not chunk:
                    break
                raw_resp += chunk
        finally:
            raw_sock.close()
        assert b" 411 " in raw_resp.split(b"\r\n", 1)[0]
        assert json.loads(raw_resp.split(b"\r\n\r\n", 1)[1].decode("utf-8")) == {
            "ok": False,
            "err": "length",
        }

        st_code, ctype, raw_body = http_req(
            "POST", "/ota", body=b"", headers={"Content-Type": "application/octet-stream", "Content-Length": "0"}
        )
        assert st_code == 411 and "application/json" in ctype
        assert json.loads(raw_body.decode("utf-8")) == {"ok": False, "err": "length"}

        # 8c. too_large (> max) -> 413 too_large
        with state.lock:
            state.ota_max = 2048
        st_code, ctype, raw_body = http_req(
            "POST", "/ota", body=valid_bin, headers={"Content-Type": "application/octet-stream"}
        )
        assert st_code == 413 and "application/json" in ctype
        assert json.loads(raw_body.decode("utf-8")) == {"ok": False, "err": "too_large"}
        with state.lock:
            state.ota_max = OTA_DEFAULT_MAX

        # 8d. bad_image: wrong byte 0, wrong chip_id (ESP32-S3), wrong desc magic, wrong project_name, BOM+project, <= 288 B
        assert parse_esp_app_header(valid_bin[: OTA_HDR_BYTES - 1]) is None
        parsed_32 = parse_esp_app_header(
            build_mock_firmware("", total_size=1024, raw_version=b"V" * 32, raw_project=b"P" * 32)
        )
        assert parsed_32 == ("V" * 32, b"P" * 32), f"32 B no-NUL parse mismatch: {parsed_32!r}"
        parsed_inv = parse_esp_app_header(
            build_mock_firmware("", total_size=1024, raw_version=b"\xff1.2.3", raw_project=b"rtk_main")
        )
        assert parsed_inv == ("\ufffd1.2.3", b"rtk_main"), f"invalid UTF-8 mismatch: {parsed_inv!r}"
        for raw_ver, want_ver in (
            (b"v1\xe2\x82", "v1\ufffd"),
            (b"\xe2\x82A", "\ufffdA"),
            (b"\xf0\x9f\x98", "\ufffd"),
            (b"\xf0\x9f.1", "\ufffd.1"),
            (b"\xf0\x9f\xe2\x82\xac", "\ufffd\u20ac"),
        ):
            parsed_trunc = parse_esp_app_header(
                build_mock_firmware("", total_size=1024, raw_version=raw_ver, raw_project=b"rtk_main")
            )
            assert parsed_trunc == (want_ver, b"rtk_main"), (
                f"truncated UTF-8 mismatch for {raw_ver!r}: {parsed_trunc!r}"
            )

        for bad_blob in (
            build_mock_firmware("6.2.0", "rtk_main", total_size=1024, img_magic=0x00),
            build_mock_firmware("6.2.0", "rtk_main", total_size=1024, chip_id=0x0009),
            build_mock_firmware("6.2.0", "rtk_main", total_size=1024, desc_magic=0x12345678),
            build_mock_firmware("6.2.0", "other_proj", total_size=1024),
            build_mock_firmware("6.2.0", total_size=1024, raw_project=b"\xef\xbb\xbf" + b"rtk_main"),
            build_mock_firmware("6.2.0", "rtk_main", total_size=OTA_HDR_BYTES),
        ):
            st_code, ctype, raw_body = http_req(
                "POST", "/ota", body=bad_blob, headers={"Content-Type": "application/octet-stream"}
            )
            assert st_code == 400 and "application/json" in ctype
            assert json.loads(raw_body.decode("utf-8")) == {"ok": False, "err": "bad_image"}

        # 8d-2. Oversized incomplete body (only 1 B sent) -> prompt 413 + no busy blockage
        with state.lock:
            state.ota_max = 2048
        s_over = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        try:
            t_over0 = time.monotonic()
            s_over.sendall(
                b"POST /ota HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Length: 4096\r\n\r\n\xe9"
            )
            over_resp = b""
            while True:
                ch = s_over.recv(4096)
                if not ch:
                    break
                over_resp += ch
            t_over = time.monotonic() - t_over0
        finally:
            s_over.close()
        assert t_over < 1.5, f"Incomplete 413 took too long: {t_over:.2f}s"
        assert b" 413 " in over_resp.split(b"\r\n", 1)[0], f"Expected 413, got {over_resp!r}"
        assert json.loads(over_resp.split(b"\r\n\r\n", 1)[1].decode("utf-8")) == {
            "ok": False,
            "err": "too_large",
        }
        with state.lock:
            state.ota_max = OTA_DEFAULT_MAX
            assert not state.ota_busy

        # 8d-3. Real receive stall after valid 288 B -> 408 timeout + busy released
        with state.lock:
            state.ota_recv_timeout_s = 0.35
        s_stall = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        try:
            stall_hdr = (
                b"POST /ota HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Length: 4096\r\n\r\n"
            )
            s_stall.sendall(stall_hdr + valid_bin[:OTA_HDR_BYTES])
            # Confirm busy is True while stalled
            deadline_b = time.monotonic() + 1.0
            saw_busy = False
            while time.monotonic() < deadline_b:
                _, _, rb_chk = http_req("GET", "/ota/info")
                if json.loads(rb_chk.decode("utf-8"))["busy"]:
                    saw_busy = True
                    break
                time.sleep(0.01)
            assert saw_busy, "Expected busy=True while receive stalled"
            stall_resp = b""
            while True:
                ch = s_stall.recv(4096)
                if not ch:
                    break
                stall_resp += ch
        finally:
            s_stall.close()
            with state.lock:
                state.ota_recv_timeout_s = OTA_RECV_TIMEOUT_S
        assert b" 408 " in stall_resp.split(b"\r\n", 1)[0], f"Expected 408, got {stall_resp!r}"
        assert json.loads(stall_resp.split(b"\r\n\r\n", 1)[1].decode("utf-8")) == {
            "ok": False,
            "err": "timeout",
        }
        _, _, rb_post_to = http_req("GET", "/ota/info")
        assert json.loads(rb_post_to.decode("utf-8"))["busy"] is False

        # 8d-4. Steady trickle (< recv timeout per chunk) exceeding total deadline -> 408 timeout
        with state.lock:
            state.ota_recv_timeout_s = 2.0
            state.ota_total_timeout_s = 0.45
        s_trickle = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        try:
            trickle_hdr = (
                b"POST /ota HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                b"Content-Type: application/octet-stream\r\n"
                b"Content-Length: 4096\r\n\r\n"
            )
            s_trickle.sendall(trickle_hdr + valid_bin[:OTA_HDR_BYTES])
            sent_off = OTA_HDR_BYTES
            for _ in range(8):
                time.sleep(0.08)
                try:
                    s_trickle.sendall(valid_bin[sent_off : sent_off + 64])
                    sent_off += 64
                except OSError:
                    break
            trickle_resp = b""
            while True:
                ch = s_trickle.recv(4096)
                if not ch:
                    break
                trickle_resp += ch
        finally:
            s_trickle.close()
            with state.lock:
                state.ota_recv_timeout_s = OTA_RECV_TIMEOUT_S
                state.ota_total_timeout_s = OTA_TOTAL_TIMEOUT_S
        assert b" 408 " in trickle_resp.split(b"\r\n", 1)[0], f"Expected 408, got {trickle_resp!r}"
        assert json.loads(trickle_resp.split(b"\r\n\r\n", 1)[1].decode("utf-8")) == {
            "ok": False,
            "err": "timeout",
        }
        _, _, rb_post_trickle = http_req("GET", "/ota/info")
        assert json.loads(rb_post_trickle.decode("utf-8"))["busy"] is False

        # 8e. Every --ota-err code maps to exact HTTP status + JSON
        for err_code, http_status in OTA_ERR_HTTP.items():
            with state.lock:
                state.ota_err = err_code
            if err_code == "state":
                sc_i, _, rb_i = http_req("GET", "/ota/info")
                assert sc_i == 200 and json.loads(rb_i.decode("utf-8"))["state"] == "pending_verify"
            st_code, ctype, raw_body = http_req(
                "POST", "/ota", body=valid_bin, headers={"Content-Type": "application/octet-stream"}
            )
            assert st_code == http_status, f"{err_code}: expected {http_status}, got {st_code}"
            assert "application/json" in ctype
            assert json.loads(raw_body.decode("utf-8")) == {"ok": False, "err": err_code}
        with state.lock:
            state.ota_err = None

        # 8f. Slow upload throttle + concurrent busy (HTTP & WS set/cmd) + reboot window atomicity + WS EOF disconnect
        slow_bin = build_mock_firmware("6.2.0-test", "rtk_main", total_size=8192)
        with state.lock:
            state.ota_slow_kbps = 32.0
            state.ota_reboot_ms = 250
        c3 = _WsClient("127.0.0.1", port)
        c3.recv_until("hello")
        c3.recv_until("config")
        c3.recv_until("status")

        upload_res: dict[str, object] = {}

        def _do_upload() -> None:
            t_start = time.monotonic()
            upload_res["resp"] = http_req(
                "POST", "/ota", body=slow_bin, headers={"Content-Type": "application/octet-stream"}
            )
            upload_res["elapsed"] = time.monotonic() - t_start

        up_th = threading.Thread(target=_do_upload)
        up_th.start()
        # Wait until upload is active
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            with state.lock:
                if state.ota_busy:
                    break
            time.sleep(0.01)
        assert state.ota_busy, "Expected ota_busy during slow upload"
        # Concurrent WS set & cmd -> busy (and config untouched); concurrent POST /ota -> 409 busy
        c3.send_json({"t": "set", "id": 40, "key": "gnss.rate", "value": "5"})
        assert c3.recv_until("ack") == {"t": "ack", "id": 40, "ok": False, "err": "busy"}
        c3.send_json({"t": "cmd", "id": 41, "name": "exit"})
        assert c3.recv_until("ack") == {"t": "ack", "id": 41, "ok": False, "err": "busy"}
        with state.lock:
            assert state.config["gnss.rate"] == 10, "WS set mutated config while ota_busy"
        sc_busy, _, rb_busy = http_req(
            "POST", "/ota", body=valid_bin, headers={"Content-Type": "application/octet-stream"}
        )
        assert sc_busy == 409 and json.loads(rb_busy.decode("utf-8")) == {"ok": False, "err": "busy"}

        up_th.join(timeout=5.0)
        assert not up_th.is_alive()
        st_code, ctype, raw_body = upload_res["resp"]  # type: ignore[misc]
        assert st_code == 200 and "application/json" in ctype
        assert json.loads(raw_body.decode("utf-8")) == {
            "ok": True,
            "ver": "6.2.0-test",
            "reboot_ms": 250,
        }
        assert float(upload_res["elapsed"]) >= 0.15, f"Slow throttle too fast: {upload_res['elapsed']}"

        # Immediately after 200 (before reboot completes): running/other/part must NOT switch yet, busy=True, 2nd POST -> 409 busy
        st_pre, _, rb_pre = http_req("GET", "/ota/info")
        assert st_pre == 200
        info_pre = json.loads(rb_pre.decode("utf-8"))
        assert info_pre["running"] == "0.1.0-mock", f"Switched before reboot: {info_pre}"
        assert info_pre["other"] is None, f"other switched before reboot: {info_pre}"
        assert info_pre["part"] == "ota_0", f"part switched before reboot: {info_pre}"
        assert info_pre["busy"] is True, f"Expected busy=True in reboot window: {info_pre}"
        sc_win, _, rb_win = http_req(
            "POST", "/ota", body=valid_bin, headers={"Content-Type": "application/octet-stream"}
        )
        assert sc_win == 409 and json.loads(rb_win.decode("utf-8")) == {"ok": False, "err": "busy"}

        # Verify simulated reboot disconnects active WS client via EOF or close frame within absolute deadline
        try:
            ws_closed = _wait_ws_closed(c3, timeout_s=2.0)
        finally:
            c3.close()
        assert ws_closed, "Expected mock reboot to close active WS connection (EOF/close within absolute deadline)"

        # Regression check: _wait_ws_closed must fail within absolute deadline even when status frames keep arriving
        class _SpamWsClient:
            def recv_next(self, deadline: float | None = None) -> tuple[int, bytes]:
                time.sleep(0.04)
                return 0x1, b'{"t":"status"}'

        t_spam0 = time.monotonic()
        assert not _wait_ws_closed(_SpamWsClient(), timeout_s=0.18), (  # type: ignore[arg-type]
            "Expected _wait_ws_closed to return False when status frames keep arriving past absolute deadline"
        )
        t_spam = time.monotonic() - t_spam0
        assert t_spam < 0.8, f"_wait_ws_closed exceeded absolute deadline bound: {t_spam:.2f}s"

        # Regression check: BrokenPipeError during initial WS snapshot send must clean up ws_clients without leaking fileno()==-1
        orig_send_status = Handler._send_status
        injected_fail = {"armed": True}

        def _fail_first_status(
            self_h: MockHandler,
            sock_h: socket.socket,
            send_lock_h: threading.Lock,
            tick_h: int,
            include_bt: list[str] | None,
        ) -> None:
            if tick_h == 0 and injected_fail["armed"]:
                injected_fail["armed"] = False
                raise BrokenPipeError("injected initial status failure")
            orig_send_status(self_h, sock_h, send_lock_h, tick_h, include_bt)

        Handler._send_status = _fail_first_status  # type: ignore[method-assign]
        try:
            c_fail = _WsClient("127.0.0.1", port)
            try:
                assert _wait_ws_closed(c_fail, timeout_s=1.5), (
                    "Expected WS connection to close after initial status send failure"
                )
            finally:
                c_fail.close()
        finally:
            Handler._send_status = orig_send_status  # type: ignore[method-assign]
        deadline_ws_clean = time.monotonic() + 1.0
        while time.monotonic() < deadline_ws_clean:
            with state.lock:
                if not state.ws_clients:
                    break
            time.sleep(0.01)
        with state.lock:
            assert not state.ws_clients, (
                f"Leaked ws_clients after initial send failure: {[s.fileno() for s in state.ws_clients]}"
            )

        # Verify /ota/info updated atomically after reboot
        st_code, _, raw_body = http_req("GET", "/ota/info")
        assert st_code == 200
        info_after = json.loads(raw_body.decode("utf-8"))
        assert info_after["running"] == "6.2.0-test"
        assert info_after["other"] == "0.1.0-mock"
        assert info_after["part"] == "ota_1"
        assert info_after["busy"] is False

        # 8f-2. Empty version consistency (POST ver="" == post-reboot GET running="") & response send failure still reboots
        with state.lock:
            state.ota_slow_kbps = 0.0
            state.ota_reboot_ms = 80
        empty_ver_bin = build_mock_firmware("", "rtk_main", total_size=1024)
        st_ev, _, rb_ev = http_req(
            "POST", "/ota", body=empty_ver_bin, headers={"Content-Type": "application/octet-stream"}
        )
        assert st_ev == 200 and json.loads(rb_ev.decode("utf-8")) == {
            "ok": True,
            "ver": "",
            "reboot_ms": 80,
        }
        deadline_r = time.monotonic() + 1.5
        while time.monotonic() < deadline_r:
            _, _, rb_r = http_req("GET", "/ota/info")
            info_ev = json.loads(rb_r.decode("utf-8"))
            if not info_ev["busy"]:
                break
            time.sleep(0.02)
        assert info_ev["busy"] is False
        assert info_ev["running"] == "", f"Empty version mismatch: {info_ev}"
        assert info_ev["other"] == "6.2.0-test"
        assert info_ev["part"] == "ota_0"

        # Client closes socket right after sending full valid body -> reboot still runs and releases busy
        s_drop = socket.create_connection(("127.0.0.1", port), timeout=5.0)
        drop_bin = build_mock_firmware("6.3.0-drop", "rtk_main", total_size=1024)
        s_drop.sendall(
            (
                f"POST /ota HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                f"Content-Type: application/octet-stream\r\n"
                f"Content-Length: {len(drop_bin)}\r\n\r\n"
            ).encode("ascii")
            + drop_bin
        )
        s_drop.close()
        deadline_d = time.monotonic() + 1.5
        while time.monotonic() < deadline_d:
            _, _, rb_d = http_req("GET", "/ota/info")
            info_d = json.loads(rb_d.decode("utf-8"))
            if info_d["running"] == "6.3.0-drop" and not info_d["busy"]:
                break
            time.sleep(0.02)
        assert info_d["running"] == "6.3.0-drop" and info_d["busy"] is False and info_d["part"] == "ota_1"

        # 8g. --no-ota returns 404 on /ota/info
        with state.lock:
            state.no_ota = True
        st_code, _, _ = http_req("GET", "/ota/info")
        assert st_code == 404

        print("SELFTEST PASS: HTTP, WS handshake, §6 examples, all §5 error codes, bye, 1009, OTA §2.3")
        return 0
    finally:
        with state.lock:
            t = state.ota_reboot_timer
            state.ota_reboot_timer = None
        if t is not None:
            t.cancel()
        srv.shutdown()
        srv.server_close()


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="RTK Web UI mock server.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--unlock-token", default="123456")
    parser.add_argument("--no-const", action="store_true", help="Omit gnss.const.* keys")
    parser.add_argument(
        "--force-err",
        default=None,
        choices=["busy", "timeout", "failed", "unsupported", "locked"],
        help="Force all set/cmd requests to fail with this error code",
    )
    parser.add_argument(
        "--drop-ack",
        default=None,
        help="Drop ack for set requests matching this key (e.g. gnss.rate)",
    )
    parser.add_argument(
        "--gnss-none",
        action="store_true",
        help="Report fix=none and omit sats/hacc_mm/hz in GNSS status",
    )
    parser.add_argument("--selftest", action="store_true", help="Run self-test and exit")
    parser.add_argument("--caps-cm680", action="store_true", help="Use verified CM680 capabilities")
    parser.add_argument(
        "--canonical-host",
        default=None,
        help="强制门户 302 目标规范 Host（默认使用绑定地址与实际端口，0.0.0.0 回退为 127.0.0.1）",
    )
    parser.add_argument(
        "--ota-err",
        default=None,
        choices=list(OTA_ERR_HTTP.keys()),
        help="Inject specified OTA error code on POST /ota",
    )
    parser.add_argument(
        "--ota-slow-kbps",
        type=float,
        default=0.0,
        help="Throttle POST /ota body read speed in KB/s (0 = unlimited)",
    )
    parser.add_argument(
        "--ota-max",
        type=int,
        default=OTA_DEFAULT_MAX,
        help="Target OTA partition max size in bytes (default 1966080)",
    )
    parser.add_argument(
        "--no-ota",
        action="store_true",
        help="Return 404 for /ota/info and /ota (simulate legacy firmware)",
    )
    args = parser.parse_args()
    if args.ota_slow_kbps < 0:
        parser.error("--ota-slow-kbps must be >= 0")
    if args.ota_max <= 0:
        parser.error("--ota-max must be > 0")

    ui_dir = repo_root / "main" / "web" / "ui"
    if args.selftest:
        return run_selftest(ui_dir, args.caps_cm680)

    state = MockState(
        unlock_token=args.unlock_token,
        no_const=args.no_const,
        caps_cm680=args.caps_cm680,
        ota_err=args.ota_err,
        ota_slow_kbps=args.ota_slow_kbps,
        ota_max=args.ota_max,
        no_ota=args.no_ota,
    )
    state.force_err = args.force_err
    state.drop_ack_key = args.drop_ack
    state.gnss_none = args.gnss_none

    class Handler(MockHandler):
        pass

    Handler.ui_dir = ui_dir
    Handler.state = state

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    actual_port = srv.server_address[1]
    Handler.canonical_host = resolve_canonical_host(args.host, actual_port, args.canonical_host)
    print(f"Serving {ui_dir} at http://{args.host}:{actual_port}/ (ws://.../ws)", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
