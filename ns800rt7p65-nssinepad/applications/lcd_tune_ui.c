/**
 * @file    lcd_tune_ui.c
 * @brief   屏幕调色 App：拖滑块实时调 gamma / 冷暖 / 明暗 / 饱和。
 *
 * 实测确认颜色没有反转（BGR 位正确），偏蓝紫来自 gamma。ILI9341 的 gamma
 * 是 0xE0 / 0xE1 两组各 16 字节的曲线，用 msh 切固定预设无法找到平衡点，
 * 因此做成 LVGL App 现场拖调。
 *
 * gamma 分段权重（0xE0 正极 / 0xE1 负极，各 16 字节对应 16 个亮度等级）：
 *   索引 0~4    暗部：决定黑色的深浅；
 *   索引 5~11   中间调：决定肤色，冷暖主要作用于此；
 *   索引 12~15  高光：决定白色的亮暗。
 *
 * 三项参数取值范围均为 -100 ~ +100：
 *   冷暖：<0 偏冷（加蓝、压红绿），>0 偏暖；
 *   明暗：>0 抬中间调（整体提亮），<0 压暗；
 *   饱和：>0 拉开暗部与高光距离（更艳），<0 降低对比。
 *
 * 交互（走 LVGL 标准 encoder group 机制）：
 *   旋钮上下/左右 = 在三个滑块之间移动焦点；
 *   旋钮左右转     = 调当前滑块；
 *   KEY           = 在三个滑块间切换 / 触发按钮；
 *   停转（RELEASED）= 一次性写 0xE0/0xE1 并落盘。
 *
 * 性能取舍：拖拽期间每步都 apply_tuning() 与整屏 invalidate，会同时触发整屏
 * 重渲染（SPI 发 153600 B）和屏控制器整屏重扫，因此拖拽中只更新滑块与数值
 * 标签（脏区几百字节），停手才写一次 gamma。SPI 侧另见 lv_port_disp.c 与
 * drv_spi.c 的 FIFO 批量发送优化。
 *
 * 使用 lv_slider 而非自行轮询旋钮：lv_port_indev.c 已把 encoder 绑进 lv_group，
 * 控件入组即自动响应旋钮。
 */

#include "ui.h"
#include "ui_text.h"
#include "settings.h"
#include "lcd_ili9341.h"
#include <rtthread.h>
#include <lvgl.h>

/* ── 布局常量（屏 240x320，body 高 = 320 - 34 = 286）──
 * 宽度预算：8(左边距) + 130(滑块) + 4 + 36(左提示 2 字) + 12 + 36(右提示 2 字)
 *           + 4 = 230，余 10px。*/
#define ROW_X        8       /* 名称与滑块的左起点 */
#define SLIDER_W     130     /* 滑块宽 */
#define HINT_LX      142     /* 左提示（"左冷"）x */
#define HINT_RX      190     /* 右提示（"右暖"）x */
#define ROW_Y0       54      /* 第一行的 y */
#define ROW_DY       62      /* 行间距 */

/* ── 调色参数（三项共用，取值 -100 ~ +100，0 为出厂默认）── */
#define TUNE_MIN      (-100)
#define TUNE_MAX      100
#define TUNE_DEFAULT  0

static int s_warm   = TUNE_DEFAULT;   /* 冷暖：<0 冷，>0 暖 */
static int s_gamma  = TUNE_DEFAULT;   /* 明暗：>0 提亮，<0 压暗 */
static int s_sat    = TUNE_DEFAULT;   /* 饱和：>0 更艳，<0 更灰 */

/* ── 寄存器收发：驱动 lcd_dev 为 static，按设备名重新取一次 SPI 设备 ── */
static struct rt_spi_device *s_lcd_spi;

