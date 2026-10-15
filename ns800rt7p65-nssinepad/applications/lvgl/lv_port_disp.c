/**
 * @file    lv_port_disp.c
 * @brief   LVGL v9.1.0 显示移植层：ILI9341（SPI），240 x 320，RGB565
 *
 * 由 packages/LVGL-v9.1.0/env_support/rt-thread/lv_rt_thread_port.c 调用：
 *   INIT_ENV_EXPORT(lvgl_thread_init) -> lv_init() -> lv_port_disp_init()
 * 该初始化早于 main()，因此屏的硬件初始化必须放在本文件（disp_init）。
 */

#include <rtthread.h>
#include <stdlib.h>                 /* atoi，dispdir 命令使用 */
#include <lvgl.h>
#include "packages/ili9341-latest/lcd_ili9341.h"

#define MY_DISP_HOR_RES     240     /* 竖屏宽，像素 */
#define MY_DISP_VER_RES     320     /* 竖屏高，像素 */

/* 运行时横屏切换（游戏 App 需要）。
 * 需求：仅在游戏页横屏，桌面与其他 App 保持竖屏。
 *
 * 实现需同时完成两件事，缺一不可：
 *   1. 屏控制器方向：LCD_direction(0|1) 改写 MADCTL，硬件旋转，零 CPU 开销；
 *   2. LVGL 分辨率：lv_display_set_resolution() 并重新调用 lv_display_set_buffers()。
 *
 * 缓冲必须按两个方向中较宽者（320）分配：
 *   lv_display_set_buffers() 按当前分辨率计算 stride 与行数
 *   （lv_display.c:413 stride = width_to_stride(w)，h = buf_size / stride），
 *   横屏 320 宽时每行需 640 B。若按竖屏 240 宽只分配 480 B/行，
 *   重新 set_buffers 后算出的可用行数偏小，而 LVGL 仍可能按 640 B 写入，
 *   导致溢出踩内存。此处统一按 LAND_W(320) 分配，两个方向都安全。
 *
 * 调用时机：必须在 LVGL 线程内，且需让 LVGL 重新渲染（invalidate 整屏）。
 * 切换前后屏内容会瞬时与新分辨率不匹配（旧方向画面加新方向分辨率），
 * 因此调用方应先清屏或重画，再切换。
 */
#define LAND_W              320             /* 横屏宽，等于竖屏高，像素 */
#define LAND_H              240             /* 横屏高，等于竖屏宽，像素 */
#define LAND_BUF_LINES      20              /* 缓冲行数，按最宽方向分配 */
#define DISP_BUF_LINES      20              /* 每方向刷新的缓冲行数 */

/* 局部刷新缓冲：按最宽方向（320）分配，两个方向共用。
 * 竖屏 240 宽时只用到前 240 * 20 * 2 字节，横屏 320 宽时正好用满。 */
static lv_color_t s_buf1[LAND_BUF_LINES * LAND_W];
static lv_color_t s_buf2[LAND_BUF_LINES * LAND_W];
/* PARTIAL 模式下 LVGL 由 buf_size 反推行数（h = buf_size / stride），
 * 故该值须跟随当前方向的实际宽度，在切换方向时更新。 */
static uint32_t   s_buf_bytes = MY_DISP_HOR_RES * DISP_BUF_LINES * 2;

/* 当前是否为横屏，供调色/UI 层查询 */
static int s_landscape = 0;

/**
 * @brief  查询当前是否为横屏
 * @return 1 横屏；0 竖屏
 */
int lv_port_disp_is_landscape(void)
{
    return s_landscape;
}

/**
 * @brief  切换横屏或竖屏
 * @param  landscape  1 切横屏（320 x 240），0 切竖屏（240 x 320）
 * @note   只能在 LVGL 线程调用；切换后整屏重画。
 */
