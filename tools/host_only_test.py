#!/usr/bin/env python3
"""
只打开**主机一个串口**的验收（从机数据经 BLE 回传，行首为 SLAVE,）。
用法: python host_only_test.py <host_port> [--runs 3] [--rounds 20] [--lead 500]

与 sync_cam_test.py 的区别：
  - 只用一根 USB 线（接主机），从机不接 PC
  - 串口里同时出现 HOST,... 与 SLAVE,... 两种行
  - 网页 acceptance.html 用的是完全相同的判据与算法
"""
import argparse
import csv
import statistics
import sys
import time

import serial

DEBUG = False


def open_port(name, baud=115200):
    # ★ 不要设置 dtr/rts（CH343 板会被复位/进下载模式）
    return serial.Serial(name, baud, timeout=0.05)


def parse_line(line):
    s = line.strip()
    if s.startswith("HOST,"):
        return ("HOST", s[5:])
    if s.startswith("SLAVE,"):
        return ("SLAVE", s[6:])
    return None


def split_ev(body):
    parts = body.split(",")
    ev = parts[0]

    def toint(x):
        x = x.strip()
        if not x:
            return None
        try:
            return int(x)
        except ValueError:
            return None

    t = toint(parts[1]) if len(parts) > 1 else None
    ex = toint(parts[2]) if len(parts) > 2 else None
    return ev, t, ex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host_port")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--lead", type=int, default=500)
    ap.add_argument("--debug", action="store_true", help="打印每轮收到的所有原始行")
    args = ap.parse_args()

    global DEBUG
    DEBUG = args.debug

    ser = open_port(args.host_port)
    print(f"已打开 {args.host_port}（只此一个串口）")

    # ---- 等主机就绪 ----
    # 判据用**心跳**（每 2s 一行）：`HOST,HB,<t>,conn=1,bound=1,calib=1`
    #   conn=1  BLE 已连接
    #   bound=1 服务发现完成、命令特征已就绪
    #   calib=1 钟差标定完成（可以开始测）
    # ★ 不能只等 BLE_CONNECTED：那行只在连接建立的那一刻打印一次，
    #   若网页/脚本在设备已经连上之后才打开串口，就永远等不到。
    print("等待主机就绪（HB conn=1 bound=1 calib=1）...")
    t0 = time.time()
    ready = False
    while time.time() - t0 < 40:
        while ser.in_waiting:
            raw = ser.readline().decode("utf-8", errors="ignore").strip()
            if raw.startswith("HOST,") or raw.startswith("SLAVE,"):
                print(f"  {raw}")
                if raw.startswith("HOST,HB,") and "conn=1" in raw and "calib=1" in raw:
                    ready = True
        if ready:
            break
        time.sleep(0.02)
    if not ready:
        print("主机未就绪，退出")
        ser.close()
        return 1

    ser.reset_input_buffer()

    rows = []
    for run in range(1, args.runs + 1):
        print(f"\n===== 轮 {run}/{args.runs} =====")
        for i in range(args.rounds):
            ser.reset_input_buffer()
            ser.write(f"FIRE,{args.lead}\n".encode())
            ser.flush()

            h = s = None
            t1 = time.time()
            while time.time() - t1 < 3.0:
                while ser.in_waiting:
                    raw = ser.readline().decode("utf-8", errors="ignore").strip()
                    pr = parse_line(raw)
                    if not pr:
                        continue
                    who, body = pr
                    if DEBUG:
                        print(f"       RAW| {who} {body}")
                    ev, tt, ex = split_ev(body)
                    if ev == "FIRE_ACTUAL" and tt is not None:
                        if ex is None:
                            ex = 0
                        if who == "HOST" and h is None:
                            h = (tt, ex)
                        elif who == "SLAVE" and s is None:
                            s = (tt, ex)
                if h is not None and s is not None:
                    break
                time.sleep(0.005)

            if h is None or s is None:
                print(f"  [{run}-{i+1:02d}] 超时 host={'OK' if h else 'MISS'} "
                      f"slave={'OK' if s else 'MISS'}")
                rows.append({"run": run, "idx": i + 1, "ok": 0})
                continue

            delta = h[0] - s[0]
            jd = abs(h[1] - s[1])
            rows.append({"run": run, "idx": i + 1, "ok": 1,
                         "host_fire": h[0], "host_jitter": h[1],
                         "slave_fire": s[0], "slave_jitter": s[1],
                         "delta": delta, "jitter_diff": jd})
            print(f"  [{run}-{i+1:02d}] Δ={delta:>12}  jitter H={h[1]:+6} S={s[1]:+6}  |Δj|={jd:5}")
            time.sleep(0.3)

    oks = [x for x in rows if x["ok"]]
    total = args.runs * args.rounds
    print("\n" + "=" * 60)
    if oks:
        deltas = [x["delta"] for x in oks]
        med = statistics.median(deltas)
        pe = [abs(d - med) for d in deltas]
        jd = [x["jitter_diff"] for x in oks]
        print(f"有效 {len(oks)}/{total}")
        print(f"  sync_error(phase) mean={statistics.mean(pe):.1f}us max={max(pe)}us min={min(pe)}us")
        print(f"  |Δjitter|         mean={statistics.mean(jd):.1f}us max={max(jd)}us")
        print(f"  PASS 判据: mean<100us 且 max<150us -> "
              f"{'PASS' if statistics.mean(pe) < 100 and max(pe) < 150 and len(oks) == total else 'FAIL'}")
    print("=" * 60)

    fname = f"host_only_acceptance_{time.strftime('%Y%m%d_%H%M%S')}.csv"
    with open(fname, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["run", "idx", "ok", "host_fire", "host_jitter",
                                          "slave_fire", "slave_jitter", "delta", "jitter_diff"])
        w.writeheader()
        w.writerows(rows)
    print(f"CSV: {fname}")
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
