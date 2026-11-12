# Generated-by: omp / GPT-6.1-sol
# 人工负责人：UNKNOWN（待人工审查签字）
# 仅作已授权 HIL 的只读保护检查；不写 eFuse 或保护配置。
import json
from pathlib import Path
from esptool import detect_chip

root = Path('/home/gtc/Desktop/workspace/ESP32_PROJ/P2_RTK/RTK_main/docs/reports/logs/HIL-FULL-REVIEW')
esp = detect_chip('/dev/ttyACM0', baud=115200)
try:
    result = {'chip': esp.CHIP_NAME,
              'secure_boot_enabled': bool(esp.get_secure_boot_enabled()),
              'flash_encryption_enabled': bool(esp.get_flash_encryption_enabled())}
    (root / 'security.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    if result['chip'] != 'ESP32' or result['secure_boot_enabled'] or result['flash_encryption_enabled']:
        raise SystemExit('BLOCKED: target/protection differs from plain ESP32 image')
finally:
    esp.hard_reset()
    esp._port.close()
