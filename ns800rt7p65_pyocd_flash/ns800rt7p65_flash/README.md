# NS800RT7P65 pyocd 烧录支持包

纳芯微（Novosense）**NS800RT7P65** 双核 Cortex-M7 芯片的 pyocd 烧录支持包，用于在 Linux 下通过 **DAP-Link / CMSIS-DAP** 烧录 RT-Thread 固件。

> 来源：从官方 SDK v0.4.0 的 `utilities/pack/MDK/NOVOSENSE.NS800RT7XXX.0.5.3.pack` 中提取。

---

## 一、芯片烧录关键参数

| 项目 | 值 |
|------|-----|
| 内核 | Cortex-M7（双核，RT-Thread 跑 CPU1） |
| Flash Bank1 | `0x08000000`，512KB（0x80000） |
| Flash Bank2 | `0x08080000`，512KB（CPU2 用） |
| 编程页大小 | 1KB（0x400） |
| DTCM | `0x20000000`，64KB |
| SRAM1 | `0x20100000`，128KB |
| SRAM2 | `0x20120000`，128KB |
| Backup SRAM | `0x400B7000`，4KB |

---

## 二、快速开始

### 1. 安装 pyocd

```bash
pip3 install pyocd
# 国内加速：
pip3 install -i https://mirrors.aliyun.com/pypi/simple/ pyocd
```

### 2. 部署本包

```bash
cd ns800rt7p65_flash
bash install.sh
```

### 3. 把 pyocd_user.py 放到 bsp 目录

```bash
cp pyocd_user.py ~/RT_Thread/rt-thread/bsp/novosns/ns800/ns800rt7p65-nssinepad/
cd ~/RT_Thread/rt-thread/bsp/novosns/ns800/ns800rt7p65-nssinepad/
```

### 4. 烧录

```bash
# 插上 DAP-Link，确认识别
pyocd list

# 烧录 RT-Thread（在 bsp 目录下，先 scons 编译出 rtthread.elf）
pyocd flash -t ns800rt7p65 rtthread.elf

# 擦除后烧录
pyocd flash -e -t ns800rt7p65 rtthread.elf
```

---

## 三、文件说明

```
ns800rt7p65_flash/
├── flash_algo/                          # Flash 算法（必须，pyocd 靠它擦写 Flash）
│   ├── NS800RT7xxx_FlashBank1.FLM       #   Bank1，0x08000000，512KB
│   ├── NS800RT7xxx_FlashBank2.FLM       #   Bank2，0x08080000，512KB
│   └── NS800RT7xxx_FlashDoubleBank.FLM  #   双 Bank 联合
├── svd/
│   └── NS800RT737x.svd                  # 寄存器描述（可选，调试显示用，同系列完整版）
├── targets/
│   └── ns800rt7p65.py                   # target 定义（备份，pyocd_user.py 已内联）
├── pyocd_user.py                        # 注册脚本，放到 bsp 目录
├── install.sh                           # 一键部署脚本
└── README.md                            # 本文件
```

---

## 四、常见问题排查

### 问题 1：`pyocd list` 报 `No available debug probes are connected`

这是 **Linux USB HID 权限问题**，不是 DAP-Link 坏了。你之前能看到 `DAPLINK` 盘符（那是 MSC 通道），但 pyocd 用的是 HID 通道，需要 udev 授权。

```bash
# ① 先确认设备在 USB 总线上
lsusb | grep -iE "dap|cmsis|arm|novo|nsi|0d28|c251|2a86|9796"

# ② 加 udev 规则（覆盖常见 CMSIS-DAP VID）
sudo tee /etc/udev/rules.d/99-cmsis-dap.rules <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="0d28", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="c251", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="2a86", MODE="0666"
KERNEL=="hidraw*", MODE="0666"
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger

# ③ 拔插 DAP-Link 后再试
pyocd list
```

> 如果 `lsusb` 看不到纳芯微的 VID，把 `lsusb` 完整输出发出来，找到纳芯微的 VID 后补充到规则里。

### 问题 2：拖拽 `.bin` 报 `Failed to reset/halt the target MCU`

这是 **SWD 物理连接 / 供电问题**，与 .bin 文件无关。排查顺序：

1. **供电**：目标板 VDD 有没有 3.3V？DAP-Link 的 3V3 有没有接到板子 VDD？很多板子需要独立供电。
2. **SWD 接线**：`SWDIO`、`SWCLK`、`GND` 三根线缺一不可。
3. **复位线**：建议把 `nRST` 也接上。
4. **芯片锁定**：之前是否烧过带读保护的固件？

> 另外，NS800RT7P65 是双核芯片，标准 DAP-Link 的拖拽烧录对双核支持较弱，**强烈建议走 pyocd 路线**。

### 问题 3：`pyocd pack install` 报 `No matching devices`

原因：官方 `.pack` 里**没有标准的 `.pdsc` 设备描述文件**（只有 FLM + SVD），所以 pyocd 的 pack 管理器认不出设备。

**本包已绕开这个问题**——直接用自定义 target + FLM 算法，不依赖 pack 索引。

---

## 五、技术说明

- FLM 算法来自官方 `NOVOSENSE.NS800RT7XXX.0.5.3.pack`，pyocd 已验证可正确解析：
  - `flash_start = 0x08000000`
  - `flash_size = 512KB`
  - `page_size = 1024B`
  - 算法函数 `Init / UnInit / EraseSector / ProgramPage / Verify / EraseChip` 齐全
- 内存映射来自 SDK `ns800rt7xxx_eflash_cpu1.ld` 链接脚本。
- 双核说明：RT-Thread 单核工程运行在 **CPU1**，对应 **Flash Bank1**（`0x08000000`）。若要烧 CPU2 的固件，需改用 `FlashBank2.FLM`（地址 `0x08080000`）。
