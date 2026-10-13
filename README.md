# 5V Power and Environment Monitor

[English](README.md) | [中文](README_zh.md)

Standalone ESP32-C3 firmware built with ESP-IDF. It monitors voltage, current, power, ambient temperature/humidity and air quality; controls two NMOS outputs using presence radar and a button; and displays readings on a 128×64 OLED. Environmental HTTP API and OTA updates run on the device without a host backend.

This README describes the current `5v` branch. The build project name remains `esp32-power-monitor`, producing `esp32-power-monitor.bin`.

## Hardware

### GPIO

| Pin | Connection | Behavior |
|---|---|---|
| IO0 | HLK-LD2410C-P OUT | Internal pull-down; HIGH occupied, LOW empty |
| IO3 | INA226 ALERT | Internal pull-up; active LOW, local overcurrent log |
| IO4 / IO5 | I²C SDA / SCL | Shared sensor/display bus |
| IO6 | NMOS CTL2 | HIGH on, LOW off |
| IO7 | NMOS CTL1 | HIGH on, LOW off |
| IO8 | Status LED | Active HIGH; off in ECO, on with WiFi in PWR |
| IO9 | Button | Internal pull-up; connect button to GND, active LOW |
| IO18 / IO19 | Native USB Serial/JTAG | D− / D+, flashing and logs |
| UART0 | Reserved radar serial port | Not used by the system console; current radar logic uses OUT |

IO9 is also the BOOT strapping pin. Holding it during reset may enter download mode.

### I²C devices

| Device | 7-bit address | Purpose |
|---|---|---|
| INA226 | `0x4F` | Voltage/current/power and overcurrent alert |
| SSD1306 | `0x3C` | 128×64 OLED, 0° rotation |
| ENS160 | `0x53` | AQI, TVOC, eCO₂ and validity |
| AHT21 | `0x38` | On the ENS160 board, supplies its compensation readings |
| SHT40 | `0x44` | Ambient temperature/humidity for API and OLED |

SHT40 uses the datasheet conversion with **no fixed temperature offset or additional humidity correction**; RH is clamped to 0–100%. Both humidity sensors verify CRC. ENS160 eCO₂ is an equivalent estimate, not a direct CO₂ measurement.

## Startup and power modes

Power modes are **PWR (standard)** and **ECO (power saving)**, independent of RA/ON/BL output control.

Startup initializes local I²C/peripherals and always enters ECO mode. After OLED initialization, `Loading...` stays visible throughout startup and for **2 seconds after startup completes**. Live UI and automatic mode decisions then begin.

WiFi connection and NTP sync run in the background. INA226 bus voltage must reach 4.5V to enable the radio. With external power but no WiFi IP, the device stays in ECO mode while attempting connection.

| Condition | Mode | INA226 | Environment polling/compensation | WiFi | OLED |
|---|---|---|---|---|---|
| Startup and following 2 seconds | ECO | 1Hz | 0.1Hz | Radio off | Full brightness, Loading... |
| Voltage <4.5V | ECO | 1Hz | 0.1Hz | Radio off | Full brightness, environment |
| Voltage ≥4.5V, no WiFi IP | ECO | 1Hz | 0.1Hz | Background connection/retries | Full brightness, environment |
| Voltage ≥4.5V, WiFi has IP | PWR | 10Hz | 1Hz | On | Full brightness, selectable UI |

Modes can switch repeatedly. WiFi loss returns to ECO. Decisions run approximately every 100ms in PWR mode and every second in ECO, plus conversion/scheduling time. There is no voltage hysteresis. Failed INA226 reads retain the last successful voltage decision while still accounting for WiFi loss; before the first successful read the radio stays off.

Environmental measurements and ENS160 host reads/compensation follow the mode. ENS160 itself remains in STANDARD continuous gas sensing to avoid repeated warm-up. The task shortens its wait when returning to PWR mode.

NTP sync is not a condition for PWR mode. Before the first sync the environmental API Unix timestamp is `null`.

## Button, outputs and display

IO9 uses 50ms debounce and a 1-second long-press threshold.

| Action | Result |
|---|---|
| Short press, on release | Cycle control mode (PWR RA/ON/BL, ECO RA/ON) |
| Hold 1 second in PWR mode | Toggle power / environment UI once per hold |
| Long hold during ECO or Loading | No UI change and no short-press action |

Control modes are `RA`, `ON`, `BL`; PWR/ECO are separate power modes.
Short presses cycle `RA → ON → BL → RA` in PWR mode, `RA ↔ ON` in ECO.
BL is PWR-only. Every `PWR ↔ ECO` transition resets control mode to `RA`,
including previous ON or BL selections. Unchanged power mode does not reset it.

IO9 always-on overrides the power-mode restrictions:

| Control | Power mode | CTL1 | CTL2 |
|---|---|---|---|
| RA | ECO (including startup) | On if occupied, off if empty | Always off |
| RA | PWR | On if occupied, off if empty | On if occupied, off if empty |
| ON | Any | Always on | Always on |
| BL | PWR | On if occupied, off if empty | Always off |

