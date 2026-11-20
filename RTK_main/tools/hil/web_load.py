#!/usr/bin/env python3
"""H1 SoftAP 负载测量。使用 curl、IDF Python 环境的 websockets，离线自检只需标准库。"""

import argparse
import json
import math
import statistics
import subprocess
import sys
import time
from pathlib import Path


SAMPLES = 20
STATUS_MISSING_GAP_S = 1.5
STATUS_TIMEOUT_S = 5.0  # 允许跨过缺帧后继续采样。
SET_KEY = "gnss.gga_gate.min_sats"


def timing(samples):
    """输出中位数及 nearest-rank P90，单位为毫秒。"""
    ordered = sorted(samples)
    return {
        "samples": len(ordered),
        "median_ms": round(statistics.median(ordered), 3),
        "p90_ms": round(ordered[math.ceil(len(ordered) * 0.9) - 1], 3),
    }


def home_load(url):
    """每次用新 curl 进程完成一轮 TCP 连接与 gzip 页面传输。"""
    samples = []
    for _ in range(SAMPLES):
        result = subprocess.run(
            ["curl", "--noproxy", "*", "--compressed", "--silent", "--show-error",
             "--max-time", "10", "--output", "/dev/null", "--write-out",
             "%{http_code} %{time_total}", url + "/"],
            capture_output=True, text=True, check=True,
        )
        code, seconds = result.stdout.split()
        if code != "200":
            raise ValueError(f"首页预期 HTTP 200，收到 {code}")
        samples.append(float(seconds) * 1000)
    return timing(samples)


def recv_frame(ws, deadline, accept):
    """在固定期限内读取符合条件的协议消息。"""
    while (remaining := deadline - time.perf_counter()) > 0:
        frame = json.loads(ws.recv(timeout=remaining))
        if not isinstance(frame, dict):
            raise ValueError("WS 消息不是 JSON 对象")
        if frame.get("t") == "bye":
            raise ValueError(f"设备结束会话 ({frame.get('reason')})")
        if accept(frame):
            return frame
    raise TimeoutError("等待 WS 消息超时")


def recv_status(ws, deadline):
    """使用带 dev.uptime_s 的周期性状态，忽略零散分组更新。"""
    def periodic(frame):
        dev = frame.get("dev")
        return (frame.get("t") == "status" and isinstance(dev, dict)
                and type(dev.get("uptime_s")) is int)

    return recv_frame(ws, deadline, periodic)["dev"]["uptime_s"]


def status_load(ws, duration):
    """按到达间隔累计缺帧，从首个周期帧起计时。"""
    uptime = recv_status(ws, time.perf_counter() + STATUS_TIMEOUT_S)
    previous = time.perf_counter()
    started = None
    max_gap = 0.0
    intervals = 0
    missing = 0
    uptime_jumps = 0
    while started is None or previous - started < duration:
        try:
            current_uptime = recv_status(ws, previous + STATUS_TIMEOUT_S)
        except TimeoutError as exc:
            raise ValueError(f"{STATUS_TIMEOUT_S:g} s 内未收到状态帧，已采样 {intervals} 个周期") from exc
        now = time.perf_counter()
        delta = current_uptime - uptime
        # 初始全量帧和首个周期帧可能落在同一秒。
        if delta < 0 or (delta == 0 and started is not None):
            raise ValueError(f"状态帧 uptime 未递增，已采样 {intervals} 个周期")
        if delta > 1:
            uptime_jumps += 1
        gap = now - previous
        if gap > STATUS_MISSING_GAP_S:
            missing += round(gap) - 1
        max_gap = max(max_gap, gap)
        if started is None:
            started = now
        else:
            intervals += 1
        previous = now
        uptime = current_uptime
    return {"duration_s": round(previous - started, 3), "intervals": intervals,
            "max_gap_ms": round(max_gap * 1000, 3), "missing": missing,
            "uptime_jumps": uptime_jumps}


def set_and_time(ws, request_id, value):
    """按页面协议发送设置，匹配同一 ID 的成功 ack。"""
    request = {"t": "set", "id": request_id, "key": SET_KEY, "value": str(value)}
    started = time.perf_counter()
    ws.send(json.dumps(request))
    ack = recv_frame(ws, started + 2, lambda frame: frame.get("t") == "ack"
                     and frame.get("id") == request_id)
    if ack.get("ok") is not True:
        raise ValueError(f"设置被拒绝，id={request_id} err={ack.get('err')}")
    return (time.perf_counter() - started) * 1000


def ack_load(ws, config):
    """交替保存相邻值与原值，异常时尝试恢复。"""
    gate = config.get("gga_gate")
    original = gate.get("min_sats") if isinstance(gate, dict) else None
    if type(original) is not int or not 0 <= original <= 40:
        raise ValueError("config 缺少有效的 gga_gate.min_sats")
    alternate = original + 1 if original < 40 else original - 1
    samples = []
    try:
        for request_id in range(1, SAMPLES + 1):
            value = alternate if request_id % 2 else original
            samples.append(set_and_time(ws, request_id, value))
    except (Exception, KeyboardInterrupt) as exc:
        try:
            set_and_time(ws, SAMPLES + 1, original)
        except Exception as restore_error:
            raise ValueError(f"{exc}。恢复失败，请在页面将 {SET_KEY} 恢复为 {original}"
                             f"（{restore_error}）") from exc
        raise
    return timing(samples)


