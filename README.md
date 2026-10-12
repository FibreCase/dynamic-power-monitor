# 12V Power Monitor (dynamic-power-monitor)

## Current `5v` firmware

Power modes automatically follow INA226 bus voltage:

| Mode | Voltage | Sampling | WiFi | OLED contrast |
|---|---|---|---|---|
| External | ≥4.5 V | 10 Hz | On, reconnects automatically | 255 |
| Battery saving | <4.5 V | 1 Hz | Radio off | 128 |

Power loss is detected within about 100 ms; restoration within about 1 second,
plus sensor conversion/scheduling time. There is no hysteresis. Failed I2C reads
retain the mode. Battery mode displays `BATTERY` and keeps checking voltage.
Boot retains normal mode and the original WiFi/NTP waits. Automatic voltage
monitoring starts 10 seconds after boot completes. WiFi restoration reconnects
asynchronously. The LED is steady on when connected, off otherwise. Contrast 128 is half
the normal register setting; perceived brightness may not be exactly halved.
HTTP/OTA are unavailable on battery and return after power and WiFi reconnect.
Configure the threshold and contrast via `CFG_EXTERNAL_POWER_MIN_V`,
`CFG_OLED_CONTRAST_NORMAL` and `CFG_OLED_CONTRAST_BATTERY` in `config.h`.

IO6 controls NMOS CTL2; IO7 controls NMOS CTL1. Both outputs are HIGH
from GPIO initialization and stay on in external and battery modes. Configure
the pins with `CFG_NMOS_CTL2_PIN` and `CFG_NMOS_CTL1_PIN`.

The current board uses a 128×64 SSD1306 I²C OLED (0x3C, 0° rotation).
Power appears above voltage/current, with WiFi and sampling status at the bottom.

The firmware now runs standalone: TCP host communication, remote sampling commands,
and sample/OCP uploads have been removed. Sampling, OLED, WiFi/NTP and local OCP
logs remain. The Python submodule is retained as legacy tooling; the dashboard
and protocol descriptions below refer to the previous firmware.

Configure the device DHCP hostname in `main/config.h`:

```c
#define CFG_HOSTNAME "power-monitor"
```

Use 1–32 letters, digits or hyphens, with a letter/digit at either end and no
suffix such as `.lan`. Rebuild and reboot to apply. If the router resolves DHCP
hostnames, use `http://power-monitor.lan/api/ota/update` (the suffix depends on
the router); otherwise use the device IP. This does not enable mDNS or `.local`.

Configure `CFG_OTA_URL` in `main/config.h` with the fixed firmware `.bin` URL.
The HTTP API listens on port 80. Trigger an asynchronous download with:

```sh
curl -X POST http://DEVICE_IP/api/ota/update
```

Override the download URL for this update only (without changing `CFG_OTA_URL`):

```sh
curl -X POST -G \
  --data-urlencode 'url=http://192.168.1.100:8000/firmware.bin' \
  'http://espressif.lan/api/ota/update'
```

`-X POST` selects POST, `-G` places parameters in the query string, and
`--data-urlencode` escapes special characters such as `?` and `&` in the firmware
URL. Lowercase `-x` specifies a proxy. Opening the endpoint in a browser sends
GET and returns 405.

The optional `url` supports HTTP/HTTPS and at most 255 decoded bytes. Omitting
it uses `CFG_OTA_URL`. Empty URLs, malformed encoding, unsupported schemes and
oversized parameters return 400.

| HTTP status | JSON response | Meaning |
|---|---|---|
| 202 | `{"status":"update_started"}` | Update task started |
| 400 | `{"error":"invalid_ota_url"}` | Invalid URL parameter |
| 409 | `{"error":"update_in_progress"}` | An update is already running |
| 503 | `{"error":"update_start_failed"}` | Update task could not start |

202 acknowledges startup only. Download results are logged locally.
Successful validation switches OTA slots and reboots; failures retain the current
firmware. After downloading, writing and validating the inactive slot, OTA
compares app-image SHA-256 hashes rather than version strings. Changed images
with identical versions (including dirty builds) can update; identical images
skip the boot-slot switch and reboot. Duplicate images are still downloaded
and written. Hash failures retain the current firmware; results are logged locally.

[English](README.md) | [中文](README_zh.md)

A real-time **12 V supply / power distribution monitor** built around an
ESP32-C3. The firmware reads an **INA226** current & power sensor, shows the
readings on a **128×32 SSD1306 OLED**, and streams samples over a long-lived
TCP link to a Python backend that persists them and serves them to a browser
dashboard. It supports **over-the-air firmware updates** (dual-slot with
rollback) and **overcurrent (OCP) alerting** with a web anomaly log.