This policy applies at GPIO initialization and updates every 50ms as power or control mode changes. Releasing a long hold does not toggle NMOS control.

Reboot defaults to radar control and power UI. Selections are not persisted. ECO always shows environment; returning to PWR restores the selected UI.

- **Power UI:** large power above voltage/current, with power/control modes (e.g. `PWR | RA`) left and AQI right below.
- **Environment UI:** large temperature upper left, smaller CO₂ digits with `ppm` upper right; humidity left/TVOC (ppb) right on the next row; power/control modes (e.g. `PWR | RA`) left and ENS160 AQI right at the bottom.
- **Data quality:** `--` before data arrives, warm-up/startup indicators, and `STALE` on cached readings after read failures.

Both modes and Loading use full OLED brightness (contrast 255). The LED is always off in ECO mode.

## Configure, build and flash

ESP-IDF v6.0.1, target `esp32c3`, with a partition table for 4MB flash. Initialize the display submodule; no Python backend is required:

```sh
git submodule update --init --recursive components/u8g2
cp main/config.h.example main/config.h
```

Edit `main/config.h` for WiFi, hostname, OTA URL, GPIO and INA226 calibration. This local file is Git-ignored; commit only the template `main/config.h.example`.

| Setting | Default / purpose |
|---|---|
| `CFG_WIFI_SSID` / `CFG_WIFI_PASS` | Replace credential placeholders |
| `CFG_HOSTNAME` | `power-monitor`, DHCP hostname, no domain suffix, 1–32 characters |
| `CFG_NTP_HOST` | `ntp.aliyun.com` |
| `CFG_OTA_URL` | Full application `.bin` URL; configure for your deployment |
| `CFG_MAX_CURRENT_A` / `CFG_SHUNT_OHM` | 10A / 0.005Ω; match the actual shunt |
| `CFG_OCP_THRESHOLD_A` | 2.5A; logs alerts, does not automatically turn off NMOS |
| `CFG_EXTERNAL_POWER_MIN_V` | 4.5V |
| `CFG_OLED_CONTRAST_NORMAL` / `CFG_OLED_CONTRAST_BATTERY` | Both 255 |
| `CFG_ENV_INTERVAL_NORMAL_MS` / `CFG_ENV_INTERVAL_SAVING_MS` | 1000 / 10000ms |
| `CFG_BUTTON_DEBOUNCE_MS` / `CFG_BUTTON_LONG_PRESS_MS` | 50 / 1000ms |

Maximum current × shunt resistance must remain below approximately 81.9mV for INA226. The template lists sensor addresses and GPIO settings.

```sh
# This machine's IDF installation; adapt for another host.
source /home/fibre/.espressif/v6.0.1/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Use the actual enumerated port. Debugging uses **native USB Serial/JTAG**, not an external USB-UART bridge connected to UART0. Console defaults are in `sdkconfig.defaults`; local configuration is in `sdkconfig`. Existing local settings are not overwritten by defaults: verify USB Serial/JTAG as primary console and no secondary console.

Flash the complete build including bootloader after changing console or partitions. Application-only OTA cannot update old bootloader settings. Early ROM output is controlled by GPIO/eFuse boot configuration, not the project's console settings.

If the build reports a mismatched Python environment, use a fresh build directory:

```sh
idf.py -B build-5v build
idf.py -B build-5v -p /dev/ttyACM0 flash monitor
```

Use `deactivate` to exit the Python venv; closing the terminal clears this session's IDF environment.

## HTTP API

The HTTP server listens on port **80** only in **PWR**. Entering **ECO** stops
the server and closes connections; returning to PWR starts it again. HTTP is
also disabled during startup and the following 2-second grace period. Examples use `power-monitor.lan`; the suffix depends on router DHCP hostname resolution. Use the device IP if necessary. The project does not provide mDNS `.local` resolution.

### GET /api/v1/sensor

Returns a cached snapshot without waiting for a sensor measurement.

```sh
curl http://power-monitor.lan/api/v1/sensor
```

Example successful response:

```json
{
  "status": "ok",
  "last_updated_unix_ms": 1791480000000,
  "reading": {
    "temperature_c": 25.4,
    "humidity_rh_pct": 43.2,
    "compensation_temperature_c": 27.1,
    "compensation_humidity_rh_pct": 40.5,
    "aqi": 1,
    "tvoc_ppb": 12,
    "eco2_ppm": 400,
    "validity": "normal",
    "new_data": true
  },
  "error": null
}
```

| Field | Meaning |
|---|---|
| `temperature_c` / `humidity_rh_pct` | SHT40 ambient °C / %RH |
| `compensation_temperature_c` / `compensation_humidity_rh_pct` | AHT21 readings supplied to ENS160 |
| `aqi` | ENS160 AQI level, normally 1–5 |
| `tvoc_ppb` / `eco2_ppm` | ENS160 TVOC / equivalent CO₂ |
| `validity` | `normal`, `warmup`, `startup` or `invalid` |
| `new_data` | New ENS160 gas data at the latest poll; not cleared by HTTP queries |
| `last_updated_unix_ms` | Latest completed combined poll; null before NTP sync |

Without new gas data, the snapshot retains previous gas values while updating temperature/humidity. Warm-up/startup return HTTP 200; inspect validity before using those readings.

Not-ready, read/CRC/device errors and invalid output return HTTP 503 with `status: "error"` and an error identifier. Read failures preserve the last successful snapshot/time and set `new_data: false`; before the first snapshot `reading: null`. The task retries automatically. Error identifiers include `not_ready`, `sht40_crc_error`, `sht40_read_failed`, `aht21_init_failed`, `aht21_crc_error`, `aht21_read_failed`, `ens160_init_failed`, `ens160_compensation_failed`, `ens160_read_failed`, `ens160_device_error`, `ens160_invalid_output`.

### POST /api/v1/control

Omit `mode` to cycle `RA ↔ BL`; from ON, the next mode is RA.
Explicit `?mode=ON` remains supported; IO9 cycling is unchanged.

```sh
curl -X POST http://power-monitor.lan/api/v1/control
```


Set control mode in PWR only. Optional, case-sensitive `mode` is `RA`, `ON` or
`BL`. No body is needed. Outputs apply on the next status update (about 50ms),
and the UI reflects the shared mode.

```sh
curl -X POST 'http://power-monitor.lan/api/v1/control?mode=RA'
curl -X POST 'http://power-monitor.lan/api/v1/control?mode=ON'
curl -X POST 'http://power-monitor.lan/api/v1/control?mode=BL'
```

Successful HTTP 200 response:

```json
{"status":"ok","power_mode":"PWR","control_mode":"BL"}
```

Invalid/empty mode returns HTTP 400 with `error: "invalid_control_mode"`.
If the device enters ECO/Loading while processing, HTTP 409 returns
`error: "pwr_required"`; HTTP itself is stopped in ECO. API and IO9 share the
same state; the latest setting wins and is not persisted. Every power-mode transition resets control mode to RA.

### POST /api/ota/update

Without parameters, download from `CFG_OTA_URL`:

```sh
curl -X POST http://power-monitor.lan/api/ota/update
```

Override the URL for this update only:

```sh
curl -X POST -G \
  --data-urlencode 'url=http://192.168.1.100:8000/firmware.bin' \
  'http://power-monitor.lan/api/ota/update'
