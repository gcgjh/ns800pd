/**
 * @file    lcd_tune.c
 * @brief   ILI9341 颜色微调：msh 命令现场调色，不必反复烧写固件。
 *
 * 照片为暖米色调，屏上却偏蓝紫。排查方向按可能性排序：
 *   1. MADCTL(0x36) 的 BGR 位（bit3）：蓝红通道互换，最典型的整体偏蓝/偏紫；
 *   2. Gamma 曲线（0xE0 / 0xE1）：影响暗部与中间调层次；
 *   3. 0xF7 的 3Gamma 控制：影响整体色偏。
 *
 * 原厂初始化中 0x36 = 0x28（bit3 = BGR = 1），而壁纸 bin 为 RGB565 小端
 * （make_wallpaper.py 按 >H 打包），两者是否匹配需实测确认。
 *
 * 用法：
 *   msh > lcdcolor                查当前设置
 *   msh > lcdcolor bgr 0|1        切 RGB/BGR（最可能有效）
 *   msh > lcdcolor inv 0|1        屏幕反显（黑白反转）
 *   msh > lcdcolor gamma 0..3     切 gamma 预设（0 默认 / 1 暖 / 2 冷 / 3 高对比）
 *   msh > lcdcolor reset          恢复厂商默认（重新走 LCD_Init 序列）
 *   msh > lcdcolor preview 0|1|2  刷纯红/绿/蓝块，判断 BGR 是否正确
 *   msh > lcdcolor refresh        调完必须执行，否则屏上不更新
 *
 * 所有参数都要配合 lcdcolor refresh 才生效：改完寄存器后帧缓冲里仍是旧像素，
 * 需强制 LVGL 重画一帧。
 */

#include "lcd_ili9341.h"
#include <rtthread.h>
#include <lvgl.h>

/* ── 寄存器收发 ──
 * 驱动内部的 LCD_WR_REG / LCD_WR_DATA 与 lcd_dev 均为 static，无法复用；
 * 这里按 rtconfig.h 中的设备名，用 rt_device_find() 重新取一次 SPI 设备。 */
static struct rt_spi_device *s_lcd_spi;

static struct rt_spi_device *lcd_spi(void)
{
    if (s_lcd_spi == RT_NULL)
        s_lcd_spi = (struct rt_spi_device *)rt_device_find(PKG_ILI_9341_SPI_DEVICE_NAME);
    return s_lcd_spi;
}

static void t_reg(uint8_t reg)
{
    if (lcd_spi() == RT_NULL) return;
    LCD_DC_CLR;
    rt_spi_send(s_lcd_spi, &reg, 1);
    LCD_DC_SET;
}

static void t_data(uint8_t data)
{
    if (lcd_spi() == RT_NULL) return;
    LCD_DC_SET;
    rt_spi_send(s_lcd_spi, &data, 1);
}

static void t_write_reg(uint8_t reg, uint8_t val)
{
    t_reg(reg);
    t_data(val);
}

/* ── 状态 ──
 * 仅记录最近写入的值供 lcdcolor 查询，不通过 0x10/0x11 回读寄存器。 */
static struct
{
    uint8_t madctl;        /* 0x36 MADCTL 当前值 */
    uint8_t gamma;         /* 0x26 Gamma 当前值 */
    uint8_t gamma_sel;     /* 当前 gamma 预设号（0..3） */
} s_tune = { 0x28, 0x01, 0 };

/* Gamma 预设：写 0xE0/0xE1 两组曲线，不同预设对应不同的暗部与中间调抬升量。
 * 数值取自 ILI9341 常见调校值，按实测效果选择。 */
struct gamma_preset
{
    const char *name;      /* 预设名，供 lcdcolor 打印 */
    uint8_t e0[16];        /* Gamma 正极曲线，16 段 */
    uint8_t e1[16];        /* Gamma 负极曲线，16 段 */
};

static const struct gamma_preset s_gamma_tab[] =
{
    /* 0: 厂商默认（原厂初始化序列） */
    { "default", {0x1F,0x1A,0x18,0x0A,0x0F,0x06,0x45,0x87,0x32,0x28,0x13,0x01,0x00,0x00,0x00,0x00},
                   {0x00,0x01,0x02,0x10,0x1F,0x1F,0x1F,0x1F,0x1E,0x1A,0x18,0x00,0x00,0x00,0x00,0x00} },
    /* 1: 暖（提红/降蓝，压暗部让肤色不发青） */
    { "warm",    {0x22,0x1C,0x18,0x0B,0x10,0x08,0x4A,0x8C,0x36,0x2C,0x14,0x02,0x01,0x00,0x00,0x00},
                   {0x01,0x02,0x03,0x12,0x22,0x22,0x24,0x24,0x22,0x1D,0x1A,0x01,0x00,0x00,0x00,0x00} },
    /* 2: 冷（提蓝，压红，抵消屏偏蓝用） */
    { "cool",    {0x1B,0x19,0x17,0x09,0x0D,0x05,0x40,0x82,0x2E,0x24,0x11,0x01,0x00,0x00,0x00,0x00},
                   {0x00,0x01,0x02,0x0E,0x1C,0x1C,0x1C,0x1C,0x1A,0x16,0x14,0x00,0x00,0x00,0x00,0x00} },
    /* 3: 高对比（加深暗部，让图标更突出） */
    { "contrast",{0x21,0x1C,0x19,0x0C,0x12,0x09,0x4C,0x8E,0x38,0x2E,0x15,0x02,0x02,0x01,0x00,0x00},
                   {0x01,0x03,0x04,0x14,0x24,0x26,0x28,0x28,0x26,0x20,0x1C,0x01,0x00,0x00,0x00,0x00} },
};

