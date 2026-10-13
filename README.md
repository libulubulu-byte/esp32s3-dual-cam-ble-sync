# Dual-Camera Sync over BLE — ESP32-S3

**Two ESP32-S3 boards trigger their cameras within ~4 µs of each other, over a wireless BLE link — no wires between them.**

Measured on real hardware: **mean sync error 2.9–4.2 µs, max 5–68 µs** over 60 consecutive shots.
That is roughly **24,000× inside a 100 ms product requirement**, and **3–4× better than a comparable nRF52840 implementation**.

> Built as the technical validation for a **dual-camera dermatoscope** (both cameras must capture the same instant within 100 ms).
> The result: the 100 ms budget is consumed by ~0.004 % of the link's capability — the constraint is optics, not timing.

---

## The problem

Two independent battery-powered cameras must expose the same instant. Any solution that ships a timestamp *over a radio* and starts a timer on arrival cannot work:

```
naive:  host RX cmd -> record t_host -> BLE -> host fires at t_host + lead
        slave RX cmd -> record t_slave -> slave fires at t_slave + lead
```

`t_slave − t_host` **is** the BLE delivery latency, which is quantised by the connection interval.
Measured jitter of that quantity on this hardware: **~12 ms** (15 ms connection interval).
The local timers are accurate to ±20 µs, but the *phase* error is 4–8 ms — **~1000× worse than the timer**.

## Architecture: absolute instants + one-time clock calibration

Two ingredients turn 12 ms of radio jitter into ~4 µs of sync error:

**1. Clock-offset calibration (once, after connect)**

```
host: t_tx ──PING──▶ slave
      t_rx ◀──PONG, t_slave
      RTT    = t_rx − t_tx
      offset = t_slave − (t_tx + RTT/2)
```

The host sends 12 samples and **keeps the one with the smallest RTT** — at that point upstream and downstream delays are both ≈0, so the symmetric-path assumption holds. Measured best RTT: **~20 ms**.

**2. Timed trigger with an absolute instant**

```
host fires at:  T_host = t_rx + lead            (its own esp_timer)
slave fires at: T_slave = T_host + offset       (converted into the slave's clock)
```

Each board counts down **locally** on its own `esp_timer` and logs its actual fire instant.
BLE latency is absorbed by `lead` (default 500 ms) and therefore drops out of the error budget entirely.

**Accuracy now depends only on two µs-grade local timers** — which is why the answer is ~4 µs rather than ~12 ms.

## Measured results

Acceptance run: 3 runs × 20 shots, two independent sessions.

| Metric | Target | Measured |
|---|---|---|
| Auto connect after power-on | < 30 s | ✅ immediate (scan → connect → discovery → CCCD → calibration) |
| Link loss detection | < 10 s | ✅ **3 s** |
| Auto reconnect | < 30 s | ✅ **5.2 s** (full re-calibration follows) |
| Shots without timeout | 20/20 | ✅ **60/60** |
| **mean sync error** | **< 100 µs** | ✅ **2.9 – 4.2 µs** |
| **max sync error** | **< 150 µs** | ✅ **5.0 – 68 µs** |
| Per-board timer jitter | — | +16…+29 µs (host) / +19…+38 µs (slave) |

