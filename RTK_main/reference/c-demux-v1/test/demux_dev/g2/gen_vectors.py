#!/usr/bin/env python3
"""G2 独立校验向量生成器（不参与构建，只用于核对 rtk_demux_decoders.c / rtk_proto_demux.c）。

全部算法用逐位（bit-by-bit）实现，不复制被核对代码的任何查表法实现；
与被核对代码的 256 项查表法互相独立，两者一致才算核对通过。

Generated-by: pi + DeepSeek-V4.1-Flash
人工负责人：UNKNOWN（待人工审查签字）
"""

# ---------------------------------------------------------------- CRC 算法


def crc24q(data: bytes) -> int:
    """CRC-24Q / CRC-24/LTE-A：poly 0x1864CFB，init 0，不反射，不取反。"""
    crc = 0
    poly = 0x1864CFB
    for byte in data:
        crc ^= byte << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= poly
    return crc & 0xFFFFFF


def crc32_novatel(data: bytes, init: int = 0) -> int:
    """NovAtel / Unicore 风格 CRC-32：反射 poly 0xEDB88320，init 可指定，不取反。"""
    crc = init
    poly = 0xEDB88320
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ poly
            else:
                crc >>= 1
    return crc & 0xFFFFFFFF


def crc32_zlib(data: bytes) -> int:
    """标准 CRC-32/IEEE（zlib 兼容）：反射 poly，init 0xFFFFFFFF，末尾取反。"""
    return crc32_novatel(data, 0xFFFFFFFF) ^ 0xFFFFFFFF


def fletcher8(data: bytes):
    """u-blox 8 位 Fletcher（CK_A / CK_B）。"""
    a = 0
    b = 0
    for byte in data:
        a = (a + byte) & 0xFF
        b = (b + a) & 0xFF
    return a, b


def xor8(data: bytes) -> int:
    acc = 0
    for byte in data:
        acc ^= byte
    return acc


# ---------------------------------------------------------------- 帧构造


def rtcm3(payload: bytes) -> bytes:
    head = bytes([0xD3, (len(payload) >> 8) & 0x03, len(payload) & 0xFF])
    body = head + payload
    crc = crc24q(body)
    return body + bytes([(crc >> 16) & 0xFF, (crc >> 8) & 0xFF, crc & 0xFF])


def nmea(body: bytes) -> bytes:
    """body 不含 '$' 与 '*'；校验覆盖 '$' 与 '*' 之间。"""
    line = b"$" + body
    return line + b"*" + ("%02X" % xor8(body)).encode() + b"\r\n"


def ubx(cls: int, mid: int, payload: bytes) -> bytes:
    body = bytes([cls, mid]) + len(payload).to_bytes(2, "little") + payload
    a, b = fletcher8(body)
    return bytes([0xB5, 0x62]) + body + bytes([a, b])


def unicore_bin(msg_id: int, payload: bytes) -> bytes:
    head = bytearray(24)
    head[0] = 0xAA
    head[1] = 0x44
    head[2] = 0xB5
    head[3] = 0x00  # CPU 空闲率
    head[4] = msg_id & 0xFF
    head[5] = (msg_id >> 8) & 0xFF
    head[6] = len(payload) & 0xFF
    head[7] = (len(payload) >> 8) & 0xFF
    body = bytes(head) + payload
    crc = crc32_novatel(body)
    return body + crc.to_bytes(4, "little")


def unicore_hash_crc(body: bytes) -> bytes:
    line = b"#" + body
    return line + b"*" + ("%08X" % crc32_novatel(body)).encode() + b"\r\n"


def unicore_hash_xor(body: bytes) -> bytes:
    line = b"#" + body
    return line + b"*" + ("%02X" % xor8(body)).encode() + b"\r\n"


def product_frame(msg_id: int, sender: int, mtype: int, payload: bytes) -> bytes:
    head = bytearray(20)
    head[0] = 0xAA
    head[1] = 0x44
    head[2] = 0x18
    head[3] = 20
    head[4] = msg_id & 0xFF
    head[5] = (msg_id >> 8) & 0xFF
    head[12] = len(payload) & 0xFF
    head[13] = (len(payload) >> 8) & 0xFF
    head[16] = sender
    head[17] = mtype
    body = bytes(head) + payload
    return body + crc32_zlib(body).to_bytes(4, "little")


