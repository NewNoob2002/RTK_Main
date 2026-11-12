# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
# 第二块 ESP32 只读备份；不改 ExternalSpeak 工程。
import hashlib
import json
import os
from pathlib import Path
from esptool.targets.esp32 import ESP32ROM
from esptool.cmds import read_flash
root=Path('/home/gtc/Desktop/workspace/ESP32_PROJ/P2_RTK/RTK_main/docs/reports/logs/HIL-FULL-REVIEW')
esp=ESP32ROM('/dev/ttyUSB0',115200)
try:
    esp.connect()
    flags={'secure_boot_enabled':bool(esp.get_secure_boot_enabled()),'flash_encryption_enabled':bool(esp.get_flash_encryption_enabled())}
    if any(flags.values()): raise SystemExit('BLOCKED protected second board')
    esp=esp.run_stub(); esp.change_baud(921600)
    backup=root/'second-original-flash.bin'
    data=read_flash(esp,0,0x800000,no_progress=True)
    if len(data)!=0x800000 or hashlib.md5(data).hexdigest()!=esp.flash_md5sum(0,len(data)):
        raise SystemExit('FAIL second-board backup verification')
    with backup.open('xb') as out:
        os.chmod(backup,0o600); out.write(data)
    result={'status':'PASS','port':'/dev/ttyUSB0','mac':'08:d1:f9:dd:3e:e4','bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'baud':921600,**flags}
    (root/'second-backup.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)
finally:
    esp.hard_reset(); esp._port.close()
