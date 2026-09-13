# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

12V 供电动态采样监测系统 — a 12V power dynamic-sampling/monitoring system with three independent components:

1. **ESP32-C3 firmware** (repo root `./`) — reads an INA226 power sensor, renders it on a 128×32 SSD1306 OLED, and streams samples over a long-lived TCP connection to the Python host. **Implemented in ESP-IDF (C), target `esp32c3`.**
2. **Python backend** (`./python` — a separate git submodule → `dynamic-power-monitor-backend.git`) — async TCP ingest server, FastAPI WebSocket broadcast + HTTP history API, SQLite persistence.
3. **Web dashboard** (`python/web/`) — a React + Vite single-page app (实时监控/Live + 历史查询/History tabs, ECharts charts) that talks to the backend's `/ws` and `/api/v1/history` endpoints; built to static assets and served same-origin by the FastAPI app.

**The authoritative specification is `TASK.md`.** Read it before changing anything about the protocol, hardware wiring, or architecture — the details below are a distilled summary of it, not a substitute.

> **Status:** all three components are implemented and build/test clean. Firmware under ESP-IDF v6.0.1 (see Build & run); the Python backend is complete and has a passing end-to-end test (`python/tests/e2e.py`); the web dashboard builds with Vite and is served statically by the backend.

## Build & run commands

### Firmware (ESP-IDF, target `esp32c3`)
The local IDF is at `/home/fibre/.espressif/v6.0.1/esp-idf`. Export it first, then:
- Build: `source $IDF_PATH/export.sh && idf.py build`
- Set target (once, already done): `idf.py set-target esp32c3`
- Flash + monitor: `idf.py -p PORT flash monitor`
- Menuconfig (pins, WiFi, etc. live in `main/config.h`, not sdkconfig): `idf.py menuconfig`
- Clean: `idf.py fullclean`

The build uses `-Werror`, so any warning in `main/` fails the build — keep it warning-free.

`sdkconfig.defaults` is the committed source of truth for build config (the live `sdkconfig` is git-ignored) and — unlike what you might expect — its values **override** an existing `sdkconfig` on the next `idf.py reconfigure`/`build`, so editing it is enough. It carries the partition-table and rollback choices plus `CONFIG_FREERTOS_HZ=1000`: at the 100 Hz default, `pdMS_TO_TICKS(5)` truncates to **0** and `vTaskDelay(0)` only yields instead of blocking, so a 5 ms poll loop spins and starves the lower-priority tasks. `main/sample_task.c` has a `_Static_assert` on `configTICK_RATE_HZ` that fails the build if the tick ever drops back below 200.

### Python backend (`./python`, managed with `uv`)
- Install/sync deps: `uv sync`
- Run the server: `uv run power-monitor`.
- End-to-end test (fake ESP32 over TCP + WS viewer, no hardware): `cd python && uv run python -m tests.e2e`
- Config via env vars (`PM_*`, see `power_monitor/config.py`): `PM_TCP_PORT` (default 38888), `PM_HTTP_HOST`/`PM_HTTP_PORT` (dashboard/API listen address, default `0.0.0.0:38000`), `PM_DB_PATH`, `PM_INTERVAL_FAST_MS` (100), `PM_INTERVAL_SLOW_MS` (10000), `PM_WEB_DIST` (default `python/web/dist`).

