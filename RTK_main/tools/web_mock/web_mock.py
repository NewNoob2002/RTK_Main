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


class MockState:
    """Shared mutable device state for the mock server."""

    def __init__(self, unlock_token: str = "123456", no_const: bool = False,
                 caps_cm680: bool = False) -> None:
        self.lock = threading.Lock()
        self.unlock_token = unlock_token
        self.no_const = no_const or caps_cm680
        self.caps_cm680 = caps_cm680
        self.force_err: str | None = None
        self.drop_ack_key: str | None = None
        self.gnss_none: bool = False
        self.wl_version: int = 0
        self.start_time = time.monotonic()
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
        return result


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

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/ws":
            self._handle_ws()
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

        with self.state.lock:
            hello = {
                "t": "hello",
                "caps": self.state.caps(),
                "proto": 1,
                "board": "P2",
                "fw": "0.1.0-mock",
                "bt_name": "P2-A1B2",
                "console": "dev",
                "factory": unlocked,
            }
            cfg = {"t": "config", **self.state.config}
            wl = list(self.state.whitelist)

        ws_send_json(sock, send_lock, hello)
        ws_send_json(sock, send_lock, cfg)
        with self.state.lock:
            last_wl_ver = self.state.wl_version
        self._send_status(sock, send_lock, 0, include_bt=wl)

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

        t_thread = threading.Thread(target=ticker, daemon=True)
        t_thread.start()

        try:
            while not stop_ev.is_set():
                try:
                    opcode, payload = ws_read_frame(sock, require_mask=True)
                except ValueError:
                    ws_send_close(sock, send_lock, 1009)
                    break
                except (EOFError, OSError):
                    break

                if opcode == 0x8:
                    break
                if opcode == 0x9:
                    with send_lock:
                        ws_write_frame(sock, 0xA, payload, mask=False)
                    continue
                if opcode != 0x1:
                    continue

                unlocked, should_close = self._on_text(
                    sock, send_lock, payload, unlocked
                )
                if should_close:
                    break
        finally:
            stop_ev.set()
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

        with self.state.lock:
            forced = self.state.force_err
        if forced is not None:
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": forced}
            )
            return unlocked, False

        if rtype == "unlock":
            tok = req["token"]
            if tok == self.state.unlock_token:
                ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
                return True, False
            ws_send_json(
                sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": "bad_value"}
            )
            return unlocked, False

        if rtype == "cmd":
            name = req["name"]
            if name == "save":
                ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
                return unlocked, False
            if name == "exit":
                ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
                ws_send_json(sock, send_lock, {"t": "bye", "reason": "exit"})
                return unlocked, True
            if name == "freset":
                if not unlocked:
                    ws_send_json(
                        sock,
                        send_lock,
                        {"t": "ack", "id": rid, "ok": False, "err": "locked"},
                    )
                    return unlocked, False
                ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
                ws_send_json(sock, send_lock, {"t": "bye", "reason": "freset"})
                return unlocked, True
            ws_send_json(
                sock,
                send_lock,
                {"t": "ack", "id": rid, "ok": False, "err": "unsupported"},
            )
            return unlocked, False

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
        with self.state.lock:
            drop_key = self.state.drop_ack_key
        if drop_key is not None and key == drop_key:
            return False

        if key == "gnss.rate":
            if val not in {str(v) for v in self.state.caps().get(key, [])}:
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return False
            with self.state.lock:
                self.state.config[key] = int(val)
                cfg = {"t": "config", **self.state.config}
            ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
            ws_send_json(sock, send_lock, cfg)
            ws_send_json(sock, send_lock, {"t": "caps", "caps": self.state.caps()})
            return False

        if key in NMEA_KEYS:
            if val not in {str(v) for v in self.state.caps().get(key, [])}:
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return False
            with self.state.lock:
                self.state.config[key] = int(val)
                cfg = {"t": "config", **self.state.config}
            ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
            ws_send_json(sock, send_lock, cfg)
            return False

        if key == "gnss.gga_gate.enabled":
            if val not in VALID_BOOL:
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return False
            with self.state.lock:
                gate = dict(self.state.config["gga_gate"])
                gate["enabled"] = val == "1"
                self.state.config["gga_gate"] = gate
                cfg = {"t": "config", **self.state.config}
            ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
            ws_send_json(sock, send_lock, cfg)
            return False

        if key == "gnss.gga_gate.min_sats":
            if not re.fullmatch(r"0|[1-9][0-9]*", val) or int(val) > 40:
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return False
            with self.state.lock:
                gate = dict(self.state.config["gga_gate"])
                gate["min_sats"] = int(val)
                self.state.config["gga_gate"] = gate
                cfg = {"t": "config", **self.state.config}
            ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
            ws_send_json(sock, send_lock, cfg)
            return False

        if key in CONST_KEYS:
            if self.state.no_const:
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "unsupported"},
                )
                return False
            if val not in VALID_BOOL:
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return False
            with self.state.lock:
                self.state.config[key] = int(val)
                cfg = {"t": "config", **self.state.config}
            ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
            ws_send_json(sock, send_lock, cfg)
            return False

        if key == "bt.pair_next":
            if val not in VALID_BOOL:
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return False
            with self.state.lock:
                self.state.config[key] = int(val)
                cfg = {"t": "config", **self.state.config}
            ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
            ws_send_json(sock, send_lock, cfg)
            return False

        if key in ("bt.wl.add", "bt.wl.del"):
            if not MAC_RE.fullmatch(val):
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return False
            mac = val.upper()
            with self.state.lock:
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
                        ws_send_json(
                            sock,
                            send_lock,
                            {"t": "ack", "id": rid, "ok": False, "err": "failed"},
                        )
                        return False
                    self.state.whitelist = new_wl
                else:
                    self.state.whitelist = [m for m in self.state.whitelist if m != mac]
                wl = list(self.state.whitelist)
                self.state.wl_version += 1
                cfg = {"t": "config", **self.state.config}
            ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
            ws_send_json(sock, send_lock, cfg)
            self._send_status(sock, send_lock, 0, include_bt=wl)
            return False

        if key == "factory.sn":
            if not unlocked:
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "locked"},
                )
                return False
            if not SN_RE.fullmatch(val):
                ws_send_json(
                    sock,
                    send_lock,
                    {"t": "ack", "id": rid, "ok": False, "err": "bad_value"},
                )
                return False
            with self.state.lock:
                self.state.config[key] = val
                cfg = {"t": "config", **self.state.config}
            ws_send_json(sock, send_lock, {"t": "ack", "id": rid, "ok": True})
            ws_send_json(sock, send_lock, cfg)
            return False

        ws_send_json(
            sock, send_lock, {"t": "ack", "id": rid, "ok": False, "err": "unknown_key"}
        )
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

    def _read_exact(self, n: int) -> bytes:
        while len(self._buf) < n:
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

    def recv_next(self) -> tuple[int, bytes]:
        hdr = self._read_exact(2)
        b0, b1 = hdr[0], hdr[1]
        opcode = b0 & 0x0F
        length = b1 & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._read_exact(2))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._read_exact(8))
        payload = self._read_exact(length) if length > 0 else b""
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

        print("SELFTEST PASS: HTTP, WS handshake, §6 examples, all §5 error codes, bye, 1009")
        return 0
    finally:
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
    args = parser.parse_args()

    ui_dir = repo_root / "main" / "web" / "ui"
    if args.selftest:
        return run_selftest(ui_dir, args.caps_cm680)

    state = MockState(unlock_token=args.unlock_token, no_const=args.no_const,
                      caps_cm680=args.caps_cm680)
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
    print(f"Serving {ui_dir} at http://{args.host}:{actual_port}/ (ws://.../ws)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
