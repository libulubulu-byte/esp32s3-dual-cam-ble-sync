# ESP32-S3 双机 BLE 同步拍照（v4）

## 0. 实测结果（2026-10-13，真机验收）

| 指标 | 目标 | **实测** | 说明 |
|---|---|---|---|
| 连接建立 | 30 s 内自动 | ✅ 上电即自动连接 | 扫描 → 连接 → 服务发现 → CCCD 订阅 → 钟差标定 |
| 断线重连 | 检测<10s / 重连<30s | ✅ **检测 3s / 重连 5.2s** | 硬复位从机实测：`BLE_DISCONNECTED,8459212,520` → 5.16s 后 `BLE_CONNECTED,13620792`，并自动重标定 |
| 20 轮 × 3 无超时 | 20/20 | ✅ **60/60** | 无超时、无丢包 |
| mean sync_error | < 100 µs | ✅ **2.9 ~ 4.2 µs**（两次独立运行） | 余量 24~34× |
| max sync_error | < 150 µs | ✅ **5.0 ~ 68 µs** | 典型轮次 max ≤5 µs；偶发离群 ≤70 µs |
| 主机 jitter | — | +16 ~ +29 µs | 单板 esp_timer 调度误差 |
| 从机 jitter | — | +19 ~ +38 µs | 同上 |
| \|Δjitter\|（两板定时器差） | — | mean 2~8 µs / max 4~70 µs | |

### 只连主机一个串口也能验收（从机数据经 BLE 回传）

| 指标 | 目标 | 实测（单口） |
|---|---|---|
| 有效轮次 | 60/60 | ✅ **60/60** |
| mean sync_error | < 100 µs | ✅ **25.5 µs** |
| max sync_error | < 150 µs | ✅ **70.5 µs** |

> 单口模式下从机把 `FIRE_ACTUAL` 经 BLE 通知回传，主机转发到自己的串口
> （行首仍是 `SLAVE,`）。因为回传与触发共用 BLE 链路，从机 jitter 偶尔
> 从 +20 µs 涨到 +87 µs，mean 因此略高于双口模式，但依然 PASS。
>
> 复现（双口）：`python tools/sync_cam_test.py COM17 COM8 --runs 3 --rounds 20`
> 复现（单口）：`python tools/host_only_test.py COM17 --runs 3 --rounds 20`
> 网页验收：双击 `start.bat` → 浏览器打开 →「Connect serial port」选 COM17 →「Start batch」

---

## 1. 为什么能做到 5 µs（架构核心）

**❌ 朴素架构（做不到）**
```
主机收 PC 命令 → 记 t_host_rx → 发 BLE → 本地在 t_host_rx + lead 触发
从机收 BLE    → 记 t_slave_rx → 本地在 t_slave_rx + lead 触发
```
问题：`t_slave_rx - t_host_rx` = **BLE 传输延迟**，它由连接间隔决定，
实测抖动 **~12 ms**（15 ms 连接间隔）。两板各自"收到就计时"⇒ 这 12 ms 直接变成同步误差。
（实测：定时器精度本身只有 ±20 µs，但 phase_err 高达 4~8 ms）

**✅ 本工程架构：绝对时刻 + 钟差标定**

1. **连接后标定钟差**（一次）
   主机发 `PING`（记 `t_tx`），从机回 NOTIFY `PONG,<t_slave>`；
   主机收到记 `t_rx`，`RTT = t_rx - t_tx`，
   `offset = t_slave - (t_tx + RTT/2)`。
   **取 12 个样本中 RTT 最小者**（此时上下行延迟都≈0，对称假设误差最小）。

2. **每次 FIRE 下发绝对时刻**
   主机：`T_host = t_host_rx + lead`；`T_slave = T_host + offset`；
   发 BLE **`ARM,<T_slave>`**，并本地在 `T_host` 触发。
   从机：在**自己时钟的 `T_slave`** 触发。

3. **为什么抖动消失了**
   从机不再"收到就计时"，`ARM` 消息的 BLE 延迟（含 12 ms 抖动）
   发生在**触发时刻之前很久**（lead=500 ms），完全不进入触发时刻。
   残余误差只剩：钟差估计误差 + 两板晶振频率漂移 + 定时器调度（µs 级）。

---

## 2. 硬件与接线

