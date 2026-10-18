/**
 * @file    spi3_probe.c
 * @brief   SPI3 外置 NOR Flash 驱动与自检（msh: spi3probe / spiflash）
 *
 * 本文件为最小实现：不依赖 SFUD / FAL，直接用 RT-Thread 的
 * rt_spi_transfer_message 拼接 SPI 时序，涵盖 NOR Flash 最基本的
 * Read ID / Read Status / Write Enable / Read / Page Program / Sector Erase
 * 六种操作，并用回环测试验证写入与读出是否一致。
 *
 * 用途：
 *   1. 硬件调试：接线后先运行 spi3probe 查看 ID，再运行 spiflash 回环，
 *      确认四线连接可靠；
 *   2. 在引入 SFUD / LittleFS 之前做前置验证，若本层不通过，上框架只会更难定位。
 *
 * 接线（SPI3 的 SCK/MOSI/MISO 由 BSP 预置于 drv_spi.c 的 BSP_USING_SPI3 段）：
 *
 *   信号        GPIO      PORT.PIN       复用      接外置 Flash
 *   ---------------------------------------------------------------
 *   SCK        GPIO_52    GPIOB.PIN_20   ALT6       CLK
 *   MOSI       GPIO_50    GPIOB.PIN_18   ALT6       DI / IO0 / MOSI
 *   MISO       GPIO_51    GPIOB.PIN_19   ALT6       DO / IO1 / MISO
 *   CS         GPIO_53    GPIOB.PIN_21   GPIO 输出  CS# / NSS
 *   VCC                  3V3                        VCC / 3V3
 *   GND                  GND                        GND
 *   （WP#/HOLD# 可直接上拉 3V3，需要写保护时另接 GPIO）
 *
 * SPI3 引脚选择说明（勿随意改动）：
 *   drv_spi.c 原模板写的 GPIOC_0/1/2 + ALT7 有误：GPIO_64/65/66 的复用表中
 *   没有 SPI3（ALT15 为 SPI2、ALT6 为 UART1）。SPI3 仅存在以下引脚组：
 *     ALT14: GPIO_20/21/22  -> GPIO_22 已作 LCD 背光，排除
 *     ALT6 : GPIO_50/51/52  -> 全空闲，采用
 *     ALT15: GPIO_69/70/71  -> GPIO_69 被 board.c 初始化为 LED2，排除
 *   已占用引脚集合（改 CS 时需避开）：{1, 6, 7, 12, 13, 40, 41, 68, 69}
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <rthw.h>

#define SPI3_BUS_NAME      "spi3"
#define SPI3_FLASH_NAME    "spiflash0"

/* CS 引脚号：GPIO_53 = GPIOB_21（由实物接线确定，改动只需改此宏）。
 * 注意不要与历史误写的 GPIO_73 混淆。 */
#define SPI3_FLASH_CS_PIN  53

/* NOR Flash 命令集（通用 JEDEC 标准，XTX / Winbond / GD 通用） */
#define CMD_WRITE_ENABLE   0x06
#define CMD_READ_STATUS1   0x05
#define CMD_READ_DATA      0x03
#define CMD_PAGE_PROGRAM   0x02
#define CMD_SECTOR_ERASE   0x20        /* 4 KB 扇区擦除 */
#define CMD_JEDEC_ID       0x9F
#define CMD_READ_SFDP      0x5A        /* 部分新片支持，用于读参数表 */

/* 状态寄存器位 */
#define SR1_BUSY           0x01        /* WIP：1 = 内部忙 */
#define SR1_WEL            0x02        /* 写使能锁存 */

/* 几何参数（XTX 16 MB / 128 Mbit 标准 NOR） */
#define FLASH_PAGE_SIZE    256
#define FLASH_SECTOR_SIZE  4096
#define FLASH_TOTAL_SIZE   (16UL * 1024 * 1024)

/* SPI 从设备对象：rt_spi_bus_attach_device_cspin() 的 dev 参数要求
 * 传入已分配好的对象（函数内部会调用 rt_spidev_device_init，
 * dev 为 NULL 会触发断言失败），并非出参。
 * 因此这里必须静态备好一块，不能传 NULL。 */
static struct rt_spi_device g_spi3_flash_dev;
static struct rt_spi_device *g_flash;      /* 挂载完成后指向它，供后续操作复用 */

/* 底层 SPI 收发 */

/**
 * @brief  一次完整收发：连发 tx[0..n)，把收到的字节放回 rx
 * @param  tx  发送缓冲
 * @param  rx  接收缓冲，可为 NULL 表示丢弃
 * @param  n   字节数
 */
static void spi_xfer(const uint8_t *tx, uint8_t *rx, rt_size_t n)
{
    struct rt_spi_message msg;

    msg.send_buf   = tx;
    msg.recv_buf   = rx;
    msg.length     = n;
    msg.cs_take    = 1;
    msg.cs_release = 1;
    msg.next       = RT_NULL;
    rt_spi_transfer_message(g_flash, &msg);
}

