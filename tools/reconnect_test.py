#!/usr/bin/env python3
"""
断线重连验收：主机在跑，中途硬复位从机，看主机多久检测到断链、多久重连成功。
用法: python reconnect_test.py <host_port> <slave_port>
"""
import subprocess
import sys
import time

import serial

host_port = sys.argv[1] if len(sys.argv) > 1 else "COM17"
slave_port = sys.argv[2] if len(sys.argv) > 2 else "COM8"

host = serial.Serial(host_port, 115200, timeout=0.05)


def pump(deadline, tag):
    t_disc = None
    t_reconn = None
    while time.time() < deadline:
        while host.in_waiting:
            line = host.readline().decode("utf-8", errors="ignore").strip()
            if line.startswith("HOST,"):
                print(f"[{tag}] {line}")
                if ",BLE_DISCONNECTED," in line and t_disc is None:
                    t_disc = time.time()
                if ",BLE_CONNECTED," in line and t_disc is not None and t_reconn is None:
                    t_reconn = time.time()
        time.sleep(0.01)
    return t_disc, t_reconn


print("阶段 1：确认已连接（读 6s）")
host.reset_input_buffer()
d0, r0 = pump(time.time() + 6, "before")

print(f"\n阶段 2：硬复位从机 {slave_port} ...")
subprocess.run(
    [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", slave_port,
     "--after", "hard_reset", "chip_id"],
    capture_output=True,
)

t_reset = time.time()
print("阶段 3：观察主机 25s ...")
d1, r1 = pump(time.time() + 25, "after")

if d1:
    print(f"\n  [OK] 检测到断链用时 {d1 - t_reset:.1f}s (目标 <10s)")
else:
    print("\n  [NG] 25s 内未检测到 BLE_DISCONNECTED")
if r1:
    print(f"  [OK] 重连成功用时 {r1 - t_reset:.1f}s (目标 <30s)")
else:
    print("  [NG] 25s 内未重连成功")

host.close()
