# 12V 供电动态采样监测系统

A 12 V power dynamic-sampling / monitoring system. An ESP32-C3 reads an INA226
power sensor, renders the readings on a 128×32 SSD1306 OLED, and streams
samples over a long-lived TCP connection to a Python backend that persists them
and serves them over WebSocket + HTTP.

The authoritative specification is [TASK.md]. This repo implements it in two
independent components:

- **Firmware** (repo root, `main/`) — ESP32-C3, ESP-IDF (C), target `esp32c3`.
- **Backend** (`python/`) — a git **submodule** → `FibreCase/dynamic-power-monitor-backend`;
  Python, managed with `uv`, FastAPI + async TCP + SQLite.

See [CLAUDE.md] for the wire protocol and architecture notes.

## Repository layout

```
.
├── main/                  # ESP32-C3 firmware (ESP-IDF component)
│   ├── app_main.c         #   NVS → I2C → OLED → INA226 → WiFi → NTP → main loop
│   ├── ina226.{h,c}       #   INA226 driver (10 A calibration, 64/512 averaging)
│   ├── ssd1306.{h,c}      #   SSD1306 128x32 driver (I2C, 5x7 text)
│   └── fonts.c            #   generated 5x7 font
├── CMakeLists.txt         # top-level ESP-IDF project
├── python/                # git submodule: Python backend
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
