# 5V 电源与环境监测控制器

[English](README.md) | [中文](README_zh.md)

基于 ESP32-C3 的独立设备固件，使用 ESP-IDF 开发。监测电压、电流、功率与环境温湿度、空气质量，通过雷达检测和按键控制两路 NMOS，并在 128×64 OLED 上显示状态。设备提供环境数据 HTTP API 和 OTA 固件更新，无需上位机后端。

本 README 描述 `5v` 分支的当前硬件与行为。构建名称仍为 `esp32-power-monitor`，输出固件为 `esp32-power-monitor.bin`。

## 硬件与接线

### GPIO

| 引脚 | 用途 | 行为 |
|---|---|---|
| IO0 | 雷达 OUT | 内部下拉；高电平有人，低电平无人 |
| IO3 | INA226 ALERT | 内部上拉；低电平过流告警，记录本地日志 |
| IO4 | I²C SDA | 与下列传感器及 OLED 共用总线 |
| IO5 | I²C SCL | 与下列传感器及 OLED 共用总线 |
| IO6 | NMOS CTL2 | 20kHz PWM，关闭 0%，开启占空比可调 |
| IO7 | NMOS CTL1 | 20kHz PWM，关闭 0%，开启占空比可调 |
| IO8 | 状态 LED | 高电平点亮；ECO模式熄灭，PWR模式连接 WiFi 时常亮 |
| IO9 | 按键 | 内部上拉，按键另一端接 GND，低电平按下 |
| IO18 / IO19 | 内置 USB Serial/JTAG | D− / D+，下载与日志调试 |
| UART0 | 雷达 AT 配置 | TX=IO21、RX=IO20，默认 115200 8N1；检测控制仍读取 OUT |

IO9 同时是 BOOT 启动配置引脚，按住复位可能进入下载模式。

### I²C 设备

| 设备 | 7 位地址 | 用途 |
|---|---|---|
| INA226 | `0x4F` | 电压、电流、功率及过流告警 |
| SSD1306 | `0x3C` | 128×64 OLED，0° 旋转 |
| ENS160 | `0x53` | AQI、TVOC、eCO₂ 与有效性状态 |
| AHT21 | `0x38` | 与 ENS160 同板，仅为 ENS160 提供温湿度补偿 |
| SHT40 | `0x44` | 环境温湿度，供 OLED 和 API 使用 |

SHT40 读数按数据手册公式换算，**不施加固定温度偏移或额外湿度修正**，相对湿度限制在 0–100%。AHT21 和 SHT40 均校验 CRC。eCO₂ 是 ENS160 的等效估算值，不是直接 CO₂ 测量。

## 启动与电源模式

电源模式统一命名为 **PWR（标准供电模式）** 和 **ECO（节能模式）**，与 RA/ON/BL 控制模式独立。

启动先初始化本地 I²C 和外设，固定进入ECO模式。OLED 初始化后显示 `Loading...`，覆盖启动过程及启动完成后的前 **2 秒**。随后进入实时界面并启用自动模式判断。

WiFi 连接和 NTP 同步均在后台进行，不阻塞启动。INA226 母线电压达到 `4.5V` 才允许开启 WiFi；外部供电但 WiFi 未取得 IP 时，仍保持ECO模式并尝试连接。

| 条件 | 模式 | INA226 采样 | 环境采样/补偿更新 | WiFi | OLED |
|---|---|---|---|---|---|
| 启动及启动完成后前 2 秒 | ECO | 1Hz | 0.1Hz | 无线电关闭 | 满亮度，Loading... |
| 电压 <4.5V | ECO | 1Hz | 0.1Hz | 无线电关闭 | 满亮度，环境界面 |
| 电压 ≥4.5V，WiFi 未取得 IP | ECO | 1Hz | 0.1Hz | 后台连接/重连 | 满亮度，环境界面 |
| 电压 ≥4.5V，WiFi 已取得 IP | PWR | 10Hz | 1Hz | 开启 | 满亮度，可切换界面 |