> **Hardware:** [OSHWHub — 12V Power Monitor](https://oshwhub.com/fibrecase/project_mfgowtnz) — design files in 嘉立创 EDA (JLCEDA).

| Hardware | Firmware | Dashboard |
|---|---|---|
| ![ESP32-C3 12V monitor — assembled PCB](assets/hardware.png) | ESP-IDF v6.0.1 | ![12V power monitor dashboard — live view](assets/web.png) |

![JLCEDA PCB render](assets/hardware_pcb.png)

## Features

- **Live power telemetry** — bus voltage (V), current (A), and power (W) at up to **10 Hz** while watched, dropping to **0.1 Hz** idle to save power/traffic.
- **Chip temperature** — the ESP32-C3's internal (die) temperature rides along with every sample and is charted separately on the dashboard.
- **On-board display** — 128×32 SSD1306 OLED (u8g2) shows power / voltage / link status on the device itself.
- **Browser dashboard** — four tabs: **Live** (live chart), **History** (query by time range), **Alerts** (anomaly/OCP log), **Firmware** (OTA).
- **Bilingual UI** — English by default (中文 when the browser is Chinese), switchable from the header; the choice is remembered.
- **SQLite history** — every sample is persisted; query by time range.
- **OTA updates** — flash new firmware over the air into the second slot, with automatic rollback if the new image fails to boot.
- **Overcurrent alerting** — two independent detectors (the INA226 **ALERT** pin *and* a backend threshold) log the event time + voltage/current/power; browse them in **Alerts**.
- **Adaptive sampling** — the device bumps to 10 Hz only when a dashboard viewer is connected, and back to 0.1 Hz when the last viewer leaves.

## Architecture

Three independent components:

1. **Firmware** (repo root, `main/`) — ESP32-C3, ESP-IDF (C), target `esp32c3`. Reads the INA226 over I²C and the chip's internal temperature sensor, drives the OLED, and owns the TCP connection.
2. **Backend** (`python/`, a git **submodule**) — Python (`uv`), FastAPI + async TCP ingest + SQLite persistence + WebSocket/HTTP APIs.
3. **Dashboard** (`python/web/`) — React + Vite + ECharts single-page app, built to static assets and served by the backend itself (same-origin, no CORS).

```
┌─────────────┐  TCP 24-byte samples / 25-byte OCP events   ┌──────────────────────┐
│  ESP32-C3   │ ──────────────────────────────────────────▶│   Python backend     │
│  + INA226   │ ◀───────────────────────────────────────── │  (FastAPI, SQLite)   │
└─────────────┘   8-byte control frames (interval / OTA)    └──────────┬───────────┘
        │   OLED (local display)                                        │
        │                                                               │ WS (live) + HTTP (history/OTA/OCP)
        └──────────────────────────────────────────────▶  browser dashboard (React/Vite/ECharts)
```

See [`CLAUDE.md`](CLAUDE.md) for the full wire protocol, task breakdown, and invariants.

## Repository layout

```
.
├── main/                  # ESP32-C3 firmware (ESP-IDF component)
│   ├── app_main.c         #   composition root: NVS → I2C → OLED → INA226 → WiFi → NTP → start tasks
│   ├── core.{h,c}         #   shared inter-task state (reading, interval, event queue, mutex)
│   ├── net_task.{h,c}     #   TCP: connect state machine, control-frame parse, sample/event send
│   ├── sample_task.{h,c}  #   INA226 read on its own period + publish
│   ├── display.{h,c}      #   OLED (u8g2): splash + live frame
│   ├── status.{h,c}       #   status LED + INA226 ALERT pin (OCP edge → event)
│   ├── wifi_ntp.{h,c}     #   WiFi STA + NTP bring-up
│   ├── ota_update.{h,c}   #   OTA client (downstream cmd=0x02 → download + reboot)
│   ├── ina226.{h,c}       #   INA226 driver (calibration, OCP threshold, 64/512 averaging)
│   ├── u8g2_esp_hal.{h,c} #   u8g2 <-> ESP-IDF I2C glue for the SSD1306 OLED
│   └── config.h           #   git-ignored; copy of config.h.example (holds WiFi creds)
├── components/u8g2/       # git submodule: u8g2 graphics library (pinned 2.37.1)
├── assets/                # README images (hardware photo, PCB render, web screenshot)
├── partitions.csv         # custom two-OTA-slot partition table (see OTA)
├── sdkconfig.defaults     # selects partitions.csv + enables app rollback
├── CMakeLists.txt         # top-level ESP-IDF project
└── python/                # git submodule: Python backend (+ web/ dashboard)
```

## Hardware

The hardware is open-sourced on [OSHWHub](https://oshwhub.com/fibrecase/project_mfgowtnz)
(design files in JLCEDA / 嘉立创 EDA). The board is a small **12 V
power-distribution + monitor** node: a DC input feeds 4× 12 V terminal
blocks, and the INA226 measures the total current drawn by the connected loads.

Main parts:

- **ESP32-C3-02E** — MCU + WiFi (built-in USB-Serial-JTAG for console/flash).
- **INA226** — bidirectional current/power sensor on a low shunt; I²C address `0x40`.
- **128×32 SSD1306 OLED** — I²C (address `0x3C`), on the shared I²C bus.
- **DC input** (XT60-style) + **4× 2-pin terminal blocks** for 12 V loads.
- **Status LED**, **BOOT** / **RESET** buttons, **USB** port.

Firmware ↔ hardware mapping (all in `main/config.h`):

| Signal | GPIO | Notes |
|---|---|---|
| I²C SDA / SCL | GPIO4 / GPIO5 | shared bus: INA226 + OLED, internal pull-ups |
| Status LED | GPIO8 | push-pull, active-high (blink rate = WiFi / sampling mode) |
| INA226 ALERT | GPIO3 | open-drain, active-low (OCP comparator) |
| USB-Serial-JTAG | GPIO18 / GPIO19 | built-in — do not repurpose |

## Firmware

Requires ESP-IDF (this repo builds with **v6.0.1**). Export it, then:

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

Set the `CFG_*` macros in `main/config.h`: WiFi SSID/password, host IP/port, NTP
server, I2C pins, the INA226 `CFG_MAX_CURRENT_A` / `CFG_SHUNT_OHM` (the shunt
resistance sets every current & power reading), and `CFG_OCP_THRESHOLD_A`
(the overcurrent alert threshold, default **2.5 A**).

## OTA (firmware update)

The firmware updates itself over the air into the second flash slot and reboots
into it, using ESP-IDF's built-in **dual-slot OTA with app rollback**: if the new
image boots and confirms, it sticks; if it fails, the bootloader automatically
reverts to the previous image — a bad update can't brick a deployed device.

The custom partition table (`partitions.csv`, no `factory`): `nvs` 0x9000,
`otadata` 0xf000, `phy_init` 0x11000, then `ota_0` 0x20000 and `ota_1` 0x1d0000
(1700 K each). A fresh app lands in `ota_0`; `app_main` marks it valid after a
healthy boot. Keep the `.bin` well under one slot (~1.65 MB; the current build is
~980 KB).

**To publish an update** (dashboard-driven):
1. `idf.py build` → `build/esp32-power-monitor.bin`.
2. Dashboard → **Firmware** → choose the `.bin` (“Choose .bin firmware”) — it uploads to the backend (`POST /ota/upload`) and is served at `/ota/firmware.bin`.
3. Click **Push to device**. The backend sends the OTA command over TCP; the device downloads and reboots. The button is enabled only when a firmware is uploaded *and* the device is online.

## Overcurrent (OCP) alerting

Two independent detectors record overcurrent events to the backend, and both are
viewable in the dashboard's **Alerts** tab:

- **Device (hardware)** — the INA226's **ALERT** comparator asserts when the
  current exceeds `CFG_OCP_THRESHOLD_A` (default 2.5 A). On the rising edge the
  firmware takes a fresh INA226 read and pushes a 25-byte OCP event over TCP.
- **Backend (threshold)** — the backend independently flags a sample whose current
  exceeds `PM_OCP_THRESHOLD_MA` (default 2500 mA) on the rising edge.

Each event stores its **timestamp** plus the **voltage / current / power** at that
moment; the two paths are distinguished by a `source` field (`device` / `host`).

## Backend

```bash
cd python
uv sync
uv run power-monitor
```

- **TCP ingest** on port `38888` (the ESP32 connects here; point the firmware's
  `CFG_HOST_IP`/`CFG_HOST_PORT` at this host).
- **`ws://<host>:38000/ws`** — live sample broadcast. The first viewer switches the
  device to 10 Hz; the last viewer leaving drops it to 0.1 Hz.
- **`GET /api/v1/history?start_ts=&end_ts=&limit=`** — persisted samples, time-descending.
- **`GET /api/v1/alerts?start_ts=&end_ts=&limit=`** — OCP events, time-descending.
- **`GET /healthz`** — `{"status", "device": <bool>, "viewers": <int>}`.
- **`GET /`** — the dashboard, once built.

Configuration is via `PM_*` environment variables — see [`python/README.md`](python/README.md).

## Dashboard

A React + Vite + ECharts single-page app (`python/web/`) with four tabs —
**Live** (live chart), **History** (query by time range), **Alerts** (OCP log), and
**Firmware** (OTA). It's served by the backend itself, same-origin, so no
separate process or CORS is needed in production.

```bash
cd python/web
npm install
npm run build        # outputs python/web/dist/, served by FastAPI at "/"
```

During development, run it against a running backend with hot reload:

```bash
cd python/web
npm run dev          # http://localhost:5173, proxies /api, /ws, /healthz, /ota -> :38000
```

## End-to-end test

The backend ships a hardware-free end-to-end test (fake ESP32 over TCP + a
WebSocket viewer) that exercises reassembly, the live broadcast, the interval
control, history, OTA, and both OCP detection paths:

```bash
cd python
uv run python -m tests.e2e
```

---

*ESP32-C3 / INA226 / SSD1306 — [dynamic-power-monitor](https://github.com/FibreCase/dynamic-power-monitor).*
