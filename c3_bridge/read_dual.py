#!/usr/bin/env python3
"""
同时读两块板的串口（NS800 控制台 + C3 日志），用于验证跨板功能。

  关键技巧（踩坑总结）：
  serial.Serial(port) **默认会通过 DTR/RTS 复位对端**。
  这里用 dtr=False, rts=False, hupcl=False 在 open() 之前设好，
  避免"一读就把板子重启"——否则 NS800 的 uptime 每次都被清零，
  根本验证不了"断电保存"。

用法: python read_dual.py <秒数>
"""
import sys, time, threading

import serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0

PORTS = {
    "NS800": "/dev/ttyACM0",
    "C3":    "/dev/ttyACM1",
}

bufs = {k: [] for k in PORTS}
lock = threading.Lock()


def worker(name, port):
    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.2
        # ⚠️ 关键：open() 之前设，避免握手线拉低导致对端复位
        s.dtr = False
        s.rts = False
        s.hupcl = False
        s.open()
    except Exception as e:
        with lock:
            bufs[name].append(f"[打不开 {port}: {e}]\n")
        return

    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            d = s.read(4096)
        except Exception:
            break
        if d:
            with lock:
                bufs[name].append(d.decode("utf-8", "replace"))
    s.close()


ths = [threading.Thread(target=worker, args=(n, p), daemon=True) for n, p in PORTS.items()]
for t in ths:
    t.start()
for t in ths:
    t.join()

for name in PORTS:
    print(f"\n{'='*20} {name} ({PORTS[name]}) {'='*20}")
    txt = "".join(bufs[name])
    print(txt if txt.strip() else "(无数据)")