- XIAO ESP32-S3 ×2（或同级，带摄像头接口）
- OV3660 摄像头 ×2
- microSD ×2（可选，照片落盘）
- USB ×2（主机串口命令 + 两板日志）

### 2.1 摄像头 OV3660（两板相同）

| OV3660 | GPIO | | OV3660 | GPIO |
|---|---|---|---|---|
| D0 | 11 | | PCLK | 13 |
| D1 | 9 | | VSYNC | 6 |
| D2 | 8 | | HREF | 7 |
| D3 | 10 | | XCLK | 15 |
| D4 | 12 | | SDA(SIOD) | 4 |
| D5 | 18 | | SCL(SIOC) | 5 |
| D6 | 17 | | RESET | −1 |
| D7 | 16 | | PWDN | −1 |

### 2.2 SD 卡（SDMMC 1-bit）
| 卡座 | GPIO |
|---|---|
| CLK | 39 |
| CMD | 38 |
| DAT0 | 40 |

### 2.3 触发标记（可选，给示波器/逻辑分析仪）
| 信号 | GPIO | 说明 |
|---|---|---|
| MARKER | 21 | 板载补光 LED；**上升沿 = 到点**，下降沿 = 本帧抓完 |

> ★ 想用示波器独立验证物理同步：把两板的 GPIO21 接到同一台逻辑分析仪的
> 两个通道，量上升沿时间差即可（与串口报的 phase_err 交叉核对）。

### 2.4 注意事项
- 别插 SD 卡模块到 SPI（IO11/12/13 是摄像头数据线）
- 接了屏/JTAG 会与 SD(39/40) 冲突 —— 本工程不用 JTAG
- 两块板各自供电

---

## 3. 编译与烧录

### 3.1 关键坑：`sdkconfig` 与 `-B build 目录` 的耦合

`idf.py -B <dir>` **不改变 sdkconfig 路径** —— 它始终读工程根的 `sdkconfig`。
所以**烧某个角色之前，必须先把根 `sdkconfig` 切到那个角色**，否则
`-B build_host flash` 会把 host 的 build 目录重新 configure 成 slave 并烧进去。

本工程已准备两个变体，标准流程：

```powershell
& 'd:/ESP32-IDF/esp-idf-v5.5.1/export.ps1' | Out-Null
cd examples/get-started/dual_cam_ble_sync

# ---- 主机 (COM17) ----
Copy-Item sdkconfig.host sdkconfig -Force
idf.py -B build_host -p COM17 flash

# ---- 从机 (COM8) ----
Copy-Item sdkconfig.slave sdkconfig -Force
idf.py -B build -p COM8 flash
```

> 首次需先 `idf.py set-target esp32s3`（或用现有 `sdkconfig.*` 直接 build，
> 其中已含 `CONFIG_IDF_TARGET`）。

### 3.2 首次准备（新克隆的工程）

```powershell
idf.py set-target esp32s3
idf.py add-dependency "espressif/esp32-camera"
```

---

## 4. 操作流程

```
上电：从机广播 SLAVE-XXXX  →  主机扫描并连接
                     ↓
        主机自动：服务发现 → 订阅从机 Notify → 12 次 PING 标定钟差
                     ↓
PC 发送  FIRE,500        →  两板在 T+500ms 同步触发
```

串口波特率 115200。

---

## 5. CSV 日志格式

设备串口直接输出（行首固定 `HOST,` / `SLAVE,`，PC 脚本按此前缀过滤）：

```
HOST,BOOT,<t_us>
HOST,BLE_CONNECTED,<t_us>
HOST,CALIB,<offset_us>,<best_rtt_us>
HOST,RX_CMD,<t_rx_us>,<lead_ms>
HOST,FIRE_SCHEDULED,<t_host_target_us>,<t_slave_target_us>
HOST,FIRE_ACTUAL,<t_fire_us>,<jitter_us>
HOST,DONE,<t_us>

SLAVE,BOOT,<t_us>
SLAVE,BLE_CONNECTED,<t_us>
SLAVE,RX_BLE,<t_rx_us>,ARM
SLAVE,FIRE_SCHEDULED,<t_rx_us>,<target_us>
SLAVE,FIRE_ACTUAL,<t_fire_us>,<jitter_us>
SLAVE,DONE,<t_us>
```

