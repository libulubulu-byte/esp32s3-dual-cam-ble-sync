#!/usr/bin/env python3
"""快速串口诊断：同时读两个串口 N 秒并原样打印，用于确认 BLE 连接状态"""
import sys
import time
import serial


def open_port(name, baud=115200):
    # ★ 不要设置 dtr/rts（CH343 板会被复位/进下载模式）
    return serial.Serial(name, baud, timeout=0.05)


def main():
    host_port = sys.argv[1] if len(sys.argv) > 1 else "COM17"
    slave_port = sys.argv[2] if len(sys.argv) > 2 else "COM8"
    dur = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0

    host = open_port(host_port)
    slave = open_port(slave_port)
    print(f"reading {host_port} and {slave_port} for {dur}s ...")

    t0 = time.time()
    while time.time() - t0 < dur:
        for ser, name in ((host, "HOST "), (slave, "SLAVE")):
            while ser.in_waiting:
                raw = ser.readline()
                if not raw:
                    break
                txt = raw.decode("utf-8", errors="ignore").rstrip()
                print(f"{name}| {txt}")
        time.sleep(0.01)

    host.close()
    slave.close()
    print("done")


if __name__ == "__main__":
    main()
