# -*- coding: utf-8 -*-
"""
NS800RT7P65 (Novosense 纳芯微) 自定义 pyocd 目标定义。

芯片关键参数（从 SDK v0.4.0 链接脚本 + FLM 算法提取）：
  - 内核：Cortex-M7（双核，RT-Thread 单核运行于 CPU1）
  - Flash Bank1：0x08000000，512KB（0x80000），编程页 1KB
  - Flash Bank2：0x08080000，512KB（CPU2 使用）
  - DTCM：0x20000000，64KB
  - SRAM1：0x20100000，128KB
  - SRAM2：0x20120000，128KB
  - Backup SRAM：0x400B7000，4KB

用法：
  1. 将本文件放入 ~/.pyocd/targets/（或通过 pyocd_user.py 注册）
  2. 将 FLM 文件放入 ~/.pyocd/flash_algo/
  3. pyocd list 后 pyocd flash -t ns800rt7p65 rtthread.elf
"""

from pathlib import Path

from pyocd.coresight.coresight_target import CoreSightTarget
from pyocd.core.memory_map import FlashRegion, MemoryMap, RamRegion


def _flm(name: str) -> str:
    """返回 FLM 算法的绝对路径（部署在 ~/.pyocd/flash_algo/ 下）。"""
    return str(Path.home() / ".pyocd" / "flash_algo" / name)


class NS800RT7P65(CoreSightTarget):
    """Novosense NS800RT7P65 目标。"""

    VENDOR = "NOVOSENSE"
    PART_NUMBERS = [
        "NS800RT7P65",
        "NS800RT7P65x",
        "NS800RT7370",
        "NS800RT737x",
        "NS800RT7XXX",
    ]

    MEMORY_MAP = MemoryMap(
        FlashRegion(
            name="eflash_bank1",
            start=0x08000000,
            length=0x00080000,        # 512KB
            blocksize=0x400,           # 1KB 擦除/编程页
            page_size=0x400,
            is_boot_memory=True,
            flm=_flm("NS800RT7xxx_FlashBank1.FLM"),
        ),
        RamRegion(name="itcm", start=0x00000000, length=0x00010000),   # 64KB
        RamRegion(name="dtcm", start=0x20000000, length=0x00010000),   # 64KB
        RamRegion(name="sram1", start=0x20100000, length=0x00020000),  # 128KB
        RamRegion(name="sram2", start=0x20120000, length=0x00020000),  # 128KB
        RamRegion(name="backup_sram", start=0x400B7000, length=0x00001000),  # 4KB
    )

    def __init__(self, session):
        super().__init__(session, self.MEMORY_MAP)