电源模式可反复切换，WiFi 断开会回到ECO模式。PWR模式约每 100ms 判断、ECO模式约每 1 秒判断，另加传感器转换和调度时间。阈值无滞回。INA226 读取失败时沿用最后成功的电压判断，但仍根据 WiFi 状态切换模式；首次读取失败时保持无线电关闭。

环境任务根据模式调整 SHT40/AHT21 测量、ENS160 读取和补偿更新频率；ENS160 内部保持 STANDARD 连续气体测量，避免反复预热。切回PWR模式时任务会缩短等待周期。

NTP 同步不是进入PWR模式的条件；首次同步前，环境 API 的 Unix 时间戳为 `null`。

## 按键、输出与显示

IO9 默认消抖时间为 50ms，长按阈值为 1 秒。

| 操作 | 功能 |
|---|---|
| 短按松开 | 切换控制模式（PWR RA/ON/BL/OFF，ECO RA/ON/OFF） |
| PWR模式长按 1 秒 | 切换功率界面 / 环境界面，每次长按只切换一次 |
| ECO或 Loading 阶段长按 | 不切换界面，也不触发 NMOS 短按操作 |

控制模式为 `RA`、`ON`、`BL`、`OFF`；PWR/ECO属于独立的电源模式。
PWR模式短按顺序为 `RA → ON → BL → OFF → RA`，ECO模式只切换 `RA → ON → OFF → RA`。
`BL` 仅在 PWR 有效。每次 `PWR ↔ ECO` 切换，控制模式都重置为 `RA`，
包括之前处于 ON 或 BL 的情况；电源模式不变时不重置。

NMOS 输出规则如下；IO9 切到常开优先于电源模式限制：

| 控制模式 | 电源模式 | CTL1 | CTL2 |
|---|---|---|---|
| RA | ECO（含启动） | 有人导通，无人关闭 | 始终关闭 |
| RA | PWR | 有人导通，无人关闭 | 有人导通，无人关闭 |
| ON | 任意模式 | 始终导通 | 始终导通 |
| OFF | 任意模式 | 始终关闭 | 始终关闭 |
| BL | PWR | 有人导通，无人关闭 | 始终关闭 |

GPIO 初始化即应用上述规则，运行时每 50ms 更新，电源或按键模式变化自动生效。长按松开不会再触发短按。

重启默认雷达控制、功率界面。模式选择不持久化。ECO固定显示环境界面，恢复PWR模式时使用之前选择的界面。

- **功率界面**：上方大字号功率，下方第一行左侧电压/电流（如 `4.96V | 2.49A`）、右侧 TVOC，底部左侧电源/控制模式（如 `PWR | RA`），右侧 AQI。
- **环境界面**：上方左侧大字号温度，右侧较小 CO₂ 数字与 `ppm`；下方第一行左侧湿度、右侧 TVOC（ppb）；第二行左侧电源/控制模式（如 `PWR | RA`），右侧 ENS160 AQI 等级。
- **状态提示**：无数据时显示 `--`，预热/初始启动显示提示，读取失败时缓存读数标为 `STALE`。

两个模式及启动画面均满亮度（对比度 255），LED 在ECO模式始终熄灭。

## 配置、构建与刷写

使用 ESP-IDF v6.0.1，目标 `esp32c3`，当前分区表适配 4MB Flash。初始化显示库子模块：

```sh
git submodule update --init --recursive components/u8g2
cp main/config.h.example main/config.h
```

编辑 `main/config.h`，配置 WiFi、hostname、OTA URL、引脚和 INA226 校准参数。该文件被 Git 忽略；可提交的模板为 `main/config.h.example`。