### Web dashboard (`python/web/`, Vite + React, plain JS)
- Install deps: `cd python/web && npm install`
- Dev server (hot reload; proxies `/api`, `/ws`, `/healthz` to the backend on `:38000`): `npm run dev` → http://localhost:5173. Requires the backend running separately (`cd python && uv run power-monitor`).
- Production build (consumed by the backend's static mount): `npm run build` → `python/web/dist/`. In production there's no separate dev server — FastAPI serves the built assets directly at `/`.

## Wire protocol (the cross-cutting contract — keep firmware, host, and web client in sync)

Any change to field order, sizing, or checksum must be mirrored across the ESP32 firmware (`main/net_task.c` `send_sample`/`process_downstream`), the Python `struct.pack`/`unpack` code, and (where relevant) the WebSocket JSON shape.

### Upstream sample — ESP32 → host, fixed **20 bytes**, little-endian
| Field | Size | Type | Value |
|---|---|---|---|
| Header | 2 | `u8[2]` | `0xAA 0x55` |
| Timestamp | 8 | `u64` | NTP-synced UTC Unix time in **milliseconds** |
| Voltage | 4 | `float32` IEEE 754 | bus voltage (V) |
| Current | 4 | `float32` IEEE 754 | sampled current (**mA**) |
| Checksum | 2 | `u16` | sum of the first 18 bytes `& 0xFFFF` |

### Upstream device-info — ESP32 → host, fixed **37 bytes**, little-endian
Sent **once per (re)connect**, before the sample stream, so the dashboard can show the
currently running firmware and its active slot. `main/net_task.c` `send_device_info()`.
| Field | Size | Type | Value |
|---|---|---|---|
| Header | 2 | `u8[2]` | `0xAA 0x53` |
| Version | 32 | `char[32]` | running app version (NUL-padded), from `esp_app_get_description()->version` |
| Slot | 1 | `u8` | running OTA slot: `1`=ota_0, `2`=ota_1, `0`=unknown (normalized from the partition subtype, whose raw values are `0x10`/`0x11`) |
| Checksum | 2 | `u16` | sum of the first 35 bytes `& 0xFFFF` |

The **version** is whatever the IDF build baked in via its auto git-describe path (no `version.txt`,
no `VERSION` in `project()`, and a `v1.0.0` tag exists → `esp_app_desc_t.version` =
`v1.0.0-<N>-g<hash>`; a `-dirty` suffix appears while the tree has uncommitted changes). No build
config is needed to make it distinct per build.

### Upstream OCP event — ESP32 → host, fixed **25 bytes**, little-endian
Sent when the INA226 **ALERT** pin asserts (shunt overcurrent, threshold `CFG_OCP_THRESHOLD_A`
in `config.h`). This is the *hardware* overcurrent path; the backend also detects overcurrent
host-side (see the Python section). `main/net_task.c` `send_event()`.
| Field | Size | Type | Value |
|---|---|---|---|
| Header | 2 | `u8[2]` | `0xAA 0x54` |
| Type | 1 | `u8` | `0x01` = shunt overcurrent |
| Timestamp | 8 | `u64` | NTP-synced UTC Unix time in **milliseconds** (edge time) |
| Voltage | 4 | `float32` IEEE 754 | bus voltage (V), fresh read at the edge |
| Current | 4 | `float32` IEEE 754 | sampled current (**mA**) |
| Power | 4 | `float32` IEEE 754 | power (**mW**) |
| Checksum | 2 | `u16` | sum of the first 23 bytes `& 0xFFFF` |

The firmware arms the comparator in `ina226_set_ocp_threshold()` (writes only `MAR`/`MCP`/`LAR` —
it never touches `REG_CAL`/`REG_CONFIG`, so the voltage/current/power reads are unaffected). The
comparator is shunt-only, non-latched (level) ALERT.

### Downstream control — host → ESP32, fixed **8 bytes**, little-endian
Same frame shape for both commands; the `Cmd` byte selects the action. Pack with
`struct.pack('<BBBBHH', 0xBB, 0x66, cmd, len, payload, checksum)`.
| Field | Size | Value |
|---|---|---|
| Header | 2 | `0xBB 0x66` |
| Cmd | 1 | `0x01` (set sampling interval) or `0x02` (start OTA) |
| Length | 1 | `0x02` (payload length) |
| Payload | 2 | `u16` LE — interval_ms for `cmd=0x01` (`100` = 10 Hz, `10000` = 0.1 Hz); unused (0) for `cmd=0x02` |
| Checksum | 2 | sum of the first 6 bytes `& 0xFFFF` |

- `cmd=0x01` — set the sampling interval; takes effect within ~5 ms.
- `cmd=0x02` — **start OTA**: the firmware spawns an update task that GETs the new `.bin` over HTTP (`http://CFG_OTA_HOST_IP:CFG_OTA_HOST_PORT<CFG_OTA_URL_PATH>`), writes it to the passive OTA slot, validates, and reboots into it (see the firmware `ota_update.c` notes below). The payload field is ignored for this command.

### WebSocket broadcast — host → web client, `ws://<host>:<port>/ws`
`{"sys_ts": <ms>, "dev_ts": <ms>, "voltage": <V>, "current": <mA>, "power": <mW>}`
Note `power` is **mW** (e.g. `12.045 V × 512.3 mA ≈ 6169.65`); current stays in mA.

### History HTTP API — `GET /api/v1/history`
Query params: `start_ts` (u64, optional), `end_ts` (u64, optional), `limit` (default 500, max 5000).
Response: a JSON array sorted by time **descending**.

## Architecture notes

### Firmware (ESP32-C3, ESP-IDF C, `main/`)
INA226 is a hand-written register-level driver (no vendor library exists for
it); the SSD1306 OLED is driven by the **u8g2** graphics library (vendored as
a git submodule at `components/u8g2`, pinned to release `2.37.1` — see
`.gitmodules`), not a hand-written driver. An earlier hand-rolled SSD1306
driver (`ssd1306.c/h`, `fonts.c`) was removed after repeated display
corruption (missing I2C control bytes, then an addressing-mode/command
mismatch); u8g2 owns the SSD1306 protocol now.
The firmware is split into one module per concern; `app_main.c` is only the
composition root. Tasks run at priorities display(4) > sample(3) > net(2) =
status(2) and share state through the mutex + length-1 sample queue in `core`.
- `app_main.c` — composition root: NVS → status GPIO → I²C bus → OLED (u8g2) → INA226 → WiFi STA (bounded wait) → NTP (bounded wait) → `esp_ota_mark_app_valid_cancel_rollback()` (confirm the running app is workable so OTA rollback is coherent) → start the four tasks.
- `core.{h,c}` — the **only** shared inter-task state: latest `struct reading`, sampling interval, TCP/WiFi status flags, the mutex + sample queue, and a small bounded **event queue** (`s_event_q`) carrying discrete `struct event`s (e.g. overcurrent). Owns `set_sampling_interval()` (period **and** INA226 hardware averaging), `event_publish()`, plus the `now_ms`/`get_epoch_ms`/`is_fast_mode` helpers.
- `net_task.c` — sole socket owner: non-blocking connect state machine (`TCP_RETRY_MS`), downstream control-frame parsing (set interval `cmd=0x01`, start OTA `cmd=0x02`), upstream sample send, a one-time **device-info** send (`send_device_info()`) each (re)connect, and an **OCP event** send (`send_event()`) drained from `s_event_q` each pass. `process_downstream()` reassembles sticky/partial packets and verifies the checksum.
- `sample_task.c` — reads the INA226 on a 5 ms cadence whenever `now - last >= s_interval_ms` (a host switch to 10 Hz takes effect within ~5 ms); publishes the reading and hands it to the net task.
- `display.c` — owns the `u8g2_t`; splash frames (boot) + live frame, redrawn every **100 ms** (`OLED_REFRESH_MS`) so it never starves I²C/TCP at 10 Hz.
- `status.c` — status LED (no WiFi → slow blink, idle → steady, 10 Hz → fast) + INA226 ALERT pin (open-drain, active-low, polled). On the ALERT rising edge it takes a fresh INA226 read and `event_publish()`es an overcurrent event for net_task to ship; also logs on transition and periodically if held.
- `wifi_ntp.c` — WiFi STA + NTP bring-up. Each boot step is bounded by a FreeRTOS **software timer + binary semaphore** (GOT_IP / the sync notification gives it on success; the timer at the deadline) — no busy-poll. On a WiFi failure it dumps a 2.4 GHz scan for diagnosis.
- `ota_update.c` — OTA client. On downstream `cmd=0x02` it spawns a task that GETs the new `.bin` over HTTP (host from `config.h`), writes it into the **passive** OTA slot (`esp_ota_begin`/`write`/`end`), validates the image, reboots into it. Any failure aborts and leaves the running firmware untouched.
- `ina226.{h,c}` — register-level driver. Calibrated at **10 A max** with the datasheet formula `Cal = 0.00512 / (current_LSB × R_shunt)`, `current_LSB = maxCurrent/32768`, power LSB = current_LSB × 25. **Rejects** any config where `maxCurrent × R_shunt > 81.9 mV`. `set_average()` flips the CONFIG averaging field between 64- and 512-sample windows.
- `u8g2_esp_hal.{h,c}` — the only OLED-specific code in this repo: bridges u8g2's byte/GPIO-delay callbacks onto the ESP-IDF `i2c_master_dev_handle_t` set up in `init_i2c()`, so it shares the bus. u8g2 is configured via `u8g2_Setup_ssd1306_i2c_128x32_univision_f()` (full frame-buffer variant — draw calls write an in-memory buffer, `u8g2_SendBuffer()` flushes it). No reset/CS/DC GPIOs are wired, so the GPIO callback only implements delays.
- `components/u8g2` — vendored u8g2 submodule; its own `CMakeLists.txt` already registers as an ESP-IDF component (globs `csrc/*.c`), so `main/CMakeLists.txt` just lists `u8g2` in `PRIV_REQUIRES`. Don't hand-edit files under here — update the submodule pin instead.

Key invariants:
- **OTA / partition table** — custom two-slot table in `partitions.csv` (no `factory`: `nvs` 0x9000, `otadata` 0xf000, `phy_init` 0x11000, `ota_0` 0x20000, `ota_1` 0x1d0000 — 1700K each). The fresh app lands in `ota_0` and the bootloader selects a slot from `otadata`. **App rollback is enabled** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` via `sdkconfig.defaults`) so a new image that fails to confirm auto-reverts to the previous one; `app_main` calls `esp_ota_mark_app_valid_cancel_rollback()` after a healthy boot to clear `PENDING_VERIFY`. The `.bin` must be < one slot (~1.65 MB; current build ~980 KB).
- **Shared I²C bus**: INA226 (`0x40`) and SSD1306 (`0x3C`) on one bus; pins `CFG_I2C_SDA`=GPIO4 / `CFG_I2C_SCL`=GPIO5, internal pull-ups enabled (the `i2c_master` driver serialises transactions across tasks).
- **Clock**: SNTP (default `ntp.aliyun.com`, UTC) with a bounded sync window before sampling. `get_epoch_ms()` wraps `gettimeofday()`; the sample timestamp is captured at read time, not send time.
- **Config lives in `main/config.h`** (created from `main/config.h.example`; the real one is git-ignored because it holds WiFi creds). Edit before flashing:
  - WiFi SSID/pass, host IP/port, NTP host.
  - I²C pins `CFG_I2C_SDA`=GPIO4 / `CFG_I2C_SCL`=GPIO5 (internal pull-ups enabled).
  - `CFG_LED_PIN`=GPIO8 (push-pull, active HIGH): no WiFi → slow blink (500 ms), idle (0.1 Hz) → steady on, 10 Hz → fast blink (100 ms).
  - `CFG_ALERT_PIN`=GPIO3 (INA226 ALERT, open-drain/active-low; polled as an input, pulled up; logged on transition and periodically if held).
  - `CFG_MAX_CURRENT_A` / `CFG_SHUNT_OHM` — the shunt sets every current & power reading (10 A needs shunt ≤ 8.2 mΩ).
  - `CFG_OTA_HOST_IP` / `CFG_OTA_HOST_PORT` / `CFG_OTA_URL_PATH` — the OTA download source `http://<ip>:<port><path>` (default: the backend host on port 38000, path `/ota/firmware.bin`).
  - GPIO18/19 are the built-in USB-Serial-JTAG — do **not** repurpose them.

### Python backend (`./python/power_monitor/`, uv-managed, Python ≥3.10)
- **`protocol.py`** — the single source of truth for the wire contract: 20-byte sample decode (`parse_sample`), 37-byte device-info decode (`parse_device_info`) + `build_device_info` (used by the e2e test), and the 8-byte control pack (`build_control_set_interval`, `build_control_start_ota`), plus checksum. Keep in sync with `main/net_task.c`.
- **`tcp.py`** — `IngestServer`: `asyncio.start_server` on port **38888** with `TCP_NODELAY` + `SO_KEEPALIVE` (to reap half-open sockets from a powered-off/dropped device), a sliding byte buffer that reassembles **both** upstream frame types — the 20-byte sample and the one-time 37-byte device-info — dispatching on the header byte pair and resyncing past a bad header/checksum. **Online status is liveness-based**: the `online` property (used by `/healthz`, `/ota/status`, and the OTA gate) is `connected AND (now - last_frame <= config.device_liveness_s)`; the raw socket flag is exposed as `connected`. The device's reported version/slot is stored via the `_on_device_info` callback. `send_control()` writes over the same device connection (thin wrappers `send_set_interval()` and `send_start_ota()`).
- **`db.py`** — `Database`: SQLite with `PRAGMA journal_mode=WAL;` + `synchronous=NORMAL`. Table `power_logs(id, sys_ts, dev_ts, voltage, current, power)` indexed on `sys_ts`. Ingest is decoupled from disk by an `asyncio.Queue`; a background writer batch-commits at **50 rows or 2 s**, whichever comes first. All SQLite calls run in the thread pool under a lock.
- **`app.py`** — FastAPI. `GET /ws` keeps a global viewer set: **0 → 1 viewers** sends `IntervalMs=100` (10 Hz), **1 → 0** sends `IntervalMs=10000` (0.1 Hz). Every ingested sample is broadcast as `{"sys_ts","dev_ts","voltage","current","power"}` (power in **mW**) at full rate, but **persistence is throttled**: `_on_sample` only calls `db.put()` once `sys_ts - _last_db_ts >= config.db_store_interval_ms` (default 10 s) has elapsed, regardless of the live sampling rate — a plain "has enough time passed" gate, no special-casing a 10 Hz↔0.1 Hz transition. `GET /api/v1/history` (params `start_ts`/`end_ts`/`limit`) returns rows **descending** by `sys_ts`. **OTA endpoints** (registered before the static mount so they take precedence): `POST /ota/upload` (multipart `.bin` → stored at `config.ota_dir/config.ota_bin_name`, overwriting; rejects empty/oversized), `GET /ota/firmware.bin` (serves the stored image — the exact path the firmware downloads from), `GET /ota/status` (`{available, size, mtime, device_online, firmware_version, ota_slot}`), `POST /ota/update` (sends `cmd=0x02` to the device; `409` if no image or device offline). The `firmware_version`/`ota_slot` are the device-reported running firmware + active slot (the one-time device-info frame), `None` until the device has connected; `device_online` is the liveness-based `tcp.online`. `App.__init__` also mounts `StaticFiles(directory=config.web_dist_dir, html=True)` at `/` as the **last** route registration (after `/ws`, `/api/v1/history`, `/healthz`, and the `/ota/*` routes), so it never shadows the API routes. The mount is checked with `.is_dir()` at construction time; if `python/web/dist/` hasn't been built yet, it's skipped with a `log.warning` instead of crashing — `App()` is instantiated at import time (the module-level singleton, and a second instance in `tests/e2e.py`), so this path must never raise.
- **`config.py`** — all tunables (ports, DB path, intervals, batch size, `web_dist_dir`, OTA dir `ota_dir`/`ota_bin_name`) as `PM_*` env vars.

### Web dashboard (`python/web/`, Vite + React, plain JS, ECharts)
- `src/App.jsx` — tab state (`live`/`history`/`ota`, no router — one page, three panels rendered conditionally so switching tabs actually unmounts the others: leaving `live` closes its WebSocket (opening `/ws` is what bumps the ESP32 to 10 Hz), and the OTA page starts/stops its poll on mount). A lifted `/healthz` poll (3s) drives the header's device-status pill, a **3-state** indicator (在线/离线/未知 — `health === null` from a backend fetch failure reads 未知, not offline).
- `src/components/OtaView.jsx` — the **固件更新（OTA）** page: two stat tiles for the device-reported **current firmware** (`firmware_version`) and **active OTA slot** (`ota_slot`, `1`→ota_0 / `2`→ota_1), then the `.bin` file picker (`POST /ota/upload`) and a 推送到设备 button (`POST /ota/update`, disabled unless an image is uploaded *and* the device is online, with a confirm dialog). Polls `GET /ota/status` (4 s) to show the stored image + upload time; shows success/error text for each action.
- `src/components/LiveView.jsx` — WS lifecycle with capped-backoff auto-reconnect; incoming samples buffer into a plain array ref (no per-message React re-render); a ~200ms tick prunes the buffer by **elapsed time** (not sample count — the device's rate varies 10 Hz active / 0.1 Hz idle) against a user-selected window, then pushes a partial series update straight into the ECharts instance via an imperative ref (`notMerge:false, lazyUpdate:true, animation:false`).
- `src/components/HistoryView.jsx` — `datetime-local` range + limit query against `/api/v1/history`; re-sorts the (descending) response ascending for charting, computes count/min/max/avg client-side, renders a chart + scrollable table.
- `src/chartOption.js` — shared ECharts option: three stacked single-axis grids (voltage/current/power), not a dual-axis chart (unrelated scales). Colors are the dataviz reference palette's categorical slots 1–3.
- `vite.config.js` — dev-server proxy for `/api`, `/ws` (`ws: true`), `/healthz`, and `/ota` to `127.0.0.1:38000`, so the app can use plain relative paths in both dev and prod and no CORS is ever needed. `build.outDir` is `dist`, matching `config.py`'s `web_dist_dir` default.