/**
 * @brief  发送一个命令字节后单独读 len 字节（如 Read Data / Read Status）
 * @param  cmd  命令字节
 * @param  rx   接收缓冲
 * @param  len  读取长度，字节
 */
static void spi_cmd_read(uint8_t cmd, uint8_t *rx, rt_size_t len)
{
    struct rt_spi_message m1, m2;

    m1.send_buf   = &cmd;
    m1.recv_buf   = RT_NULL;
    m1.length     = 1;
    m1.cs_take    = 1;
    m1.cs_release = 0;
    m1.next       = &m2;

    m2.send_buf   = RT_NULL;
    m2.recv_buf   = rx;
    m2.length     = len;
    m2.cs_take    = 0;
    m2.cs_release = 1;
    m2.next       = RT_NULL;

    rt_spi_transfer_message(g_flash, &m1);
}

/* Flash 原语 */

/**
 * @brief  读取状态寄存器 SR1
 * @return SR1 值
 */
static uint8_t flash_read_status(void)
{
    uint8_t sr = 0;

    spi_cmd_read(CMD_READ_STATUS1, &sr, 1);
    return sr;
}

/**
 * @brief  等待 WIP 位清零
 * @param  timeout_ms  超时，毫秒
 * @return RT_EOK 就绪；-RT_ETIMEOUT 超时
 * @note   擦除/写入需要时间，整片最大实测约 3 s，此处按调用方给的超时保护。
 */
static rt_err_t flash_wait_ready(rt_int32_t timeout_ms)
{
    rt_tick_t t0 = rt_tick_get();

    while (flash_read_status() & SR1_BUSY)
    {
        if ((rt_tick_get() - t0) > rt_tick_from_millisecond(timeout_ms))
            return -RT_ETIMEOUT;
        rt_thread_mdelay(1);
    }
    return RT_EOK;
}

/**
 * @brief  发送 Write Enable 命令
 */
static void flash_write_enable(void)
{
    uint8_t c = CMD_WRITE_ENABLE;

    spi_xfer(&c, RT_NULL, 1);
}

/**
 * @brief  读任意长度
 * @param  addr  起始地址
 * @param  buf   输出缓冲
 * @param  len   读取长度，字节
 * @note   无需按页对齐，直接连续读。
 */
static void flash_read(rt_uint32_t addr, uint8_t *buf, rt_size_t len)
{
    uint8_t cmd[4];
    struct rt_spi_message m1, m2;

    cmd[0] = CMD_READ_DATA;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);

    m1.send_buf   = cmd;
    m1.recv_buf   = RT_NULL;
    m1.length     = 4;
    m1.cs_take    = 1;
    m1.cs_release = 0;
    m1.next       = &m2;

    m2.send_buf   = RT_NULL;
    m2.recv_buf   = buf;
    m2.length     = len;
    m2.cs_take    = 0;
    m2.cs_release = 1;
    m2.next       = RT_NULL;

    rt_spi_transfer_message(g_flash, &m1);
}

/**
 * @brief  写一页
 * @param  addr  起始地址
 * @param  buf   待写数据
 * @param  len   写入长度，字节，范围 1 到 256
 * @return RT_EOK 成功；-RT_EINVAL 长度或跨页非法；-RT_ERROR 写使能失败
 * @note   写入长度不能超过一页（256 B），且不能跨页边界，调用方需自行拆分。
 */
static rt_err_t flash_page_program(rt_uint32_t addr, const uint8_t *buf, rt_size_t len)
{
    uint8_t cmd[4];
    struct rt_spi_message m1, m2;
    rt_err_t r;

    if (len == 0 || len > FLASH_PAGE_SIZE) return -RT_EINVAL;
    /* 跨页会回卷覆盖，必须调用方拆好 */
    if ((addr / FLASH_PAGE_SIZE) != ((addr + len - 1) / FLASH_PAGE_SIZE))
        return -RT_EINVAL;

    flash_write_enable();
    if ((flash_read_status() & SR1_WEL) == 0)
        return -RT_ERROR;               /* 写使能未锁上，后续写入无效 */

    cmd[0] = CMD_PAGE_PROGRAM;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);

    m1.send_buf   = cmd;
    m1.recv_buf   = RT_NULL;
    m1.length     = 4;
    m1.cs_take    = 1;
    m1.cs_release = 0;
    m1.next       = &m2;

    m2.send_buf   = buf;
    m2.recv_buf   = RT_NULL;
    m2.length     = len;
    m2.cs_take    = 0;
    m2.cs_release = 1;
    m2.next       = RT_NULL;

    rt_spi_transfer_message(g_flash, &m1);

    r = flash_wait_ready(500);
    return r;
}

