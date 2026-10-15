/**
 * @file    spi3_flash.h
 * @brief   SPI3 外置 NOR Flash 的共享接口定义
 *
 * spi3_sfud.c 实现，其余文件引用。当前 SPI 设备名为 "spi30"，
 * SFUD 块设备名为 "spiflash0"。
 * 需要 sfud 句柄的场合（如 LittleFS 的 MTD 桥接）调用 spi3_sfud_get()。
 */

#ifndef __SPI3_FLASH_H__
#define __SPI3_FLASH_H__

#include <rtthread.h>
#include "dev_spi_flash.h"          /* rt_spi_flash_device_t */
#include "sfud/inc/sfud.h"          /* sfud_flash */

#define SPI3_BUS_NAME      "spi3"        /* SPI 总线名 */
#define SPI3_SPI_DEV_NAME  "spi30"       /* SPI 从设备名（挂 CS 用） */
#define SPI3_BLK_NAME      "spiflash0"   /* SFUD 注册出的块设备名 */
#define SPI3_MTD_NAME      "norflash0"   /* 桥接出的 MTD NOR 设备名（LittleFS 用） */
#define SPI3_LFS_MNT       "/"           /* LittleFS 挂载点 */
#define SPI3_FLASH_CS_PIN  53            /* flash 片选引脚号，GPIO_53 */

/**
 * @brief  获取 SFUD flash 句柄：attach -> probe -> 缓存，幂等
 * @param  out_sfud  输出，sfud_flash 句柄，可为 NULL
 * @param  out_rtt   输出，rt_spi_flash_device 句柄，可为 NULL
 * @return RT_EOK 成功；失败返回负错误码
 * @note   返回 RT_EOK 时 *out_sfud 与 *out_rtt 均有效。
 */
rt_err_t spi3_sfud_get(sfud_flash **out_sfud, rt_spi_flash_device_t *out_rtt);

/**
 * @brief  幂等挂载 LittleFS（内部会先建立 MTD 设备）
 * @return RT_EOK 成功；-RT_EIO 或 -RT_ERROR 失败
 * @note   壁纸加载等场景可直接调用。
 */
rt_err_t spi3_lfs_ensure_mounted(void);

#endif /* __SPI3_FLASH_H__ */
