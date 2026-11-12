# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
import time
from pathlib import Path
import serial
from esptool.reset import HardReset
root=Path('/home/gtc/Desktop/workspace/ESP32_PROJ/P2_RTK/RTK_main/docs/reports/logs/HIL-FULL-REVIEW')
p=serial.Serial(); p.port='/dev/ttyACM0'; p.baudrate=115200; p.timeout=0.1; p.dtr=False; p.rts=False; p.open()
data=bytearray()
try:
    HardReset(p)()
    until=time.monotonic()+5
    while time.monotonic()<until: data.extend(p.read(8192))
    for cmd in ['status','policy']:
        p.write((cmd+'\r').encode())
        until=time.monotonic()+1
        while time.monotonic()<until: data.extend(p.read(8192))
finally: p.close()
text=data.decode(errors='replace'); (root/'product-uart.log').write_text(text)
print(text)
if 'link: running' not in text or 'bt: ready' not in text: raise SystemExit(1)
print('PASS: product running, Bluetooth ready; power hold initialized')