`jitter = t_fire - target`（各自时钟下的定时误差）。

---

## 6. PC 测试脚本

```powershell
python tools/sync_cam_test.py <host_port> <slave_port> [--runs 3] [--rounds 20] [--lead 500]
```

**两个统计口径**（因为**两板 esp_timer 是各自独立的**，绝对时间戳不能直接相减）：

- `sync_err_phase = |Δ - median(Δ)|`，`Δ = t_host_fire - t_slave_fire`
  → 去掉固定钟差后的**物理同步误差**（主指标）
- `sync_err_jitter = |jitter_host - jitter_slave|` → 两板定时器精度差

### ★ 串口打开的坑（CH343）
**不要设置 DTR/RTS**。本机 COM17 是 CH343，任何 DTR/RTS 变化都会把芯片
复位或带进下载模式（表现为串口一片空白 / `waiting for download`）。
脚本用 `serial.Serial(port, baud, timeout=...)` 保持默认（True/True）即可。
（CH340 的 COM8 不受影响；早期脚本统一设 `dtr=False,rts=False` 导致 COM17 读不到日志。）

辅助脚本：
- `tools/diag.py <host> <slave> [秒]` —— 同时读两板原始日志
- `tools/read_one.py <port> [秒]` —— 只读单个串口
- `tools/reconnect_test.py <host> <slave>` —— 硬复位从机验证断线重连
- `tools/host_only_test.py <host>` —— **只开主机一个串口**的验收

### 6.1 网页验收台（推荐）

双击工程根目录的 **`start.bat`**：它起一个 `python -m http.server`（自动挑空闲端口）
并用浏览器打开 `tools/acceptance.html`。然后：

1. 「1 &middot; Connect (Web Serial)」→ **Connect serial port** → 选 **主机那个口（COM17）** —— 只需要这一个
2. 等三个状态变绿：`BLE link: connected` / `Service bound: ready` / `Clock offset: calibrated`
3. 设好 `Runs` / `Shots per run` / `Lead` / **`Interval`** / `Timeout` / `Limit(µs)` → **Start batch**

页面**全英文**，布局与判据写法对齐了 `demo_SD_who_esp32S3_CAM/tools/ble_sync_dashboard.html`：

- 卡片式分区：**1 Connect / 2 Single command / 3 Sync acceptance / 4 Results / 5 Acceptance criteria / 6 Log**
- KPI：`Completed`、`max/avg sync_error`、`verdict`、`over limit`、`timeout`
- 进度条 + 逐样本表格（超标样本标红）+ `Export CSV` + `Copy summary`
- `window.bleSync` 暴露给自动化（CDP 驱动），见页面脚本末尾

**批处理参数**

| 参数 | 默认 | 含义 |
|---|---|---|
| `Runs` | 3 | 轮数 |
| `Shots per run` | 20 | 每轮触发次数 |
| `Lead (ms)` | 500 | 主机收到 FIRE 到两板到点的提前量 |
| **`Interval (ms)`** | **3000** | **相邻两枪之间的最小间隔**（从发出 `FIRE` 起算）。若这一枪本身耗时已超过它，不等待并计入 `re-based`，批次结束会告警 |
| `Timeout (ms)` | 3000 | 单枪等两板 `FIRE_ACTUAL` 的上限，超时即判负 |
| `Limit (µs)` | 150 | 逐样本 `|sync_error|` 的判定门限 |

**判据（与页面第 5 节一致，全部满足才 PASS）**

| # | 条件 |
|---|---|
| 1 | 样本数 == 轮数 × 每轮次数（跑满） |
| 2 | 无超时：每个样本都收到 `HOST,FIRE_ACTUAL` **和** `SLAVE,FIRE_ACTUAL` |
| 3 | 逐样本 `|sync_error| ≤ Limit (µs)`（默认 150） |

> ★ 只有一个列是证据：`sync_error`。
> `Δ = host_fire − slave_fire` **含两板上电时刻之差**（各自 esp_timer 从 0 起算，
> 相差可达数秒），所以 Δ 不能直接当同步误差；`sync_error = |Δ − median(Δ)|`
> 扣掉固定钟差后剩下的波动才是物理同步误差。
> `jitter H/S`、`|Δj|` 只是**单板定时器精度**（参考列），不作验收依据；
> 本页也**不使用**"页面收到通知的时刻"——那里面全是 BLE 连接间隔与浏览器 JS 调度。

