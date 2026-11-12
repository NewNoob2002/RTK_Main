# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
# 已授权 UART0 HIL 控制台；不改变 DTR/RTS，不自动复位。
import json
from pathlib import Path
import select
import sys
import time
import serial
port=sys.argv[1]
log=Path(sys.argv[2])
serial_port=serial.Serial(); serial_port.port=port; serial_port.baudrate=115200
serial_port.timeout=0; serial_port.dtr=False; serial_port.rts=False; serial_port.open()
start=time.monotonic()
with log.open('a',buffering=1) as output:
    print('HIL_SERIAL_READY '+port,flush=True)
    try:
        while True:
            ready,_,_=select.select([serial_port.fileno(),sys.stdin],[],[],0.2)
            if serial_port.fileno() in ready:
                raw=serial_port.read(8192)
                if raw:
                    text=raw.decode(errors='replace')
                    entry=f'[{time.monotonic()-start:.3f}] '+text
                    output.write(entry); print(entry,end='',flush=True)
            if sys.stdin in ready:
                line=sys.stdin.readline()
                if not line: break
                command=line.rstrip('\r\n')
                if command=='__EXIT__': break
                output.write(f'\n[{time.monotonic()-start:.3f}] COMMAND {command}\n')
                serial_port.write((command+'\r').encode())
    finally:
        serial_port.close()
