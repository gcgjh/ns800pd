/**
 * @file    wallpaper_store.c
 * @brief   壁纸存取：240x320 全分辨率壁纸存外置 flash，开机读入 SRAM
 *
 * 早期版本把壁纸缩成 120x320 再横向放大 2 倍，因为内部 flash 只剩 156KB，
 * 放不下 150KB 的全宽图，代价是画面横向模糊。接入 LittleFS（16MB 外置 NOR）
 * 后图片可放外置 flash，但 LVGL 显示需要可寻址内存，仍须在 RAM 中保留一份。
 *
 * 缓冲放在 .wall_buf 段：150KB 跨越 0x20120000 边界（SRAM1 只有 128K），
 * 因此 link.lds 把 SRAM1 与 SRAM2 合并为 SRAM12（256K），LVGL 池缩到 64K
 * 放在头部，壁纸缓冲紧随其后：
 *     .lvgl_pool  64K   @ 0x20100000
 *     .wall_buf  150K   @ 0x20110000
 * 实测 LVGL 池只用约 16K（msh lvmem），64K 留了 4 倍余量。
 *
 * 图片无法编进固件（424K + 150K > 512K），改用 pyocd 直接写 SRAM：
 *     1) 主机：pyocd load /tmp/wallpaper.bin@<wallstatus 打印的地址>
 *     2) 设备：msh wallsave <hash>
 * wallsave 先以 FNV-1a 校验和与主机给的对比，防止 pyocd 未写入却存入垃圾。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <dfs_fs.h>
#include <unistd.h>
#include <fcntl.h>

#include "spi3_flash.h"
#include "wallpaper_store.h"

/* 150KB 缓冲，置于 .wall_buf 段（见 link.lds）。
 * NOLOAD 表示不占 flash、启动不清零，开机由 wall_load_file() 填充。 */
static uint8_t g_wall_buf[WALL_BYTES] __attribute__((section(".wall_buf")));

static rt_bool_t g_tried;   /* 是否已尝试加载（失败也不反复重试） */
static rt_bool_t g_ok;      /* 缓冲中是否为有效壁纸数据 */

/**
 * @brief  计算 FNV-1a 32 位哈希
 * @param  p 数据首地址
 * @param  n 字节数
 * @return 哈希值
 * @note   主机与设备两侧算法必须一致，具体见 make_wallpaper.py。
 */
rt_uint32_t wallpaper_hash(const uint8_t *p, rt_size_t n)
{
    rt_uint32_t h = 2166136261u;        /* FNV-1a 32 位 offset basis */

    while (n--)
    {
        h ^= *p++;
        h *= 16777619u;                 /* FNV prime */
    }
    return h;
}

/**
 * @brief  从 /wallpaper.bin 读入整张壁纸到 g_wall_buf
 * @return RT_TRUE 读满 WALL_BYTES；RT_FALSE 挂载失败、打开失败或长度不足
 */
static rt_bool_t wall_load_file(void)
{
    int fd;
    rt_ssize_t n;
    rt_size_t total = 0;

    if (spi3_lfs_ensure_mounted() != RT_EOK)
        return RT_FALSE;

    fd = open(WALL_PATH, O_RDONLY);
    if (fd < 0)
        return RT_FALSE;

    while (total < WALL_BYTES)
    {
        n = read(fd, g_wall_buf + total, WALL_BYTES - total);
        if (n <= 0)
            break;
        total += (rt_size_t)n;
    }
    close(fd);

    if (total != WALL_BYTES)
    {
        rt_kprintf("[wall] %s 只读到 %u / %u 字节\n",
                   WALL_PATH, (unsigned)total, (unsigned)WALL_BYTES);
        return RT_FALSE;
    }

    return RT_TRUE;
}

/**
 * @brief  取壁纸 RGB565 数据指针
 * @return 缓冲首地址；载入失败返回 RT_NULL，调用方应退回纯色背景
 * @note   首次调用时从 WALL_PATH 载入，之后复用缓存，返回的指针常驻，不要 free。
 */
const uint8_t *wallpaper_get(void)
{
    if (!g_tried)
    {
        g_tried = RT_TRUE;
        g_ok = wall_load_file();
        rt_kprintf("[wall] %s (%u B @ %p)\n",
                   g_ok ? "已从外置 flash 载入壁纸" : "载入失败 → 退回纯色背景",
                   (unsigned)WALL_BYTES, (void *)g_wall_buf);
    }
    return g_ok ? g_wall_buf : RT_NULL;
}

/**
 * @brief  丢弃缓存，使下次 wallpaper_get() 重新加载
 * @note   重新灌图后调用。
 */
void wallpaper_invalidate(void)
{
    g_tried = RT_FALSE;
    g_ok    = RT_FALSE;
}