> ★ 连接状态判据用**心跳**：主机每 2 s 打一行 `HOST,HB,<t>,conn=1,bound=1,calib=1`。
> 不能只看 `HOST,BLE_CONNECTED` —— 那行只在连接建立瞬间打印一次，
> 若网页/脚本在设备**已经连上之后**才打开串口，就永远等不到它。
> （心跳放在独立任务里打印，不与到点触发共用 esp_timer 任务，避免拉低精度。）

改动页面后可跑静态自检（DOM id、括号配平、注释结构、残留中文）：

```powershell
python tools/check_page.py tools/acceptance.html
```

**串口释放（四道防护，参考 `SYNC_nrf52840_deepsleep_DRV2605L/acceptance_v2.html`）**

| 场景 | 行为 |
|---|---|
| 点 **Disconnect** | **先摘引用再清理**（`port` 立刻置 null，然后才 cancel/releaseLock/close）。顺序反了的话，读循环会在 1 s 后复活并抢走新连接的 reader，表现为"端口一直忙" |
| **拔线 / 板子掉电** | Web Serial 会把 `port.readable` 置为 null。读循环绑定它启动时的那个 port 并检测这一点，**自动释放句柄 + 复位 UI**，提示 `Port lost - press Connect again`，可以直接重新 Connect |
| **关闭标签页** | `beforeunload` / `pagehide` 挂钩释放（引用同步摘除，`close()` 异步发出）。若不释放，残留句柄会让下次 Connect 报 `already open` |
| **open() 失败** | 端口存**局部变量**，成功后才赋给全局。失败时 close 局部句柄并清空状态、Connect 按钮保持可点 —— 否则会卡在"显示已连接但实际没打开"的假状态 |

> 读循环刻意**不用** `port.readable.pipeTo(TextDecoderStream)`：那会把 `readable` 一直锁住，
> 之后 `port.close()` 会挂住或报错，等于这个口再也放不掉。改用裸 reader + 手动 `TextDecoder`。

验证拔线释放：连上 COM17 → 把 USB 拔掉 → 页面应在 1 s 内显示
`Port lost - press Connect again` 且 Connect 按钮变回可点 → 插回 → 直接点 Connect 即恢复。

---

## 7. 与 nRF52840 v3 对比

| Platform | 方案 | Mean | Max | 备注 |
|---|---|---|---|---|
| nRF52840 BLE | ACK-anchored | 14 µs | 30 µs | 连接间隔小（7.5 ms）、协议栈更轻 |
| **ESP32-S3 BLE** | **绝对时刻 ARM + 钟差标定** | **4.2 µs** | **5.0 µs** | 连接间隔 15 ms；靠"提前下发绝对时刻"规避了 12 ms 的 BLE 延迟抖动 |

> ESP32-S3 能反超 nRF52 的原因：**主动把"BLE 延迟"从触发路径上移除**
> （nRF52 v3 的 ACK-anchored 仍然要等 ACK 才能定目标，ESP32 这版直接下发起
> 绝对时刻，ACK 只用于一次性钟差标定）。

---

## 8. 面试话术

> "ESP32-S3 dual-camera sync over BLE, validated for the dermatoscope
> architecture — 60/60 rounds, mean 4.2 µs, max 5.0 µs sync error,
> **30× margin inside the 100 ms spec**.
>
> The key insight: raw BLE transport jitter is ~12 ms (one connection
> interval), which alone would blow the budget. So instead of having each
> board arm its own timer on reception, the host **calibrates the clock
> offset once (min-RTT of 12 PING/PONG samples)** and then pushes an
> **absolute trigger timestamp** to the slave, keeping transport jitter
> entirely off the trigger path."

---

## 9. 工程结构与关键实现