Both boards also upload their frames to Telegram after the burst — see [Deferred upload](#deferred-upload-photograph-first-then-transmit) below.

Raw per-sample logs from the runs behind the table above are committed under [`docs/data/`](docs/data/) — `dualport_*.csv` is the two-port session, `host_only_*.csv` the single-port (USB) session where the slave reports over BLE.

## How sync_error is defined

```
Δ          = host_fire − slave_fire          ← contains the fixed offset between the two power-on instants
sync_error = |Δ − median(Δ)|                 ← the fixed offset removed; this fluctuation IS the sync error
```

`Δ` alone is meaningless (the two `esp_timer`s start at different moments, so `Δ` is a large constant). Subtracting `median(Δ)` — the fixed clock difference — leaves the physical sync error. **Only this column is used for the verdict**; per-board `jitter` is single-board timer precision and says nothing about the two boards agreeing.

The web bench computes this live and judges **per sample** against a configurable `Limit (µs)`.

## Reproduce

```bash
# 1) credentials (WiFi / Telegram) — never committed
cp main/app_conf.local.h.example main/app_conf.local.h   # then fill it in
#    leave it empty/absent => no WiFi, sync capture still works fully

# 2) build both roles (role is selected by Kconfig, one source tree)
idf.py -B build_slave -p <SLAVE_PORT> flash
idf.py -B build_host  -p <HOST_PORT>  flash

# 3) either: web acceptance bench
start.bat            # opens tools/acceptance.html (Web Serial, host port only)

#     or: command-line acceptance
python tools/sync_cam_test.py <HOST_PORT> <SLAVE_PORT> --runs 3 --rounds 20
python tools/host_only_test.py <HOST_PORT> --runs 3 --rounds 20   # slave data relayed over BLE
```

The host is the only USB connection required for acceptance: the slave's fire instants are relayed back over BLE and printed on the host's own serial line with a `SLAVE,` prefix.

## Protocol

**PC → host (UART)** `FIRE,<lead_ms>` · `SNAP` · `PING`
**host → slave (BLE GATT)** `ARM,<absolute_instant_us>`
**host → PC (UART)** `HOST,<event>,<t_us>[,<extra>]`
**slave → host (BLE notify)** `E,<event>,...` → re-printed as `SLAVE,<event>,...`

| Event | Meaning |
|---|---|
| `HB,<t>,conn=,bound=,calib=` | 2 s heartbeat, used as the connection-state predicate |
| `CALIB,<offset_us>,<best_rtt_us>` | clock offset and the RTT of the winning sample |
| `RX_CMD` / `BLE_RX` | command received (host) / BLE instruction received (slave) |
| `FIRE_SCHEDULED` / `FIRE_ACTUAL` | scheduled instant / actual fire instant + jitter |
| `DONE` | frame captured and queued |

## Deferred upload: photograph first, then transmit

Photo upload (Telegram) runs as a **low-priority background task** so it can never disturb the sync path:

* Capturing only **enqueues** to a 40-frame PSRAM queue — no radio activity.
* Upload starts only after **5 s of silence** (no new capture), and the whole batch is then transmitted in one session.
* A new capture **aborts** an in-flight upload (chunked writes, checked every 2 KB) and puts that frame back at the tail of the queue.
* If WiFi is not associated yet, the frame is **requeued rather than dropped**.

Result: during a 20-shot burst the radio belongs entirely to BLE + the camera; the upload happens afterwards, when nothing is timing-critical.

## Project layout

```
main/
  app_conf.h / app_conf.local.h(.example)   config; credentials live in the ignored local file
  ble_host.c                                Central: scan, auto-connect, service discovery, calibration, timed fire
  ble_slave.c                               Peripheral: advertise, GATT server, timed fire, event relay
  camera_hal.c / sd_hal.c                   OV3660 + SDMMC (1-bit) HALs
  upload_telegram.c / wifi_hal.c            deferred upload queue + minimal WiFi STA
  uart_cmd.c / logger.c                     command parser, CSV logger
tools/
  acceptance.html                           browser acceptance bench (Web Serial)
  sync_cam_test.py / host_only_test.py      CLI acceptance (dual-port / host-only)
  burst_test.py / tg_dual_test.py           burst behaviour + upload-path diagnostics
  reconnect_test.py / verify_boot.py        link-loss recovery, boot health check
  check_page.py                             static checks for the web page
docs/
  DEBUG_LOG.md                              engineering log: the bugs, the dead ends, the root causes
```

## Engineering notes

**Over twenty** non-obvious defects were found and fixed during bring-up — each documented in **[docs/DEBUG_LOG.md](docs/DEBUG_LOG.md)** with symptom, evidence, root cause and the generalisable lesson, plus an appendix of **falsified hypotheses** (so they are not re-investigated). Three highlights:

* **BLE scanning at 100 % duty cycle starved the WiFi.** `itvl == window == 10 ms` meant the radio never stopped scanning while looking for the peer; WiFi association collapsed (DHCP needed three attempts) and TLS handshakes timed out. Dropping to 20 % duty fixed it. A Peripheral never scans — which is exactly why one board worked and the other didn't.
* **Radio timing units.** BLE intervals are in **1.25 ms** units, not ms. `15 * 8` silently produced a 150 ms interval instead of 15 ms.
* **`esp_http_client_perform()` swallows the response body**, so a success check on `"ok":true` can never pass; and Telegram returns HTTP 200 even on error. Requires a manual `open → write → fetch_headers → read_response → close` sequence.

Also in the log: a hotspot whose DNS returned proxy fake-IPs (`198.18.0.0/15`), and a **hardware** conclusion reached only by swapping the two boards' roles — the firmware was never at fault.

## Comparison

| Platform | mean | max | Sync mechanism |
|---|---|---|---|
| nRF52840 (v3, previous work) | 14 µs | 30 µs | ACK-anchored |
| **ESP32-S3 (this project)** | **2.9 – 4.2 µs** | **5 – 68 µs** | absolute instant + clock-offset calibration |
| Product requirement (dermatoscope) | — | < 100 ms | — |

## License

MIT — see [LICENSE](LICENSE).

---

<sub>中文文档见 [README.zh-CN.md](README.zh-CN.md)（含更详细的接线、参数与逐条踩坑记录）。</sub>
