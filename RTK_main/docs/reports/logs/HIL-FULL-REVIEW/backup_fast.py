# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
# 已授权 HIL：只读保护检查和 921600 baud 备份；不写 Flash/eFuse。
import hashlib
import json
import os
from pathlib import Path
import time
from esptool.targets.esp32 import ESP32ROM
from esptool.cmds import read_flash

root = Path('/home/gtc/Desktop/workspace/ESP32_PROJ/P2_RTK/RTK_main/docs/reports/logs/HIL-FULL-REVIEW')
start = time.monotonic()
esp = ESP32ROM('/dev/ttyACM0', 115200)
try:
    esp.connect()
    state = {'chip': esp.CHIP_NAME, 'secure_boot_enabled': bool(esp.get_secure_boot_enabled()),
             'flash_encryption_enabled': bool(esp.get_flash_encryption_enabled())}
    (root / 'security.json').write_text(json.dumps(state, indent=2) + '\n')
    print('Protection:', json.dumps(state), flush=True)
    if state['secure_boot_enabled'] or state['flash_encryption_enabled']:
        raise SystemExit('BLOCKED: protected device; no backup/flash assumed safe')
    esp = esp.run_stub()
    esp.change_baud(921600)
    parts = []
    for index in range(8):
        part = root / f'original-part-{index}.bin'
        if part.exists():
            data = part.read_bytes()
            if len(data) != 0x100000 or hashlib.md5(data).hexdigest() != esp.flash_md5sum(index * 0x100000, len(data)):
                raise SystemExit(f'FAIL existing backup block {index} differs from device')
            print(f'Existing block {index+1}/8 verified; skipping reread', flush=True)
        else:
            print(f'Reading block {index+1}/8 at 921600 baud', flush=True)
            pieces = []
            for sub in range(4):
                pieces.append(read_flash(esp, index * 0x100000 + sub * 0x40000, 0x40000, no_progress=True))
            data = b''.join(pieces)
            if len(data) != 0x100000 or hashlib.md5(data).hexdigest() != esp.flash_md5sum(index * 0x100000, len(data)):
                raise SystemExit(f'FAIL device MD5 at block {index}')
            with part.open('xb') as out:
                os.chmod(part, 0o600)
                out.write(data)
            print(f'Block {index+1}/8 verified', flush=True)
        parts.append(part)
    backup = root / 'original-flash.bin'
    with backup.open('xb') as out:
        os.chmod(backup, 0o600)
        for part in parts:
            out.write(part.read_bytes())
    data = backup.read_bytes()
    device_md5 = esp.flash_md5sum(0, len(data))
    if len(data) != 0x800000 or hashlib.md5(data).hexdigest() != device_md5:
        raise SystemExit('FAIL: complete backup verification')
    result = {'status': 'PASS', 'bytes': len(data), 'baud': 921600,
              'sha256': hashlib.sha256(data).hexdigest(), 'md5_device_verified': True,
              'elapsed_seconds': round(time.monotonic()-start, 2)}
    (root / 'backup.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result), flush=True)
finally:
    esp.hard_reset()
    esp._port.close()