void lv_port_disp_set_orientation(int landscape)
{
    lv_display_t *disp = lv_display_get_default();
    int hor, ver;

    if (disp == NULL) return;
    if (landscape == s_landscape) return;           /* 已是目标方向，无需重复切换 */

    /* 1. 屏控制器硬件方向：先切换，使后续刷新落在正确坐标 */
    LCD_direction(landscape ? 1 : 0);

    /* 2. LVGL 分辨率 */
    hor = landscape ? LAND_W : MY_DISP_HOR_RES;
    ver = landscape ? LAND_H : MY_DISP_VER_RES;
    lv_display_set_resolution(disp, hor, ver);

    /* 3. 按新分辨率重算缓冲布局，否则 stride 与行数仍为旧值 */
    s_buf_bytes = (uint32_t)hor * DISP_BUF_LINES * 2;
    lv_display_set_buffers(disp, s_buf1, s_buf2, s_buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_landscape = landscape;
    lv_obj_invalidate(lv_screen_active());          /* 整屏重画 */
    rt_kprintf("[lvgl] 方向 → %s (%dx%d)\n",
               landscape ? "横屏" : "竖屏", hor, ver);
}

static void disp_init(void);        /* 屏硬件初始化，定义见文件末尾 */

/* LVGL 把渲染好的矩形区域交给这里刷到屏上。
 * px_map 为 RGB565 原始像素，尺寸 = w * h * 2 字节。 */
/* 由 ui.c 的 fpsinfo 命令读取，用于测真实帧率 */
extern volatile rt_uint32_t g_fps_flush_lines;
/* 按真正刷出的像素数统计，比统计 flush 次数更能反映优化效果：
 * 脏区重绘后每次 flush 面积可能只有几百像素，
 * 按次数折算为整屏的口径已经失真。 */
extern volatile rt_uint32_t g_fps_flush_px;

/**
 * @brief  LVGL 刷屏回调：把脏区像素写入 ILI9341
 * @param  disp    显示设备
 * @param  area    脏区范围
 * @param  px_map  RGB565 像素数据，尺寸 w * h * 2 字节
 */
static void disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

    g_fps_flush_lines++;              /* 帧率统计：每次 flush 累一格 */
    g_fps_flush_px += w * h;          /* 同时累计实际像素数 */

    /* ILI9341 经 SPI 需要大端 RGB565，此处做字节交换。
     * v8 时代使用 LV_COLOR_16_SWAP，v9 已移除该宏，改为在 flush_cb 中调用。 */
    lv_draw_sw_rgb565_swap(px_map, w * h);

    lcd_fill_array_spi((uint16_t)area->x1, (uint16_t)area->y1,
                       (uint16_t)area->x2, (uint16_t)area->y2,
                       px_map);

    /* 必须通知 LVGL 刷新完成，否则不会下发下一帧 */
    lv_display_flush_ready(disp);
}

/**
 * @brief  初始化 LVGL 显示：创建显示设备并挂接刷屏回调
 * @note   由 LVGL 线程在 lv_init() 之后调用，早于 main()。
 */
void lv_port_disp_init(void)
{
    disp_init();

    lv_display_t *disp = lv_display_create(MY_DISP_HOR_RES, MY_DISP_VER_RES);
    if (disp == NULL)
    {
        rt_kprintf("[lvgl] lv_display_create failed\n");
        return;
    }
    lv_display_set_flush_cb(disp, disp_flush);

    /* 双缓冲（原先为单缓冲：buf, NULL, ...）。
     *
     * 单缓冲的问题：LVGL 渲染进 buf 后触发 flush，SPI 同步刷屏，
     * 整个刷完（约 10 ms 以上）期间 buf 被占用，无法渲染下一帧，
     * 表现为翻页或开 App 动画卡顿、帧率上不去。
     *
     * 双缓冲：LVGL 渲染进 buf1 的同时 SPI 刷 buf2，
     * 渲染与刷屏流水线并行，动画明显流畅。
     *
     * 代价：多占一块缓冲（静态数组，不走 LVGL 内存池）。
     * 缓冲数组已上提至文件上方（s_buf1/s_buf2），因为运行时横竖屏切换
     * 需要重新调用 lv_display_set_buffers() 重算布局。
     * 必须使用两个独立数组，不能把同一个 buf 传入两次，
     * 否则 LVGL 会在两个 buffer 中写入同一份数据。 */
    s_landscape = 0;
    s_buf_bytes = MY_DISP_HOR_RES * DISP_BUF_LINES * 2;

    lv_display_set_buffers(disp, s_buf1, s_buf2, s_buf_bytes,
                          LV_DISPLAY_RENDER_MODE_PARTIAL);
}

/* msh 命令 dispdir：手动切换屏方向（调试用）
 *   dispdir      查看当前方向
 *   dispdir 1    切横屏 320 x 240
 *   dispdir 0    切竖屏 240 x 320
 * 必须经 lv_async_call 进入 LVGL 线程（msh 线程直接调用会破坏 LVGL 内部状态）。 */

/**
 * @brief  lv_async_call 回调：在 LVGL 线程内切换方向
 * @param  p  目标方向，0 竖屏，1 横屏
 */
static void dispdir_async(void *p)
{
    lv_port_disp_set_orientation((int)(rt_base_t)p);
}

/**
 * @brief  msh 命令 dispdir：查看或切换屏方向
 *         用法 dispdir [0|1]
 * @param  0 切竖屏 240 x 320，1 切横屏 320 x 240；不带参数时仅打印当前方向
 */