| 配置 | 默认值/说明 |
|---|---|
| `CFG_WIFI_SSID` / `CFG_WIFI_PASS` | WiFi 凭据，必须修改模板占位值 |
| `CFG_HOSTNAME` | `power-monitor`，DHCP 主机名，无域名后缀，1–32 字符 |
| `CFG_NTP_HOST` | `ntp.aliyun.com` |
| `CFG_OTA_URL` | 完整固件 `.bin` 下载地址，按部署环境修改 |
| `CFG_MAX_CURRENT_A` / `CFG_SHUNT_OHM` | 10A / 0.005Ω，按实际分流电阻校准 |
| `CFG_OCP_THRESHOLD_A` | 2.5A，仅记录告警，不自动关闭 NMOS |
| `CFG_EXTERNAL_POWER_MIN_V` | 4.5V |
| `CFG_OLED_CONTRAST_NORMAL` / `CFG_OLED_CONTRAST_BATTERY` | 均为 255 |
| `CFG_ENV_INTERVAL_NORMAL_MS` / `CFG_ENV_INTERVAL_SAVING_MS` | 1000 / 10000ms |
| `CFG_BUTTON_DEBOUNCE_MS` / `CFG_BUTTON_LONG_PRESS_MS` | 50 / 1000ms |

INA226 要求最大电流 × 分流电阻不超过约 81.9mV。地址与其余 GPIO 定义见模板。

```sh
# 本机 IDF 安装路径；其他电脑按实际路径调整
source /home/fibre/.espressif/v6.0.1/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

端口以实际枚举结果为准。调试接口为 ESP32-C3 **内置 USB Serial/JTAG**，不是连接 UART0 的外置 USB 转串口芯片。控制台配置保存在 `sdkconfig.defaults`，当前本地配置为 `sdkconfig`。已有 sdkconfig 的设置不会自动被 defaults 覆盖；应确认主控制台选择 USB Serial/JTAG、关闭次控制台。

修改控制台或分区后应完整刷写，包括 bootloader；仅更新应用的 OTA 无法更改旧 bootloader 配置。复位最早期的 ROM 日志由芯片 GPIO/eFuse 配置控制，不由本项目控制台设置关闭。

若构建目录提示 Python 环境不一致，可使用新的构建目录：

```sh
idf.py -B build-5v build
idf.py -B build-5v -p /dev/ttyACM0 flash monitor
```

退出 Python venv 可执行 `deactivate`；关闭终端可清除本次 IDF 环境设置。

## HTTP API

HTTP 服务仅在 **PWR** 下监听 **80 端口**，进入 **ECO** 时停止服务并关闭连接；回到 PWR 自动重启服务。启动及前 2 秒也不启用 HTTP。下面示例使用 `power-monitor.local`，通过同一局域网内的 mDNS 解析，名称复用 `CFG_HOSTNAME`（配置值不带 `.local` 后缀），同时广播 80 端口的 `_http._tcp` 服务。mDNS 随 PWR HTTP 服务启动，ECO 下释放任务及定时器；初始化失败自动重试，不阻塞采样或影响 HTTP 启动。若客户端或网络不支持 mDNS，可使用设备 IP。

### GET /api/v1/sensor

返回环境缓存，不在请求中等待传感器测量。

```sh
curl http://power-monitor.local/api/v1/sensor
```

成功响应示例：

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

| 字段 | 含义 |
|---|---|
| `temperature_c` / `humidity_rh_pct` | SHT40 环境温度（°C）/相对湿度（%RH） |
| `compensation_temperature_c` / `compensation_humidity_rh_pct` | AHT21 温湿度，提供给 ENS160 进行补偿 |
| `aqi` | ENS160 AQI 等级，正常范围 1–5 |
| `tvoc_ppb` / `eco2_ppm` | ENS160 TVOC / 等效 CO₂ |
| `validity` | `normal`、`warmup`、`startup` 或 `invalid` |
| `new_data` | 最近后台采样时 ENS160 是否产生新数据，不因 HTTP 查询而清除 |
| `last_updated_unix_ms` | 最近完成的组合采样时间；NTP 未同步时为 null |

ENS160 没有新气体数据时，保留之前气体读数并更新温湿度。预热及初始启动返回 HTTP 200，但需结合 validity 判断是否适合使用。

未就绪、读取/CRC/设备错误或无效输出返回 HTTP 503、`status: "error"` 和错误标识。读取失败保留最后成功快照与时间戳并设置 `new_data: false`；首次无快照时 `reading: null`。任务自动重试。错误包括 `not_ready`、`sht40_crc_error`、`sht40_read_failed`、`aht21_init_failed`、`aht21_crc_error`、`aht21_read_failed`、`ens160_init_failed`、`ens160_compensation_failed`、`ens160_read_failed`、`ens160_device_error`、`ens160_invalid_output`。

### 网页控制 / GET /api/v1/control

PWR 下打开 `http://power-monitor.local/`，或使用设备 IP。页面提供四档滑块，
按 `RA / BL / ON / OFF` 排列，松开后设置对应模式。每 5 秒自动同步，
也可点击“重新同步”；IO9 操作同样反映在页面上。断连后禁用滑块，
恢复 PWR 后可重新同步。ECO 下网页和 API 均关闭。

