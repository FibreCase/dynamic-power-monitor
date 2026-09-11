# 12V 供电动态采样监测系统

A 12 V power dynamic-sampling / monitoring system. An ESP32-C3 reads an INA226
power sensor, renders the readings on a 128×32 SSD1306 OLED, and streams
samples over a long-lived TCP connection to a Python backend that persists them
and serves them over WebSocket + HTTP.

The authoritative specification is [TASK.md]. This repo implements it in three
independent components:

- **Firmware** (repo root, `main/`) — ESP32-C3, ESP-IDF (C), target `esp32c3`.
- **Backend** (`python/`) — a git **submodule** → `FibreCase/dynamic-power-monitor-backend`;
  Python, managed with `uv`, FastAPI + async TCP + SQLite.
- **Dashboard** (`python/web/`) — React + Vite + ECharts, served by the backend itself.

See [CLAUDE.md] for the wire protocol and architecture notes.

## Repository layout

```
.
├── main/                  # ESP32-C3 firmware (ESP-IDF component)
│   ├── app_main.c         #   NVS → I2C → OLED → INA226 → WiFi → NTP → main loop
│   ├── ina226.{h,c}       #   INA226 driver (10 A calibration, 64/512 averaging)
│   └── u8g2_esp_hal.{h,c} #   u8g2 <-> ESP-IDF I2C glue for the SSD1306 OLED
├── components/u8g2/       # git submodule: u8g2 graphics library (pinned 2.37.1)
├── CMakeLists.txt         # top-level ESP-IDF project
├── python/                # git submodule: Python backend (+ web/ dashboard, see Dashboard section)
└── TASK.md                # specification (not committed)
```

## Firmware

Requires ESP-IDF (this repo builds with v6.0.1). Export it, then:

```bash
source $IDF_PATH/export.sh
idf.py set-target esp32c3     # once
idf.py build
idf.py -p PORT flash monitor
```

Before flashing, edit the `CFG_*` macros at the top of `main/app_main.c`
(WiFi SSID/password, host IP/port, NTP server, I2C pins, and the INA226
`CFG_MAX_CURRENT_A` / `CFG_SHUNT_OHM`). The shunt resistance sets every
current and power reading — for 10 A it must be ≤ 8.2 mΩ (default 5 mΩ).

## Backend

```bash
cd python
uv sync
uv run uvicorn power_monitor.app:app --host 0.0.0.0 --port 8000
```

- TCP ingest on port `8888` (the ESP32 connects here; set `CFG_HOST_IP`/`CFG_HOST_PORT`
  in the firmware to reach it).
- `ws://<host>:8000/ws` — live broadcast. The first viewer switches the ESP32
  to 10 Hz; the last viewer leaving drops it back to 0.1 Hz.
- `GET /api/v1/history?start_ts=&end_ts=&limit=` — persisted samples, time-descending.
- End-to-end test (no hardware): `uv run python -m tests/e2e.py`.

Configuration is via `PM_*` environment variables (see `python/power_monitor/config.py`).

## Dashboard

A React + Vite single-page app (`python/web/`) with two tabs — **实时监控**
(live, WebSocket-driven chart) and **历史查询** (history, queries
`/api/v1/history`) — built with ECharts. It's served by the backend itself,
same-origin, so no separate process or CORS is needed in production.

```bash
cd python/web
npm install
npm run build        # outputs python/web/dist/, served by FastAPI at "/"
```

During development, run it against a running backend with hot reload:

```bash
cd python/web
npm run dev           # http://localhost:5173, proxies /api, /ws, /healthz -> :8000
```