static struct rt_spi_device *lcd_spi(void)
{
    if (s_lcd_spi == RT_NULL)
        s_lcd_spi = (struct rt_spi_device *)
                     rt_device_find(PKG_ILI_9341_SPI_DEVICE_NAME);
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

/* ── gamma 曲线计算：以出厂默认曲线为基底，按三项参数做分段线性修正 ── */
static void calc_gamma_curve(uint8_t *e0, uint8_t *e1)
{
    /* 这两组默认值必须与 lcd_ili9341.c 初始化序列完全一致，否则进调色页
     * 就等于换了一套 gamma，出现过曝或发灰。
     *
     * 注意两点：
     *   1. E1 不能手写为递增序列：驱动真实值在暗部抬升明显，手写会导致整屏发白；
     *   2. 厂商驱动的 0xE1 只发了 15 个字节（标准为 16），最后一个字节会保留
     *      残值，每帧重写结果可能不同，因此这里补 0x00 作为第 16 个。
     *
     * 提取方法（改动驱动后需重新核对）：从 lcd_ili9341.c 中 LCD_WR_REG(0xE1)
     * 之后取前 16 个 LCD_WR_DATA。
     */
    static const uint8_t def_e0[16] =
        { 0x1F,0x1A,0x18,0x0A,0x0F,0x06,0x45,0x87,
          0x32,0x0A,0x07,0x02,0x07,0x05,0x00,0x00 };
    static const uint8_t def_e1[16] =
        { 0x00,0x25,0x27,0x05,0x10,0x09,0x3A,0x78,
          0x4D,0x05,0x18,0x0D,0x38,0x3A,0x2F,0x00 };   /* 末位补 0 */

    /* 中间调三角权重（索引 5..11）：0.3, 0.7, 1.0, 1.0, 0.7, 0.3, 0 */
    static const rt_uint8_t w_mid[7] = { 3, 7, 10, 10, 7, 3, 0 };
    int i;
    int mid_boost;    /* 冷暖换算成中间调增益 */

    mid_boost = (s_warm * 10) / 100;      /* ±10 幅度，实测足够看出区别 */

    for (i = 0; i < 16; i++)
    {
        int v0 = def_e0[i];
        int v1 = def_e1[i];

        /* 1) 冷暖：主要改中间调（5..11）*/
        if (i >= 5 && i <= 11)
        {
            int weight = w_mid[i - 5];
            v0 += (mid_boost * weight) / 10;
            /* 暖的时候略压负极高光，避免整体发白 */
            v1 -= (mid_boost * weight) / 20;
        }

        /* 2) 明暗：抬/压中间偏上（3..12），不动两端以保持纯黑纯白 */
        if (i >= 3 && i <= 12)
        {
            int lift = (s_gamma * 6) / 100;
            v0 += lift;
            v1 += lift / 2;
        }

        /* 3) 饱和：暗侧压下去、亮侧抬上来，使中间调更陡、更艳 */
        if (i >= 4 && i <= 11)
        {
            int push = (s_sat * 5) / 100;
            if (i <= 7)  v0 += push;
            else         v1 += push / 2;
        }

        if (v0 < 0)   v0 = 0;   if (v0 > 255) v0 = 255;
        if (v1 < 0)   v1 = 0;   if (v1 > 255) v1 = 255;
        e0[i] = (uint8_t)v0;
        e1[i] = (uint8_t)v1;
    }
}

/* 把当前三项参数写入屏的 gamma 寄存器（0xE0 / 0xE1 共 32 字节）。
 *
 * 一次 32 字节的 SPI 事务本身很快，但每步都调用会叠加 cs_take、spi_lock 与
 * 32 次单字节发送；而肉眼只能看到停住那一刻的值。因此拖拽期间不写，
 * 等停手（RELEASED）再写一次。 */
static void apply_tuning(void)
{
    uint8_t e0[16], e1[16];
    int i;

    calc_gamma_curve(e0, e1);

    t_reg(0xE0);
    for (i = 0; i < 16; i++) t_data(e0[i]);

    t_reg(0xE1);
    for (i = 0; i < 16; i++) t_data(e1[i]);
}

/* ── UI ── */
/* 三项参数的下标；ITEM_COUNT 为项数 */
enum { ITEM_WARM = 0, ITEM_GAMMA, ITEM_SAT, ITEM_COUNT };

static lv_obj_t *g_val_lb;
static lv_obj_t *s_slider[ITEM_COUNT];
static int       s_val[ITEM_COUNT];
static int       s_cur = ITEM_WARM;
static int       s_dirty     = 0;        /* 1 = 有 gamma 改动待写硬件 */
static int       s_dirty_idx = -1;       /* 待存盘的那一项（-1 = 无） */

/* 定义在下方（slider_cb / focus_cb 要用，先声明） */
static void value_update(void);
static void value_flush(void);
static void value_defer(void);

/* msh tune drag 的压测节拍器（定义在文件末尾，teardown 需先行引用） */
static lv_timer_t *s_tune_rate_timer;
static int         s_tune_left;
static int         g_tune_rate_hz = 20;

/* 文案全部取自 ui_text.h：那里注释中的汉字是字库 gen_font_zh.sh 的输入来源，
 * 直接写中文字面量会漏出字库范围，屏上显示为空心方块。 */
static const char *s_item_name[ITEM_COUNT] = { S_TN_WARM, S_TN_GAMMA, S_TN_SAT };
static const char *s_item_hint[ITEM_COUNT][2] =
{
    { S_TN_H_COLD, S_TN_H_WARM },
    { S_TN_H_DARK, S_TN_H_LIGHT },
    { S_TN_H_GRAY, S_TN_H_VIVID },
};

/* ── 脏区重绘 ──
 *
 * 默认不做整屏 invalidate。调色台按「壁纸 → 暗纱 → 控件」逐层合成
 * （见 ui.c 的 flat_nav 分支），整屏 invalidate 会让 LVGL 重渲染 240x320
 * 全屏，仅 SPI 就要发 153600 B；拖滑块一次十几帧，SPI 带宽（50 MHz 约
 * 6.25 MB/s）会直接见底。
 *
 * gamma 是屏控制器内部的曲线，修改它不改变 LVGL 已算好的像素数据，真正
 * 需要重画的只有滑块轨道与把手。因此只 invalidate 三个滑块和数值标签所在的
 * 矩形，静态背景由 LVGL 脏区机制自动跳过，SPI 只发变化的像素。
 *
 * （定义放在状态变量之后：它引用 g_val_lb / s_slider / ITEM_COUNT。）
 *
 * 调试开关：tune fullscreen 1 切回整屏 invalidate 的旧行为，
 * 便于用 fpsinfo 做 A/B 对照。
 */
int g_tune_fullscreen = 0;      /* 0 = 脏区（新，默认）/ 1 = 整屏（旧，仅对照）*/
unsigned s_step_count = 0;      /* slider_cb 被触发的次数（压测诊断用）*/

static void redraw_now(void)
{
    int i;

    if (g_tune_fullscreen)
    {
        /* 整屏 invalidate 只做这一件事，且不再 invalidate 任何子对象。
         *
         * LVGL 的 _lv_inv_area()（lv_refr.c）有去重合并：待加入区域若已被
         * inv_areas 中某个区域完全包含，就直接丢弃。slider 改值时自身已把滑块
         * 的小区域塞入 inv_areas，此时再 invalidate 整屏会被该小区域包含而
         * 静默丢弃。整屏模式下既然全屏都要重画，子对象本就包含在内。 */
        lv_obj_invalidate(lv_scr_act());
        return;
    }

    /* 脏区模式：只 invalidate 会变的控件（滑块轨道/把手 + 数值标签）。
     * 这几个矩形互不包含，入队顺序无影响。 */
    if (g_val_lb) lv_obj_invalidate(g_val_lb);
    for (i = 0; i < ITEM_COUNT; i++)
        if (s_slider[i]) lv_obj_invalidate(s_slider[i]);
}

/* 取第 item 项参数对应的运行时变量地址 */
static int *param_of(int item)
{
    if (item == ITEM_WARM)  return &s_warm;
    if (item == ITEM_GAMMA) return &s_gamma;
    return &s_sat;
}

/* 滑块改动回调：刷新数值并重画滑块，gamma 延后到松手才写硬件。
 *
 * 不每步 apply_tuning() 的原因：32 字节 gamma 走 SPI 是 32 次单字节事务；
 * 更关键的是屏控制器收到 gamma 写入后要重新扫一遍整屏（约几十毫秒，属屏
 * 内部行为，不受 SPI 带宽影响），每步都写会拖住视觉响应造成一格一顿，而肉眼
 * 只能看到停住那一刻的值。
 *
 * 因此拖拽中只更新滑块位置与数值标签，停手（RELEASED）再写一次 gamma 并落盘。 */
static void slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int idx;
    int v;   /* -100..100 */

