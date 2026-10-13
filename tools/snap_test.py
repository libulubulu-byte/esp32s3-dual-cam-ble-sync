#!/usr/bin/env python3
"""单板验证上传链路：等 WiFi 拿到 IP -> 发 SNAP -> 观察 TG 上传结果"""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 1
KEY = ("got ip", "TG:", "WIFI:", "Guru", "abort", "SNAP ->", "sendPhoto",
       "grab", "photo", "retry", "upload")

ser = serial.Serial(port, 115200, timeout=0.05)
print(f"[{port}] 等待 WiFi 获取 IP ...")

got_ip = False
t0 = time.time()
while time.time() - t0 < 45:
    while ser.in_waiting:
        raw = ser.readline().decode("utf-8", errors="ignore").strip()
        if any(k in raw for k in KEY):
            print("  " + raw)
        if "got ip" in raw:
            got_ip = True
    if got_ip:
        break
    time.sleep(0.02)

print("[OK] WiFi 已连接" if got_ip else "!! 未拿到 IP，仍继续尝试")

for i in range(rounds):
    print(f"\n--- SNAP #{i+1} ---")
    ser.reset_input_buffer()
    ser.write(b"SNAP\n")
    ser.flush()

    t0 = time.time()
    sent_ok = False
    while time.time() - t0 < 45:
        while ser.in_waiting:
            raw = ser.readline().decode("utf-8", errors="ignore").strip()
            if any(k in raw for k in KEY):
                print("  " + raw)
                if "sendPhoto ok" in raw or "sendPhoto FAILED" in raw:
                    sent_ok = True
        if sent_ok:
            break
        time.sleep(0.02)
    time.sleep(0.5)

ser.close()
print("\ndone")