`web/index.html` 为单文件页面，不依赖 CDN。构建时由 `web/compress.py`
生成 gzip，嵌入固件并通过根路径返回 `Content-Encoding: gzip`。
修改 HTML 后重新构建即可更新页面，无需额外上传文件。

```sh
curl http://power-monitor.local/api/v1/control
curl --compressed http://power-monitor.local/
```

GET 控制接口只查询、不切换：

```json
{"status":"ok","power_mode":"PWR","control_mode":"RA"}
```

### POST /api/v1/control

不带 `mode` 时仅轮换 `RA ↔ BL`，当前为 ON 或 OFF 时下一次切到 RA。
仍可通过 `?mode=ON` 显式选择 ON；IO9 的短按顺序保持不变。

```sh
curl -X POST http://power-monitor.local/api/v1/control
```


仅 PWR 下可设置控制模式，`mode` 可选且区分大小写，取值 `RA`、`ON`、`BL` 或 `OFF`。
无需请求体，输出在状态任务下一次更新时生效（约 50ms），屏幕同步显示。

```sh
curl -X POST 'http://power-monitor.local/api/v1/control?mode=RA'
curl -X POST 'http://power-monitor.local/api/v1/control?mode=ON'
curl -X POST 'http://power-monitor.local/api/v1/control?mode=BL'
```

成功返回 HTTP 200：

```json
{"status":"ok","power_mode":"PWR","control_mode":"BL"}
```

非法或空 mode 参数返回 HTTP 400，`error: "invalid_control_mode"`。
若处理请求时设备已进入 ECO/Loading，返回 HTTP 409，`error: "pwr_required"`；
ECO 下 HTTP 服务本身关闭。API 与 IO9 共享状态，最后一次设置生效，
不持久化；任何电源模式切换都会将控制模式重置为 RA。

### PWM 占空比 / GET、POST /api/v1/pwm

CTL1（IO7）和 CTL2（IO6）均使用 LEDC 20kHz、8 位 PWM。每路分别保存
一个 0–100% 占空比，PWR 的 RA / BL 共用，不按模式分别保存：

- PWR RA：两路在雷达允许开启时使用各自保存的占空比。
- PWR BL：CTL1 在有人时使用其保存占空比，CTL2 始终关闭。
- ON：两路固定 100%，不使用保存值。
- ECO：原有开关规则不变，允许开启的输出固定 100%，被关闭的输出为 0%。

网页增加两个占空比滑块。拖动只预览，点击“保存占空比”后应用并写入 NVS。
相同值不重复写 Flash，保存失败不更新运行值。重启恢复占空比；控制模式
仍默认 RA。NVS 尚无配置时使用 `CFG_CTL1_PWM_DUTY_PCT` /
`CFG_CTL2_PWM_DUTY_PCT` 默认值（均 100）。频率由 `CFG_NMOS_PWM_FREQ_HZ`
配置，两路共用定时器。100% 持续高电平，0% 持续低电平。