    for (idx = 0; idx < ITEM_COUNT; idx++)
        if (s_slider[idx] == sl) break;
    if (idx >= ITEM_COUNT) return;

    v = lv_slider_get_value(sl) - 100;      /* 滑块 0..200 → -100..100 */
    s_val[idx] = v;
    *param_of(idx) = v;                     /* 运行时状态立刻更新 */

    /* 拖动回调运行在 encoder 读取流程中（不在渲染中），可直接更新 */
    s_cur = idx;
    value_update();
    value_flush();                          /* 大字读数立刻跟着动 */
    redraw_now();                           /* 只重画滑块与读数；不写硬件、不落盘 */

    s_dirty = 1;                            /* 标记有待写入的 gamma */
    s_dirty_idx = idx;
    s_step_count++;                         /* 压测诊断：确认回调真的被触发 */
}

/* 松手时才真正写硬件并存盘：RELEASED 仅在拖动结束时触发一次 */
static void release_cb(lv_event_t *e)
{
    rt_settings_t c;

    if (s_dirty)
    {
        apply_tuning();     /* 拖拽期间累积的改动，此刻一次性写入 gamma */
        s_dirty = 0;
    }

    /* 存盘只写改动的那一项。settings_update_one 内部会调用 settings_save()
     * 落盘一次，无需再额外调用：LittleFS 的 erase/write 耗时几十毫秒，
     * 重复写盘没有意义。 */
    if (s_dirty_idx >= 0)
        settings_update_one(s_dirty_idx == ITEM_WARM  ? 'w' :
                            s_dirty_idx == ITEM_GAMMA ? 'b' : 's',
                            *param_of(s_dirty_idx));

    settings_get(&c);
    rt_kprintf("[tune] 已保存到 flash（冷暖=%d 明暗=%d 饱和=%d）\r\n",
               c.tune_warm, c.tune_bright, c.tune_sat);

    s_dirty_idx = -1;
}

