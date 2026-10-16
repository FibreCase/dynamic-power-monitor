# Android 蓝牙控制

## 使用

1. 编译并刷入包含 BLE 的固件。BLE 广播名称为 `5V-<CFG_HOSTNAME>`（名称最多 26 字节），PWR、ECO 均可连接，最多一个手机连接。
2. 将 `web/ble.html` 上传到自己的 HTTPS 服务器，例如 `https://example.com/power/ble.html`。这是独立的单 HTML 文件，无外部依赖，无需服务器后端或 API 转发。
3. 在 Android Chrome 中打开页面，点击“连接蓝牙”，选择附近的 `5V-...` 设备并授权。不要通过聊天应用内置浏览器打开。手机需要开启蓝牙并授予系统要求的权限。
4. 连接后显示环境读数、当前电源/控制模式，滑动模式条后立即写入并读取实际状态。点击“断开连接”断开；断连后再次点击“连接蓝牙”。

服务器只负责提供页面，读数和指令直接经过手机蓝牙。ESP 无需连接服务器；ECO 下 WiFi 和 HTTP 仍关闭。HTTPS 页面也能在 PWR 使用 BLE。本地 `web/index.html` 仍为原有完整 WiFi 控制页面；蓝牙页面只提供环境显示和模式选择，没有 PWM、雷达配置、监视器或 OTA。

本次实现不含离线页面缓存，首次打开需要手机能够访问 HTTPS 服务器。若服务器配置了 Permissions-Policy，需允许当前页面使用 `bluetooth`；不要下发 `bluetooth=()`。当前页面使用内联脚本和样式，需要服务器 CSP 允许，或自行配置相应哈希。正常静态 HTML 服务即可使用，无需跨域访问设备 HTTP API。

当前 BLE 接口不要求设备配对或 PIN，浏览器的设备选择授权不等于设备端访问认证；附近支持本协议的客户端也可连接。最多一个连接，不构成身份验证。

## 功能规则

- 环境显示：SHT40 实际温湿度、AHT21 用于 ENS160 补偿的校准温湿度、ENS160 估算 CO₂（ppm）、TVOC（ppb）、AQI 等级。
- Startup/Warmup：CO₂ 数值加删除线，显示未稳定状态。读取/传感器错误时清空读数，避免将旧值当作当前数据；BLE 状态读取失败时暂停滑块，取得新状态后再恢复。
- PWR 可选 RA / BL / ON / OFF；ECO 可选 RA / ON / OFF，ECO 的 RA 为 CTL1 跟随雷达、CTL2 关闭。ECO 请求 BL 会被固件拒绝。
- ON 两路持续高电平，OFF 两路关闭。现有 PWM 设置和电源模式规则继续由同一控制模块执行。
- 启动阶段显示 Loading，并禁止模式写入。PWR/ECO 切换时仍由原有逻辑重置为 RA。
- IO9、WiFi API、BLE 共用模式状态；页面每秒读取一次，能跟随按钮或电源切换更新。GATT 读写串行执行，避免 Android 的并发操作错误。
- 页面隐藏时暂停状态轮询，不承诺锁屏或后台持续连接；页面恢复后继续读取，若系统断开需重新连接。
- BLE 只读取内存中的传感器快照，不触发额外 I2C 采样。采样保持 PWR 1 Hz、ECO 0.1 Hz。

## 固件配置和资源

`sdkconfig.defaults` 启用 NimBLE 外设、单连接、WiFi/BLE 软件共存，以及使用主晶振的蓝牙 modem sleep。广播间隔 PWR 为 1–1.2 秒，ECO 为 2–2.4 秒。连接后和电源切换时，向手机请求 PWR 50–100 ms、ECO 100–200 ms 的连接间隔，latency=3；手机可选择自己的连接参数。电源切换只向 NimBLE 主机排队更新策略，不主动断开连接。参数请求立即失败时最多尝试 3 次，避免长期重试唤醒。

这是蓝牙射频间歇休眠，不会自动把当前系统改成 CPU light/deep sleep。主机任务优先级为 2，回调只处理缓存快照和模式写入，不等待 I2C/UART、写 NVS 或启动 WiFi。启动 BLE 前已建立环境模块的缓存锁。

已有 `sdkconfig` 的项目需要确认这些选项生效，因为 defaults 不会覆盖已有的关闭值：

```text
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_CTRL_BLE_MAX_ACT=2
CONFIG_BT_NIMBLE_ROLE_CENTRAL=n
CONFIG_BT_NIMBLE_ROLE_OBSERVER=n
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y
```

不要把 `CONFIG_BT_ENABLED` 关闭后直接编译当前源码，BLE 模块需要 NimBLE 依赖。

## 协议 v1

| 项目 | UUID | 权限 |
| --- | --- | --- |
| 服务 | `5e7a0001-8e6d-4b28-a5c1-9f0260d84e31` | — |
| 状态 | `5e7a0002-8e6d-4b28-a5c1-9f0260d84e31` | READ |
| 模式 | `5e7a0003-8e6d-4b28-a5c1-9f0260d84e31` | WRITE with response |

状态为固定 28 字节，小端序：

| 偏移 | 类型 | 内容 |
| --- | --- | --- |
| 0 | u8 | 协议版本：1 |
| 1 | u8 | bit0 Loading、bit1 ECO、bit2 环境读数存在、bit3 环境错误 |
| 2 | u8 | RA=0、ON=1、BL=2、OFF=3 |
| 3 | u8 | ENS160 normal=0、warmup=1、startup=2、invalid=3 |
| 4 / 8 | float32 | 实际温度 °C / 湿度 %RH |
| 12 / 16 | float32 | 补偿温度 °C / 湿度 %RH |
| 20 / 22 | u16 | eCO₂ ppm / TVOC ppb |
| 24 | u8 | AQI 等级 |
| 25 | u8 | ENS160 new_data |
| 26–27 | u8[2] | 保留为 0 |

HTTP、BLE、IO9 均调用 `nmos_control_change`，在同一把锁下校验启动状态、计算模式并写入；HTTP 仅增加 PWR 访问门槛。状态接口从同一快照读取电源/控制模式。

模式写入严格接受一个字节，枚举同偏移 2；成功后读取状态确认实际模式。非法长度返回 ATT 0x0D、非法模式返回 0x13、Loading 或 ECO 请求 BL 返回应用错误 0x80。浏览器使用 readValue 获取整个特征值，无需自行分片。广播携带服务 UUID，名称放在扫描响应中。

## 验证

```sh
python3 -m unittest discover -s tests -v
node tests/test_ble_web.cjs
node tests/test_radar_web.cjs
idf.py build
```

主机测试覆盖状态编码、PWR/ECO/Loading 写入策略、原有 HTTP 限制、网页连接与环境读数、串行 GATT、模式变化和重连。仍需真实 Android 和 ESP32-C3 验证发现/连接、射频共存、电源切换时连接保持、功耗及堆/栈余量；模拟测试不替代上板验证。
