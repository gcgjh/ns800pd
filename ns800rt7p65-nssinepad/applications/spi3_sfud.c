/**
 * @file    spi3_sfud.c
 * @brief   SPI3 外置 NOR Flash 的 SFUD 接入层（msh: sfudprobe / sfudtest）
 *
 * 与 spi3_probe.c 的分工：
 *   spi3_probe.c  手写 SPI 时序，用于验证硬件（接线是否正确、片子是否存在）；
 *   本文件        用 SFUD 自动识别参数，用于跑功能，为文件系统提供块设备。
 *
 * 不引入官方 sfud 包的原因：
 *   RT-Thread 主线的 components/drivers/spi/ 自带 SFUD 源码与适配层
 *   （dev_spi_flash_sfud.c），打开 RT_USING_SFUD 即一并编译。
 *   官方包索引（~/.env/packages）中已无 sfud 包，无需另外拉取。
 *
 * 注意事项 1：本 BSP 的 RT_VER_NUM = 0x50300（RT-Thread 5.3.0），而包索引中
 *   system/fal 的 Kconfig 写死 depends on RT_VER_NUM < 0x40100，
 *   官方 fal 包在本工程不可选。因此本文件不依赖 FAL，直接使用 SFUD 生成的
 *   块设备（rt_sfud_flash_probe 会注册名为 <flash_name> 的块设备）。
 *
 * 注意事项 2：rt_sfud_flash_probe(spi_flash_dev_name, spi_dev_name) 的两个
 *   参数不能同名。第二个必须是已存在的 SPI 设备（probe 内部 rt_device_find
 *   它并检查 type == RT_Device_Class_SPIDevice），第一个是 probe 新建的块设备名。
 *   若同名，第二次 rt_device_find 拿到的是块设备，其 user_data 指向
 *   spi_flash_device 而非 sfud_flash，导致 sfud_flash* 取不到，
 *   表现为 SFUD 未真正识别芯片。故 SPI 设备名取 "spi30"，块设备名取 "spiflash0"。
 *
 * 接线同 spi3_probe.c：
 *   SCK GPIO_52 / MOSI GPIO_50 / MISO GPIO_51 / CS GPIO_53
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>

/* SFUD 适配层（RT-Thread 主线自带，位置按 SConscript 的 CPPPATH 走） */
#include "dev_spi_flash.h"
#include "dev_spi_flash_sfud.h"
#include "spi3_flash.h"



/* probe 结果缓存：
 *   g_rtt_dev  -> spi_flash_device（块设备本体）
 *   g_sfud     -> sfud_flash（erase/write/read 使用） */
static rt_spi_flash_device_t g_rtt_dev;
static sfud_flash           *g_sfud;

/**
 * @brief  在 SPI3 总线上挂载 flash 对应的 SPI 从设备（幂等）
 * @return RT_EOK 成功；-RT_ENOSYS 总线不存在；-RT_ENOMEM 分配失败
 */
static rt_err_t spi3_attach(void)
{
    if (rt_device_find(SPI3_SPI_DEV_NAME) != RT_NULL)
        return RT_EOK;                  /* 已挂载（例如 spi3probe 已执行过） */

    if (rt_device_find(SPI3_BUS_NAME) == RT_NULL)
        return -RT_ENOSYS;

    {
        struct rt_spi_device *spi_dev = rt_malloc(sizeof(struct rt_spi_device));
        rt_err_t r;

        if (spi_dev == RT_NULL)
            return -RT_ENOMEM;

        r = rt_spi_bus_attach_device_cspin(spi_dev, SPI3_SPI_DEV_NAME,
                                           SPI3_BUS_NAME, SPI3_FLASH_CS_PIN, RT_NULL);
        if (r != RT_EOK)
            rt_free(spi_dev);
        return r;
    }
}

/**
 * @brief  获取 SFUD flash 句柄：attach -> probe -> 缓存，幂等
 * @param  out_sfud  输出，sfud_flash 句柄，可为 NULL
 * @param  out_rtt   输出，rt_spi_flash_device 句柄，可为 NULL
 * @return RT_EOK 成功；-RT_ENOSYS 设备或总线不存在；
 *         -RT_EIO probe 失败；-RT_ERROR 未取到 sfud 句柄
 * @note   sfudprobe、sfudtest 与 LittleFS 的 MTD 桥接均调用本函数。
 */
rt_err_t spi3_sfud_get(sfud_flash **out_sfud, rt_spi_flash_device_t *out_rtt)
{
    struct rt_spi_device *spi;
    rt_spi_flash_device_t rtt;

    if (g_sfud == RT_NULL)
    {
        if (spi3_attach() != RT_EOK)
            return -RT_ENOSYS;

        spi = (struct rt_spi_device *)rt_device_find(SPI3_SPI_DEV_NAME);
        if (spi == RT_NULL)
            return -RT_ENOSYS;

        /* 识别阶段必须低速 Mode0：JEDEC SFDP 命令要求不高于 50 MHz，1 MHz 最稳。
         * 不依赖 rt_sfud_flash_probe 的默认配置，显式配置一次更可控。 */
        {
            struct rt_spi_configuration cfg;
            cfg.data_width = 8;
            cfg.max_hz     = 1 * 1000 * 1000;
            cfg.mode       = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB;
            rt_spi_configure(spi, &cfg);
        }

        rtt = rt_sfud_flash_probe(SPI3_BLK_NAME, SPI3_SPI_DEV_NAME);
        if (rtt == RT_NULL)
            return -RT_EIO;

        /* probe 成功后，待 SPI 空闲再提速（常规读命令不受 50 MHz 限制） */
        {
            struct rt_spi_configuration cfg;
            cfg.data_width = 8;
            cfg.max_hz     = 20 * 1000 * 1000;
            cfg.mode       = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB;
            rt_spi_configure(spi, &cfg);
        }

        g_rtt_dev = rtt;
        g_sfud    = (sfud_flash *)rtt->user_data;   /* 适配层把 sfud_flash* 存这 */
        if (g_sfud == RT_NULL)
            return -RT_ERROR;
    }

    if (out_sfud) *out_sfud = g_sfud;
    if (out_rtt)  *out_rtt  = g_rtt_dev;
    return RT_EOK;
}