/* 数值显示：跟随当前焦点项。
 *
 * lv_label_set_text() 内部会 lv_obj_invalidate()，而本逻辑被 focus_cb
 * （LV_EVENT_FOCUSED）调用，FOCUSED 可能发生在 lv_group_set_editing() 的
 * 重绘流程内，此时 disp->rendering_in_progress 为真，任何 invalidate 都会触发
 * LVGL 断言 "Invalidate area is not allowed during rendering."。
 *
 * 因此这里只记录应显示的数值，真正的 set_text 交给一次性 lv_async_call
 * （LVGL 在主循环安全时机执行，此时渲染已结束），见 value_defer()。
 *
 * 不使用长寿命 lv_timer：ui.c 关页面时 lv_obj_clean(g_app_body) 会释放
 * g_val_lb，但框架不会通知 App 销毁自己的定时器，定时器下个 tick 即野指针
 * （实测挂死在 lv_obj_get_disp，usage fault UNALIGNED）。一次性 async 回调
 * 没有此问题：要么立刻执行完，要么在对象销毁时被安全丢弃。 */
static int  s_val_lb_shown = 0x7FFFFFFF;   /* 标签当前显示的数值（哨兵：未同步） */
static int  s_val_lb_want  = 0;            /* 待显示的数值 */
static int  s_val_defer_pending = 0;       /* 已排入 async 回调，避免重复排队 */

/* async 回调：在安全时机把数值写入标签（见上方说明） */
static void value_flush_async(void *p)
{
    (void)p;
    s_val_defer_pending = 0;

    if (g_val_lb == RT_NULL) return;       /* 页面已关：什么都不做 */
    if (s_val_lb_want == s_val_lb_shown) return;

    s_val_lb_shown = s_val_lb_want;
    if (s_val_lb_shown == 0)      lv_label_set_text(g_val_lb, "0");
    else if (s_val_lb_shown > 0)  lv_label_set_text_fmt(g_val_lb, "+%d", s_val_lb_shown);
    else                          lv_label_set_text_fmt(g_val_lb, "%d", s_val_lb_shown);
}

