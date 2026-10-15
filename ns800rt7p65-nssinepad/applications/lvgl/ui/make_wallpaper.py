#!/usr/bin/env python3
"""照片 -> 240x320 RGB565 原始数据（.bin），直接喂给设备外置 flash 的壁纸文件

用法:
    python3 make_wallpaper.py <源图> [输出.bin]

产物:
    · /tmp/wallpaper.bin        240*320*2 = 153600 字节，RGB565 小端
    · 同时打印 FNV-1a 32 哈希 —— 灌图时 `wallsave <hash>` 要用（主机/设备两侧一致）

为什么不再输出 C 数组（wallpaper.h）:
    150KB 编进固件会顶爆内部 flash（去掉壁纸后固件已 424K / 512K）。
    现在改成 pyocd 直接写 SRAM 缓冲、再由固件存进 LittleFS，图完全不进固件。

为什么是 240 宽而不是 120 宽 + 放大:
    120 宽横向放大 2 倍是上一版的妥协（当时内部 flash 放不下全宽图），
    画面明显发糊。现在图在外置 flash，用原生 240 宽 1:1 显示。

构图:
    源图 864x1656，底部约 156px 是抖音水印 → 裁掉，剩 864x1500。
    直接 resize 到 240x320（等比拉伸到屏比例，与上一版构图一致）。
    如需保持原始比例（居中裁剪），把 FIT 改成 "crop"。
"""
import sys
from PIL import Image, ImageFilter

W, H = 240, 320
FIT = "crop"             # "crop" = 居中裁剪保比例（2026-10-06 选定，不再横向拉伸）
SHARPEN = True           # 降采样后轻微锐化，对抗小屏糊感

SRC = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/wallpaper.bin"


def to565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def main():
    im = Image.open(SRC).convert("RGB")
    print(f"源图 {im.size[0]}x{im.size[1]}")

    # 原图 864x1656，底部约 156px 是抖音水印，裁掉
    if im.width == 864 and im.height == 1656:
        im = im.crop((0, 0, 864, 1500))
        print("已裁掉底部水印 -> 864x1500")

    if FIT == "crop":
        # 居中裁剪到 3:4（240:320）再缩，保持原始比例
        target = W / H
        w, h = im.size
        if w / h > target:
            nw = int(h * target)
            im = im.crop(((w - nw) // 2, 0, (w - nw) // 2 + nw, h))
        else:
            nh = int(w / target)
            im = im.crop((0, (h - nh) // 2, w, (h - nh) // 2 + nh))

    im = im.resize((W, H), Image.LANCZOS)
    if SHARPEN:
        im = im.filter(ImageFilter.UnsharpMask(radius=1.1, percent=65, threshold=3))

    px = im.load()
    data = bytearray()
    for y in range(H):
        for x in range(W):
            v = to565(*px[x, y])
            data.append(v & 0xFF)          # 小端：低字节在前
            data.append((v >> 8) & 0xFF)

    with open(OUT, "wb") as f:
        f.write(data)

    print(f"已写出 {OUT}: {len(data)} B ({len(data)/1024:.1f} KB)  {W}x{H} RGB565")
    print(f"FNV-1a 32 = 0x{fnv1a32(data):08X}")
    print(f"灌图: pyocd load {OUT}@<wallstatus 打印的地址>")
    print(f"      msh → wallsave 0x{fnv1a32(data):08X}")


if __name__ == "__main__":
    main()