/* msh 命令 sfudprobe：用 SFUD 自动识别 flash 并注册块设备。用法 sfudprobe */

/**
 * @brief  msh 命令 sfudprobe：probe SPI3 外置 NOR flash 并注册块设备
 *         用法 sfudprobe
 */
static void sfudprobe(int argc, char **argv)
{
    rt_device_t blk;

    (void)argc;
    (void)argv;

    if (spi3_sfud_get(RT_NULL, RT_NULL) != RT_EOK)
    {
        rt_kprintf("[sfud]   初始化/probe 失败\n");
        rt_kprintf("       ① 先跑 spi3probe 确认接线和 JEDEC ID；\n");
        rt_kprintf("       ② 若硬件 OK，说明片子不在 SFUD 内置表且不支持 SFDP\n");
        return;
    }

    rt_kprintf("[sfud]   probe OK: %s\n", g_sfud->name);
    rt_kprintf("[sfud]   flash: %s, capacity %u KB\n",
               g_sfud->chip.name[0] ? g_sfud->chip.name : "(SFDP)",
               (unsigned)(g_sfud->chip.capacity / 1024));
    rt_kprintf("[sfud]   erase granularity: %u B\n",
               (unsigned)g_sfud->chip.erase_gran);
    /* rt_device_blk_geometry 字段：sector_count / bytes_per_sector / block_size */
    rt_kprintf("[sfud]   blk geometry: %u sectors x %u B\n",
               (unsigned)g_rtt_dev->geometry.sector_count,
               (unsigned)g_rtt_dev->geometry.bytes_per_sector);

    blk = rt_device_find(SPI3_BLK_NAME);
    if (blk != RT_NULL)
        rt_kprintf("[sfud]   block device '%s' registered (type=%d)\n",
                   SPI3_BLK_NAME, blk->type);

    rt_kprintf("[sfud] SPI 已提速至 20 MHz，接着可以跑 sfudtest\n");
}
MSH_CMD_EXPORT(sfudprobe, probe SPI3 NOR flash by SFUD and register blk device);

/* msh 命令 sfudtest：走 SFUD API 做一次擦/写/读回环。
 * 用法 sfudtest [addr]，默认地址 0（会擦除该扇区）。 */

/**
 * @brief  msh 命令 sfudtest：经 SFUD API 做一次擦/写/读回环
 *         用法 sfudtest [addr]
 * @param  addr  起始地址，默认 0，会擦除该地址所在扇区
 * @note   写入数据与地址相关，避免全 0 造成的假通过。
 */
static void sfudtest(int argc, char **argv)
{
    rt_uint32_t base = 0;
    static uint8_t wr[512], rd[512];
    rt_uint32_t i;
    int errors = 0;

    if (argc >= 2)
        base = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);

    if (spi3_sfud_get(RT_NULL, RT_NULL) != RT_EOK || g_sfud == RT_NULL)
    {
        rt_kprintf("[sfud] !! SFUD 未就绪，先跑 sfudprobe\n");
        return;
    }

    /* 造数据：用地址相关值，避免全 0 造成的假通过 */
    for (i = 0; i < sizeof(wr); i++)
        wr[i] = (uint8_t)((i * 13 + 0x5A) & 0xFF);

    if (sfud_erase(g_sfud, base, 4096) != SFUD_SUCCESS)
    {
        rt_kprintf("[sfud] !! erase @0x%X fail\n", (unsigned)base);
        return;
    }

    if (sfud_write(g_sfud, base, sizeof(wr), wr) != SFUD_SUCCESS)
    {
        rt_kprintf("[sfud] !! write @0x%X fail\n", (unsigned)base);
        return;
    }

    if (sfud_read(g_sfud, base, sizeof(rd), rd) != SFUD_SUCCESS)
    {
        rt_kprintf("[sfud] !! read @0x%X fail\n", (unsigned)base);
        return;
    }

    for (i = 0; i < sizeof(rd); i++)
    {
        if (rd[i] != wr[i])
        {
            if (errors < 8)
                rt_kprintf("[sfud] mismatch @[%u]: wr=0x%02X rd=0x%02X\n",
                           i, wr[i], rd[i]);
            errors++;
        }
    }

    if (errors == 0)
        rt_kprintf("[sfud]   API loopback PASS: %u bytes 写读一致 @0x%X\n",
                   (unsigned)sizeof(wr), (unsigned)base);
    else
        rt_kprintf("[sfud]   API loopback FAIL: %d/%u 不符\n",
                   errors, (unsigned)sizeof(wr));
}
MSH_CMD_EXPORT(sfudtest, SFUD API loopback test: sfudtest [addr]);