def rtcm_1005_payload() -> bytes:
    """RTCM 1005（基站坐标）载荷 19 B，字段按 RTCM 3.2 位域排布（示例值，只用于核对校验）。"""
    bits = ""

    def put(value, width):
        nonlocal bits
        bits += format(value & ((1 << width) - 1), "0%db" % width)

    put(1005, 12)
    put(0, 12)          # 基准站 ID
    put(0, 6)           # ITRF 实现年
    put(0, 1)           # GPS 指示
    put(0, 1)           # GLONASS 指示
    put(0, 1)           # Galileo 指示
    put(0, 1)           # 参考站指示
    put(4000000000, 38) # ECEF-X 0.0001 m
    put(0, 1)           # 单接收机振荡器指示
    put(0, 1)           # 保留
    put(300000000, 38)  # ECEF-Y
    put(4500000000, 38) # ECEF-Z
    bits += "0" * (19 * 8 - len(bits))  # 150 bit + 2 位补齐 = 19 B
    return int(bits, 2).to_bytes(19, "big")


def hexs(data: bytes) -> str:
    return " ".join("%02X" % b for b in data)


def main() -> None:
    print("== 标准核对值 ==")
    print('CRC-24Q("123456789")        = 0x%06X (期望 0xCDE703)' % crc24q(b"123456789"))
    print('zlib CRC-32("123456789")     = 0x%08X (期望 0xCBF43926)' % crc32_zlib(b"123456789"))
    print('NovAtel CRC-32("123456789")  = 0x%08X' % crc32_novatel(b"123456789"))
    print('NovAtel CRC-32("")           = 0x%08X' % crc32_novatel(b""))
    a, b = fletcher8(b"\x0A\x04\x00\x00")
    print("UBX 0A 04 00 00 的 Fletcher   = CK_A=0x%02X CK_B=0x%02X (期望 0x0E/0x34)" % (a, b))

    print("\n== 帧向量 ==")
    r = rtcm3(rtcm_1005_payload())
    print("RTCM3 1005   len=%d  %s" % (len(r), hexs(r)))
    print("   CRC-24Q = 0x%06X" % crc24q(r[:-3]))

    n = nmea(b"GNGGA,000000.00,3945.12345,N,11617.12345,E,1,12,0.85,45.6,M,-8.4,M,,")
    print("NMEA GNGGA   len=%d  %s" % (len(n), n.decode()))
    print("   XOR = 0x%02X" % xor8(n[1 : n.index(b"*")]))

    u = ubx(0x0A, 0x04, b"")
    print("UBX MON-VER  len=%d  %s" % (len(u), hexs(u)))

    ub = unicore_bin(0x0001, bytes(range(8)))
    print("Unicore BIN  len=%d  %s" % (len(ub), hexs(ub)))
    print("   CRC-32 = 0x%08X" % crc32_novatel(ub[:-4]))

    uh = unicore_hash_crc(b"VERSION,88,GPS,FINE,2305,123456000,0,0,18,15;")
    print("Hash CRC     len=%d  %s" % (len(uh), uh.decode()))
    uhx = unicore_hash_xor(b"HWSTATUS,..,V,0000")
    print("Hash XOR     len=%d  %s" % (len(uhx), uhx.decode()))

    p = product_frame(0x0102, 0x00, 0x00, b"\x01\x02\x03\x04")
    print("产品帧       len=%d  %s" % (len(p), hexs(p)))
    print("   CRC-32 = 0x%08X" % crc32_zlib(p[:-4]))

    print("\n== 边界帧 ==")
    print("RTCM3 最大载荷 1023 len=%d" % len(rtcm3(bytes(1023))))
    print("NMEA 244 B   len=%d" % len(nmea(b"A" * 236)))
    print("NMEA 256 B   len=%d" % len(nmea(b"A" * 250)))
    print("UBX 4088 载荷 len=%d" % len(ubx(0x01, 0x07, bytes(4088))))
    print("Unicore 4068 载荷 len=%d" % len(unicore_bin(0x0001, bytes(4068))))
    print("Hash 最大     len=%d" % len(unicore_hash_crc(b"V" + b"0" * 1011)))


if __name__ == "__main__":
    main()
