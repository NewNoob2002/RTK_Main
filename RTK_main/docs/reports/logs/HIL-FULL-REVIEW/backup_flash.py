# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
# 授权 HIL：分块只读备份；校验前不烧录。
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path('/home/gtc/Desktop/workspace/ESP32_PROJ/P2_RTK/RTK_main')
logs = root / 'docs/reports/logs/HIL-FULL-REVIEW'
python = '/home/gtc/.espressif/python_env/idf6.1_py3.14_env/bin/python'
parts = []
for index in range(8):
    dest = logs / f'original-part-{index}.bin'
    command = [python, '-m', 'esptool', '--chip', 'esp32', '--port', '/dev/ttyACM0',
               '--baud', '230400', 'read-flash', hex(index * 0x100000), '0x100000', str(dest)]
    print(f'Reading backup block {index + 1}/8', flush=True)
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    (logs / f'backup-block-{index}.log').write_text('$ ' + ' '.join(command) + '\n' + result.stdout + result.stderr + f'\nEXIT={result.returncode}\n')
    if result.returncode != 0 or not dest.is_file() or dest.stat().st_size != 0x100000:
        raise SystemExit(f'FAIL backup block {index}, see backup-block-{index}.log; no flash writes')
    os.chmod(dest, 0o600)
    parts.append(dest)
backup = logs / 'original-flash.bin'
with backup.open('xb') as target:
    os.chmod(backup, 0o600)
    for part in parts:
        target.write(part.read_bytes())
metadata = {'bytes': backup.stat().st_size, 'sha256': hashlib.sha256(backup.read_bytes()).hexdigest(),
            'blocks': [{'file': p.name, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()} for p in parts]}
(logs / 'backup.json').write_text(json.dumps(metadata, indent=2) + '\n')
print('Backup saved: ' + json.dumps({'bytes': metadata['bytes'], 'sha256': metadata['sha256']}), flush=True)
command = [python, '-m', 'esptool', '--chip', 'esp32', '--port', '/dev/ttyACM0',
           '--baud', '230400', 'verify-flash', '0', str(backup)]
result = subprocess.run(command, capture_output=True, text=True, timeout=120)
(logs / 'backup-verify.log').write_text('$ ' + ' '.join(command) + '\n' + result.stdout + result.stderr + f'\nEXIT={result.returncode}\n')
print(result.stdout + result.stderr, flush=True)
if result.returncode:
    raise SystemExit('FAIL: backup does not verify; no flash writes')
print('PASS complete 8MB backup verified against device', flush=True)
