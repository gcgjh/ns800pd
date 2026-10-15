# 多通道信号采集与监测终端

一块 240×320 触摸屏 + 一块联网模块，组成可脱离电脑独立运行的监测终端。
全部操作靠一个旋钮加一个按键完成，中文界面，开机即用。

- **NS800RT7P65**（Cortex-M7）负责显示与交互：RT-Thread 5.3.0 + LVGL 9.1.0，
  14 个功能页，旋钮选择、按键确认。
- **ESP32-C3** 负责联网与上报：WiFi 连接、网页管理页、OTA 升级、NTP 对时。
- 两块板之间用一根 UART 相连，屏上随时能看到联网状态，也能直接从屏上给 C3 下配置。

这样搭配是因为各取所长：NS800 有屏无网，C3 有网无屏。


## 功能一览

共 14 个页面，桌面 3×3 网格分两页摆放：

| 页面 | 说明 |
|---|---|
| 温度 | DHT22 与板载温度传感器读数 |
| 湿度 | DHT22 相对湿度 |
| 编码器 | EC11 旋钮的格数与方向，eQEP 正交解码 |
| 系统 | 运行时长、内存占用，以及 C3 的链路与联网状态 |
| 秒表 | 计时 |
| 蜂鸣器 | 短响 / 长响 / 连响三档测试音 |
| LED | LED1 四档模式（慢闪 / 快闪 / 常亮 / 关闭），LED2 心跳开关 |
| 背光 | 屏幕背光开关，状态掉电保存 |
| 图片 | 看图工具：原寸 / 铺满 / 放大三视图，数据源同桌面壁纸 |
| 关于 | 开发板、芯片、主频、屏幕、图形库、构建时间等信息 |
| 调色 | 实时调 gamma 曲线的冷暖 / 明暗 / 饱和度 |
| 游戏 | 横屏跑酷（猫跳石头），最高分持久化 |
| 配网 | 给 ESP32-C3 下发 WiFi 配置：连路由器 或 开热点 |
| 闹钟 | 最多 4 个，可调时间、到点响铃，掉电保存 |

桌面顶部状态栏显示时间、日期与联网状态；壁纸可更换，存在外置 Flash 里。


## 实物与界面

下面三张图待补。把图片放到 `docs/images/` 后，去掉注释标记即可显示。

| 首页 | 游戏 | 两板接线 |
|---|---|---|
| _待补 `docs/images/home.jpg`_ | _待补 `docs/images/game.jpg`_ | _待补 `docs/images/wiring.jpg`_ |

<!-- 图片就位后删除这组注释标记
![首页](docs/images/home.jpg)
![游戏](docs/images/game.jpg)
![两板接线](docs/images/wiring.jpg)
-->


## 硬件构成

**NS800RT7P65 侧**

| 项目 | 说明 |
|---|---|
| 主控 | NS800RT7P65（Cortex-M7），RT-Thread 5.3.0 |
| 显示 | ILI9341 240×320，SPI 接口，实测 50 MHz 可用（70 MHz 会花屏） |
| 输入 | EC11 旋钮（eQEP 正交解码）+ 板载按键 |
| 传感 | DHT22 温湿度、板载温度传感器 |
| 存储 | 片内 Flash + 外置 SPI NOR（LittleFS，存配置 / 闹钟 / 壁纸） |
| 输出 | 有源蜂鸣器、两路 LED、屏幕背光 |

**ESP32-C3 侧**

| 项目 | 说明 |
|---|---|
| 环境 | ESP-IDF v6.1，4 MB Flash |
| 联网 | WiFi（STA 连路由 / AP 开热点） |
| 服务 | HTTP 管理页、本地文件 OTA 升级（A/B 双槽 + 启动失败回滚保护） |
| 对时 | NTP，时间经链路同步给 NS800 |

**板间链路**

| 方向 | 接线 |
|---|---|
| NS800 → C3 | PB6（UART2_TX）→ GPIO5（RX） |
| C3 → NS800 | GPIO4（TX）→ PB7（UART2_RX） |
| 共地 | GND ↔ GND（必须接，否则收到的全是乱码） |

帧格式：`0xAA 0x55 | LEN | PAYLOAD | CRC8`，115200 8N1，CRC-8/ATM。
除状态查询、时间同步、OTA 触发等固定命令外，还有一条通用的 `key=value`
文本上行通道——于是新增一种数据不必改协议。


## 快速上手

### 首次克隆先补齐依赖

