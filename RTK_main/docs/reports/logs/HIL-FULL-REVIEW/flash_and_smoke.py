# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
# 已授权 HIL：921600 烧录，先保证完整备份，运行纯逻辑测试后恢复产品及 NVS。
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import serial
from esptool.reset import HardReset

root = Path('/home/gtc/Desktop/workspace/ESP32_PROJ/P2_RTK/RTK_main')
logs = root / 'docs/reports/logs/HIL-FULL-REVIEW'
product = Path('/tmp/rtk-full-review-fix-1mf12pxp/idf')
pure = Path('/tmp/rtk-deepseek-full-review.W0mIk7/testapp-esp32')
backup = (logs / 'original-flash.bin').read_bytes()
meta = json.loads((logs / 'backup.json').read_text())
security = json.loads((logs / 'security.json').read_text())
if meta['status'] != 'PASS' or len(backup) != 0x800000 or hashlib.sha256(backup).hexdigest() != meta['sha256']:
    raise SystemExit('BLOCKED: backup not verified')
if security['secure_boot_enabled'] or security['flash_encryption_enabled']:
    raise SystemExit('BLOCKED: protected target')
# 根据备份中的实际分区表确定 NVS，而非假定旧固件布局。
partitions = []
for offset in range(0x8000, 0x8C00, 32):
    entry = backup[offset:offset+32]
    if entry[:2] != b'\xAA\x50':
        break
    partitions.append({'type': entry[2], 'subtype': entry[3],
                       'offset': int.from_bytes(entry[4:8], 'little'),
                       'size': int.from_bytes(entry[8:12], 'little'),
                       'label': entry[12:28].split(b'\0',1)[0].decode()})
(logs / 'original-partitions.json').write_text(json.dumps(partitions, indent=2)+'\n')
nvs = next((p for p in partitions if p['label']=='nvs' and p['type']==1 and p['subtype']==2), None)
if nvs is None or nvs['offset']!=0xA000 or nvs['size']!=0x9000:
    raise SystemExit('BLOCKED: original NVS layout differs; do not overwrite')
nvs_path = logs / 'original-nvs.bin'
with nvs_path.open('xb') as out:
    os.chmod(nvs_path,0o600)
    out.write(backup[0xA000:0x14000])

def command(args, name, cwd=root):
    print('Running '+name, flush=True)
    proc = subprocess.run(args,cwd=cwd,capture_output=True,text=True,timeout=120)
    (logs / (name+'.log')).write_text('$ '+' '.join(map(str,args))+'\n'+proc.stdout+proc.stderr+'\nEXIT='+str(proc.returncode)+'\n')
    print(proc.stdout[-1400:]+proc.stderr, flush=True)
    if proc.returncode:
        raise RuntimeError(name+' failed')

def flash(build, name, extra=None):
    args=[sys.executable,'-m','esptool','--chip','esp32','--port','/dev/ttyACM0','--baud','921600','--after','no-reset','write-flash','@flash_args']
    if extra: args += extra
    command(args,name,build)

def capture(name, seconds=8, commands=()):
    port=serial.Serial()
    port.port='/dev/ttyACM0'; port.baudrate=115200; port.timeout=0.1
    port.dtr=False; port.rts=False; port.open()
    data=bytearray()
    try:
        HardReset(port)()
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            data.extend(port.read(8192))
        for text in commands:
            port.write((text+'\r').encode())
            until=time.monotonic()+2
            while time.monotonic()<until:
                data.extend(port.read(8192))
    finally:
        port.close()
    text=data.decode(errors='replace')
    (logs/(name+'.log')).write_text(text)
    print(text[-5000:],flush=True)
    return text

pure_result='NOT_RUN'
try:
    flash(pure,'pure-flash')
    text=capture('pure-uart',8)
    if re.search(r'12 Tests 0 Failures 0 Ignored',text):
        pure_result='PASS'
    else:
        pure_result='FAIL'
    print('Manual Unity HIL-0: '+pure_result,flush=True)
finally:
    # 即使 Unity 失败也恢复已验证产品及全部原 NVS 数据。
    flash(product,'product-flash',['0xa000',str(nvs_path)])
    command([sys.executable,'-m','esptool','--chip','esp32','--port','/dev/ttyACM0','--baud','921600','--after','no-reset','verify-flash','0x1000','bootloader/bootloader.bin','0x8000','partition_table/partition-table.bin','0x14000','ota_data_initial.bin','0x20000','rtk_main.bin','0xa000',str(nvs_path)],'product-verify',product)
    boot=capture('product-uart',8,('status','policy'))
    summary={'manual_unity':pure_result,'product_boot': 'PASS' if 'link: running' in boot and 'bt: ready' in boot else 'FAIL',
             'product_bin_sha256':hashlib.sha256((product/'rtk_main.bin').read_bytes()).hexdigest(),'baud':921600}
    (logs/'flash-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary),flush=True)