/* 请求在下一次主循环刷新数值标签（安全时机；可重复调用，内部去重） */
static void value_defer(void)
{
    if (s_val_defer_pending) return;
    s_val_defer_pending = 1;
    lv_async_call(value_flush_async, RT_NULL);
}

/* 立即刷新（仅在确定不在渲染中时使用，例如拖动/按钮回调） */
static void value_flush(void)
{
    if (g_val_lb == RT_NULL) return;
    if (s_val_lb_want == s_val_lb_shown) return;

    s_val_lb_shown = s_val_lb_want;
    if (s_val_lb_shown == 0)      lv_label_set_text(g_val_lb, "0");
    else if (s_val_lb_shown > 0)  lv_label_set_text_fmt(g_val_lb, "+%d", s_val_lb_shown);
    else                          lv_label_set_text_fmt(g_val_lb, "%d", s_val_lb_shown);
}

static void value_update(void)
{
    s_val_lb_want = *param_of(s_cur);
    /* 不在此处 set_text：可能处于渲染中（见上）。
     * 改排一个 async 回调，由 LVGL 在主循环安全时机执行。 */
    value_defer();
}

/* 焦点切换回调：记录当前项，并把数值显示切到该项 */
static void focus_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target(e);
    int i;

    for (i = 0; i < ITEM_COUNT; i++)
        if (s_slider[i] == t)
        {
            s_cur = i;
            value_update();
            break;
        }
}

/* ── 恢复默认按钮：三项参数归零，写硬件并写盘 ── */
static void reset_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int i;

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED)
        return;

    s_warm = s_gamma = s_sat = TUNE_DEFAULT;
    for (i = 0; i < ITEM_COUNT; i++)
    {
        s_val[i] = 0;
        lv_slider_set_value(s_slider[i], 100, LV_ANIM_OFF);
    }
    apply_tuning();
    redraw_now();

    /* 恢复默认不是拖拽流程，直接写盘，同时清掉待写标记 */
    s_dirty = 0;
    s_dirty_idx = -1;
    value_update();
    {
        rt_settings_t c;
        settings_get(&c);
        c.tune_warm = c.tune_bright = c.tune_sat = TUNE_DEFAULT;
        settings_set(&c);
        settings_save();
    }
    rt_kprintf("[tune] 已恢复出厂默认（冷暖/明暗/饱和 = 0）并写入 flash\r\n");
}

/**
 * @brief  构建调色页（由 ui.c 在打开 App 时于 LVGL 线程调用）
 * @param  body  挂载控件的容器对象
 * @note   读回 flash 配置并应用到运行时状态与硬件 gamma，再创建数值标签、三条
 *         滑块与恢复默认按钮。本页无内部定时器，退出时无需停止。
 */