```sh
curl http://power-monitor.local/api/v1/pwm
curl -X POST 'http://power-monitor.local/api/v1/pwm?ctl1=35&ctl2=65'
```

```json
{"status":"ok","frequency_hz":20000,"ctl1_duty_pct":35,"ctl2_duty_pct":65}
```

POST 必须同时指定 ctl1、ctl2，使用 0–100 的整数；非法参数返回 400
`invalid_pwm_duty`，NVS 保存失败返回 500 `pwm_save_failed`，
非 PWR/启动阶段返回 409 `pwr_required`。GET 返回保存的配置值，
不代表被雷达/模式规则关闭的输出当前实际占空比。

### POST /api/ota/update

不带参数时，从 `CFG_OTA_URL` 下载应用固件：

```sh
curl -X POST http://power-monitor.local/api/ota/update
```

临时指定本次下载 URL，不修改默认配置：

```sh
curl -X POST -G \
  --data-urlencode 'url=http://192.168.1.100:8000/firmware.bin' \
  'http://power-monitor.local/api/ota/update'
```

`url` 支持 HTTP/HTTPS，解码后最多 255 字节，必须正确 URL 编码。`-X` 是请求方法，`-x` 是代理配置。浏览器地址栏发送 GET，该升级接口会返回 405。

| HTTP 状态 | 响应 | 含义 |
|---|---|---|
| 202 | `{"status":"update_started"}` | 异步升级任务已启动 |
| 400 | `{"error":"invalid_ota_url"}` | 无效、空或过长 URL 参数 |
| 409 | `{"error":"update_in_progress"}` | 已有升级正在运行 |
| 503 | `{"error":"update_start_failed"}` | 无法启动升级任务 |

202 不表示升级完成，结果通过 USB 日志查看。托管文件应为构建产生的应用 `.bin`，不是合并刷机镜像；服务器应直接返回 HTTP 200 固件内容。

OTA 写入备用槽并完整校验后，比较新旧应用镜像 **SHA-256**。同版本号/dirty 构建只要内容不同仍可更新；内容相同则不切槽、不重启，但仍会下载和写入备用槽。下载、校验或哈希计算失败时保留当前固件。成功则切槽并重启。

`partitions.csv` 包含两个各 1700KiB 的 OTA 槽，启用应用回滚。新应用完成本地初始化后确认有效，WiFi/NTP 成功不是确认条件；未确认前启动失败可回滚。

## 代码结构

| 文件 | 职责 |
|---|---|
| `main/app_main.c` | 外设初始化、启动任务、OTA 启动确认 |
| `main/core.{h,c}` | 共享读数、模式状态、采样间隔及同步 |
| `main/ina226.{h,c}` / `main/sample_task.{h,c}` | 电源采样、告警配置和模式判断 |
| `main/environment_sensor.{h,c}` | SHT40/AHT21/ENS160、补偿与缓存 |
| `main/status.{h,c}` | 雷达、按键消抖、NMOS、LED 与告警日志 |
| `main/display.c` / `main/u8g2_esp_hal.c` | OLED 界面与 I²C 显示驱动 |
| `main/wifi_ntp.{h,c}` | 后台 WiFi 与 NTP |
| `main/http_api.c` / `main/ota_update.c` | HTTP API 和 OTA |
| `main/config.h.example` | 硬件/网络配置模板 |
| `sdkconfig.defaults` / `partitions.csv` | USB 控制台、系统配置及 OTA 分区 |

`components/u8g2` 是显示库子模块。

## 许可证

[MIT](LICENSE)

两个界面底栏均只显示电源/控制模式和 AQI，使用 12px 字号；不显示预热/启动/正常状态，传感器详细状态通过 API 查看。

功率/环境界面下方两排固定使用相同的 ProFont 12 字体，不再按内容缩小。

ENS160 处于 `startup` 或 `warmup` 时，环境界面的 CO₂ 数字加删除线，
`ppm` 标签不变；恢复 `normal` 后删除线消失。

