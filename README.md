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
│   ├── app_main.c         #   composition root: NVS → I2C → OLED → INA226 → WiFi → NTP → start tasks
│   ├── core.{h,c}         #   shared inter-task state (reading, interval, mutex + sample queue)
│   ├── net_task.{h,c}     #   TCP: connect state machine, control-frame parse, sample send
│   ├── sample_task.{h,c}  #   INA226 read on its own period + publish
│   ├── display.{h,c}      #   OLED (u8g2): splash + live frame
│   ├── status.{h,c}       #   status LED + INA226 ALERT pin
│   ├── wifi_ntp.{h,c}     #   WiFi STA + NTP bring-up
│   ├── ota_update.{h,c}   #   OTA client (downstream cmd=0x02 → download + reboot)
│   ├── ina226.{h,c}       #   INA226 driver (10 A calibration, 64/512 averaging)
│   ├── u8g2_esp_hal.{h,c} #   u8g2 <-> ESP-IDF I2C glue for the SSD1306 OLED
│   └── config.h           #   git-ignored; copy of config.h.example (holds WiFi creds)
├── components/u8g2/       # git submodule: u8g2 graphics library (pinned 2.37.1)
├── partitions.csv         # custom two-OTA-slot partition table (see OTA)
├── sdkconfig.defaults     # selects partitions.csv + enables app rollback
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

Before flashing, copy the config template and edit it for your board/network:

```bash
cp main/config.h.example main/config.h   # then edit main/config.h (git-ignored)
```

Set the `CFG_*` macros in `main/config.h` (WiFi SSID/password, host IP/port, NTP
server, I2C pins, OTA host, and the INA226 `CFG_MAX_CURRENT_A` / `CFG_SHUNT_OHM`).
The shunt resistance sets every current and power reading — for 10 A it must be
≤ 8.2 mΩ (default 5 mΩ).

The custom two-OTA-slot partition table (`partitions.csv`) is selected via
`sdkconfig.defaults`, so `idf.py flash` writes the app into the `ota_0` slot and
the bootloader picks a slot from the `otadata` record.

## OTA (firmware update)

The firmware updates itself over the air into the second flash slot and reboots
into it. It uses ESP-IDF's built-in dual-slot OTA with **app rollback**: if the
new image boots and confirms itself, it sticks; if it fails to confirm, the
bootloader automatically reverts to the previous image — so a bad update cannot
brick a deployed device.

**How it works** (see `main/ota_update.c`):
- A downstream control frame `cmd=0x02` triggers an update (below). The firmware
  spawns an OTA task that `GET`s the new image from
  `http://CFG_OTA_HOST_IP:CFG_OTA_HOST_PORT<CFG_OTA_URL_PATH>` (default
  `192.168.32.11:8000/ota/firmware.bin` — i.e. the backend host on its HTTP port).
- It checks the `Content-Length` fits the target slot and that the image version
  differs from the running one, then streams it into the **passive** slot with
  `esp_ota_write`, validates it with `esp_ota_end`, points the boot record at the
  new slot (`esp_ota_set_boot_partition`), and reboots.
- Any failure (no signal, slot full, validation error, incomplete download)
  aborts the update and leaves the current firmware running — the device stays
  online. The running app and its sample/display/net tasks keep running while the
  update task works; only the single-core C3 stalls briefly per flash block.

**Partition table** (`partitions.csv`, selected by `sdkconfig.defaults`):
no `factory` — `nvs` 0x9000, `otadata` 0xf000, `phy_init` 0x11000, then
`ota_0` 0x20000 and `ota_1` 0x1d0000 (1700K each). Rollback is enabled via
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A fresh app lands in `ota_0`;
`app_main` calls `esp_ota_mark_app_valid_cancel_rollback()` after a healthy boot
so the running image is confirmed valid (and the next OTA is allowed). Keep the
`.bin` well under one slot (~1.65 MB; the current build is ~980 KB).

**To publish an update:**
1. Build the new firmware: `idf.py build` → `build/esp32-power-monitor.bin`.
   (Bump the version so the firmware can tell it differs from the running one;
   a same-version image is rejected as a no-op.)
2. Serve that `.bin` at the configured path. The backend host is the default
   target, e.g. `python/` serving `/ota/firmware.bin` on port `8000`.
3. Send the downstream OTA command over the TCP connection — the 8-byte frame
   `struct.pack('<BBBBHH', 0xBB, 0x66, 0x02, 0x02, 0x00, checksum)` (payload is
   ignored for this command; `checksum = sum of the first 6 bytes & 0xFFFF`).

> **Status:** the firmware side is implemented and builds clean. The trigger
> today is the raw `cmd=0x02` frame; the backend endpoint that serves the `.bin`
> and the dashboard "update" button are not yet wired up (intended next step).

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
