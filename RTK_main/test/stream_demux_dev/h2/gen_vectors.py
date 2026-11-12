#!/usr/bin/env python3
"""H2 自测向量生成器（不交付、不参与构建；任务卡允许放 test/stream_demux_dev/h2/）。

独立性：本脚本只用移位/异或/加法逐位实现 CRC 与校验，不调用被测 C++ 代码；产品帧的 CRC-32 用
Python 标准库 zlib（与 rtk_proto_frame.c 声称的 zlib 等价性互相核对）。

输出：
  * h2_vectors.h —— h2_check.cpp 使用的帧向量；
  * 标准输出 —— docs/reports/H2.md §3 的已知向量核对表数据。
"""

import zlib

CRC24Q_POLY = 0x1864CFB
CRC24Q_MASK = 0xFFFFFF
CRC32_POLY = 0xEDB88320


def crc24q_bit(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= CRC24Q_POLY
        crc &= CRC24Q_MASK
    return crc


def crc32_novatel_bit(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ CRC32_POLY if crc & 1 else crc >> 1
    return crc & 0xFFFFFFFF


def fletcher8(data: bytes):
    a = b = 0
    for byte in data:
        a = (a + byte) & 0xFF
        b = (b + a) & 0xFF
    return a, b


def xor8(data: bytes) -> int:
    acc = 0
    for byte in data:
        acc ^= byte
    return acc


def rtcm3(payload: bytes) -> bytes:
    head = bytes([0xD3, (len(payload) >> 8) & 0x03, len(payload) & 0xFF])
    body = head + payload
    return body + crc24q_bit(body).to_bytes(3, "big")


def nmea(text: str, lower_hex: bool = False) -> bytes:
    body = text.encode("ascii")
    chk = xor8(body)
    digits = f"{chk:02X}" if not lower_hex else f"{chk:02x}"
    return b"$" + body + b"*" + digits.encode("ascii") + b"\r\n"


def ubx(cls: int, mid: int, payload: bytes) -> bytes:
    core = bytes([cls, mid, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) + payload
    a, b = fletcher8(core)
    return b"\xB5\x62" + core + bytes([a, b])


def unicore_bin(payload: bytes) -> bytes:
    # 24 B 固定头：AA 44 B5 | 3 B 类型/标志 | 2 B 载荷长（偏移 6，u16 LE）| 16 B 保留 | 载荷 | CRC-32 LE
    head = b"\xAA\x44\xB5" + b"\x00\x01\x00" + len(payload).to_bytes(2, "little") + bytes(16)
    assert len(head) == 24
    body = head + payload
    return body + crc32_novatel_bit(body).to_bytes(4, "little")


def unicore_hash_crc(text: str) -> bytes:
    body = text.encode("ascii")
    return b"#" + body + b"*" + f"{crc32_novatel_bit(body):08X}".encode("ascii") + b"\r\n"


def unicore_hash_xor(text: str) -> bytes:
    body = text.encode("ascii")
    return b"#" + body + b"*" + f"{xor8(body):02X}".encode("ascii") + b"\r\n"


def proto_frame(msg_id: int, sender: int, msg_type: int, payload: bytes) -> bytes:
    head = bytearray(20)
    head[0:3] = b"\xAA\x44\x18"
    head[3] = 20
    head[4:6] = msg_id.to_bytes(2, "little")
    head[12:14] = len(payload).to_bytes(2, "little")
    head[16] = sender
    head[17] = msg_type
    body = bytes(head) + payload
    return body + zlib.crc32(body).to_bytes(4, "little")


def cpp_array(name: str, data: bytes) -> str:
    values = ", ".join(f"0x{b:02X}" for b in data)
    return f"static const uint8_t {name}[] = {{{values}}};"


def main() -> None:
    vectors = {
        "kRtcm3_1005": rtcm3(bytes.fromhex("3ED0000000EE6B28000011E1A3000430E23400")),
        "kRtcm3_Empty": rtcm3(b""),
        "kNmea_Gga": nmea("GNGGA,000000.00,0000.0000,N,00000.0000,E,0,0,,0,M,0,M,,"),
        "kNmea_LowerHex": nmea("GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W", lower_hex=True),
        "kUbxMonVer": ubx(0x0A, 0x04, b""),
        "kUbxNavPvt": ubx(0x01, 0x07, bytes(range(1, 9))),
        "kUcbin_Min": unicore_bin(b""),
        "kUcbin_Payload": unicore_bin(bytes(range(1, 9))),
        "kHash_Crc": unicore_hash_crc("VERSION,88,GPS,FINE,2305,123456000,0,0,18,15;"),
        "kHash_Xor": unicore_hash_xor("HWSTATUS,..,V,0000"),
        "kProto_Data": proto_frame(0x0002, 0x00, 0x04, bytes([1, 2, 3, 4])),
        "kProto_Empty": proto_frame(0x0001, 0x01, 0x04, b""),
        "kProto_Max": proto_frame(0xFFFF, 0x03, 0x05, bytes(range(256))),
    }

    lines = [
        "// H2 自测向量（由 test/stream_demux_dev/h2/gen_vectors.py 生成，不交付、不参与构建）。",
        "// Generated-by: pi + workbuddy/deepseek-v4.1-flash",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
    ]
    for name, data in vectors.items():
        lines.append(cpp_array(name, data))
        lines.append("")
    with open("h2_vectors.h", "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))

    print("=== 已知向量核对表（独立逐位实现 / zlib） ===")
    print(f"CRC-24Q(\"123456789\") = 0x{crc24q_bit(b'123456789'):06X}")
    print(f"CRC-32 NovAtel(\"123456789\") = 0x{crc32_novatel_bit(b'123456789'):08X}")
    print(f"CRC-32 NovAtel(\"\") = 0x{crc32_novatel_bit(b''):08X}")
    print(f"zlib CRC-32(\"123456789\") = 0x{zlib.crc32(b'123456789'):08X}")
    print(f"分段累计 CRC-24Q(4|5) = 0x{crc24q_bit(b'56789'):06X} / 0x{crc24q_bit(b'123456789'):06X}")
    for name, data in vectors.items():
        print(f"{name:16s} len={len(data):4d}  {data[:24].hex(' ').upper()}{' ...' if len(data) > 24 else ''}")
    ubx_body = bytes([0x0A, 0x04, 0x00, 0x00])
    print(f"Fletcher8({ubx_body.hex(' ').upper()}) = {tuple(hex(v) for v in fletcher8(ubx_body))}")
    print(f"XOR8(GNGGA...) = 0x{xor8(b'GNGGA,000000.00,0000.0000,N,00000.0000,E,0,0,,0,M,0,M,,'):02X}")
    print(f"XOR8(HWSTATUS,..,V,0000) = 0x{xor8(b'HWSTATUS,..,V,0000'):02X}")
    print(f"RTCM3 CRC 1005 = 0x{crc24q_bit(bytes.fromhex('3ED0000000EE6B28000011E1A3000430E23400')):06X}")
    print(f"UnicoreBin payload01..08 CRC = 0x{crc32_novatel_bit(bytes.fromhex('AA44B50001000800') + bytes(16) + bytes(range(1, 9))):08X}")
    print(f"Hash CRC(VERSION,...) = 0x{crc32_novatel_bit(b'VERSION,88,GPS,FINE,2305,123456000,0,0,18,15;'):08X}")
    print(f"Proto data msg CRC = 0x{zlib.crc32(bytes(vectors['kProto_Data'])[:-4]):08X}")
    print(f"Proto empty msg CRC = 0x{zlib.crc32(bytes(vectors['kProto_Empty'])[:-4]):08X}")
    print(f"Proto max msg CRC = 0x{zlib.crc32(bytes(vectors['kProto_Max'])[:-4]):08X}")


if __name__ == "__main__":
    main()