```bash
# 1) 指向 RT-Thread 内核源码（需 5.3.0）
export RTT_ROOT=/path/to/rt-thread

# 2) 芯片厂商 SDK（公开仓库，本仓库未收录）
git clone https://github.com/RT-Thread-packages/novosns-series.git \
    libraries/novosns-series-latest

# 3) RT-Thread 软件包（LVGL / littlefs / button / dhtxx / ili9341 / beep / CMSIS-Core）
cd ns800rt7p65-nssinepad && pkgs --update
```

### 编译与烧录

```bash
# NS800：加载环境（会激活 RT-Thread Env 并切到 BSP 根目录）
cd <仓库根目录>
source env.sh

# 编译 + 烧录
source f.sh

# 若 pyocd 未识别到芯片，先装烧录算法包
bash ns800rt7p65_pyocd_flash/ns800rt7p65_flash/install.sh
```

```bash
# ESP32-C3
source $IDF_PATH/export.sh
cd c3_bridge && idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`pyocd` 的子命令必须在 BSP 根目录执行——自定义 target `ns800rt7p65`
由同目录的 `pyocd_user.py` 提供，换个目录会报 `Target not recognized`。


## 目录结构

```
ns800rt7p65-nssinepad/     NS800 侧 BSP 本体（RT-Thread）
  applications/            应用代码：bridge/（板间链路）、lvgl/ui/（界面）、游戏、调色台
  board/                   板级初始化与引脚定义
  LVGL_AI_STANDARD.md      UI 开发规范
  FLASHING_AI_STANDARD.md  编译烧录规范与排障
libraries/HAL_Drivers/     板级外设驱动
libraries/novosns-series-latest/   芯片厂商 SDK（需自行克隆）
ns800rt7p65_pyocd_flash/   pyocd 烧录算法与 target 定义
c3_bridge/                 ESP32-C3 侧（ESP-IDF 工程）
```


## 进度

- [x] 点亮 LED
- [x] LVGL 中文界面，14 个功能页，旋钮 + 按键交互
- [x] 数字编码器（EC11 + eQEP）
- [x] 传感器（DHT22 温湿度、板载温度）
- [x] 外置 SPI NOR + LittleFS（配置 / 闹钟 / 壁纸持久化）
- [x] 闹钟：到点响铃、掉电保存、停在桌面也照样响
- [x] 横屏跑酷游戏、gamma 调色台
- [x] 与 ESP32-C3 的板间链路：状态查询、时间同步、OTA 触发、通用数据上行
- [x] ESP32-C3 侧：WiFi 管理页、本地文件 OTA、NTP 对时、业务数据上报
- [ ] 链路上行提速（评估把波特率提到 460800 / 921600）
- [ ] OTA 回滚实机验证（注入启动失败场景，确认回滚保护生效）


## 开发须知

改动前先看这几条，都是踩过坑之后定下来的：

- **协议头两份拷贝必须一致。** `ns800_link.h` 在两侧各存一份，改动要同步：
  ```bash
  diff c3_bridge/main/ns800_link.h \
       ns800rt7p65-nssinepad/applications/bridge/ns800_link.h
  ```
- **屏上中文必须走 `ui_text.h` 的宏**，不要在 `.c` 里直接写中文字面量
  （字库脚本只扫 `ui_text.h`，漏字在屏上会显示成空心方块）。改完文案重跑
  `sh ns800rt7p65-nssinepad/applications/lvgl/ui/gen_font_zh.sh`。
- **新增页面要改 5 处**：`ui_text.h`、`ui_apps.c`、`ui_apps.h`、`ui.c`、`ui.h`。
  详见 `LVGL_AI_STANDARD.md`。
- **`c3_bridge/sdkconfig` 不入库**（会保存 WiFi 明文凭据）。
  改了 menuconfig 后用 `cd c3_bridge && idf.py save-defconfig` 折回 `sdkconfig.defaults`。

仓库忽略规则分两层：根目录 `.gitignore` 管两个子工程共有的部分
（AI 工具产物、构建产物、环境与本地配置、编辑器与系统杂项），
`ns800rt7p65-nssinepad/.gitignore` 是 RT-Thread BSP 自带的一份。


## 已知限制

- **无视频播放能力。** 板间链路 115200 下每秒约 11 KB，整屏 240×320 RGB565 为 150 KB，
  一帧要十几秒。真要做需要 C3 侧编码 JPEG 并把串口提速到 921600，画质与帧率都要压得很低。
- **界面没有二级子页面。** 硬件上旋钮只有一个按键，进去就出不来，因此所有交互都在一层。
- **时间依赖 ESP32-C3。** NS800 没有独立时钟源，C3 离线时时间与闹钟判定不可用。
- **NS800 片内 Flash 余量紧张**（已用约 93.6%），加功能前需要先核算空间。