static void dispdir(int argc, char **argv)
{
    if (argc >= 2)
    {
        lv_async_call(dispdir_async, (void *)(rt_base_t)atoi(argv[1]));
        return;
    }
    rt_kprintf("[disp] 当前 %s, %dx%d\n",
               lv_port_disp_is_landscape() ? "横屏" : "竖屏",
               (int)lv_display_get_horizontal_resolution(lv_display_get_default()),
               (int)lv_display_get_vertical_resolution(lv_display_get_default()));
}
MSH_CMD_EXPORT(dispdir, display orientation: dispdir [0=portrait|1=landscape]);

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN && LV_MEM_ADR != 0
/* LVGL 内存池地址自检。
 *
 * lv_conf.h 中的 LV_MEM_ADR 是硬编码地址（0x20100000，SRAM1），
 * 该空间由 link.lds 的 .lvgl_pool 段占用。两者一旦漂移，LVGL 的池会覆盖
 * 其他数据，此类错误难以定位，因此启动时先校验并打印。 */
extern uint8_t __lvgl_pool_start[];

/**
 * @brief  校验 LVGL 内存池地址与链接脚本是否一致
 * @note   不一致时仅打印错误，不中止运行。
 */
static void lvgl_pool_selftest(void)
{
    if ((uint32_t)__lvgl_pool_start != (uint32_t)LV_MEM_ADR)
    {
        rt_kprintf("[lvgl] !! POOL ADDR MISMATCH: linker=0x%08x conf=0x%08x\n",
                   (unsigned)(uint32_t)__lvgl_pool_start, (unsigned)(uint32_t)LV_MEM_ADR);
    }
    else
    {
        rt_kprintf("[lvgl] mem pool: %u KB @ 0x%08x (SRAM1, builtin TLSF, 不占 RT-Thread 堆)\n",
                   (unsigned)(LV_MEM_SIZE / 1024U), (unsigned)(uint32_t)LV_MEM_ADR);
    }
}
#else
#define lvgl_pool_selftest()   do { } while (0)
#endif

/* 屏硬件初始化，全工程唯一的 LCD 初始化点。
 * 不要在别处（例如 main()）重复调用 spi_lcd_init()：其内部会经
 * LCD_Init() -> LCD_RESET() 对屏做硬件复位，已画好的画面会被一并清除。
 * 本函数由 INIT_ENV_EXPORT 启动的 LVGL 线程调用，早于 main()。 */

/**
 * @brief  初始化 LCD 硬件：自检内存池并调用 spi_lcd_init()
 */
static void disp_init(void)
{
    /* 注意：DC 引脚不能落在调试串口 TX 上（NS800 的 UART1 TX = GPIO_12 / PA12）。
     * 一旦冲突，rt_pin_mode() 会把 PA12 从 UART1 复用改为普通 GPIO，
     * 串口立即变为乱码，同时 LCD 的 DC 电平受串口外设干扰，屏会白黑乱闪。 */
    rt_kprintf("[lvgl] lcd init start: DC=%d RES=%d CS=%d BLK=%d (DC must NOT be 12)\n",
               PKG_ILI_9341_DC_PIN, PKG_ILI_9341_RES_PIN,
               PKG_ILI_9341_CS_PIN, PKG_ILI_9341_BLK_PIN);

    lvgl_pool_selftest();

    /* SPI 速度，单位 MHz。
     *
     * 一屏 = 240 x 320 x 2 B = 76800 B，其 SPI 传输时间直接决定帧率上限
     * （LVGL 为脏区渲染，翻页或开 App 动画时刷整屏）：
     *     50 MHz -> 约 25 ms/屏 -> 理论 40 FPS
     *     70 MHz -> 约 18 ms/屏 -> 理论 55 FPS
     *     80 MHz -> 约 16 ms/屏 -> 理论 63 FPS
     *
     * 实测 70 MHz 会花屏，整屏全黑，故这块屏与排线的实际上限为 50 MHz，
     * 不要再尝试更高频率。理论值 55 FPS 看似可行，但走线与排插带宽不足，
     * 采样点错位即表现为花屏，理论值不等于可运行值。 */
#define LCD_SPI_MHZ    50

    if (spi_lcd_init(LCD_SPI_MHZ) != RT_EOK)
    {
        rt_kprintf("[lvgl] spi_lcd_init FAILED (check BSP_USING_SPI1 / device name)\n");
        return;
    }
    rt_kprintf("[lvgl] display ready: ILI9341 %dx%d @%dMHz\n",
               MY_DISP_HOR_RES, MY_DISP_VER_RES, LCD_SPI_MHZ);
}
