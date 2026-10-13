#!/usr/bin/env python3
"""
ESP32-S3 Dual-Camera BLE Sync Acceptance Test

用法:
    python sync_cam_test.py <host_port> <slave_port> [--runs 3] [--rounds 20] [--lead 500]

架构说明（重要）:
    两块板的 esp_timer 是**各自独立**的（上电从 0 开始），因此
    `t_host_fire` 与 `t_slave_fire` 不能直接相减 —— 它们包含一个固定的
    时钟偏移（两板上电时刻差，可达数秒）。

    真正可比较的量是：
      jitter = t_fire - (t_rx + lead)     每块板自己的"相对定时误差"
    物理同步误差 ≈ |(t_host_fire - t_slave_fire) - 固定偏移| 的波动

    本脚本同时输出两种口径：
      sync_err_phase = |Δ - median(Δ)|         Δ = host_fire - slave_fire
                       （去掉固定钟差后的物理同步误差，主指标）
      sync_err_jitter = |jitter_host - jitter_slave|
                       （两板定时器精度差）
"""

import argparse
import csv
import statistics
import sys
import time

import serial


def open_port(name, baud=115200):
    # ★ 不要设置 dtr/rts：本机 COM17 是 CH343 板，任何 DTR/RTS 变化都会把
    #   芯片复位或带进下载模式。保持 pyserial 默认(True,True) 才能正常读日志。
    return serial.Serial(name, baud, timeout=0.05)


def parse_line(line):
    """只认行首为 HOST, / SLAVE, 的 CSV 行；其它（ESP 日志）返回 None"""
    s = line.strip()
    if s.startswith("HOST,"):
        rest = s[5:]
    elif s.startswith("SLAVE,"):
        rest = s[6:]
    else:
        return None
    parts = rest.split(",")
    if len(parts) < 2:
        return None
    rec = {"event": parts[0], "t": None, "extra": None}
    try:
        rec["t"] = int(parts[1])
    except ValueError:
        return None
    if len(parts) > 2:
        try:
            rec["extra"] = int(parts[2])
        except ValueError:
            rec["extra"] = None
    return rec


def drain(ser, lines, tag=""):
    """把串口当前可读的行全部读出来"""
    while ser.in_waiting:
        raw = ser.readline()
        if not raw:
            break
        try:
            txt = raw.decode("utf-8", errors="ignore")
        except Exception:
            continue
        lines.append((tag, txt))


def wait_connected(host, slave, timeout_s=30):
    print(f"等待两板 BLE 连接（最多 {timeout_s}s）...")
    t0 = time.time()
    hc = sc = False
    while time.time() - t0 < timeout_s:
        for ser, name in ((host, "HOST"), (slave, "SLAVE")):
            while ser.in_waiting:
                raw = ser.readline()
                if not raw:
                    break
                txt = raw.decode("utf-8", errors="ignore").strip()
                rec = parse_line(txt)
                if rec:
                    if rec["event"] == "BLE_CONNECTED":
                        print(f"  {name}: BLE_CONNECTED")
                        if name == "HOST":
                            hc = True
                        else:
                            sc = True
                    elif rec["event"] == "BLE_DISCONNECTED":
                        print(f"  {name}: BLE_DISCONNECTED")
                        if name == "HOST":
                            hc = False
                        else:
                            sc = False
        if hc and sc:
            return True
        time.sleep(0.05)
    print(f"超时：HOST 连接={hc} SLAVE 连接={sc}")
    return False