HTTP 启停由独立后台任务负责，不阻塞 INA226 采样或电源模式判断。
ECO 下仍允许按供电条件后台连接 WiFi，但不提供传感器/OTA HTTP 接口。

OFF 将两路输出固定为 0%，可通过 IO9、网页或 `?mode=OFF` 选择。
无参数 API 仍仅轮换 RA/BL，电源模式切换仍重置 RA，占空比设置保留。

功率界面电流显示两位小数。较长读数先去掉分隔符两侧空格，
TVOC 仍放不下时以整数 k ppb 简写，保留下方两排统一字号。

功率和环境界面均显示 `TVOC 12` 这类标签及数值，不显示 ppb；API 的 `tvoc_ppb` 字段及单位不变。

功率界面电压移到右上角：小字号 V 标签，下面 17px 电压数值；
上方左侧保留大功率，下方第一行左侧电流、右侧 TVOC，第二行模式/AQI 不变。

## 调度与性能检查

采样按周期等待，OLED 仅在画面变化时传输。WiFi/HTTP 与环境任务使用模式通知，
NVS 写入不持有 PWM 读锁；采样和输出控制优先于显示/OTA。告警日志使用最近
采样缓存。检查结论、回归测试及上板验收见 [性能检查记录](docs/performance-review.md)。

## 雷达 AT 参数配置（USRR235L）

按本地 `uart-at-debug/usrr235.py` 的协议实现。UART0 默认 ESP TX=IO21 → 雷达 RX、ESP RX=IO20 ← 雷达 TX，共地、3.3V TTL、115200 8N1、无流控。可在 `main/config.h` 中用 `CFG_RADAR_UART_TX_PIN`、`CFG_RADAR_UART_RX_PIN`、`CFG_RADAR_UART_BAUD` 覆盖。系统日志继续使用 USB Serial/JTAG。

PWR 下打开设备首页，在“雷达参数”中选择参数、输入数值，点击“写入并重启雷达”。显示的是待写入值，**不是当前参数读数**。支持 BAUD、GPIO、TAGOUT、HOLD、R1/R2/R3、MR1TH/MR2TH/MR3TH、R1TH/R2TH/R3TH、TRITH、ONTH、OUTTRI、FREQ、FTIME、STIME；范围与调试工具一致。GPIO 极性会影响现有 OUT 高电平有人逻辑，应按接线选择。

```sh
# 提交单参数配置；202 表示已接收，不代表已写入成功
curl -X POST 'http://power.local/api/v1/radar?parameter=HOLD&value=40'
# 查询参数列表、当前串口波特率及最近任务结果
curl 'http://power.local/api/v1/radar'
```

将地址替换为实际设备地址。GET 返回 `status`、`baud`、`parameters` 和 `job`；任务有 `id`、`busy`、`phase`（idle/working/done/error）、`parameter`、`value`、`written`、`restarted`、`error`。POST 参数必须是白名单中的大写名称与合法非负整数；非法值返回 400，已有任务或非 PWR 返回 409。刷新网页后也能继续查看最近任务。ECO 下 HTTP 关闭；已接收的串口任务继续完成。

独立低优先级任务发送七个空格和 CRLF，进入 STOP 并用 AT 验证；写入等待 OK/AT+OK，忽略命令回显及检测数据。距离参数先查询并验证 R1 ≤ R2 ≤ R3。最后发送 AT+RESET，并等待参数输出或检测数据。失败也尝试重启，界面分别显示写入确认和重启确认，超时不自动重复写入。配置期间雷达暂停检测，NMOS 仍按现有 OUT 信号和控制模式工作。

BAUD 使用序号 1..8，映射见网页；成功确认写入后 ESP 保存新波特率到 NVS，并在 RESET 发送完成后切换 UART 波特率。初次通信失败时尝试工具支持的八种速率恢复，最坏可能需要约一分钟。若模块立即改变波特率导致写入确认丢失，任务会报超时；下一次操作会重新探测，不能将超时视为未写入。NVS 保存失败会单独报错。雷达参数本身由模块保存。

