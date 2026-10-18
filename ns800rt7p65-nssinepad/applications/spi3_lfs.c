/**
 * @file    spi3_lfs.c
 * @brief   SPI3 外置 NOR Flash 的 LittleFS 文件系统（msh: lfsmount / lfsformat / lfstest）
 *
 * 需要本文件的原因：
 *   SFUD 产出的是块设备（RT_Device_Class_Block），而 LittleFS 包
 *   （packages/littlefs-latest/dfs_lfs.c）硬性要求 MTD NOR 设备：
 *       if (DFS_LFS_MNT_DEV(dfs)->type != RT_Device_Class_MTD) -> must be MTD!
 *   它通过 rt_mtd_nor_read/write/erase_block 访问 flash，并读取
 *   mtd_nor->block_size / block_start / block_end 计算容量。
 *   因此这里手写一层 SFUD -> MTD NOR 桥接，把 SFUD 包装成 MTD 设备。
 *
 * 不走 FAL 的原因：官方 system/fal 包的 Kconfig 写死
 *   depends on RT_VER_NUM < 0x40100，本 BSP 为 0x50300（RT 5.3.0），无法选用。
 *   且 FAL 的作用正是分区管理与 SFUD 适配，用一层桥接即可替代。
 *
 * 命名：
 *   spi30      SPI 从设备（CS 挂在其上）
 *   spiflash0  SFUD 注册的块设备（由 sfudprobe 建立）
 *   norflash0  本文件注册的 MTD NOR 设备（LittleFS 使用）
 *
 * LittleFS 占满整片 16 MB（block_size = 4096，共 4096 个块），挂载点默认 "/"。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>

/* DFS 头文件：dfs_mount / dfs_mkfs 在 dfs_fs.h；
 * open/read/write/close 为 POSIX 风格，声明在 unistd.h 与 fcntl.h
 * （本 BSP 未开 libc，这两个头由 components/libc/compilers/common 提供，
 *   用法与 components/finsh/msh_file.c 一致）。 */
#include <dfs_fs.h>
#include <unistd.h>
#include <fcntl.h>

#include "spi3_flash.h"

/* 设备名 SPI3_MTD_NAME 在 spi3_flash.h 里；这里只补两个本地别名 */
#define LFS_MOUNT_POINT    SPI3_LFS_MNT
#define LFS_FS_TYPE        "lfs"

/* SFUD -> MTD NOR 桥接。
 * 每个回调均从 dev->parent.user_data 取 sfud_flash*，
 * 因此同一份代码可桥接多片 flash（当前只用一片）。 */

/**
 * @brief  从 MTD 设备取回 sfud_flash 句柄
 * @param  dev  MTD NOR 设备
 * @return sfud_flash 句柄
 */
static sfud_flash *mtd_flash(struct rt_mtd_nor_device *dev)
{
    return (sfud_flash *)dev->parent.user_data;
}

/**
 * @brief  MTD 读 ID 接口，返回 JEDEC ID
 * @param  dev  MTD NOR 设备
 * @return JEDEC ID，格式 mf_id << 16 | type_id << 8 | capacity_id
 * @note   MTD 层签名写的是 rt_err_t，但 rt_mtd_nor_read_id() 将其作为
 *         rt_uint32_t 返回，这是 RT-Thread 的历史遗留；两者皆为 32 位整型，
 *         直接打包返回即可。
 */
static rt_err_t sfud_mtd_read_id(struct rt_mtd_nor_device *dev)
{
    sfud_flash *sf = mtd_flash(dev);

    return (rt_err_t)(((uint32_t)sf->chip.mf_id << 16) |
                      ((uint32_t)sf->chip.type_id << 8) |
                       (uint32_t)sf->chip.capacity_id);
}

/**
 * @brief  MTD 读接口，转调 sfud_read
 * @param  dev     MTD NOR 设备
 * @param  offset  字节偏移
 * @param  data    输出缓冲
 * @param  length  读取长度，字节
 * @return 实际读到的字节数，0 表示失败
 * @note   SFUD 无页对齐要求，可直接连续读。
 */
static rt_ssize_t sfud_mtd_read(struct rt_mtd_nor_device *dev, rt_off_t offset,
                                rt_uint8_t *data, rt_size_t length)
{
    if (sfud_read(mtd_flash(dev), (uint32_t)offset, length, data) != SFUD_SUCCESS)
        return 0;                   /* 返回实际读到的字节数，0 表示失败 */
    return (rt_ssize_t)length;
}

/**
 * @brief  MTD 写接口，转调 sfud_write
 * @param  dev     MTD NOR 设备
 * @param  offset  字节偏移
 * @param  data    待写数据
 * @param  length  写入长度，字节
 * @return 实际写入的字节数，0 表示失败
 * @note   LittleFS 保证写前该块已擦除，故直接 page program（NOR 只能 1 变 0）。
 */
static rt_ssize_t sfud_mtd_write(struct rt_mtd_nor_device *dev, rt_off_t offset,
                                 const rt_uint8_t *data, rt_size_t length)
{
    if (sfud_write(mtd_flash(dev), (uint32_t)offset, length, data) != SFUD_SUCCESS)
        return 0;
    return (rt_ssize_t)length;
}