def selftest():
    """用录制形状的消息和模拟时钟覆盖采样、掉帧与 ack 匹配。"""
    from unittest.mock import Mock, patch

    if timing(range(1, 21)) != {"samples": 20, "median_ms": 10.5, "p90_ms": 18}:
        raise ValueError("百分位自检失败")
    with patch.object(subprocess, "run", return_value=Mock(stdout="200 0.012")) as curl:
        if home_load("http://offline")["median_ms"] != 12 or curl.call_count != SAMPLES:
            raise ValueError("首页采样自检失败")

    def sample_status(frames, duration):
        arrivals = iter(frames)
        now = 0.0

        def recv(timeout):
            nonlocal now
            arrival, uptime = next(arrivals, (math.inf, None))
            if arrival - now > timeout:
                raise TimeoutError("no frame")
            now = arrival
            return json.dumps({"t": "status", "dev": {"uptime_s": uptime}})

        with patch.object(time, "perf_counter", side_effect=lambda: now):
            return status_load(Mock(recv=recv), duration)

    jitter = sample_status([(0, 4), (1, 5), (2.15, 7)], 1)
    if jitter["missing"] != 0 or jitter["uptime_jumps"] != 1 or jitter["max_gap_ms"] != 1150:
        raise ValueError("1.15 s 间隔内 uptime 跳秒应计 0 个缺帧")
    alignment = sample_status([(0, 4), (1.15, 6), (2.3, 7)], 1)
    if alignment["missing"] != 0 or alignment["uptime_jumps"] != 1:
        raise ValueError("首帧对齐的 uptime 跳秒应计 0 个缺帧")
    dropped = sample_status([(0, 4), (1, 5), (3, 7)], 1)
    if dropped["missing"] != 1 or dropped["max_gap_ms"] != 2000:
        raise ValueError("2.0 s 间隔应计 1 个缺帧")
    continued = sample_status([(0, 4), (1, 5), (3, 7), (4, 8), (6, 10)], 5)
    if continued["missing"] != 2 or continued["duration_s"] != 5 or continued["intervals"] != 3:
        raise ValueError("缺帧后应继续采样到指定时长")
    try:
        sample_status([(0, 4), (1, 5)], 1)
    except ValueError as exc:
        if "未收到状态帧" not in str(exc):
            raise
    else:
        raise ValueError("超时应判失败")

    ws = Mock()
    acks = [{"t": "ack", "id": 65535, "ok": False}]
    acks.extend({"t": "ack", "id": n, "ok": True} for n in range(1, SAMPLES + 1))
    ws.recv.side_effect = map(json.dumps, acks)
    if ack_load(ws, {"gga_gate": {"min_sats": 40}})["samples"] != SAMPLES:
        raise ValueError("ack 采样自检失败")
    sent = [json.loads(call.args[0]) for call in ws.send.call_args_list]
    if sent[0] != {"t": "set", "id": 1, "key": SET_KEY, "value": "39"} or sent[-1]["value"] != "40":
        raise ValueError("设置请求或恢复值自检失败")
    print(json.dumps({"selftest": "PASS"}, ensure_ascii=False))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.10.12", help="SoftAP 的设备地址")
    parser.add_argument("--stage", choices=("all", "home", "status", "ack"), default="all")
    parser.add_argument("--duration", type=int, default=600, help="状态采样持续秒数")
    parser.add_argument("--json-out", type=Path, help="同时保存 JSON 摘要")
    parser.add_argument("--selftest", action="store_true", help="无板离线自检")
    args = parser.parse_args()
    if args.duration <= 0:
        parser.error("--duration 必须大于 0")
    if args.selftest:
        return selftest()

    summary = {"host": args.host, "verdict": "PASS"}
    try:
        if args.stage in ("all", "home"):
            summary["home"] = home_load(f"http://{args.host}")
        if args.stage in ("all", "status", "ack"):
            from websockets.sync.client import connect

            with connect(f"ws://{args.host}/ws", proxy=None, compression=None,
                         open_timeout=5, close_timeout=2) as ws:
                config = recv_frame(ws, time.perf_counter() + 5,
                                    lambda frame: frame.get("t") == "config")
                if args.stage in ("all", "status"):
                    status = status_load(ws, args.duration)
                    summary["status"] = status
                    if status["missing"] > 0:
                        summary.update(verdict="FAIL", error=f"状态采样缺帧 {status['missing']} 次")
                if args.stage in ("all", "ack") and summary["verdict"] == "PASS":
                    summary["ack"] = ack_load(ws, config)
    except subprocess.CalledProcessError as exc:
        summary.update(verdict="FAIL", error=exc.stderr.strip() or str(exc))
    except (Exception, KeyboardInterrupt) as exc:
        summary.update(verdict="FAIL", error=f"{type(exc).__name__}: {exc}")
    output = json.dumps(summary, ensure_ascii=False, indent=2)
    print(output)
    if args.json_out is not None:
        try:
            args.json_out.write_text(output + "\n", encoding="utf-8")
        except OSError as exc:
            print(f"写摘要失败：{exc}", file=sys.stderr)
            return 2
    return 0 if summary["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
