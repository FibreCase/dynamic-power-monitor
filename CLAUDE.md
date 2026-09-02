# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

12V 供电动态采样监测系统 — a 12V power dynamic-sampling/monitoring system with two independent components:

1. **ESP32-C3 firmware** (repo root `./`) — reads an INA226 power sensor, renders it on a 128×32 SSD1306 OLED, and streams samples over a long-lived TCP connection to the Python host. **Implemented in ESP-IDF (C), target `esp32c3`.**
2. **Python backend** (`./python` — a separate git submodule → `dynamic-power-monitor-backend.git`) — async TCP ingest server, FastAPI WebSocket broadcast + HTTP history API, SQLite persistence.

**The authoritative specification is `TASK.md`.** Read it before changing anything about the protocol, hardware wiring, or architecture — the details below are a distilled summary of it, not a substitute.

> **Status:** both components are implemented and build/test clean. Firmware under ESP-IDF v6.0.1 (see Build & run); the Python backend is complete and has a passing end-to-end test (`python/tests/e2e.py`).

## Build & run commands

### Firmware (ESP-IDF, target `esp32c3`)
The local IDF is at `/home/fibre/.espressif/v6.0.1/esp-idf`. Export it first, then:
- Build: `source $IDF_PATH/export.sh && idf.py build`
- Set target (once, already done): `idf.py set-target esp32c3`
- Flash + monitor: `idf.py -p PORT flash monitor`
- Menuconfig (pins, WiFi, etc. live in `main/config.h`, not sdkconfig): `idf.py menuconfig`
- Clean: `idf.py fullclean`

The build uses `-Werror`, so any warning in `main/` fails the build — keep it warning-free.

### Python backend (`./python`, managed with `uv`)
- Install/sync deps: `uv sync`
- Run the server: `uv run uvicorn power_monitor.app:app --host 0.0.0.0 --port 8000` (or `uv run power-monitor`).
- End-to-end test (fake ESP32 over TCP + WS viewer, no hardware): `cd python && uv run python -m tests.e2e`
- Config via env vars (`PM_*`, see `power_monitor/config.py`): `PM_TCP_PORT` (default 8888), `PM_DB_PATH`, `PM_INTERVAL_FAST_MS` (100), `PM_INTERVAL_SLOW_MS` (10000).

## Wire protocol (the cross-cutting contract — keep firmware, host, and web client in sync)

Any change to field order, sizing, or checksum must be mirrored across the ESP32 firmware (`main/app_main.c` `send_sample`/`process_downstream`), the Python `struct.pack`/`unpack` code, and (where relevant) the WebSocket JSON shape.

### Upstream sample — ESP32 → host, fixed **20 bytes**, little-endian
| Field | Size | Type | Value |
|---|---|---|---|
| Header | 2 | `u8[2]` | `0xAA 0x55` |
| Timestamp | 8 | `u64` | NTP-synced UTC Unix time in **milliseconds** |
| Voltage | 4 | `float32` IEEE 754 | bus voltage (V) |
| Current | 4 | `float32` IEEE 754 | sampled current (**mA**) |
| Checksum | 2 | `u16` | sum of the first 18 bytes `& 0xFFFF` |

### Downstream control — host → ESP32, fixed **8 bytes**, little-endian
Host packs it as `struct.pack('<BBBBHH', 0xBB, 0x66, 0x01, 0x02, interval_ms, checksum)`:
| Field | Size | Value |
|---|---|---|
| Header | 2 | `0xBB 0x66` |
| Cmd | 1 | `0x01` (set sampling interval) |
| Length | 1 | `0x02` (payload length) |
| IntervalMs | 2 | `u16` little-endian — `100` = 10 Hz, `10000` = 0.1 Hz |
| Checksum | 2 | sum of the first 6 bytes `& 0xFFFF` |

### WebSocket broadcast — host → web client, `ws://<host>:<port>/ws`
`{"sys_ts": <ms>, "dev_ts": <ms>, "voltage": <V>, "current": <mA>, "power": <mW>}`
Note `power` is **mW** (e.g. `12.045 V × 512.3 mA ≈ 6169.65`); current stays in mA.

### History HTTP API — `GET /api/v1/history`
Query params: `start_ts` (u64, optional), `end_ts` (u64, optional), `limit` (default 500, max 5000).
Response: a JSON array sorted by time **descending**.

## Architecture notes