/**
 * @brief  擦除一个 4 KB 扇区
 * @param  addr  地址，内部会向下对齐到扇区首地址
 * @return RT_EOK 成功；-RT_ERROR 写使能失败；-RT_ETIMEOUT 超时
 * @note   NOR 只能 1 变 0，写前必须擦成 0xFF。
 */
static rt_err_t flash_sector_erase(rt_uint32_t addr)
{
    uint8_t cmd[4];

    addr &= ~(FLASH_SECTOR_SIZE - 1);   /* 对齐到扇区首地址 */

    flash_write_enable();
    if ((flash_read_status() & SR1_WEL) == 0)
        return -RT_ERROR;

    cmd[0] = CMD_SECTOR_ERASE;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr);
    spi_xfer(cmd, RT_NULL, 4);

    return flash_wait_ready(3000);      /* 4 KB 擦除典型 30~100 ms，留足余量 */
}

/**
 * @brief  把 SPI3 从设备挂载到总线
 * @return RT_EOK 成功；-RT_ENOSYS 总线不存在
 */
static rt_err_t spi3_flash_init(void)
{
    struct rt_spi_configuration cfg;
    rt_err_t r;

    if (g_flash) return RT_EOK;         /* 已初始化 */

    if (rt_device_find(SPI3_BUS_NAME) == RT_NULL)
        return -RT_ENOSYS;

    r = rt_spi_bus_attach_device_cspin(&g_spi3_flash_dev, SPI3_FLASH_NAME,
                                       SPI3_BUS_NAME, SPI3_FLASH_CS_PIN, RT_NULL);
    if (r != RT_EOK && rt_device_find(SPI3_FLASH_NAME) == RT_NULL)
        return r;

    /* Mode0 / 8 bit / 1 MHz：上电低速最稳，识别到 ID 后由调用方决定是否提速 */
    cfg.data_width = 8;
    cfg.max_hz     = 1 * 1000 * 1000;
    cfg.mode       = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB;
    rt_spi_configure(&g_spi3_flash_dev, &cfg);

    g_flash = &g_spi3_flash_dev;
    return RT_EOK;
}

/* msh 命令 spi3probe：读取 JEDEC ID，不动数据。用法 spi3probe */

/**
 * @brief  按厂商 ID 返回厂商名
 * @param  id  JEDEC 厂商 ID
 * @return 厂商名字符串，未知厂商返回 "unknown"
 */
static const char *jedec_vendor(uint8_t id)
{
    switch (id)
    {
    case 0xEF: return "Winbond";
    case 0xC8: return "GigaDevice";
    case 0x20: return "Micron/ST";
    case 0x1F: return "Adesto/Atmel";
    case 0x01: return "Spansion";
    case 0x0B: return "XTX";
    case 0x5E: return "Zbit";
    case 0xA1: return "Fudan Micro";
    case 0xBF: return "SST";
    default:   return "unknown";
    }
}

/**
 * @brief  msh 命令 spi3probe：读取外置 flash 的 JEDEC ID 与容量
 *         用法 spi3probe
 */
static void spi3probe(int argc, char **argv)
{
    uint8_t id[3] = {0, 0, 0};
    rt_uint32_t cap = 0;

    (void)argc;
    (void)argv;

    if (spi3_flash_init() != RT_EOK)
    {
        rt_kprintf("[spi3] init FAIL —— 检查 rtconfig.h 的 BSP_USING_SPI3 "
                   "和 drv_spi.c 里 spi_config[] 的引脚复用\n");
        return;
    }
    rt_kprintf("[spi3] bus ok, CS=GPIO_%d\n", SPI3_FLASH_CS_PIN);

    spi_cmd_read(CMD_JEDEC_ID, id, 3);
    rt_kprintf("[spi3] JEDEC ID = %02X %02X %02X  (%s)\n",
               id[0], id[1], id[2], jedec_vendor(id[0]));

    if (id[0] == 0x00 || id[0] == 0xFF)
    {
        rt_kprintf("[spi3] !! 全 0 或全 FF —— 接线/CS/复用 之一有问题：\n");
        rt_kprintf("        全 00：MISO 被拉低或没接\n");
        rt_kprintf("        全 FF：MISO 悬空（没供电 / CS 没选中 / SCK 不出波形）\n");
        return;
    }

    if (id[2] >= 8 && id[2] <= 32)
        cap = 1UL << id[2];
    rt_kprintf("[spi3] capacity = %u KB (%u MB)  [id[2]=0x%02X]\n",
               (unsigned)(cap / 1024), (unsigned)(cap / 1024 / 1024), id[2]);
    rt_kprintf("[spi3] OK\n");
}
MSH_CMD_EXPORT(spi3probe, probe SPI3 external flash: read JEDEC ID);