```

HTTP/HTTPS URLs are supported, with at most 255 decoded bytes. URL-encode the parameter. Uppercase `-X` selects the method; lowercase `-x` configures a proxy. Opening this update endpoint in a browser sends GET and returns 405.

| HTTP status | Response | Meaning |
|---|---|---|
| 202 | `{"status":"update_started"}` | Asynchronous update task started |
| 400 | `{"error":"invalid_ota_url"}` | Invalid, empty or oversized URL |
| 409 | `{"error":"update_in_progress"}` | Another update is running |
| 503 | `{"error":"update_start_failed"}` | Cannot start the update task |

202 acknowledges startup, not completion; inspect USB logs for the result. Serve the built application `.bin`, not a merged flash image, as a direct HTTP 200 response.

After writing and validating the inactive slot, OTA compares complete application-image **SHA-256** hashes. Changed images with identical version strings/dirty builds can update; identical images skip the slot switch/reboot, although download and writes still happen. Download, validation or hash failures retain the current firmware. Successful updates switch slots and reboot.

`partitions.csv` defines two 1700KiB OTA slots with application rollback enabled. A new application confirms after local initialization; WiFi/NTP availability is not required. Boot failures before confirmation can roll back.

## Source layout

| File | Responsibility |
|---|---|
| `main/app_main.c` | Local initialization, task startup and OTA confirmation |
| `main/core.{h,c}` | Shared readings, modes, interval and synchronization |
| `main/ina226.{h,c}` / `main/sample_task.{h,c}` | Power measurements, alerts and mode policy |
| `main/environment_sensor.{h,c}` | SHT40/AHT21/ENS160, compensation and cache |
| `main/status.{h,c}` | Radar, button, NMOS, LED and alert logs |
| `main/display.c` / `main/u8g2_esp_hal.c` | OLED UI and I²C transport |
| `main/wifi_ntp.{h,c}` | Background WiFi and NTP |
| `main/http_api.c` / `main/ota_update.c` | HTTP endpoints and OTA |
| `main/config.h.example` | Hardware/network template |
| `sdkconfig.defaults` / `partitions.csv` | USB console, system configuration and OTA slots |

`components/u8g2` is the display library submodule. The retained `python/` submodule is legacy host tooling and is not required to build or run this project.

## License

[MIT](LICENSE)

Both footers show only power/control modes and AQI in the 12px font.
Sensor warm-up/startup/normal states remain available through the API.

Both lower rows in the power/environment UI use the same fixed ProFont 12 font, without automatic shrinking.

During ENS160 `startup` or `warmup`, the environment UI strikes through the
CO₂ digits only. The `ppm` label stays unchanged; normal operation removes the line.

A separate background task controls HTTP start/stop without blocking power
sampling or mode decisions. ECO may attempt WiFi connection when externally
powered, but does not serve sensor/OTA HTTP endpoints.
