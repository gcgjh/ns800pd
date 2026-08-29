# -*- coding: utf-8 -*-
"""
pyocd 用户脚本：注册 NS800RT7P65 自定义目标。

把这个文件复制到你的 RT-Thread bsp 目录（例如
bsp/novosns/ns800/ns800rt7p65-nssinepad/），然后在该目录下运行 pyocd，
它就会自动加载本脚本并注册目标。

前提：FLM 算法文件已放在 ~/.pyocd/flash_algo/ 下。
"""

from pathlib import Path

from pyocd.coresight.coresight_target import CoreSightTarget
from pyocd.core.memory_map import FlashRegion, MemoryMap, RamRegion
from pyocd.target import TARGET


def _flm(name: str) -> str:
    """返回 FLM 算法的绝对路径（部署在 ~/.pyocd/flash_algo/ 下）。"""
    return str(Path.home() / ".pyocd" / "flash_algo" / name)


class NS800RT7P65(CoreSightTarget):
    """Novosense NS800RT7P65 目标（Cortex-M7，RT-Thread 跑 CPU1）。"""

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
            blocksize=0x400,           # 1KB
            page_size=0x400,
            is_boot_memory=True,
            flm=_flm("NS800RT7xxx_FlashBank1.FLM"),
        ),
        RamRegion(name="itcm", start=0x00000000, length=0x00010000),   # 64KB
        RamRegion(name="dtcm", start=0x20000000, length=0x00010000),   # 64KB
        RamRegion(name="sram1", start=0x20100000, length=0x00020000),  # 128KB
        RamRegion(name="sram2", start=0x20120000, length=0x00020000),  # 128KB
        RamRegion(name="backup_sram", start=0x400B7000, length=0x00001000),
    )

    def __init__(self, session):
        super().__init__(session, self.MEMORY_MAP)


# 注册目标类型名（供 pyocd flash -t <name> 使用）
TARGET["ns800rt7p65"] = NS800RT7P65
TARGET["ns800rt7xxx"] = NS800RT7P65
TARGET["ns800"] = NS800RT7P65