/* msh 命令 spiflash：回环读写自检。
 * 用法：
 *   spiflash          对地址 0 做完整回环（擦 -> 写 -> 读 -> 比对 -> 报告）
 *   spiflash <addr>   指定起始地址（会擦除该地址所在扇区）
 *   spiflash info     只打印状态寄存器，不动数据
 * 会破坏目标扇区数据，默认只用地址 0，请确保该处为空闲区。 */
#define TEST_LEN   512                  /* 跨 2 页，验证跨页写入拆分 */

/**
 * @brief  msh 命令 spiflash：SPI3 NOR flash 回环读写自检
 *         用法 spiflash [addr|info]
 * @param  addr  起始地址，默认 0，会擦除该地址所在扇区
 * @note   会破坏目标扇区数据；写入数据与地址相关，避免全 0 造成的假通过。
 */
static void spiflash(int argc, char **argv)
{
    rt_uint32_t base = 0;
    uint8_t wr[TEST_LEN], rd[TEST_LEN];
    rt_uint32_t i, off;
    int errors = 0;
    rt_err_t r;

    if (spi3_flash_init() != RT_EOK)
    {
        rt_kprintf("[flash] init FAIL\n");
        return;
    }

    if (argc >= 2 && rt_strcmp(argv[1], "info") == 0)
    {
        uint8_t sr = flash_read_status();
        rt_kprintf("[flash] SR1 = 0x%02X (WIP=%d WEL=%d)\n",
                   sr, !!(sr & SR1_BUSY), !!(sr & SR1_WEL));
        return;
    }
    if (argc >= 2)
        base = (rt_uint32_t)strtoul(argv[1], RT_NULL, 0);

    if (base + TEST_LEN > FLASH_TOTAL_SIZE)
    {
        rt_kprintf("[flash] addr 0x%06X 超出容量\n", (unsigned)base);
        return;
    }

    rt_kprintf("[flash] loopback @ 0x%06X, %d bytes (占 %d 个扇区)\n",
               (unsigned)base, TEST_LEN,
               (TEST_LEN + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE);

    /* 1) 造数据：用地址相关值，避免全 0 造成的假通过 */
    for (i = 0; i < TEST_LEN; i++)
        wr[i] = (uint8_t)((i * 7 + 0x3C) & 0xFF);

    /* 2) 擦除覆盖到的扇区 */
    for (off = 0; off < TEST_LEN; off += FLASH_SECTOR_SIZE)
    {
        r = flash_sector_erase(base + off);
        if (r != RT_EOK)
        {
            rt_kprintf("[flash] !! erase 0x%06X failed (%d)\n",
                       (unsigned)(base + off), r);
            return;
        }
    }

    /* 3) 擦后校验：必须是全 0xFF */
    flash_read(base, rd, TEST_LEN);
    for (i = 0; i < TEST_LEN; i++)
    {
        if (rd[i] != 0xFF)
        {
            rt_kprintf("[flash] !! erase 后非 0xFF @ [%u] = 0x%02X"
                       "（擦除不干净 → 供电/时序问题）\n", i, rd[i]);
            return;
        }
    }
    rt_kprintf("[flash] erase ok (全 0xFF)\n");

    /* 4) 按页写入（跨页要拆） */
    for (off = 0; off < TEST_LEN; off += FLASH_PAGE_SIZE)
    {
        rt_size_t n = TEST_LEN - off;
        if (n > FLASH_PAGE_SIZE) n = FLASH_PAGE_SIZE;

        r = flash_page_program(base + off, wr + off, n);
        if (r != RT_EOK)
        {
            rt_kprintf("[flash] !! program @0x%06X failed (%d)\n",
                       (unsigned)(base + off), r);
            return;
        }
    }

    /* 5) 读回比对 */
    flash_read(base, rd, TEST_LEN);
    for (i = 0; i < TEST_LEN; i++)
    {
        if (rd[i] != wr[i])
        {
            if (errors < 8)             /* 只报前几个，避免刷屏 */
                rt_kprintf("[flash] mismatch @[%u]: wr=0x%02X rd=0x%02X\n",
                           i, wr[i], rd[i]);
            errors++;
        }
    }

    if (errors == 0)
    {
        rt_kprintf("[flash]   loopback PASS: %d bytes 写读一致\n", TEST_LEN);
        rt_kprintf("[flash] 硬件可靠，可以上 SFUD / LittleFS 了\n");
    }
    else
    {
        rt_kprintf("[flash]   loopback FAIL: %d/%d 字节不符\n", errors, TEST_LEN);
        rt_kprintf("[flash] 排查：供电不稳 / SCK 太快 / 别处也在用同一片 FLASH\n");
    }
}
MSH_CMD_EXPORT(spiflash, SPI3 NOR flash loopback test: spiflash [addr|info]);