/**
 * @brief  MTD 擦除接口，转调 sfud_erase
 * @param  dev     MTD NOR 设备
 * @param  offset  字节偏移
 * @param  length  擦除长度，字节
 * @return RT_EOK 成功；-RT_EIO 失败
 * @note   offset/length 均为字节，sfud_erase 内部按扇区对齐处理。
 */
static rt_err_t sfud_mtd_erase_block(struct rt_mtd_nor_device *dev, rt_off_t offset,
                                     rt_size_t length)
{
    if (sfud_erase(mtd_flash(dev), (uint32_t)offset, length) != SFUD_SUCCESS)
        return -RT_EIO;
    return RT_EOK;
}

static const struct rt_mtd_nor_driver_ops sfud_mtd_ops =
{
    sfud_mtd_read_id,
    sfud_mtd_read,
    sfud_mtd_write,
    sfud_mtd_erase_block,
};

/**
 * @brief  注册 MTD NOR 设备，幂等
 * @return RT_EOK 成功；-RT_EIO SFUD 未就绪或注册失败
 */
static rt_err_t spi3_mtd_ensure(void)
{
    rt_spi_flash_device_t rtt;
    sfud_flash *sf;
    static struct rt_mtd_nor_device mtd;    /* 静态：必须常驻 */

    if (rt_device_find(SPI3_MTD_NAME) != RT_NULL)
        return RT_EOK;                      /* 已注册 */

    if (spi3_sfud_get(&sf, &rtt) != RT_EOK)
        return -RT_EIO;

    rt_memset(&mtd, 0, sizeof(mtd));
    mtd.parent.user_data = sf;              /* 回调靠它找 sfud 句柄 */
    mtd.ops          = &sfud_mtd_ops;
    mtd.block_size   = sf->chip.erase_gran; /* 4096：MTD 的块即 flash 擦除粒度 */
    mtd.block_start  = 0;
    /* block_end 是块个数而非字节数（dfs_lfs.c 中乘以 block_size 得到总容量） */
    mtd.block_end    = sf->chip.capacity / sf->chip.erase_gran;

    return rt_mtd_nor_register_device(SPI3_MTD_NAME, &mtd);
}

/* 幂等挂载入口（公共）。
 * 不采用开机一次性挂载并依赖顺序的原因：SPI 总线为 INIT_BOARD_EXPORT，
 * dfs_lfs 注册 "lfs" 为 INIT_COMPONENT_EXPORT，而 LVGL 线程为
 * INIT_ENV_EXPORT，与自动挂载同级，顺序不保证。故把挂载封装为幂等函数，
 * 由需要者先行调用（壁纸加载、msh 命令均调用它），顺序即无关紧要。 */
static rt_bool_t g_lfs_mounted = RT_FALSE;

/**
 * @brief  挂载 LittleFS，幂等
 * @return RT_EOK 成功；-RT_EIO MTD 注册失败；-RT_ERROR 挂载失败
 */
rt_err_t spi3_lfs_ensure_mounted(void)
{
    if (g_lfs_mounted)
        return RT_EOK;

    if (spi3_mtd_ensure() != RT_EOK)
        return -RT_EIO;

    if (dfs_mount(SPI3_MTD_NAME, LFS_MOUNT_POINT, LFS_FS_TYPE, 0, RT_NULL) != 0)
        return -RT_ERROR;

    g_lfs_mounted = RT_TRUE;
    return RT_EOK;
}

/**
 * @brief  开机自动挂载，失败不致命
 * @return RT_EOK
 * @note   失败可能是首次尚未格式化。
 */
static int lfs_automount(void)
{
    if (spi3_lfs_ensure_mounted() == RT_EOK)
        rt_kprintf("[lfs] auto-mounted '%s' at '%s'\n", SPI3_MTD_NAME, LFS_MOUNT_POINT);
    else
        rt_kprintf("[lfs] 外置 flash 未挂载（首次使用请跑 lfsformat）\n");
    return RT_EOK;
}
INIT_ENV_EXPORT(lfs_automount);

/* msh 命令 lfsmount：挂载已有 LittleFS，不格式化。用法 lfsmount [path] */

/**
 * @brief  msh 命令 lfsmount：挂载已有 LittleFS，不格式化
 *         用法 lfsmount [path]
 * @param  path  挂载点，默认 SPI3_LFS_MNT
 */
static void lfsmount(int argc, char **argv)
{
    const char *mnt = (argc >= 2) ? argv[1] : LFS_MOUNT_POINT;

    if (spi3_lfs_ensure_mounted() == RT_EOK)
    {
        rt_kprintf("[lfs]   mounted '%s' at '%s'\n", SPI3_MTD_NAME, mnt);
        rt_kprintf("[lfs] 试试：ls %s\n", mnt);
    }
    else
    {
        rt_kprintf("[lfs]   mount 失败\n");
        rt_kprintf("       若这是第一次用，分区还没格式化 → 跑 lfsformat\n");
        rt_kprintf("       （注意 lfsformat 会清空整片外置 flash！）\n");
    }
}
MSH_CMD_EXPORT(lfsmount, mount LittleFS on SPI3 flash: lfsmount [path]);

