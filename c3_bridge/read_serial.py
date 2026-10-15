#!/usr/bin/env python3
"""
读 ESP32-C3 串口日志（USB-Serial/JTAG）。

为什么不用 esp_idf_monitor：
  它接管终端（raw 模式）、要按 Ctrl+] 才能退出，在非交互脚本里会一直挂着
  把 timeout 也不当事。这个脚本纯读，到点自己退。

复位方式：C3 的 USB-Serial/JTAG 用 DTR/RTS 组合触发复位 ——
  RTS=1 & DTR=0 拉低 EN 脚。注意有些组合会误入 bootloader 下载模式，
  所以用标准 reset 序列：先 DTR/RTS 各拨一次，最后放开时正常启动。

用法:  python read_serial.py <port> <seconds>
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

s = serial.Serial(port, 115200, timeout=0.2)

# ── 硬复位（标准 esptool 手法）──
s.setDTR(False)
s.setRTS(True)
time.sleep(0.12)
s.setRTS(False)
time.sleep(0.12)
# 清掉复位期间产生的垃圾
s.reset_input_buffer()

t0 = time.time()
buf = []
try:
    while time.time() - t0 < secs:
        data = s.read(4096)
        if data:
            txt = data.decode("utf-8", "replace")
            sys.stdout.write(txt)
            sys.stdout.flush()
            buf.append(txt)
finally:
    s.close()

if not buf:
    print(f"[read_serial] {secs}s 内没有收到任何数据 —— "
          f"检查：① 是否被别的程序占用端口 ② 波特率 ③ 板子是否真在跑",
          file=sys.stderr)
