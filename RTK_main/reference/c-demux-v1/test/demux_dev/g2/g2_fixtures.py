#!/usr/bin/env python3
"""把 gen_vectors.py 构造的帧写成 C 头文件，供 g2_check.c 核对（不参与构建）。

Generated-by: pi + DeepSeek-V4.1-Flash
人工负责人：UNKNOWN（待人工审查签字）
"""

import gen_vectors as g


def cbytes(data):
    return "{" + ", ".join("0x%02X" % b for b in data) + "}"


def main():
    out = []
    out.append("/* 由 g2_fixtures.py 生成：G2 自测向量（不参与构建）。 */")
    out.append("#pragma once")
    out.append("#include <stddef.h>")
    out.append("#include <stdint.h>")
    out.append("")

    valid = {
        "k_rtcm3_1005": g.rtcm3(g.rtcm_1005_payload()),
        "k_rtcm3_max": g.rtcm3(bytes(1023)),
        "k_rtcm3_empty": g.rtcm3(b""),
        "k_nmea_gga": g.nmea(b"GNGGA,000000.00,3945.12345,N,11617.12345,E,1,12,0.85,45.6,M,-8.4,M,,"),
        "k_nmea_short": g.nmea(b"GPGLL,V"),
        "k_nmea_lower_hex": g.nmea(b"GNGSA,A,3,01,02,03,04,05,06,07,08,09,10,11,12,1.2,0.9,0.8".lower().upper()),
        "k_nmea_max": g.nmea(b"A" + b"B" * 249),
        "k_ubx_poll": g.ubx(0x0A, 0x04, b""),
        "k_ubx_payload": g.ubx(0x01, 0x07, bytes(range(92))),
        "k_ubx_max": g.ubx(0x01, 0x07, bytes(4088)),
        "k_ucbin_min": g.unicore_bin(0x0001, b""),
        "k_ucbin_payload": g.unicore_bin(0x0001, bytes(range(8))),
        "k_ucbin_max": g.unicore_bin(0x0001, bytes(4068)),
        "k_hash_crc": g.unicore_hash_crc(b"VERSION,88,GPS,FINE,2305,123456000,0,0,18,15;"),
        "k_hash_xor": g.unicore_hash_xor(b"HWSTATUS,..,V,0000"),
        "k_hash_max": g.unicore_hash_crc(b"V" + b"0" * 1011),
        "k_proto": g.product_frame(0x0102, 0x00, 0x00, b"\x01\x02\x03\x04"),
        "k_proto_empty": g.product_frame(0x0001, 0x01, 0x04, b""),
        "k_proto_max": g.product_frame(0xFFFF, 0x03, 0x05, bytes(256)),
    }
    for name, data in valid.items():
        out.append("static const uint8_t %s[] = %s;" % (name, cbytes(data)))
        out.append("")

    # 校验错：把最后一个字节（校验字段最后一位）翻转
    bad = {}
    for name, data in valid.items():
        flipped = bytearray(data)
        flipped[-1] ^= 0xFF
        bad["b_" + name] = bytes(flipped)

    for name, data in bad.items():
        out.append("static const uint8_t %s[] = %s;" % (name, cbytes(data)))
        out.append("")

    with open("g2_vectors.h", "w") as handle:
        handle.write("\n".join(out) + "\n")
    print("wrote g2_vectors.h (%d valid + %d corrupted)" % (len(valid), len(bad)))


if __name__ == "__main__":
    main()