/* wallstatus —— 显示壁纸缓冲地址、状态与 hash，并给出灌图步骤。用法: wallstatus */
static void wallstatus(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("[wall] 缓冲 @ %p, %u 字节 (%u KB, .wall_buf 段)\n",
               (void *)g_wall_buf, (unsigned)WALL_BYTES,
               (unsigned)(WALL_BYTES / 1024));
    rt_kprintf("[wall] 分辨率 %dx%d RGB565, 缩放 %d 倍\n",
               WALL_SRC_W, WALL_SRC_H, WALL_ZOOM);
    rt_kprintf("[wall] 状态: tried=%d ok=%d, 文件 %s %s\n",
               g_tried, g_ok, WALL_PATH, g_ok ? "已载入" : "未载入");
    rt_kprintf("[wall] 当前缓冲内容 hash = 0x%08X\n",
               (unsigned)wallpaper_hash(g_wall_buf, WALL_BYTES));
    rt_kprintf("[wall] 灌图步骤：\n");
    rt_kprintf("       pyocd load wallpaper.bin@0x%08X\n", (unsigned)(uintptr_t)g_wall_buf);
    rt_kprintf("       然后 msh 里跑 wallsave <上面的 hash>\n");
}
MSH_CMD_EXPORT(wallstatus, show wallpaper buffer address/state/hash);

/* wallsave —— 把 SRAM 缓冲存入 /wallpaper.bin。用法: wallsave [hash]
 * hash 可选：给出时先比对，防止 pyocd 未写入就存入垃圾。
 * 会覆盖同名文件，仅影响外置 flash。 */
static void wallsave(int argc, char **argv)
{
    static uint8_t chk[512];            /* 回读比对的小块，避免再申请 150KB */
    rt_uint32_t sum, expect = 0;
    int fd;
    rt_ssize_t n;
    rt_size_t total = 0, i;
    int errors = 0;

    sum = wallpaper_hash(g_wall_buf, WALL_BYTES);

    if (argc >= 2)
    {
        expect = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);
        if (expect != sum)
        {
            rt_kprintf("[wall]   hash 不符：主机给 0x%08X，缓冲实际 0x%08X\n",
                       (unsigned)expect, (unsigned)sum);
            rt_kprintf("[wall]    → pyocd 没写进去？确认地址是 wallstatus 打印的那个\n");
            return;
        }
        rt_kprintf("[wall] hash 校验通过 (0x%08X)\n", (unsigned)sum);
    }
    else
    {
        rt_kprintf("[wall]    未给 hash，跳过校验（当前缓冲 hash = 0x%08X）\n",
                   (unsigned)sum);
    }

    if (spi3_lfs_ensure_mounted() != RT_EOK)
    {
        rt_kprintf("[wall] !! 文件系统没挂上（先跑 lfsformat）\n");
        return;
    }

    fd = open(WALL_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        rt_kprintf("[wall] !! 建文件 '%s' 失败 (%d)\n", WALL_PATH, fd);
        return;
    }

    while (total < WALL_BYTES)
    {
        n = write(fd, g_wall_buf + total, WALL_BYTES - total);
        if (n <= 0)
        {
            rt_kprintf("[wall] !! 写失败 @ %u\n", (unsigned)total);
            close(fd);
            return;
        }
        total += (rt_size_t)n;
    }
    close(fd);
    rt_kprintf("[wall] 已写 %u 字节 → %s\n", (unsigned)total, WALL_PATH);

    /* 回读校验：分块比对，避免再申请一块 150KB 内存 */
    fd = open(WALL_PATH, O_RDONLY);
    if (fd < 0)
    {
        rt_kprintf("[wall] !! 回读时打不开\n");
        return;
    }
    total = 0;
    while (total < WALL_BYTES)
    {
        rt_size_t want = WALL_BYTES - total;
        if (want > sizeof(chk)) want = sizeof(chk);

        n = read(fd, chk, want);
        if (n <= 0) break;

        for (i = 0; i < (rt_size_t)n; i++)
        {
            if (chk[i] != g_wall_buf[total + i])
            {
                if (errors < 8)
                    rt_kprintf("[wall] mismatch @%u: exp=0x%02X got=0x%02X\n",
                               (unsigned)(total + i), g_wall_buf[total + i], chk[i]);
                errors++;
            }
        }
        total += (rt_size_t)n;
    }
    close(fd);

    if (errors == 0 && total == WALL_BYTES)
    {
        rt_kprintf("[wall]   回读校验 PASS：%u 字节一致\n", (unsigned)total);
        wallpaper_invalidate();         /* 令下次 wallpaper_get() 重新读取 */
    }
    else
    {
        rt_kprintf("[wall]   校验 FAIL：读过 %u 字节，%d 处不符\n",
                   (unsigned)total, errors);
    }
}
MSH_CMD_EXPORT(wallsave, save SRAM wallpaper buffer to /wallpaper.bin: wallsave [hash]);
