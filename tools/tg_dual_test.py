#!/usr/bin/env python3
"""验证同步触发（FIRE）时**两块板是否都上传了照片**到 Telegram。

背景：从机的 Telegram 上传日志只出现在从机自己的串口上，**不经 BLE 回传**
（BLE 只回传 E,FIRE_ACTUAL / E,DONE 这类短事件），所以只连主机的网页
永远看不到从机的上传结果。本脚本同时接两个口，把两边都摊开看。

用法:
    python tg_dual_test.py [host_port] [slave_port] [lead_ms] [wait_s]
默认:
    COM17  COM8  500  0

wait_s: 标定完成后再等多少秒才发 FIRE。用来区分"刚上电 WiFi/射频共存还没稳"
        和"角色本身的共存问题"——两个都测一次就能看出是哪一种。

注意：打开串口会拉 DTR/RTS，HOST 口（CH343）接了自动复位电路，所以主机
      会被复位；脚本会自动等 BLE 重连 + 重新标定，然后才发 FIRE。
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
LEAD_MS = sys.argv[3] if len(sys.argv) > 3 else '500'
WAIT_S = int(sys.argv[4]) if len(sys.argv) > 4 else 0
BAUD = 115200
# 第 3 个参数是纯数字就当成 lead -> "FIRE,<lead>"；否则原样当命令
# （例如 "SNAP" = 本板立刻拍一张并上传，完全不经过 BLE，用来隔离 WiFi/TLS 本身）
CMD = ('FIRE,' + LEAD_MS) if str(LEAD_MS).isdigit() else str(LEAD_MS)

q = queue.Queue()
stop = threading.Event()
ser_host = None                 # 保持句柄，写命令复用它（重开会被自己占用）
ser_slave = None

# 启动阶段只回显这些关键字，避免被 wifi/ble 的初始化刷屏
BOOT_KEYS = ('BLE:', 'TG:', 'WIFI:', 'UPLOAD', 'PHOTO', 'APP:', 'CALIB',
             'SD ', 'SD:', 'CAM:', 'got ip', 'ERR', 'E (', 'W (')


def _pump(name, ser):
    buf = b''
    try:
        while not stop.is_set():
            data = ser.read(4096)
            if not data:
                continue
            buf += data
            while b'\n' in buf:
                raw, buf = buf.split(b'\n', 1)
                text = raw.decode('utf-8', errors='ignore').strip()
                if text:
                    q.put((name, text))
    except Exception as e:
        q.put((name, '!! read error: %s' % e))
    finally:
        try:
            ser.close()
        except Exception:
            pass


def open_reader(name, port):
    """打开端口并启动读线程，返回 serial 对象（失败返回 None）。"""
    try:
        ser = serial.Serial(port, BAUD, timeout=0.2)
    except Exception as e:
        q.put((name, '!! open %s failed: %s' % (port, e)))
        print('!! 无法打开 %s: %s' % (port, e))
        if 'PermissionError' in str(e) or '拒绝' in str(e):
            print('   -> 该口正被别的程序占用（浏览器页面 / 其它脚本）。'
                  '关掉占用它的页面或程序再试。')
        return None
    print('-- %s open @%d' % (port, BAUD))
    threading.Thread(target=_pump, args=(name, ser), daemon=True).start()
    return ser


def drain(seconds, keep):
    """吃掉 seconds 秒日志；keep(name,text) 决定是否回显。"""
    t_end = time.time() + seconds
    while time.time() < t_end:
        try:
            name, text = q.get(timeout=0.2)
        except queue.Empty:
            continue
        if keep(name, text):
            print('%-5s | %s' % (name, text))


def main():
    global ser_host, ser_slave

    no_slave = str(SLAVE_PORT).upper() in ('NONE', '-', 'OFF', '')
    ser_host = open_reader('HOST', HOST_PORT)
    if no_slave:
        print('-- 第二个参数是 %s -> 只读 HOST 一个口' % SLAVE_PORT)
    else:
        ser_slave = open_reader('SLAVE', SLAVE_PORT)
    if ser_host is None:
        stop.set()
        sys.exit(1)

    # 1) 等两板启动 + BLE 连接 + 标定
    print('\n=== 等待 BLE 连接 / 标定（最多 40 s）===')
    saw_calib = False
    t_end = time.time() + 40
    while time.time() < t_end:
        try:
            name, text = q.get(timeout=0.3)
        except queue.Empty:
            continue
        if any(k in text for k in BOOT_KEYS):
            print('%-5s | %s' % (name, text))
        if 'CALIB,' in text:
            saw_calib = True
            break
    if not saw_calib:
        print('!! 40 s 内没看到 HOST 的 CALIB —— 两板可能没连上，仍继续发 FIRE 试试')

    # 1b) 可选：等 WiFi / 射频共存稳定，再发 FIRE
    if WAIT_S > 0:
        print('\n=== 标定完成，额外等 %d s 让 WiFi / 共存稳定 ===' % WAIT_S)
        drain(WAIT_S, lambda n, t: ('TG:' in t or 'WIFI:' in t or 'got ip' in t
                                    or 'HB,' in t))

    # 2) 清空积压，然后发 FIRE（复用已打开的句柄！）
    while not q.empty():
        try:
            q.get_nowait()
        except queue.Empty:
            break

    print('\n=== 发送 %s ===' % CMD)
    ser_host.write((CMD + '\n').encode())
    ser_host.flush()
    print('已发送，等两板到点 + 上传（最多 45 s）...\n')

    # 3) 收集到点与上传日志
    batch = []

    # ★ 注意：这里**漏一个关键字就会把关键证据滤掉**。曾经漏了 "upload mode"，
    #   而主机那条日志文本恰好是 "upload mode ON: conn itvl -> 100ms"（不含
    #   "scan"），于是被静默丢弃，害我误判成"弱符号没生效"。凡是被调查中的
    #   模块，日志一律放行。
    KEEP_KEYS = ('TG:', 'FIRE_ACTUAL', 'queued for telegram', 'photo saved',
                 'DONE', 'SD not mounted', 'UPLOAD', 'upload mode', 'conn itvl',
                 'grab', 'marker', 'WIFI:', 'scan', 'BLE:', 'HB,')

    def keep(n, t):
        return any(k in t for k in KEEP_KEYS)

    t_end = time.time() + 45
    while time.time() < t_end:
        try:
            name, text = q.get(timeout=0.2)
        except queue.Empty:
            continue
        batch.append((name, text))
        if keep(name, text):
            print('%-5s | %s' % (name, text))
        if sum(1 for _, t in batch if 'sendPhoto' in t) >= 2:
            # 两板都出了结果，再等 2 s 收尾
            drain(2, keep)
            break

    stop.set()

    # 4) 汇总
    print('\n' + '=' * 70)
    per = {'HOST': {'fire': 0, 'queued': 0, 'ok': 0, 'fail': 0, 'wifi': 0},
           'SLAVE': {'fire': 0, 'queued': 0, 'ok': 0, 'fail': 0, 'wifi': 0}}
    for name, text in batch:
        if name not in per:
            continue
        if 'FIRE_ACTUAL' in text:
            per[name]['fire'] += 1
        if 'queued for telegram' in text:
            per[name]['queued'] += 1
        if 'sendPhoto ok' in text:
            per[name]['ok'] += 1
        if 'sendPhoto fail' in text or 'sendPhoto FAIL' in text or '上传失败' in text:
            per[name]['fail'] += 1
        if 'got ip' in text:
            per[name]['wifi'] += 1

    for name in ('HOST', 'SLAVE'):
        d = per[name]
        print('%-5s  FIRE_ACTUAL=%d  queued=%d  upload_ok=%d  upload_fail=%d'
              % (name, d['fire'], d['queued'], d['ok'], d['fail']))
    print('-' * 70)
    out = []
    for name in ('HOST', 'SLAVE'):
        d = per[name]
        if d['ok']:
            out.append('%s: OK' % name)
        elif d['queued']:
            out.append('%s: QUEUED BUT UPLOAD FAILED' % name)
        elif d['fire']:
            out.append('%s: FIRED BUT NEVER QUEUED' % name)
        else:
            out.append('%s: NO FIRE_ACTUAL SEEN' % name)
    print('  |  '.join(out))
    print('Telegram 每台设备各应收到 1 张，caption 形如 '
          '"HOST sync t_fire=...us" / "SLAVE sync t_fire=...us"')
    print('提示：上传有 1~5 s 的重试退避，且同 chat 有节流，'
          '日志里的耗时通常 2~6 s。')


if __name__ == '__main__':
    main()