def run_once(host, slave, lead_ms, timeout_s=3.0, verbose=False):
    """执行一次 FIRE，返回 (host_fire, host_jitter, slave_fire, slave_jitter) 或 None"""
    # 清掉缓冲
    for s in (host, slave):
        s.reset_input_buffer()

    host.write(f"FIRE,{lead_ms}\n".encode())
    host.flush()

    h = None
    sl = None
    raw_h = []
    raw_s = []
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        while host.in_waiting:
            raw = host.readline()
            if not raw:
                break
            txt = raw.decode("utf-8", errors="ignore")
            rec = parse_line(txt)
            if rec and rec["event"] == "FIRE_ACTUAL":
                if h is None:
                    h = (rec["t"], rec["extra"])
            elif rec:
                raw_h.append(txt.strip())
                if rec["event"] in ("BLE_CONNECTED", "BLE_DISCONNECTED"):
                    print(f"       HOST {rec['event']} extra={rec['extra']}")
        while slave.in_waiting:
            raw = slave.readline()
            if not raw:
                break
            txt = raw.decode("utf-8", errors="ignore")
            rec = parse_line(txt)
            if rec and rec["event"] == "FIRE_ACTUAL":
                if sl is None:
                    sl = (rec["t"], rec["extra"])
            elif rec:
                raw_s.append(txt.strip())
                if rec["event"] in ("BLE_CONNECTED", "BLE_DISCONNECTED"):
                    print(f"       SLAVE {rec['event']} extra={rec['extra']}")
        if h is not None and sl is not None:
            break
        time.sleep(0.01)

    if h is None or sl is None:
        print(f"       !! 超时: host_fire={'OK' if h else 'MISS'} "
              f"slave_fire={'OK' if sl else 'MISS'}")
        for x in raw_h[-8:]:
            print(f"         H| {x}")
        for x in raw_s[-8:]:
            print(f"         S| {x}")
        return None
    return {"host_fire": h[0], "host_jitter": h[1],
            "slave_fire": sl[0], "slave_jitter": sl[1]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host_port")
    ap.add_argument("slave_port")
    ap.add_argument("--runs", type=int, default=3, help="轮数（默认 3）")
    ap.add_argument("--rounds", type=int, default=20, help="每轮次数（默认 20）")
    ap.add_argument("--lead", type=int, default=500, help="lead_ms（默认 500）")
    args = ap.parse_args()

    host = open_port(args.host_port)
    slave = open_port(args.slave_port)
    print(f"已打开 host={args.host_port} slave={args.slave_port}")

    if not wait_connected(host, slave):
        print("未连接，退出")
        host.close()
        slave.close()
        return 1

    # 等主机完成服务发现 + CCCD 订阅 + 钟差标定（约 0.5s），再多留一点余量
    print("等待钟差标定完成 (3s)...")
    time.sleep(3.0)
    # 把标定期间产生的日志读掉，避免干扰第一轮
    for s in (host, slave):
        s.reset_input_buffer()

    all_rows = []
    run_summary = []

    for run in range(1, args.runs + 1):
        print(f"\n===== 第 {run}/{args.runs} 轮 =====")
        rows = []
        for i in range(args.rounds):
            r = run_once(host, slave, args.lead)
            if r is None:
                print(f"  [{run}-{i+1:02d}] 超时（无 FIRE_ACTUAL）")
                rows.append({"run": run, "idx": i + 1, "ok": 0})
                continue
            delta = r["host_fire"] - r["slave_fire"]
            jd = abs(r["host_jitter"] - r["slave_jitter"])
            row = {
                "run": run, "idx": i + 1, "ok": 1,
                "host_fire": r["host_fire"], "host_jitter": r["host_jitter"],
                "slave_fire": r["slave_fire"], "slave_jitter": r["slave_jitter"],
                "delta": delta, "jitter_diff": jd,
            }
            rows.append(row)
            print(f"  [{run}-{i+1:02d}] Δ={delta:>12}  jitter H={r['host_jitter']:+6} "
                  f"S={r['slave_jitter']:+6}  |Δj|={jd:5}")
            time.sleep(0.3)

        oks = [x for x in rows if x["ok"]]
        if oks:
            deltas = [x["delta"] for x in oks]
            med = statistics.median(deltas)
            phase_errs = [abs(d - med) for d in deltas]
            jdiffs = [x["jitter_diff"] for x in oks]
            run_summary.append({
                "run": run, "valid": len(oks),
                "phase_mean": statistics.mean(phase_errs),
                "phase_max": max(phase_errs),
                "jdiff_mean": statistics.mean(jdiffs),
                "jdiff_max": max(jdiffs),
            })
            print(f"  -- 本轮: 有效 {len(oks)}/{args.rounds}  "
                  f"phase_err mean={statistics.mean(phase_errs):.0f} "
                  f"max={max(phase_errs)}  "
                  f"|Δjitter| mean={statistics.mean(jdiffs):.0f} max={max(jdiffs)}")
        all_rows.extend(rows)

    # 汇总
    print("\n" + "=" * 62)
    for s in run_summary:
        print(f"  轮 {s['run']}: 有效 {s['valid']}/{args.rounds}  "
              f"phase_err mean={s['phase_mean']:.0f}us max={s['phase_max']}us  "
              f"|Δjitter| mean={s['jdiff_mean']:.0f}us max={s['jdiff_max']}us")
    print("=" * 62)

    oks = [x for x in all_rows if x["ok"]]
    total = args.runs * args.rounds
    if oks:
        deltas = [x["delta"] for x in oks]
        med = statistics.median(deltas)
        phase_errs = [abs(d - med) for d in deltas]
        jdiffs = [x["jitter_diff"] for x in oks]
        print(f"总体: 有效 {len(oks)}/{total}")
        print(f"  同步误差(phase)  mean={statistics.mean(phase_errs):.1f}us "
              f"max={max(phase_errs)}us min={min(phase_errs)}us")
        print(f"  |Δjitter|        mean={statistics.mean(jdiffs):.1f}us max={max(jdiffs)}us")
        print(f"  固定钟差(median Δ) = {med}us")

    fname = f"sync_cam_acceptance_{time.strftime('%Y%m%d_%H%M%S')}.csv"
    with open(fname, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["run", "idx", "ok", "host_fire", "host_jitter",
                                          "slave_fire", "slave_jitter", "delta", "jitter_diff"])
        w.writeheader()
        w.writerows(all_rows)
    print(f"CSV 已保存: {fname}")

    host.close()
    slave.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