/* ── 应用 gamma 预设：写 0xE0 / 0xE1 各 16 字节 ── */
static void apply_gamma(uint8_t idx)
{
    const struct gamma_preset *p;
    int i;

    if (idx >= sizeof(s_gamma_tab) / sizeof(s_gamma_tab[0]))
        idx = 0;
    p = &s_gamma_tab[idx];
    s_tune.gamma_sel = idx;

    t_reg(0xE0);
    for (i = 0; i < 16; i++) t_data(p->e0[i]);

    t_reg(0xE1);
    for (i = 0; i < 16; i++) t_data(p->e1[i]);
}

/* msh lcdcolor: ILI9341 现场调色（bgr/inv/gamma/reset/refresh/preview）。
 * 改完寄存器须执行 lcdcolor refresh 才在屏上生效。 */
static void lcdcolor(int argc, char **argv)
{
    const char *cmd = (argc >= 2) ? argv[1] : NULL;
    int v;

    if (cmd == RT_NULL)
    {
        rt_kprintf("[lcd] MADCTL(0x36) = 0x%02X  (bit3=BGR: %u)\r\n",
                   s_tune.madctl, (s_tune.madctl >> 3) & 1U);
        rt_kprintf("[lcd] Gamma(0x26)   = 0x%02X  预设=%u (%s)\r\n",
                   s_tune.gamma, s_tune.gamma_sel, s_gamma_tab[s_tune.gamma_sel].name);
        rt_kprintf("\r\n[lcd] 用法:\r\n");
        rt_kprintf("  lcdcolor bgr 0|1        切RGB/BGR（最可能影响整体色偏）\r\n");
        rt_kprintf("  lcdcolor gamma 0..3     0默认 1暖 2冷 3高对比\r\n");
        rt_kprintf("  lcdcolor inv 0|1        屏幕反显\r\n");
        rt_kprintf("  lcdcolor reset          恢复厂商默认\r\n");
        rt_kprintf("  lcdcolor refresh        强制重画（改完必须执行）\r\n");
        rt_kprintf("  lcdcolor preview <0|1>  1=灌入纯红/纯绿/纯蓝纯色块，判BGR 是否正确\r\n");
        return;
    }

    if (rt_strcmp(cmd, "bgr") == 0)
    {
        v = (argc >= 3) ? atoi(argv[2]) : 0;
        v &= 1;
        if (v) s_tune.madctl |=  (1 << 3);
        else    s_tune.madctl &= ~(1 << 3);
        t_write_reg(0x36, s_tune.madctl);
        rt_kprintf("[lcd] MADCTL = 0x%02X  BGR = %u  (记得 lcdcolor refresh)\r\n",
                   s_tune.madctl, v);
        return;
    }

    if (rt_strcmp(cmd, "inv") == 0)
    {
        v = (argc >= 3) ? atoi(argv[2]) : 0;
        v &= 1;
        /* INV bit = MADCTL bit6 */
        if (v) s_tune.madctl |=  (1 << 6);
        else    s_tune.madctl &= ~(1 << 6);
        t_write_reg(0x36, s_tune.madctl);
        rt_kprintf("[lcd] MADCTL = 0x%02X  INV = %u  (记得 lcdcolor refresh)\r\n",
                   s_tune.madctl, v);
        return;
    }

    if (rt_strcmp(cmd, "gamma") == 0)
    {
        v = (argc >= 3) ? atoi(argv[2]) : 0;
        if (v < 0 || v >= (int)(sizeof(s_gamma_tab) / sizeof(s_gamma_tab[0])))
        {
            rt_kprintf("[lcd] gamma 范围 0..%u\r\n",
                       (unsigned)(sizeof(s_gamma_tab) / sizeof(s_gamma_tab[0]) - 1));
            return;
        }
        apply_gamma((uint8_t)v);
        rt_kprintf("[lcd] gamma = %u (%s)  (记得 lcdcolor refresh)\r\n",
                   v, s_gamma_tab[v].name);
        return;
    }

    if (rt_strcmp(cmd, "reset") == 0)
    {
        s_tune.madctl = 0x28;
        s_tune.gamma  = 0x01;
        t_write_reg(0x36, s_tune.madctl);
        t_write_reg(0x26, s_tune.gamma);
        apply_gamma(0);
        rt_kprintf("[lcd] 已恢复厂商默认 (MADCTL=0x28, gamma=default)\r\n");
        rt_kprintf("[lcd] 记得 lcdcolor refresh\r\n");
        return;
    }

    if (rt_strcmp(cmd, "refresh") == 0)
    {
        /* LVGL 对象只能在 LVGL 线程操作，本命令跑在 msh 线程，因此只做置脏：
         * 把当前屏幕 invalidate，LVGL 下一次定时器刷新时会重画全屏。 */
        lv_obj_invalidate(lv_scr_act());
        rt_kprintf("[lcd] 已标记重绘（LVGL 下一次刷新会重画全屏）\r\n");
        return;
    }

    /* 纯色块预览：判断 BGR 顺序最直接的办法。
     * 显示纯红表示 RGB 顺序正确，显示青蓝表示 R/B 颠倒。 */
    if (rt_strcmp(cmd, "preview") == 0)
    {
        v = (argc >= 3) ? atoi(argv[2]) : 0;
        LCD_Clear(v == 0 ? 0xF800 : (v == 1 ? 0x07E0 : 0x001F));
        rt_kprintf("[lcd] 已刷纯%s 色块（0红1绿2蓝）\r\n",
                   v == 0 ? "红" : (v == 1 ? "绿" : "蓝"));
        return;
    }

    rt_kprintf("[lcd] 未知命令: %s\r\n", cmd);
}
MSH_CMD_EXPORT(lcdcolor, tune ILI9341 color: bgr/gamma/inv/preview/refresh - no comma);