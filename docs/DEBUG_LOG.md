# Engineering Log — ESP32-S3 Dual-Camera BLE Sync

Every non-obvious defect found while bringing this project up, in the order they were hit.
Each entry: **symptom → evidence → root cause → fix → lesson**.

The last one is the most instructive: after a dozen software hypotheses were individually falsified, the real cause turned out to be **hardware**. Getting there required a controlled experiment (swapping the two boards' roles), not more code reading.

---

## 1. `nimble` is not an ESP-IDF component name

**Symptom** — CMake configure fails:

```
Failed to resolve component 'nimble' required by component 'main': unknown name.
```

**Root cause** — On ESP-IDF 5.x the NimBLE host is shipped inside the `bt` component; `nimble` is not a resolvable component name.

**Fix** — `REQUIRES bt` in `main/CMakeLists.txt`.

**Lesson** — Component names in `REQUIRES` must be *component* names, not header namespaces. The error message even suggests `idf.py add-dependency`, which is wrong here and leads down a blind alley.

---

## 2. `ble_gattc_write()` does not exist — and neither does the header you expect

**Symptom** — compile error on a client write.

**Root cause** — NimBLE's client write API is `ble_gattc_write_no_rsp()` (and `ble_gattc_write_without_rsp`); there is no `ble_gattc_write(conn, handle, om, flags)` with a flags argument as the older GAP docs suggest.

**Fix** — `ble_gattc_write_no_rsp(s_conn_handle, s_cmd_handle, om)`.

**Lesson** — grep the actual bundled headers (`components/bt/host/nimble/**/include/host/ble_gatt.h`) rather than trusting an API shape remembered from ESP-IDF 4.x.

---

## 3. BLE intervals are in **1.25 ms** units, not milliseconds

**Symptom** — link felt sluggish; supervision timeout misbehaved.

**Root cause** — The connection-interval field is `units of 1.25 ms`. Writing `15 * 8 = 120` (an attempt to express "15 ms") actually requests **150 ms**.

**Fix**

```c
#define BLE_CONN_INTERVAL_MS   15
#define BLE_CONN_ITVL_UNITS    (BLE_CONN_INTERVAL_MS * 4 / 5)   /* 12 = 15 ms */
```

**Lesson** — Always write the unit conversion as an expression with the *destination* unit in the name. A bare magic number in a timing register is a future bug.

---

## 4. Weak symbols silently resolved to the empty implementation

**Symptom** — A cross-module optimisation (pause BLE scanning during upload) produced **no log output at all** on the board that mattered, while the other board logged its no-op branch.

**Root cause** — The pattern was:

```c
__attribute__((weak)) void ble_host_set_upload_mode(bool on) { (void)on; }   /* in upload_telegram.c */
void ble_host_set_upload_mode(bool on) { ...real... }                        /* in ble_host.c       */
```

Both files live in the same component (same static library). The linker picked the weak definition for the call site. The build succeeded, the symbol existed, `nm` looked fine — the optimisation was simply never executed.

**Fix** — Select at **compile time** instead of relying on link order:

```c
#if BOARD_ROLE_HOST
#include "ble_host.h"
#define UPLOAD_SET_MODE(on)   ble_host_set_upload_mode(on)
#else
#define UPLOAD_SET_MODE(on)   ((void)(on))
#endif
```

**Lesson** — Never rely on strong-over-weak resolution across translation units in the same library. And when a change produces *zero* expected log lines, suspect it never ran — verify with `objdump` that the call instruction is actually present before theorising further.

> ⚠️ Meta-lesson from this same episode: my first conclusion ("weak symbol") was itself wrong. The log line *was* being produced; **my own test script's keyword filter dropped it** (see §12). Verify the observation channel before trusting the absence of evidence.

---

## 5. `esp_http_client_perform()` discards the response body

**Symptom** — Upload appeared to succeed-by-status but the `"ok":true` check never matched; response string came back empty (0 bytes).

**Root cause** — `esp_http_client_perform()` runs two internal `get_data` loops at the end that consume and discard the body. A subsequent `esp_http_client_read_response()` returns 0.

**Fix** — Drive the client manually:

```c
esp_http_client_open(cli, content_length);
esp_http_client_write(cli, body, len);
esp_http_client_fetch_headers(cli);
int rd = esp_http_client_read_response(cli, resp, sizeof(resp) - 1);
esp_http_client_close(cli);
```

**Lesson** — For APIs that "do everything", read the implementation before depending on post-conditions.

---

## 6. Telegram returns HTTP 200 on failure

**Symptom** — Errors counted as successes.

**Root cause** — The Bot API signals application-level errors (`chat not found`, rate limit) with **HTTP 200** and `{"ok":false,...}` in the body. Checking only the status code is wrong.

**Fix** — `status == 200 && strstr(resp, "\"ok\":true") != NULL`.

**Lesson** — HTTP status is a transport signal, not an application result. Always parse the documented success field.

---

## 7. `esp_wifi_set_max_tx_power()` before `esp_wifi_start()` aborts the chip

**Symptom** — Boot loop (reset cycle) with no obvious application code in the backtrace.

**Root cause** — Called before `esp_wifi_start()`, it returns `ESP_ERR_WIFI_NOT_STARTED`; wrapping that in `ESP_ERROR_CHECK` calls `abort()`.

**Fix** — Call it after `esp_wifi_start()`, and do **not** wrap it in `ESP_ERROR_CHECK` (a power setting should not be fatal).

```c
esp_wifi_start();
e = esp_wifi_set_max_tx_power(APP_WIFI_MAX_TX_DBM * 4);   /* dBm * 4 ! */
if (e != ESP_OK) LOGW(TAG, "set_max_tx_power failed 0x%x", e);
```

**Lesson** — Order-sensitive driver APIs + `ESP_ERROR_CHECK` is a boot-loop generator. Reserve unconditional checks for initialisation the system genuinely cannot run without. (Also note the `dBm × 4` scaling.)

---

## 8. Reducing TX power was required to avoid brown-out

**Symptom** — Sporadic resets while bringing up the radio.

**Root cause** — Inrush current when the RF front-end powers up, on a board already carrying a camera. `BROWNOUT` reset.

**Fix** — `APP_WIFI_MAX_TX_DBM = 11` instead of 20.

**Lesson** — On ESP32-S3 boards with camera + radio, treat supply headroom as a real design constraint. If uploads are slow, don't reach for TX power first.

---

## 9. BLE scanning at 100 % duty cycle starved the WiFi

**Symptom** — The host could not associate reliably with the AP: `disconnected reason=2 / 201 / 205` every 5–7 s, DHCP took three attempts (`got ip` at 12.6 s), and Telegram uploads failed with `open 失败 0x7002` after a full ~20 s TCP timeout. The slave, on the same AP with the same credentials, uploaded in 3.3 s.

**Evidence**

```
# scan parameters as originally written:
params.itvl   = 0x0010;   /* 10 ms */
params.window = 0x0010;   /* 10 ms  -> window == interval == 100% duty */
```

Log: `reason=201` is `WIFI_REASON_NO_AP_FOUND` — the beacon was not being received *at all*. RSSI later measured a healthy **−43 dBm**, so this was never a signal-strength problem.

**Root cause** — Scanning with `window == interval` occupies the radio continuously. The ESP32-S3 has a single 2.4 GHz radio; in coexistence the WiFi RX window is squeezed to nothing. A Peripheral never scans, which is exactly why the two boards behaved differently with identical firmware.

**Fix**

```c
params.itvl   = 0x0050;   /* 50 ms */
params.window = 0x0010;   /* 10 ms -> 20% duty */
```

Plus: pause scanning entirely while an upload is in progress (upload happens during an idle window, when nothing is timing-critical).

**Lesson** — Scan duty cycle is a coexistence parameter, not a performance knob. Asymmetric behaviour between two identically-flashed boards is a strong hint to look at what is *role-specific* (Central scans; Peripheral does not).

---

## 10. `port.readable.pipeTo(TextDecoderStream)` makes the port unreleasable

**Symptom** — A Web Serial page could not release the serial port; `port.close()` hung or threw, and the browser kept holding the handle (subsequent flashing failed with `PermissionError(13)`).

**Root cause** — Piping `port.readable` into a stream keeps the readable **locked**. Everything downstream still "works", so this is invisible until you try to close.

**Fix** — Use a plain reader and decode manually:

```js
reader = myPort.readable.getReader();
...
rxBuf += dec.decode(value, { stream: true });
```

Plus the four standard Web Serial guards: detach references *before* `close()`, bind the read loop to the port instance it started for, detect `!port.readable` (device unplugged) and release, and hook `beforeunload`/`pagehide`.

**Lesson** — In Web Serial, "can I still read?" and "can I release this?" are independent properties. Test *releasing* explicitly; don't infer it from a working data path.

---

## 11. Dropping photos whenever WiFi was not ready

**Symptom** — Frames silently lost; the batch outcome depended on which side of a WiFi dropout the shot landed.

**Root cause**

```c
while (!wifi_hal_connected() && waited < 150) { ... }    /* 15 s */
if (!wifi_hal_connected()) {
    s_fail++;
    free(it.buf);          /* ← the photo is gone forever */
    continue;
}
```

**Fix** — Requeue instead of dropping, and wait longer (`TG_WIFI_WAIT_S`, default 60 s). The upload is a background task; it has no reason to be impatient.

**Lesson** — For a *background* consumer, the only defensible failure policy is "retry later". Dropping is acceptable only when the queue itself must be bounded — and even then, drop the oldest, never the newest.

---

## 12. My own test harness filtered out the evidence

**Symptom** — A new log line (`upload mode ON: conn itvl -> 100ms`) never appeared, which led me to the incorrect weak-symbol diagnosis in §4.

**Root cause** — The Python harness selected which lines to print by keyword:

```python
if any(k in t for k in ('TG:', 'FIRE_ACTUAL', 'scan', 'HB,')): print(t)
```

The host's line contained **none** of those keywords, so it was silently discarded. The slave's line happened to contain `scan` and therefore showed up — making it look like a role-specific difference.

**Fix** — Added the missing keywords, and switched the collector to "anything from a module under investigation always passes".

**Lesson** — A test harness that filters is an **observability component**: a missing keyword is a silent false negative, and it is indistinguishable from "the code did not run". When a log line is the evidence, verify the collection path first. `objdump` is unfiltered truth.

---

## 13. Waiting for the idle window once per *photo*, not once per *session*

**Symptom** — A 20-frame batch would have spent 100 s doing nothing: 5 s of idle-wait before *each* frame.

**Fix** — Enter an "upload session": wait for silence once, then stream the whole queue; exit the session only when a new capture arrives or the queue empties.

**Lesson** — Idle-based scheduling needs an explicit session notion, otherwise the gate is re-evaluated at the wrong granularity.

---

## 14. A hotspot whose DNS returns proxy fake-IPs

**Symptom** — Every upload from one board failed at connect. Adding diagnostics made it concrete:

```
HOST  | TG:   DNS ok: api.telegram.org -> 198.18.0.41
```

**Analysis** — `198.18.0.0/15` is the RFC 2544 benchmarking range — and the default `fake-ip` pool of Clash / v2ray-style proxies in TUN mode. The host machine had `v2cloud Tunnel` and an `Array Networks SSL VPN Adapter`. Windows Mobile Hotspot (ICS) relays the **host machine's** resolver to its clients, so the ESP32 received an address only the host's proxy understands.

Attempting to override it failed in an informative way:

```
> nslookup api.telegram.org 223.5.5.5
Server:  public1.alidns.com
Address: 223.5.5.5
Name:    api.telegram.org
Address:  198.18.0.41          ← still the fake IP
```

Even an explicit public resolver was redirected — TUN mode intercepts UDP 53.

**Fix (environmental)** — Disable TUN mode, allow the domain to bypass the proxy, or use a phone hotspot.

**Lesson** — This was a *red herring for the primary defect*, and worth recording as such: the other board resolved the **identical** IP and uploaded fine (fake-IP traffic does traverse the host proxy correctly). Proving DNS was **not** the blocker required measuring both boards, not just the failing one.

---

## 15. Root cause: it was the board, not the firmware

After every software hypothesis above had been individually falsified, one fact remained:

| Configuration | Board on port A | Board on port B |
|---|---|---|
| original | **HOST → failed** | SLAVE → 3.3 s |
| **roles swapped** | **SLAVE → 22.8 s / 11.9 s** | **HOST → 4.2 s / 3.8 s** |

**The defect followed the board, not the role.** That single experiment ended a long debugging session: the original board's WiFi front-end (or its USB cable / USB port / physical placement) was simply poor — `got ip` took **44.5 s** on it versus **2.7 s** on a replacement, with RSSI −43 dBm on both.

**Lesson** — When two identical units behave differently, **swap the units** before writing another hypothesis. It is one flash cycle and it partitions the problem space cleanly into "code" versus "hardware". I should have done it far earlier.

---

## 16. Firmware skeleton: defects from the first bring-up

Collected from the initial build-out, kept because each one cost real time.

### 16.1 The board was always the slave, no matter how it was built

**Symptom** — Rebuilding with the host configuration still produced a slave.

**Root cause** — `app_conf.h` decides the role from `CONFIG_BOARD_ROLE_HOST`, which comes from `sdkconfig.h`. ESP-IDF does **not** force-include `sdkconfig.h` into every translation unit, and in `main.c` the `#include "sdkconfig.h"` line sat *below* `#include "app_conf.h"`. The macro was therefore undefined, the `#else` branch always won, and the role silently defaulted to SLAVE.

**Fix** — Include `sdkconfig.h` explicitly at the top of `app_conf.h`, with a comment explaining why.

**Lesson** — Never depend on include order you do not control. If a header reads a build-system macro, it should include the header that defines it.

### 16.2 `-B <build dir>` does not relocate `sdkconfig`

**Symptom** — Flashed the host build directory; the board ran the slave firmware.

**Root cause** — `idf.py -B build_host` only moves the *output* directory. `sdkconfig` is always read from the project root, so the build was re-configured with whatever role was in the root file at that moment.

**Fix** — Keep one full configuration per role (`sdkconfig.host`, `sdkconfig.slave`) and copy the right one into `sdkconfig` immediately before flashing.

**Lesson** — For a single source tree serving two roles, keep the *role configuration* under version control as an explicit artefact. "It built fine" says nothing about which role was selected.

### 16.3 `nimble_port_freertos_init(NULL)` crashes at boot

**Symptom** — `Guru Meditation Error ... InstrFetchProhibited`, `PC=0x0` at startup.

**Root cause** — That argument is used **directly as the FreeRTOS task entry point**. Passing `NULL` means the host task jumps to address 0.

**Fix** — Pass a real `host_task` function.

**Lesson** — When an API takes a function pointer, `NULL` is almost never "use the default".

### 16.4 Host jitter measured 1–2 ms (60× worse than the timer)

**Symptom** — `jitter_host` around 1–2 ms while `esp_timer`'s own precision is ~20 µs.

**Root cause**

```c
esp_timer_start_once(s_fire_timer, lead_ms * 1000);   /* relative! */
```

The relative delay started **after** the BLE write and log calls — 1–2 ms of work had already elapsed since `t_rx` was recorded, and that time was added on top of `lead`.

**Fix** — Use the *remaining* time: `target − now_us()`.

**Lesson** — "Schedule relative to now" and "schedule at an absolute instant" differ by whatever happened in between. Compute the delta at the moment of arming.

### 16.5 Serial console goes permanently silent after ~30 commands

**Symptom** — The host stopped responding to UART after roughly 30 commands, with no error.

**Root cause** — `uart_cmd.c` tracked a `pos` offset and "consumed" the buffer with `memmove(buf, &buf[0], pos)` — which moves nothing. Once the buffer filled, `uart_read_bytes` had no space and returned 0 bytes forever, and the task sat silently in a loop.

**Fix** — Consume lines properly and `memmove` the *unconsumed remainder* to the front.

**Lesson** — A no-op `memmove` (source == destination) is a classic silent bug. Unit-test ring buffers with a fixed, oversized input.

### 16.6 Slave timestamps arrived truncated, with jitter always 0

**Symptom** — Relay arrived as `FIRE_ACTUAL,247611` instead of `FIRE_ACTUAL,247611xxx,21`, and jitter was always `0`.

**Root cause** — **Silent truncation at the ATT MTU.** With the default MTU of 23, a notification carries at most **20 bytes**, while `E,FIRE_ACTUAL,<10-digit µs>,<jitter>` needs 25. The tail was dropped with no error anywhere.

**Fix** — Both sides raise the MTU (`ble_att_set_preferred_mtu(247)`) and the host runs `ble_gattc_exchange_mtu()` right after connecting.

**Lesson** — Notifications are truncated, not rejected. If a payload is "almost" complete, check MTU before debugging the parser.

### 16.7 "The board never connects" — when it had connected minutes ago

**Symptom** — A page or script opened after the link was already up would wait forever for `BLE_CONNECTED`.

**Root cause** — That event is printed exactly once, at the moment of connection. An observer that starts later can never see it.

**Fix** — Add a 2 s heartbeat (`HOST,HB,<t>,conn=1,bound=1,calib=1`) and make *that* the readiness predicate. It is also printed from its own task so it cannot be delayed by the timing-critical path.

**Lesson** — One-shot events are not state. Design any observability channel so a late-joining observer can learn the current state.

---

## Appendix: falsified hypotheses

Recorded so they are not re-investigated:

| Hypothesis | Killed by |
|---|---|
| Out of memory during TLS handshake | `heap=6.6 MB / PSRAM=6.5 MB free` logged at the moment of failure |
| DNS hijacking breaks only the host board | Both boards resolved the **same** fake IP; the slave uploaded fine |
| BLE radio contention (central role) | Reducing scan duty to 20 %, pausing scan during upload, widening the connection interval to 100 ms — all failed; then role-swap proved it followed the board |
| Concurrent uploads to the same chat | Slave staggered 8 s clear of the host; host still failed |
| Same-chat rate limiting | Failure is at TCP connect (`0x7002`), before any Telegram-level response |
| `TG_TIMEOUT_MS` too small | Widened 15 s → 45 s; the failure surfaced at an internal ~20.5 s sub-timeout regardless |
| Weak-symbol linkage (§4) | Disassembly showed the call present and correctly parameterised |