### Firmware (ESP32-C3, ESP-IDF C, `main/`)
The firmware has no external display/sensor libraries — INA226 and SSD1306 are hand-written drivers, so the build is pure-IDF with no registry component:
- `app_main.c` — `app_main()`: NVS → I²C bus → OLED → INA226 → WiFi STA (block) → NTP (block) → main loop (TCP + sample + OLED).
- `ina226.{h,c}` — register-level driver. Calibrated at **10 A max** with the datasheet formula `Cal = 0.00512 / (current_LSB × R_shunt)`, `current_LSB = maxCurrent/32768`, power LSB = current_LSB × 25. **Rejects** any config where `maxCurrent × R_shunt > 81.9 mV`. `set_average()` flips the CONFIG averaging field between 64- and 512-sample windows.
- `ssd1306.{h,c}` — 128×32 driver over I²C (page addressing), column-major framebuffer, 5×7 text. Uses the "univision" init sequence (same remap/scan/com pins as U8g2's `U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C`).
- `fonts.c` — generated 5×7 font (chars 0x20–0x7E).

Key invariants (all in `app_main.c` unless noted):
- **Shared I²C bus**: INA226 (`0x40`) and SSD1306 (`0x3C`) on one bus; pins `CFG_I2C_SDA`=GPIO4 / `CFG_I2C_SCL`=GPIO5, internal pull-ups enabled.
- **OLED anti-stutter**: `render_oled()` is capped at once per **100 ms** (`OLED_REFRESH_MS`) so it never starves I²C/TCP at 10 Hz.
- **Clock**: SNTP via `esp_sntp_*` (server `ntp.aliyun.com`, UTC) and **blocks until synced** (`ntp_sync_block`) before sampling. `get_epoch_ms()` wraps `gettimeofday()`.
- **TCP**: blocking socket with `TCP_NODELAY`; a small state machine (`tcp_step`) handles non-blocking connect, `poll`, and auto-reconnect every `TCP_RETRY_MS`. `process_downstream()` reassembles sticky/partial packets, verifies the checksum, and applies `IntervalMs` (updates the sampling period **and** the OLED mode label).
- **Config lives in `main/config.h`** (created from `main/config.h.example`; the real one is git-ignored because it holds WiFi creds). Edit before flashing:
  - WiFi SSID/pass, host IP/port, NTP host.
  - I²C pins `CFG_I2C_SDA`=GPIO4 / `CFG_I2C_SCL`=GPIO5 (internal pull-ups enabled).
  - `CFG_LED_PIN`=GPIO8 (push-pull, active HIGH): no WiFi → slow blink (500 ms), idle (0.1 Hz) → steady on, 10 Hz → fast blink (100 ms).
  - `CFG_ALERT_PIN`=GPIO3 (INA226 ALERT, open-drain/active-low; polled as an input, pulled up; logged on transition and periodically if held).
  - `CFG_MAX_CURRENT_A` / `CFG_SHUNT_OHM` — the shunt sets every current & power reading (10 A needs shunt ≤ 8.2 mΩ).
  - GPIO18/19 are the built-in USB-Serial-JTAG — do **not** repurpose them.

### Python backend (`./python/power_monitor/`, uv-managed, Python ≥3.10)
- **`protocol.py`** — the single source of truth for the wire contract: 20-byte sample decode (`parse_sample`) and the 8-byte control pack (`build_control_set_interval`), plus checksum. Keep in sync with `main/app_main.c`.
- **`tcp.py`** — `IngestServer`: `asyncio.start_server` on port **8888** with `TCP_NODELAY`, a sliding byte buffer to absorb sticky/partial packets (resyncs past a bad header/checksum), and `send_set_interval()` over the same device connection.
- **`db.py`** — `Database`: SQLite with `PRAGMA journal_mode=WAL;` + `synchronous=NORMAL`. Table `power_logs(id, sys_ts, dev_ts, voltage, current, power)` indexed on `sys_ts`. Ingest is decoupled from disk by an `asyncio.Queue`; a background writer batch-commits at **50 rows or 2 s**, whichever comes first. All SQLite calls run in the thread pool under a lock.
- **`app.py`** — FastAPI. `GET /ws` keeps a global viewer set: **0 → 1 viewers** sends `IntervalMs=100` (10 Hz), **1 → 0** sends `IntervalMs=10000` (0.1 Hz). Every ingested sample is persisted and broadcast as `{"sys_ts","dev_ts","voltage","current","power"}` (power in **mW**). `GET /api/v1/history` (params `start_ts`/`end_ts`/`limit`) returns rows **descending** by `sys_ts`.
- **`config.py`** — all tunables (ports, DB path, intervals, batch size) as `PM_*` env vars.

