#!/usr/bin/env python3
"""同时监听两个串口，验证上电后：两板启动 -> BLE 自动连接 -> 心跳/标定就绪。

用法:
    python verify_boot.py [host_port] [slave_port] [seconds]
默认:
    host_port=COM17  slave_port=COM8  seconds=30

两个口都可用时按两板模式读；只给一个口就只读那个口。
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

# Windows 控制台默认 GBK，中文标签会乱码
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except Exception:
    pass

HOST_PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM17'
SLAVE_PORT = sys.argv[2] if len(sys.argv) > 2 else 'COM8'
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
BAUD = 115200

lines = queue.Queue()
stats = {}


def reader(name, port, q):
    try:
        ser = serial.Serial(port, BAUD, timeout=0.2)
    except Exception as e:
        q.put((name, '!! 打开 %s 失败: %s' % (port, e)))
        return
    stats[name] = {'port': port, 'lines': 0, 'events': set()}
    q.put((name, '-- 已打开 %s @%d' % (port, BAUD)))
    buf = b''
    t_end = time.time() + SECONDS
    try:
        while time.time() < t_end:
            data = ser.read(4096)
            if not data:
                continue
            buf += data
            while b'\n' in buf:
                raw, buf = buf.split(b'\n', 1)
                text = raw.decode('utf-8', errors='ignore').strip()
                if text:
                    stats[name]['lines'] += 1
                    for key in ('BOOT', 'BLE_CONNECTED', 'BLE_DISCONNECTED',
                                'CALIB', 'HB', 'FIRE_ACTUAL', 'DONE'):
                        if key in text:
                            stats[name]['events'].add(key)
                    q.put((name, text))
    finally:
        ser.close()


def main():
    threads = []
    if SLAVE_PORT:
        for name, port in (('HOST', HOST_PORT), ('SLAVE', SLAVE_PORT)):
            t = threading.Thread(target=reader, args=(name, port, lines), daemon=True)
            t.start()
            threads.append(t)
    else:
        t = threading.Thread(target=reader, args=('HOST', HOST_PORT, lines), daemon=True)
        t.start()
        threads.append(t)

    seen = []
    t_end = time.time() + SECONDS
    while time.time() < t_end:
        try:
            name, text = lines.get(timeout=0.3)
        except queue.Empty:
            continue
        print('%-5s | %s' % (name, text))
        seen.append((name, text))

    print('\n' + '=' * 62)
    for name, st in stats.items():
        ev = sorted(st['events'])
        print('%-5s %-6s lines=%-5d events=%s'
              % (name, st['port'], st['lines'], ','.join(ev) or '(none)'))

    joined = '\n'.join(t for _, t in seen)
    chek = [
        ('从机广播',       'SLAVE' in joined or 'advertis' in joined.lower()),
        ('BLE 已连接',     'BLE_CONNECTED' in joined or 'CONNECTED' in joined.upper()),
        ('心跳',           ',HB,' in joined),
        ('已标定',         'CALIB' in joined),
    ]
    print('-' * 62)
    for label, ok in chek:
        print('  [%s] %s' % ('OK' if ok else '--', label))


if __name__ == '__main__':
    main()