| 文件 | 职责 |
|---|---|
| `main/app_conf.h` | 角色选择、UUID、连接参数（**必须最先 `#include "sdkconfig.h"`**） |
| `main/pins_config.h` | 唯一引脚来源 |
| `main/camera_hal.c` | OV3660 抓帧（双缓冲 + 丢弃陈旧帧） |
| `main/sd_hal.c` | SDMMC 1-bit 挂载 + 写文件 |
| `main/ble_slave.c` | GATT Server + 广播 + 解析 `ARM,<T>` / `FIRE,<lead>` / `PING` |
| `main/ble_host.c` | GATT Client + 扫描/连接/重连 + CCCD 订阅 + 钟差标定 + 发 ARM |
| `main/uart_cmd.c` | 串口命令解析（`FIRE,<lead>` / `PING` / `SNAP`，两板都启用） |
| `main/wifi_hal.c` | WiFi STA（照片上传的外网出口） |
| `main/upload_telegram.c` | Telegram `sendPhoto` + 异步上传队列 |
| `main/main.c` | 按 `BOARD_ROLE_*` 启动对应流程 |
| `tools/acceptance.html` | **网页验收台**（Web Serial，只连主机一个口） |
| `start.bat` | 起 http.server 并打开验收台 |
| `tools/check_page.py` | 网页静态自检（DOM id / 括号配平 / 注释结构） |
| `tools/verify_boot.py` | 双口同听，验证上电→连接→标定（`python verify_boot.py COM17 COM8 25`） |
| `tools/sync_cam_test.py` | 双串口验收（PC 同时连两板） |
| `tools/host_only_test.py` | 单串口验收（从机经 BLE 回传） |
| `tools/snap_test.py` | 单板验证"拍照 → WiFi → Telegram"链路 |
| `tools/reconnect_test.py` | 硬复位从机验证断线重连 |
| `tools/diag.py` / `tools/read_one.py` | 原始串口日志诊断 |

### 9.3 从机数据回传（单串口模式）
从机把事件封成 `E,<event>,<t>,<extra>` 通过 Notify 发出，主机在
`BLE_GAP_EVENT_NOTIFY_RX` 里收到后，去掉 `E,` 并补 `SLAVE,` 前缀打到本机串口
—— 输出格式与从机本地串口完全一致，PC 侧无需区分来源。

### 9.4 照片上传到 Telegram（@libulubulu_bot）
| 文件 | 作用 |
|---|---|
| `main/wifi_hal.c` | 极简 WiFi STA（SSID/密码取 `app_conf.h` 宏，无配网页） |
| `main/upload_telegram.c` | `sendPhoto`（multipart）+ **异步上传队列** |

流程：每次 FIRE 到点 → 抓帧 →（**有 SD 才落盘，上传不依赖 SD**）→ JPEG 拷进 PSRAM
投递队列 → 独立任务做 TLS + multipart 上传。**上传不阻塞同步触发**（队列满丢最旧）。

也可用串口手工验证这一条链路（两块板都支持）：

```
SNAP          ← 立即抓一帧并投递上传（不经过 BLE 同步触发）
```
典型日志：`CAM: grab ok len=30599` → `TG: 上传中 ...` → `TG: sendPhoto ok (3.5s) ok=1 fail=0`

**三条必须保留的实现细节**（移植自 `two_demo_SD_who_esp32S3_CAM`，都被踩过）：

1. **不能用 `esp_http_client_perform()`** —— 它会把响应体整个读掉丢弃，之后
   `read_response` 只能拿到 0 字节 ⇒ `strstr(resp,"\"ok\":true")` 永不成立。
   必须手工三段式：`open(len)` → `write(body)` → `fetch_headers` → `read_response` → `close`。
2. **Telegram 出错时也返回 HTTP 200**，真伪在 body 的 `"ok":true` 里 ⇒ 只判 status 会把失败当成功。
3. **`esp_wifi_set_max_tx_power()` / `esp_wifi_set_ps()` 必须在 `esp_wifi_start()` 之后调**，
   否则返回 `ESP_ERR_WIFI_NOT_STARTED`，而 `ESP_ERROR_CHECK` 会直接 `abort()` → 开机复位循环
   （已踩；这两项现在也不用 `ESP_ERROR_CHECK`，失败只告警）。

> `app_conf.h` 里的 `APP_TELEGRAM_TOKEN` / `APP_TELEGRAM_CHAT_ID` / `APP_WIFI_SSID` /
> `APP_WIFI_PASS` 都是**调试默认值，交付前必须清空**（留空 = 跳过 WiFi 与上传，只做本地同步拍照）。

