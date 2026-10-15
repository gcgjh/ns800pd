# 多通道信号采集与监测终端

- 基于纳芯微 **NS800RT7P65D**，Ubuntu 环境开发的多通道信号采集与监测终端。
  搭载 LVGL 图形界面，通过数字编码器与按键组合完成菜单交互。
- 另配一块 **ESP32-C3** 做联网与上报：WiFi 连接、HTTP 管理页、OTA 升级、NTP 对时，
  两块板之间用 UART2 板间链路（115200 8N1）通信，NS800 为被动端。
- 原厂 OpenOCD 缺少 NS800RT7P65D 烧录算法，本项目补齐芯片烧录算法，提供一键编译烧录脚本。

选择双板是为了取长补短：NS800 有屏无网，C3 有网无屏。


## 目录结构

```
ns800rt7p65-nssinepad/        NS800 侧 BSP 本体（RT-Thread 5.3.0）
  applications/
    bridge/                   板间链路：c3_link.c / c3_report.c / ns800_link.h
    lvgl/ui/                  UI：ui.c（桌面框架 + App 注册表）、ui_apps.c（各 App）、
                              ui_text.h（中文文案唯一出处）、app_alarm.c、settings.c、
                              wallpaper_store.c
    lcd_game_ui.c             横屏跑酷游戏
    lcd_tune_ui.c             调色台（gamma / 色温 / 亮度 / 饱和度）
  board/                      板级初始化与引脚定义
  SConstruct  rtconfig.h      构建脚本与内核配置
  f.sh                        一键编译 + 烧录
  LVGL_AI_STANDARD.md         UI 开发规范（脏区重绘、内存预算、新增 App 清单）
  FLASHING_AI_STANDARD.md     编译烧录规范与排障

libraries/HAL_Drivers/        BSP 外设驱动（UART 引脚表就在 drv_uart.c 里）
libraries/novosns-series-latest/
                              芯片厂商 SDK —— 不入库，需手动克隆（见下）
ns800rt7p65_pyocd_flash/      pyocd 烧录算法与 target 定义
tools/                        辅助脚本

c3_bridge/                    ESP32-C3 侧（ESP-IDF 工程）
  main/                       全部业务代码
  partitions.csv              分区表（OTA 双槽布局）
  sdkconfig.defaults          配置基线（sdkconfig 本身不入库）
```


## 使用流程

```bash
# 加载环境变量（会激活 RT-Thread Env 的 venv 并切到 BSP 根目录）
source env.sh

# 编译 + 烧录
source f.sh

# 如果 pyocd 未识别到芯片，装上烧录算法包
bash ns800rt7p65_pyocd_flash/ns800rt7p65_flash/install.sh
```

### 首次克隆需先补齐依赖

**1. RT-Thread 内核源码。** 本仓库只含 BSP，不含内核。`SConstruct` 在未设置
`RTT_ROOT` 时取「当前目录向上四级」作为内核根目录，本仓库布局下会指到错的地方，
因此必须显式指定：

```bash
export RTT_ROOT=/path/to/rt-thread      # 需要 RT-Thread 5.3.0
```

**2. 芯片厂商 SDK。** `libraries/novosns-series-latest/` 来自公开仓库，本仓库未收录
（它自带 `.git`，父仓库只能记一个空引用，故在 `.gitignore` 中整体忽略）：

```bash
git clone https://github.com/RT-Thread-packages/novosns-series.git \
    libraries/novosns-series-latest
```

**3. RT-Thread 软件包。** LVGL、littlefs、button、dhtxx、ili9341、beep、CMSIS-Core
由 RT-Thread Env 按 `packages/pkgs.json` 下载，不随仓库分发：

```bash
cd ns800rt7p65-nssinepad && pkgs --update
```

### ESP32-C3 侧

```bash
source $IDF_PATH/export.sh          # 激活 ESP-IDF v6.1 环境
cd c3_bridge
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```


## 开发任务

- [x] 点亮 LED 🥰
- [x] 完成 LVGL 界面（14 个 App，240×320，旋钮 + 单按键交互）
- [x] 加入数字编码器（EC11 + eQEP 正交解码）
- [x] 加入传感器（DHT22 温湿度、板载温度）
- [x] 外置 SPI NOR Flash + LittleFS 持久化（配置、闹钟、壁纸）
- [x] 闹钟（到点响铃、掉电保存、常驻判定）
- [x] 横屏跑酷游戏、调色台
- [x] 与 ESP32-C3 的板间链路：状态查询、时间同步、OTA 触发、通用数据上行（0x88）
- [x] ESP32-C3 侧：WiFi 管理页、本地文件 OTA、NTP 对时、业务数据上报
- [ ] 链路上行带宽优化（评估把波特率提到 460800 / 921600）
- [ ] OTA 回滚实机验证（注入启动失败场景，确认 PENDING_VERIFY 逻辑生效）


## 维护约定

这几条是踩过坑之后定下来的，改动前请先看一眼。

**协议头两份拷贝必须一致。** `ns800_link.h` 在两侧各存一份（没有软链），改动必须同步：

```bash
diff c3_bridge/main/ns800_link.h \
     ns800rt7p65-nssinepad/applications/bridge/ns800_link.h
```

**屏上中文必须走 `ui_text.h` 的宏**，不要在 `.c` 里直接写中文字面量
（字库脚本只扫 `ui_text.h`，漏字在屏上会显示成空心方块）。改完文案后重跑：

```bash
sh ns800rt7p65-nssinepad/applications/lvgl/ui/gen_font_zh.sh
```

脚本会先校验 `ui_text.h` 的 UTF-8 转义字节与行尾注释是否一致，不一致直接中止。

**新增 App 要改 5 处**：`ui_text.h`（文案 + 重生成字库）、`ui_apps.c`（实现）、
`ui_apps.h`（声明）、`ui.c`（`APP_COUNT` 与注册表）、`ui.h`（图标色）。
`APP_COUNT` 必须与表项数严格相等，写大了会越界调用空指针。详见 `LVGL_AI_STANDARD.md`。

**sdkconfig 不入库。** 它由 `sdkconfig.defaults` + `Kconfig.projbuild` 推导生成，
且会保存 `CONFIG_C3_WIFI_SSID` / `CONFIG_C3_WIFI_PASSWORD` 明文。
在 menuconfig 里改了需要长期保留的项之后，用下面命令折回基线文件：

```bash
cd c3_bridge && idf.py save-defconfig
```


## 关于 .gitignore

分两层，叠加生效：

- **仓库根 `.gitignore`** —— 管两个子工程共有的四类：AI 工具产物、
  构建与编译产物、环境与本地配置、编辑器与系统杂项。
- **`ns800rt7p65-nssinepad/.gitignore`** —— RT-Thread BSP 自带的一份，
  负责该目录下的 `*.elf` / `*.bin` / `*.map` / `packages/` 等。