当前协议流程已通过模拟串口测试和 ESP-IDF 编译；真实模块的 STOP、BAUD 生效时序和重启输出仍需上板验证。

```sh
python3 -m unittest discover -s tests -v
node tests/test_radar_web.cjs
```

### 查询全部雷达参数

网页点击“查询全部参数”，弹窗显示本次读取的当前值、查询失败项，以及仅支持设置的项目。支持查询 BAUD、GPIO、TAGOUT、HOLD、R1/R2/R3、MR1TH/MR2TH/MR3TH、R1TH/R2TH/R3TH、TRITH、ONTH。OUTTRI、FREQ、FTIME、STIME 在工具协议中没有查询指令，显示“仅支持设置”，不会将待写入值当成实际值。

`POST /api/v1/radar/query` 异步启动查询（202），随后轮询 `GET /api/v1/radar`。`job.operation` 为 `query`，`job.resumed` 表示是否确认恢复检测；参数列表增加 `queryable`、`value`（未读或失败为 null）和 `query_status`（pending/ok/error/unsupported）。每次重新查询都清除上次结果。查询和写入共用串口任务，忙时返回 409。

查询进入 STOP 后逐项读取，个别失败继续读取其他项目，最后使用 AT+START 恢复检测，无需常规重启；START 失败时尝试 RESET 恢复。查询期间检测暂停；最坏等待时间取决于串口探测和各项超时。关闭弹窗不取消正在执行的任务。

### 雷达输出监视器

网页“输出监视器”打开实时输出弹窗，持续显示 UART0 收到的原始字节（保留大小写、空格和换行；其他不可显示字节显示为 `\xNN`）。支持切换自动滚动和清空窗口；关闭弹窗后停止网页轮询。打开监视器不发送任何指令，不改变雷达状态或 TAGOUT。TAGOUT=0 时可能没有检测输出；执行参数查询或写入时，也会看到模块回应。

`GET /api/v1/radar/output` 首次读取最近最多 512 字节；随后请求 `?after=<next>` 获取后续字节。返回 `hex`（原始字节十六进制）、`next`（字符串游标）、`dropped`（被覆盖字节数的字符串）、`reset`（设备重启等导致游标重置）、`more`。各浏览器使用独立游标，不会互相消费数据。固件保存最近 4096 字节，网页保留最近 32 KiB 文本；数据过快或断连太久会发生覆盖并提示，监视器不保证无损录制。接收与配置操作共用同一串口任务，不会争抢回应。ECO 时 HTTP 关闭，网页提示断连并自动重试。

### 网页环境读数

首页每秒读取 `/api/v1/sensor`，显示 SHT40 实际温湿度、AHT21 校准温湿度（供 ENS160 补偿）、ENS160 估算 CO₂、TVOC 和 AQI 等级。温湿度显示一位小数，空气质量显示整数。Startup/Warmup 时 CO₂ 数值加删除线并提示未稳定；数据获取失败时清空读数并自动重试。页面隐藏时暂停环境数据轮询。

### 固件版本

`GET /api/v1/version` 返回当前运行固件的 `status`、`version`、`idf_version`、`build_date`、`build_time` 和 `elf_sha256`。例如：`curl http://power.local/api/v1/version`，地址按实际设备替换。网页底部显示版本及 ELF SHA256 前 8 位，用于区分版本号相同的不同构建；完整构建信息可通过接口获取。网页每 30 秒刷新版本，点击“重新同步”也会刷新。与其他 HTTP API 一样仅 PWR 下可用。

## Android 蓝牙控制

新增独立页面 [`web/ble.html`](web/ble.html)，部署到 HTTPS 服务器后可用 Android Chrome 连接设备 BLE，在 PWR / ECO 下查看环境读数及切换控制模式。使用与部署、协议和验证说明见 [Android 蓝牙控制](docs/android-ble.md)。