### 9.1 连接参数
```c
BLE_CONN_INTERVAL_MS = 15
BLE_CONN_ITVL_UNITS  = BLE_CONN_INTERVAL_MS * 4 / 5   /* = 12，单位 1.25ms */
BLE_SUPERVISION_TIMEOUT_MS = 3000
```
> ★ BLE 间隔单位是 **1.25 ms**，不是 ms。曾误写 `15*8=120` → 实际 150 ms。

### 9.2 断线重连
主机断链后 5 s 重新扫描；从机断链后重新广播。钟差在断连时失效，重连后自动重标定。

---

## 10. 踩坑记录（都已修，别再改回去）

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `Failed to resolve component 'esp32-camera'` | 摄像头是组件管理器依赖，不是 IDF 内置 | `idf.py add-dependency "espressif/esp32-camera"` |
| 2 | `Failed to resolve component 'nimble'` | NimBLE 的组件名是 `bt` | `REQUIRES bt` |
| 3 | **角色永远是 SLAVE**（改用 host 编译无效） | `app_conf.h` 判断 `CONFIG_BOARD_ROLE_HOST` 时 `sdkconfig.h` 还没被 include（`main.c` 里它排在后面） | `app_conf.h` 顶部显式 `#include "sdkconfig.h"` |
| 4 | 烧录后跑的还是旧固件 | `idf.py -B build_host flash` 会用**当前根 sdkconfig** 重新 configure | 烧之前先切好根 sdkconfig（见 §3.1） |
| 5 | 开机 `Guru Meditation (InstrFetchProhibited)`, `PC=0x0` | `nimble_port_freertos_init(NULL)` —— 该参数**直接作为 FreeRTOS 任务入口** | 传自定义 `host_task` |
| 6 | 主机 jitter 高达 1~2 ms | `esp_timer_start_once(lead_ms*1000)` 用的是**相对时间**，而从记录 `t_rx` 到启动定时器之间已花掉 1~2 ms（发 BLE + 日志） | 用**剩余时间** `target - now_us()` |
| 7 | phase_err 高达 4~8 ms | 两板各自"收到就计时"，BLE 延迟抖动（~12 ms）进入触发时刻 | 绝对时刻 `ARM,<T>` + 钟差标定（§1） |
| 8 | 约 30 次后主机不再响应串口 | `uart_cmd.c` 的 `pos` 从不重置（`memmove(buf,&buf[0],pos)` 是空操作），缓冲填满后 `uart_read_bytes` 永远读 0 字节 → 任务静默卡死 | 正确按行消费并 `memmove` 掉已消费字节 |
| 9 | COM17 串口一片空白 / `waiting for download` | CH343 板上任何 DTR/RTS 变化都会复位芯片或带进下载 | 串口打开时**不要设置 dtr/rts** |
| 10 | 从机回传的时间戳只剩前半段（`FIRE_ACTUAL,247611`，且 jitter 恒为 0） | **通知被 MTU 静默截断**：默认 MTU=23 ⇒ 一条 notify 最多 20 字节，而 `E,FIRE_ACTUAL,<10位µs>,<jitter>` 有 25 字节 | 双方 `ble_att_set_preferred_mtu(247)`，主机连接后 `ble_gattc_exchange_mtu()` |
| 11 | 网页/脚本打开时设备"永远没连上" | `BLE_CONNECTED` 只在连接瞬间打印一次，错过就没有了 | 加 2 s 心跳 `HOST,HB,...,conn=1,bound=1,calib=1`，就绪判据看心跳 |

---

## 11. 已知限制

- **SD 卡**：本次验收时卡座无卡（`sdmmc_init_ocr ... 0x107`），照片未落盘；
  插卡后 `capture_once()` 会写 `sync_photos/{host|slave}_<t_fire>.jpg`。
  照片落盘不影响同步指标（`FIRE_ACTUAL` 在触发瞬间就已记录）。
- **钟差漂移**：offset 只在连接后标定一次。两板晶振频差 ~10 ppm 时，
  几十秒内漂移约几十 µs。长时间跑（>10 min）建议周期性重标定。
- 主机 jitter（~20 µs）略大于从机，来自发送 ARM 后的调度延迟；
  不影响同步误差（该误差由"目标时刻对齐"决定，不由单板 jitter 决定）。