/* msh 命令 lfsformat：格式化整片外置 flash 后再挂载。
 * 破坏性操作：清空整个 norflash0（16 MB）。用法 lfsformat */

/**
 * @brief  msh 命令 lfsformat：格式化整片外置 flash 并挂载
 *         用法 lfsformat
 * @note   破坏性操作，会清空整个 norflash0（16 MB）；已挂载时先卸载再 mkfs。
 */
static void lfsformat(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (spi3_mtd_ensure() != RT_EOK)
    {
        rt_kprintf("[lfs] !! MTD 注册失败\n");
        return;
    }

    /* 已挂载要先卸载，否则 mkfs 会失败 */
    if (g_lfs_mounted)
    {
        dfs_unmount(LFS_MOUNT_POINT);
        g_lfs_mounted = RT_FALSE;
    }

    rt_kprintf("[lfs]    即将格式化 '%s'（清空整片外置 16MB flash）\n", SPI3_MTD_NAME);

    if (dfs_mkfs(LFS_FS_TYPE, SPI3_MTD_NAME) != 0)
    {
        rt_kprintf("[lfs] !! mkfs 失败\n");
        return;
    }
    rt_kprintf("[lfs] mkfs ok\n");

    if (spi3_lfs_ensure_mounted() == RT_EOK)
        rt_kprintf("[lfs]   formatted & mounted at '%s'\n", LFS_MOUNT_POINT);
    else
        rt_kprintf("[lfs] !! mkfs 后挂载失败\n");
}
MSH_CMD_EXPORT(lfsformat, format & mount LittleFS on SPI3 flash (DESTRUCTIVE));

/* msh 命令 lfstest：写文件 -> 读回 -> 比对。
 * 用法 lfstest [path]，默认 /lfstest.bin
 * 写入 8 KB 伪随机数据（跨多个 4 KB 块），读回后逐字节比对。 */
#define LFS_TEST_SIZE  (8 * 1024)       /* 测试文件大小，字节 */

/**
 * @brief  msh 命令 lfstest：LittleFS 写读一致性测试
 *         用法 lfstest [path]
 * @param  path  测试文件路径，默认 /lfstest.bin
 * @note   写入 8 KB 伪随机数据（跨多个 4 KB 块），读回后逐字节比对。
 */
static void lfstest(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : "/lfstest.bin";
    static uint8_t wr[512], rd[512];
    int fd, i, errors = 0;
    rt_uint32_t off, seed = 0x12345678;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        /* 没挂载就先挂（幂等），再试一次 */
        if (spi3_lfs_ensure_mounted() == RT_EOK)
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    }
    if (fd < 0)
    {
        rt_kprintf("[lfs] !! open('%s') for write 失败 (%d)\n", path, fd);
        rt_kprintf("       先跑 lfsformat 初始化（会清空外置 flash）\n");
        return;
    }

    for (off = 0; off < LFS_TEST_SIZE; off += sizeof(wr))
    {
        for (i = 0; i < (int)sizeof(wr); i++)
        {
            /* xorshift：确定性序列，相邻字节差异大，避免全 0 造成的假通过 */
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            wr[i] = (uint8_t)seed;
        }
        if (write(fd, wr, sizeof(wr)) != (int)sizeof(wr))
        {
            rt_kprintf("[lfs] !! write 失败 @off %u\n", (unsigned)off);
            close(fd);
            return;
        }
    }
    close(fd);
    rt_kprintf("[lfs] wrote %d bytes to '%s'\n", LFS_TEST_SIZE, path);

    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        rt_kprintf("[lfs] !! open 读失败 (%d)\n", fd);
        return;
    }

    seed = 0x12345678;              /* 重置，重放同一序列 */
    for (off = 0; off < LFS_TEST_SIZE; off += sizeof(rd))
    {
        if (read(fd, rd, sizeof(rd)) != (int)sizeof(rd))
        {
            rt_kprintf("[lfs] !! read 短读 @off %u\n", (unsigned)off);
            close(fd);
            return;
        }
        for (i = 0; i < (int)sizeof(rd); i++)
        {
            uint8_t exp;
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            exp = (uint8_t)seed;
            if (rd[i] != exp)
            {
                if (errors < 8)
                    rt_kprintf("[lfs] mismatch @%u: exp=0x%02X got=0x%02X\n",
                               (unsigned)(off + i), exp, rd[i]);
                errors++;
            }
        }
    }
    close(fd);

    if (errors == 0)
        rt_kprintf("[lfs]   PASS: %d bytes 写读一致\n", LFS_TEST_SIZE);
    else
        rt_kprintf("[lfs]   FAIL: %d 字节不符\n", errors);
}
MSH_CMD_EXPORT(lfstest, LittleFS write/read test: lfstest [path]);
