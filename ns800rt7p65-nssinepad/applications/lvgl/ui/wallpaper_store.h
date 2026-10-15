/**
 * @file    wallpaper_store.h
 * @brief   壁纸存取接口（实现见 wallpaper_store.c）
 *
 * 数据流：
 *   主机  make_wallpaper.py      生成 /tmp/wallpaper.bin（240x320 RGB565）
 *   主机  pyocd load wallpaper.bin@<缓冲地址>      直接写入 SRAM
 *   设备  msh: wallsave <hash>                    存入 LittleFS
 *   设备  之后每次开机自动从 /wallpaper.bin 读回 SRAM
 *
 * 分辨率 240x320（1:1 原分辨率，不缩放），WALL_BYTES = 153600。
 * 缓冲位于 link.lds 的 .wall_buf 段（SRAM12，紧跟 LVGL 池之后，
 * 起始约 0x20110000）。
 */

#ifndef __WALLPAPER_STORE_H__
#define __WALLPAPER_STORE_H__

#include <rtthread.h>

#ifndef WALL_SRC_W
#define WALL_SRC_W   240            /* 与屏同宽，1:1 不放大 */
#endif
#ifndef WALL_SRC_H
#define WALL_SRC_H   320
#endif
#ifndef WALL_ZOOM
#define WALL_ZOOM    1              /* LVGL 缩放系数 = WALL_ZOOM*256，1.0 即 256 */
#endif
#ifndef WALL_BYTES
#define WALL_BYTES   (WALL_SRC_W * WALL_SRC_H * 2)   /* RGB565 = 2 字节/像素 = 150K */
#endif
#ifndef WALL_PATH
#define WALL_PATH    "/wallpaper.bin"
#endif

/**
 * @brief  取壁纸 RGB565 数据指针
 * @return 缓冲首地址；失败返回 RT_NULL，调用方应退回纯色背景
 * @note   首次调用时从 WALL_PATH 加载，返回的指针常驻，不要 free。
 */
const uint8_t *wallpaper_get(void);

/**
 * @brief  丢弃缓存，使下次 wallpaper_get() 重新加载
 * @note   重新灌图后调用。
 */
void wallpaper_invalidate(void);

/**
 * @brief  计算 FNV-1a 32 位哈希
 * @param  p 数据首地址
 * @param  n 字节数
 * @return 哈希值
 * @note   主机与设备两侧算法必须一致，见 make_wallpaper.py。
 */
rt_uint32_t wallpaper_hash(const uint8_t *p, rt_size_t n);

#endif /* __WALLPAPER_STORE_H__ */
