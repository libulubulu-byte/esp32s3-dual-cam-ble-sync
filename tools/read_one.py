#!/usr/bin/env python3
"""只读一个串口 N 秒，用于精确定位某个板的固件角色"""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM17"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

# ★ 不要设置 dtr/rts：CH343 板上任何 DTR/RTS 变化都会把芯片复位/带进下载模式
ser = serial.Serial(port, 115200, timeout=0.05)

print(f"[{port}] reading {dur}s ...")
t0 = time.time()
while time.time() - t0 < dur:
    while ser.in_waiting:
        raw = ser.readline()
        if not raw:
            break
        print(f"[{port}] {raw.decode('utf-8', errors='ignore').rstrip()}")
    time.sleep(0.01)

ser.close()
print("done")