void app_tune_build(lv_obj_t *body)
{
    lv_obj_t *lb, *hint, *btn;
    int i;
    /* 从 flash 读回上次调好的值。settings_load 幂等，首次开机读不到
     * 时使用出厂默认值（settings.c 已铺好）。 */
    int cfg_saved[ITEM_COUNT];
    rt_settings_t cfg;

    settings_get(&cfg);
    cfg_saved[ITEM_WARM]  = cfg.tune_warm;
    cfg_saved[ITEM_GAMMA] = cfg.tune_bright;
    cfg_saved[ITEM_SAT]   = cfg.tune_sat;

    /* 把读回的值应用到运行时状态 + 硬件 gamma */
    s_warm  = cfg_saved[ITEM_WARM];
    s_gamma = cfg_saved[ITEM_GAMMA];
    s_sat   = cfg_saved[ITEM_SAT];
    apply_tuning();

    s_cur = ITEM_WARM;

    /* 重进页面需清掉上一轮遗留的待写/待存标记：滑块对象是新建的，而这两个
     * static 仍保留旧值，不清会导致首次松手时拿旧 idx 存盘。 */
    s_dirty     = 0;
    s_dirty_idx = -1;

    /* 数值标签的显示缓存同样要重置：新标签对象配旧 s_val_lb_shown 会使
     * "值未变"的判据失效。 */
    s_val_lb_shown  = 0x7FFFFFFF;
    s_val_lb_want   = 0;
    s_val_defer_pending = 0;

    /* 数值显示（大字，居中偏上） */
    g_val_lb = ui_label(body, "0", &zh18, 0xFFFFFF);
    lv_obj_align(g_val_lb, LV_ALIGN_TOP_MID, 0, 14);
    value_update();

    /* 三条滑块 */
    for (i = 0; i < ITEM_COUNT; i++)
    {
        lb = ui_label(body, s_item_name[i], &zh18, 0xFFFFFF);
        lv_obj_align(lb, LV_ALIGN_TOP_LEFT, ROW_X, ROW_Y0 + i * ROW_DY);

        /* 左右两个提示分开放（"左冷" "右暖"）。
         * 两者都用 LV_ALIGN_TOP_LEFT 加固定 x，不用 TOP_RIGHT：后者按对象实际
         * 宽度反推 x，标签宽度变化时位置会漂。
         * 宽度预算（屏宽 240，中文 18px/字）：滑块 8..138，左提示 142..178
         * （2 字 36px），右提示 190..226，两段各留 12px 间隙不重叠。 */
        hint = ui_label(body, s_item_hint[i][0], &zh18, 0xFFFFFF);
        lv_obj_align(hint, LV_ALIGN_TOP_LEFT, HINT_LX, ROW_Y0 + i * ROW_DY);
        hint = ui_label(body, s_item_hint[i][1], &zh18, 0xFFFFFF);
        lv_obj_align(hint, LV_ALIGN_TOP_LEFT, HINT_RX, ROW_Y0 + i * ROW_DY);

        s_slider[i] = lv_slider_create(body);
        lv_obj_set_size(s_slider[i], SLIDER_W, 10);
        lv_obj_align(s_slider[i], LV_ALIGN_TOP_LEFT, ROW_X, ROW_Y0 + i * ROW_DY + 22);
        lv_slider_set_range(s_slider[i], 0, 200);
        /* 初值来自 flash 配置：开机与进页面都恢复上次效果。
         * 滑块值 = 参数 + 100（0 位于正中）。 */
        lv_slider_set_value(s_slider[i], cfg_saved[i] + 100, LV_ANIM_OFF);
        s_val[i] = cfg_saved[i];
        lv_slider_set_mode(s_slider[i], LV_SLIDER_MODE_NORMAL);
        lv_obj_clear_flag(s_slider[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_slider[i], LV_OBJ_FLAG_CLICK_FOCUSABLE);

        /* 样式沿用 ui 的原始控件样式（低透明轨道 + 亮把手），只压低底色不透明度，
         * 保证既能看清背景又保留控件形态。去除的叠加层指 g_layer_app 那层 90%
         * 深蓝加导航栏/按钮底色，不含滑块自身的轨道与把手。
         * LVGL 9.x 的 lv_obj_set_style_* 第三个参数是 selector 而非 part：
         * 轨道、已选段、把手分别为 LV_PART_MAIN / LV_PART_INDICATOR / LV_PART_KNOB。 */
        lv_obj_set_style_outline_width(s_slider[i], 0, LV_PART_MAIN);
        /* 用 pad 把轨道做出细条效果（LVGL 9 没有 bg_width） */
        lv_obj_set_style_pad_ver(s_slider[i], 2, LV_PART_MAIN);
        lv_obj_set_style_radius(s_slider[i], 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_slider[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_slider[i], LV_OPA_30, LV_PART_MAIN);

        /* 已经拖过的部分：亮一点，让人知道当前值在哪 */
        lv_obj_set_style_bg_color(s_slider[i], lv_color_hex(0xFFFFFF),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_slider[i], LV_OPA_70, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_slider[i], 3, LV_PART_INDICATOR);

        /* 滑块把手（knob）—— 大圆点 + 投影，旋钮拖动的目标 */
        lv_obj_set_style_bg_color(s_slider[i], lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(s_slider[i], LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_pad_all(s_slider[i], 9, LV_PART_KNOB);
        lv_obj_set_style_radius(s_slider[i], LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_shadow_width(s_slider[i], 2, LV_PART_KNOB);
        lv_obj_set_style_shadow_opa(s_slider[i], LV_OPA_40, LV_PART_KNOB);

        lv_obj_add_event_cb(s_slider[i], slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(s_slider[i], release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_slider[i], focus_cb, LV_EVENT_FOCUSED, NULL);
    }

    /* 恢复默认按钮：保留 ui_button 的原始控件样式（半透明底 + 描边），
     * 去除的叠加层不含按钮本身。 */
    btn = ui_button(body, S_TN_RESET, 96, 34, NULL);
    /* y=234 而非 262：body 可用高为 320 - 34(NAV_H) = 286，y=262 加高 38 会超
     * 出底边被裁掉。x 右移到 140：左下角被 LVGL 的 FPS/CPU 监视器（宽约 80）
     * 占用，居中放置会重叠；140 + 96 = 236，右侧留 4px。 */
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 140, 234);
    lv_obj_add_event_cb(btn, reset_cb, LV_EVENT_ALL, NULL);

    /* 应用当前值到硬件 */
    for (i = 0; i < ITEM_COUNT; i++)
        s_val[i] = *param_of(i);
    apply_tuning();

    rt_kprintf("[tune] 调色台就绪：旋钮选择滑块 / 左右转调值\r\n");
}

/**
 * @brief  清理调色页（由 ui.c 的 close_done_cb 在 lv_obj_clean 之后调用）
 * @note   必须把 g_val_lb 与 s_slider 全部置 NULL：label 已随
 *         lv_obj_clean(g_app_body) 释放，但可能仍有排队中的 lv_async_call
 *         （value_flush_async）未执行，解引用旧 g_val_lb 即野指针（实测挂在
 *         LVGL 线程，usage fault UNALIGNED）。置 NULL 后回调开头的判据可安全短路。
 *         其余缓存一并归零，避免下次进页面沿用旧值。
 */
void app_tune_teardown(void)
{
    g_val_lb        = RT_NULL;
    s_slider[0]     = RT_NULL;
    s_slider[1]     = RT_NULL;
    s_slider[2]     = RT_NULL;
    s_val_lb_shown  = 0x7FFFFFFF;
    s_val_lb_want   = 0;
    s_val_defer_pending = 0;
    s_dirty         = 0;
    s_dirty_idx     = -1;

    /* 压测定时器要停掉，否则回调里 s_slider[s_cur] 读到刚置空的指针。
     * s_slider 已全 NULL 且 tune_step_one 开头有判据，但停掉更干净。 */
    s_tune_left = 0;
    if (s_tune_rate_timer) { lv_timer_pause(s_tune_rate_timer); }
}

/* msh tune: 调色台对照与压测开关。用法
 *   tune fullscreen 0|1  0 脏区重绘（默认），1 整屏 invalidate（旧行为，供 A/B）
 *   tune drag N          模拟旋钮拖动当前滑块 N 格，走 slider_cb 真实路径
 *   tune rate HZ         设置模拟旋钮转速，默认 20 格/秒
 *   tune status          查看当前模式与 slider_cb 触发次数
 * 本命令跑在 msh 线程，须经 lv_async_call 进入 LVGL 线程。
 *
 * 压测口径：一格 = 一次 lv_async_call。在 async 回调里连发多次
 * lv_slider_set_value 再调 lv_timer_handler()，会被 already_running 重入保护
 * 合并为一次重绘；改用 rt_thread_mdelay 节流则睡住 LVGL 线程本身，导致主循环
 * 停摆、整批帧冻结。正确做法是每次 async 只改一格，由 lv_timer_create 按
 * 1000/N 毫秒周期驱动，与真实旋钮每格间隔一致。 */
static void tune_async(void *p) { (void)p; lv_obj_invalidate(lv_scr_act()); }

/* 每格一步：由 tune_rate_timer 按额定转速驱动，每 tick 只走一格。
 * 定时器句柄 s_tune_rate_timer、计数 s_tune_left、转速 g_tune_rate_hz 在文件
 * 上方声明，因为 app_tune_teardown 也要用。 */
static void tune_step_one(void)
{
    lv_obj_t *sl = s_slider[s_cur];
    lv_group_t *g;

    if (sl == RT_NULL) return;

    /* 复刻真机旋钮路径，缺一步都不进 slider_cb：
     *   lv_slider_set_value() 走 LV_ANIM_OFF 分支，只 invalidate，不发
     *   LV_EVENT_VALUE_CHANGED；lv_obj_send_event(..., LV_EVENT_KEY, ...) 会被
     *   父类 lv_obj 的焦点导航吃掉。只有 group 处于 edit 模式时，旋钮 diff 才经
     *   lv_group_send_data() 以 LV_EVENT_KEY 发给当前焦点对象，绕过父类导航。
     *
     * 因此需要三步：1) 焦点在滑块上（进页面时焦点默认在返回按钮，须先聚焦滑块）；
     * 2) 进入 edit 模式；3) 发一格 LV_KEY_RIGHT。 */
    g = lv_obj_get_group(sl);
    if (g == RT_NULL) return;

    if (lv_group_get_focused(g) != sl) lv_group_focus_obj(sl);
    if (!lv_group_get_editing(g))      lv_group_set_editing(g, true);

    lv_group_send_data(g, LV_KEY_RIGHT);
}

/* 压测节拍器：按额定转速每周期走一格，走完 s_tune_left 格后暂停自己 */
static void tune_rate_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_tune_left <= 0)
    {
        lv_timer_pause(t);
        return;
    }
    s_tune_left--;
    tune_step_one();
}

/* async 回调：启动或更新压测定时器，随后立刻走第一格 */
static void tune_drag(void *p)
{
    int n = (int)(rt_base_t)p;                 /* 低 16 位 = 格数 */

    n = n & 0xFFFF;
    if (s_slider[s_cur] == RT_NULL) return;

    if (s_tune_rate_timer == RT_NULL)
    {
        uint32_t period = (uint32_t)(1000 / (g_tune_rate_hz > 0 ? g_tune_rate_hz : 20));
        s_tune_rate_timer = lv_timer_create(tune_rate_timer_cb, period, NULL);
    }
    /* 更新周期（转速可能被 tune rate 改过），并补跑一格 */
    lv_timer_set_period(s_tune_rate_timer,
                        (uint32_t)(1000 / (g_tune_rate_hz > 0 ? g_tune_rate_hz : 20)));
    s_tune_left = n;
    lv_timer_resume(s_tune_rate_timer);
    tune_step_one();       /* 立刻走第一格，不等一个周期 */
    s_tune_left--;
}

static void tune(int argc, char **argv)
{
    if (argc >= 3 && rt_strcmp(argv[1], "fullscreen") == 0)
    {
        g_tune_fullscreen = (atoi(argv[2]) != 0);
        lv_async_call(tune_async, RT_NULL);
        rt_kprintf("[tune] redraw = %s\r\n",
                   g_tune_fullscreen ? "整屏 invalidate（旧行为）" : "脏区（新，默认）");
        return;
    }
    if (argc >= 2 && rt_strcmp(argv[1], "drag") == 0)
    {
        int n = (argc >= 3) ? atoi(argv[2]) : 20;
        s_step_count = 0;                   /* 诊断：清零，配合 tune status 看回调有没有真跑 */
        lv_async_call(tune_drag, (void *)(rt_base_t)(n & 0xFFFF));
        rt_kprintf("[tune] drag %d 格 @ %d Hz（当前滑块 idx=%d）\r\n",
                   n, g_tune_rate_hz, s_cur);
        return;
    }
    if (argc >= 2 && rt_strcmp(argv[1], "rate") == 0)
    {
        int hz = (argc >= 3) ? atoi(argv[2]) : 20;
        if (hz < 1)   hz = 1;
        if (hz > 500) hz = 500;
        g_tune_rate_hz = hz;
        rt_kprintf("[tune] 旋钮转速 = %d 格/秒\r\n", hz);
        return;
    }
    rt_kprintf("[tune] redraw = %s | slider_cb 触发 %u 次 | 转速 %d Hz  (usage: tune fullscreen 0|1 | tune rate HZ | tune drag N)\r\n",
               g_tune_fullscreen ? "整屏 invalidate（旧行为）" : "脏区（新，默认）",
               (unsigned)s_step_count, g_tune_rate_hz);
}
MSH_CMD_EXPORT(tune, tune app: fullscreen 0|1 | rate HZ | drag N | status (A/B test));