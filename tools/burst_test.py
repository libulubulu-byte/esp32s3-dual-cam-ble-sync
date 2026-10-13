#!/usr/bin/env python3
"""连发 N 张 FIRE，验证"拍完再上传"的低优先级上传行为。

期望：
  - 批次进行中：**不应**出现 "TG: 上传中"（射频留给 BLE + 摄像头）
  - 批次结束后静默 TG_IDLE_BEFORE_UPLOAD_MS(默认 5000ms)：出现
    "静默 5000 ms -> 开始上传"
  - 若批次中恰好有上传在跑，应看到 "有新拍照 -> 打断本次上传"

用法:
    python burst_test.py [host_port] [slave_port] [shots] [gap_ms] [tail_s]
默认:
    COM17 COM8 6 1000 20
"""
import sys
import time
import threading
import queue

try:
    import serial
except ImportError:
    print('需要 pyserial:  pip install pyserial')
    sys.exit(2)

try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

HOST_PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM17'
SLAVE_PORT = sys.argv[2] if len(sys.argv) > 2 else 'COM8'
SHOTS = int(sys.argv[3]) if len(sys.argv) > 3 else 6
GAP_MS = int(sys.argv[4]) if len(sys.argv) > 4 else 1000
TAIL_S = int(sys.argv[5]) if len(sys.argv) > 5 else 20
BAUD = 115200

q = queue.Queue()
stop = threading.Event()
ser = {}


def open_pump(name, port):
    try:
        s = serial.Serial(port, BAUD, timeout=0.2)
    except Exception as e:
        print('!! 打不开 %s: %s' % (port, e))
        return None
    print('-- %s open @%d' % (port, BAUD))

    def pump():
        buf = b''
        try:
            while not stop.is_set():
                data = s.read(4096)
                if not data:
                    continue
                buf += data
                while b'\n' in buf:
                    raw, buf = buf.split(b'\n', 1)
                    t = raw.decode('utf-8', errors='ignore').strip()
                    if t:
                        q.put((name, t))
        finally:
            try:
                s.close()
            except Exception:
                pass

    threading.Thread(target=pump, daemon=True).start()
    return s


def main():
    h = open_pump('HOST', HOST_PORT)
    if h is None:
        sys.exit(1)
    open_pump('SLAVE', SLAVE_PORT)

    # 等连接/标定
    print('\n=== 等待 BLE 连接 + 标定（最多 40 s）===')
    t_end = time.time() + 40
    while time.time() < t_end:
        try:
            n, t = q.get(timeout=0.3)
        except queue.Empty:
            continue
        if any(k in t for k in ('CALIB,', 'got ip', 'upload mode')):
            print('%-5s | %s' % (n, t))
        if 'CALIB,' in t:
            break

    while not q.empty():
        try:
            q.get_nowait()
        except queue.Empty:
            break

    # ---- 连发 ----
    print('\n=== 连发 %d 张，间隔 %d ms ===' % (SHOTS, GAP_MS))
    t_burst0 = time.time()
    for i in range(1, SHOTS + 1):
        h.write(b'FIRE,500\n')
        h.flush()
        print('  -> FIRE,500  (%d/%d)' % (i, SHOTS))
        if i < SHOTS:
            time.sleep(GAP_MS / 1000.0)
    t_burst_end = time.time()
    print('=== 发完，观察 %d s ===' % TAIL_S)

    # ---- 观察 ----
    interesting = []
    log = []
    t_end = time.time() + TAIL_S
    while time.time() < t_end:
        try:
            n, t = q.get(timeout=0.2)
        except queue.Empty:
            continue
        log.append((time.time(), n, t))
        if any(k in t for k in ('TG:', 'FIRE_ACTUAL', 'DONE', 'upload mode',
                                '静默', '打断', 'queued', 'stagger',
                                'HB,', 'DNS', 'BLE_CONNECTED', 'BLE_DISCONNECTED',
                                '断开', 'upload:')):
            print('%-5s | %s' % (n, t))
        if 'sendPhoto' in t:
            interesting.append((time.time(), n, t))

    stop.set()

    # ---- 判定 ----
    print('\n' + '=' * 70)
    uploads = [(ts, n, t) for ts, n, t in log if 'TG: 上传中' in t]
    aborts = [(ts, n, t) for ts, n, t in log if '打断本次上传' in t]
    idle_ok = [(ts, n, t) for ts, n, t in log if '静默' in t and '开始上传' in t]
    oks = [(ts, n, t) for ts, n, t in log if 'sendPhoto ok' in t]
    fails = [(ts, n, t) for ts, n, t in log if 'open 失败' in t or 'sendPhoto FAILED' in t]

    dur = t_burst_end - t_burst0
    print('连发窗口: %.1f s（%d 张 / %d ms）' % (dur, SHOTS, GAP_MS))
    print('批次期间出现 "上传中" 的次数 : %d %s'
          % (len([1 for ts, _, _ in uploads if t_burst0 <= ts <= t_burst_end]),
             '<- 应为 0（射频要让给拍照）'))
    print('"静默 -> 开始上传" 次数      : %d' % len(idle_ok))
    print('"有新拍照 -> 打断上传" 次数  : %d' % len(aborts))
    print('sendPhoto 成功 / 失败        : %d / %d' % (len(oks), len(fails)))
    for ts, n, t in oks:
        print('    OK  %-5s  发完批次后 %.1f s 开始/完成' % (n, ts - t_burst_end))


if __name__ == '__main__':
    main()
