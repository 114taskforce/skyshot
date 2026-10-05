# ===========================================================================
#  串口抓取（非交互）：复位板子并打印 N 秒输出
#  用 PlatformIO 自带的 pyserial，不必开 pio device monitor
#
#  用法：python tools/monitor.py [COM口] [秒数]   （默认 COM13 / 10 秒）
# ===========================================================================
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else 'COM13'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

s = serial.Serial(port, 115200, timeout=0.2)
# USB-Serial-JTAG：RTS 接 EN（复位），DTR 接 IO0（拉高 = 正常启动）
s.setDTR(False)
s.setRTS(True)
time.sleep(0.15)
s.setRTS(False)

t0 = time.time()
while time.time() - t0 < secs:
    data = s.read(8192)
    if data:
        sys.stdout.write(data.decode('utf-8', 'replace'))
        sys.stdout.flush()
s.close()
print(f'\n[monitor] {secs:.0f} 秒抓取结束')
