/**
 * @file    ui.c
 * @brief   NS800RT7P65 桌面框架：桌布层 / App 页 / 状态栏 / 壁纸 / msh 调试命令
 *
 * 240x320 竖屏、LVGL 9.1.0。桌面为 3 列 App 网格（每页 APPS_PER_PAGE 个，
 * 共 APP_COUNT 个）加底部翻页条；顶部状态栏显示时间 / 日期 / 联网状态。
 * 壁纸来自外置存储，焦点反馈由「高亮框 + 标签提亮」承担。
 *
 * 交互（旋钮 + 单按键）：
 *   旋转  = 在 App 之间顺序移动焦点，到底回绕
 *   KEY   = 打开焦点所在 App
 *   App 内：旋转选择控件，KEY 执行；左上角「返回」回到桌面
 *
 * 文件分工：本文件管桌面、导航与 App 页框架；各 App 的内容在 ui_apps.c。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <lvgl.h>
#include <stdlib.h>          /* atoi（uiapp 命令用） */

#include "ui.h"
#include "ui_text.h"
#include "settings.h"
#include "ui_apps.h"
#include "wallpaper_store.h"
#include "c3_link.h"         /* 桌面状态栏：时间 / 联网状态 */
#include "c3_report.h"       /* 业务数据周期上行到 C3 */
#include "app_alarm.h"       /* 闹钟：常驻到点判定 + 响铃 */

/* ── 尺寸 ── */
#define SCR_W        240
#define SCR_H        320
#define SB_H         24                     /* 桌面状态栏高（时间 + 日期 + 联网） */
#define NAV_H        34                     /* App 页导航栏高 */
/* 网格起点 = 状态栏高度 + 12 px 间距。
 * 3 行 x CELL_H(82) = 246，自 y=36 起占到 282；底部翻页条在 290..316，互不重叠。 */
#define GRID_Y       (SB_H + 12)
#define CELL_W       80
/* cell 高 = 图标 50 + 间距 4 + 标签 20 = 74；高亮框内缩 3 px 后高 76，
 * 上下各留 1 px，图标顶与标签底均不越出高亮框。 */
#define CELL_H       82
#define APP_COUNT    14
#define APPS_PER_PAGE 9                /* 每页 9 个（3 列 x 3 行）*/
#define APP_PAGES    ((APP_COUNT + APPS_PER_PAGE - 1) / APPS_PER_PAGE)
#define SLIDE_MS     240

/* ── App 描述表 ── */
typedef struct
{
    const char *name;                   /* 中文名（zh18 字库） */
    const char *sym;                    /* 图标内容：LVGL 符号，或 "°C" 这类文字 */
    uint32_t    color;                  /* 图标主色 */
    void      (*build)(lv_obj_t *body); /* 构建页面内容，入参为 body 容器 */
    void      (*tick)(void);            /* 周期刷新回调，可为 NULL */
    uint32_t    period;                 /* tick 周期（ms）；0 = 不需要周期刷新 */
    /* 1 = 这个 App 不叠加导航栏/按钮的第二层底色；全屏暗纱始终保留。
     * 暗纱两边都需要（否则壁纸过曝，颜色被推到高光），置 1 只为避免
     * 「暗纱 + 导航栏底」两层叠加导致顶部比中间更暗。 */
    uint8_t     flat_nav;
    /* 可选：离开 App 时的清理钩子，在 lv_obj_clean(g_app_body) 之后调用。
     * 供自建长寿命对象/定时器/async 回调的 App 使用 —— 框架不会代为销毁，
     * 不清理则下一个 tick 会拿已释放对象当指针。不需要的 App 填 NULL。 */
    void      (*teardown)(void);
} app_def_t;

void app_tune_build(lv_obj_t *body);
void app_tune_teardown(void);

static const app_def_t g_apps[APP_COUNT] =
{
    /* 温度：flat_nav = 0；tick 2000 ms 刷新读数 */
    { S_A_TEMP,  "\xC2\xB0" "C",         AC_TEMP,  app_temp_build,      app_temp_tick,      2000 },
    /* 湿度：flat_nav = 0；tick 2000 ms 刷新读数 */
    { S_A_HUMI,  LV_SYMBOL_TINT,         AC_HUMI,  app_humi_build,      app_humi_tick,      2000 },
    /* 编码器：flat_nav = 0；tick 100 ms 刷新格数 */
    { S_A_ENC,   LV_SYMBOL_REFRESH,      AC_ENC,   app_enc_build,       app_enc_tick,        100 },
    /* 系统：flat_nav = 0；tick 500 ms 刷新运行信息 */
    { S_A_SYS,   LV_SYMBOL_SETTINGS,     AC_SYS,   app_sys_build,       app_sys_tick,        500 },
    /* 秒表：flat_nav = 0；tick 50 ms 刷新计时 */
    { S_A_STOPW, LV_SYMBOL_PLAY,         AC_STOPW, app_stopwatch_build, app_stopwatch_tick,   50 },
    /* 蜂鸣器：flat_nav = 0；无 tick（纯操作页） */
    { S_A_BUZZ,  LV_SYMBOL_BELL,         AC_BUZZ,  app_buzzer_build,    NULL,                  0, 0 },
    /* LED：flat_nav = 0；无 tick（纯操作页） */
    { "LED",     LV_SYMBOL_BULLET,       AC_LED,   app_led_build,       NULL,                  0, 0 },
    /* 背光：flat_nav = 0；无 tick（纯操作页） */
    { S_A_BL,    LV_SYMBOL_EYE_OPEN,     AC_BL,    app_backlight_build, NULL,                  0, 0 },
    /* 图片：看图工具（原寸 / 铺满 / 放大），数据源同桌面壁纸。
     * flat_nav = 1：不叠加 g_layer_app 的 90% 深蓝底，否则图片被蒙上蓝灰，
     * 颜色与桌面壁纸对不上。
     * teardown 需要：模块级 lv_image 与取景框指针要清空。 */
    { S_A_IMG,   LV_SYMBOL_IMAGE,        AC_IMG,   app_img_build,       NULL,                  0, 1,
      app_img_teardown },
    /* 关于：flat_nav = 0；无 tick（静态信息页） */
    { S_A_ABOUT, LV_SYMBOL_LIST,         AC_ABOUT, app_about_build,     NULL,                  0, 0 },
    /* 调色台：gamma 曲线实时调冷暖/明暗/饱和。
     * flat_nav = 1：不叠加导航栏底色，使顶部与中部暗度一致，便于横向比较颜色。 */
    { S_A_TUNE,  LV_SYMBOL_SETTINGS,     AC_BL,    app_tune_build,      NULL,                  0, 1,
      app_tune_teardown },
    /* 游戏：横屏跑酷（猫跳石头）。
     * flat_nav = 1：游戏自绘整屏背景，不能叠加 g_layer_app 的 90% 深蓝底与导航栏底。
     * teardown 需要：收回 30 ms 定时器并切回竖屏，否则回到桌面仍是横屏、定时器拿野指针。
     * 实现见 applications/lcd_game_ui.c（声明在 ui_apps.h）。 */
    { S_A_GAME,  LV_SYMBOL_PLAY,         AC_GAME,  app_game_build,      NULL,                  0, 1,
      app_game_teardown },
    /* 配网：给 ESP32-C3 下发 WiFi 配置（STA 连路由 / AP 开热点）。
     * flat_nav = 0；无 tick；无 teardown（只建常驻控件，框架的 lv_obj_clean 够用）。 */
    { S_A_NET,   LV_SYMBOL_WIFI,         AC_NET,   app_net_build,       NULL,                  0, 0 },
    /* 闹钟：最多 4 个，到点响铃。
     * flat_nav = 0；tick 500 ms 轮询「响铃状态有没有变」，变了才重画。
     * 真正的到点判定不在此处，而在常驻的 alarm_watch_cb()：闹钟与用户在哪个
     * 界面无关，放 App tick 里则停在桌面永远不会响。 */
    { S_A_ALARM, LV_SYMBOL_BELL,         AC_ALARM, app_alarm_build,     app_alarm_tick,      500, 0,
      app_alarm_teardown },
};

/* 调色 App 实现于 applications/lcd_tune_ui.c，声明已在 g_apps 表之前给出。 */

/* 编译期校验：表项数必须等于 APP_COUNT。
 * 二者不一致时 uitest/uiapp 会按 APP_COUNT 遍历，越界读出 NULL 的 build
 * 指针，调用即 usage fault，因此必须在这里拦截。
 * g_apps 是文件作用域数组，sizeof 在编译期可定值。 */
_Static_assert(sizeof(g_apps) / sizeof(g_apps[0]) == APP_COUNT,
               "APP_COUNT 与 g_apps[] 表项数不一致！");

/* ── 界面状态 ── */
static lv_obj_t *g_layer_home;
static lv_obj_t *g_layer_app;
static lv_obj_t *g_scrim;   /* 全屏暗纱（常开，防壁纸过曝） */
static lv_obj_t *g_nav;     /* App 顶部导航栏 */
static lv_obj_t *g_cell[APP_COUNT];

/* 左右翻页箭头（底部右下角）。group 重建时要用，所以放全局区。 */
static lv_obj_t *g_pg_prev, *g_pg_next;
static lv_obj_t *g_icon[APP_COUNT];
static lv_obj_t *g_tag[APP_COUNT];

static lv_obj_t *g_nav_title;
static lv_obj_t *g_btn_back;
static lv_obj_t *g_app_body;

/* ── 桌面状态栏（时间 / 日期 / 联网状态）──
 * 时间来自 C3 的 NTP 对时（经板间链路下发），
 * 详见 c3_link.c 的 NS800_CMD_TIME。 */
static lv_obj_t *g_sb_time;      /* "14:23:59" */
static lv_obj_t *g_sb_date;      /* "10/11 周日" */
static lv_obj_t *g_sb_arc[3];    /* WiFi 图标的三段弧，从外到内 */
static lv_obj_t *g_sb_dot;       /* WiFi 图标底部的圆点 */
static lv_timer_t *g_sb_timer;   /* 1s 刷新 */

/* 累计运行时长（断电保存）的常驻落盘定时器，创建处见 ui_init()。
 * 与状态栏分开：状态栏只在首页存在，而 uptime 落盘必须始终进行。 */
static lv_timer_t *g_uptime_timer;

static int          g_home_page = 0;   /* 当前页（0..APP_PAGES-1） */
static lv_obj_t*g_page_dot[APP_PAGES];  /* 页码小圆点 */
static int          g_cur_app   = -1;   /* -1 = 在桌面 */
/* 正在关闭的 App 索引。app_close() 立即把 g_cur_app 置 -1（防止重入），
 * 但 teardown 要等淡出动画结束才调，那时必须知道关的是谁，故单独存一份。
 * 见 close_done_cb() 的注释。 */
static int          g_closing_app = -1;
static int          g_home_sel  = 0;
static int          g_anim_lock = 0;
static lv_timer_t  *g_app_timer;

/* 打开/关闭 App（定义在后面，事件回调要先用） */
void app_open(int idx);
void app_close(void);

/* ── 通用工具 ── */

/**
 * @brief  一次按键只放行一次的边沿判定
 * @return 1 = 本次可响应；0 = 距上次过近，应忽略
 * @note   LVGL encoder 会为一次 KEY 连发多个事件，所以设 150 ms 去抖窗口。
 */
int ui_click_ok(void)
{
    static rt_tick_t last = 0;
    rt_tick_t now = rt_tick_get();

    if (last != 0 && (now - last) < rt_tick_from_millisecond(150))
        return 0;
    last = now;
    return 1;
}

/* 颜色按比例变暗（图标纵向渐变用：同色系下压一档，有厚度不刺眼） */
static uint32_t col_dim(uint32_t c, int pct)
{
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)pct / 100U;
    uint32_t g = ((c >>  8) & 0xFF) * (uint32_t)pct / 100U;
    uint32_t b = ( c        & 0xFF) * (uint32_t)pct / 100U;
    return (r << 16) | (g << 8) | b;
}

static void anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void slide_lock_release(lv_anim_t *a);

/* ── 淡入淡出（替代横向滑动）──
 *
 * 横向滑动只改 x 坐标，但 LVGL 会重绘整个移动图层（整屏 240x320 = 76800 B）；
 * SPI 50 MHz 刷一屏约 25 ms，240 ms 动画只能跑约 10 帧，视觉上是「闪一下」，
 * 且滑动时两侧会露出黑边。
 * 淡入淡出只改不透明度，旧层淡出后立即隐藏，重绘面积约为单层，刷屏量减半。 */
static void anim_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* 让 obj 从当前不透明度渐变到 target。
 * 起点必须取当前实际值，否则从 0 开始会闪一下。 */
static void fade_to(lv_obj_t *o, lv_opa_t target, uint32_t ms,
                    lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_opa_t cur = lv_obj_get_style_opa(o, 0);

    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, (int32_t)cur, (int32_t)target);
    lv_anim_set_time(&a, ms);
    lv_anim_path_ease_out(&a);
    /* 无论业务要不要回调，都必须挂上 —— 动画结束是唯一能解锁的时机，
     * 漏掉一次 g_anim_lock 就永远为 1，返回键从此失效。 */
    lv_anim_set_completed_cb(&a, done ? done : slide_lock_release);
    lv_anim_start(&a);
    g_anim_lock = 1;
}

/* 滑入动画的默认收尾：只解锁。滑出时用 close_done_cb（还要隐藏+清空）。 */
static void slide_lock_release(lv_anim_t *a)
{
    (void)a;
    g_anim_lock = 0;
}

static void slide_x(lv_obj_t *o, lv_coord_t from, lv_coord_t to,
                    lv_anim_completed_cb_t done)
{
    lv_anim_t a;

    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, SLIDE_MS);
    lv_anim_path_ease_out(&a);
    /* 与 fade_to 同理：completed_cb 必须挂，否则 g_anim_lock 无法解锁。 */
    lv_anim_set_completed_cb(&a, done ? done : slide_lock_release);
    lv_anim_start(&a);
    g_anim_lock = 1;
}

/* ── 组件工厂 ── */

/**
 * @brief  让对象可聚焦并加入默认 group
 * @param  o  目标对象
 */
void ui_make_focusable(lv_obj_t *o)
{
    lv_group_t *g = lv_group_get_default();

    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    if (g) lv_group_add_obj(g, o);
}

/* 焦点高亮：平时淡描边，聚焦时青色描边 + 背景提亮 */
static void focus_style_cb(lv_event_t *e)
{
    lv_obj_t *o  = lv_event_get_target(e);
    int on = (lv_event_get_code(e) == LV_EVENT_FOCUSED);

    lv_obj_set_style_border_color(o, lv_color_hex(on ? C_ACCENT : C_LINE), 0);
    lv_obj_set_style_border_opa(o, on ? LV_OPA_COVER : LV_OPA_20, 0);
    lv_obj_set_style_border_width(o, on ? 2 : 1, 0);

    /* 调色页要求完全露出背景，聚焦时提亮成白块会毁掉调色参照，
     * 所以「本来就想透明」的对象（bg_opa 为 0）不加底色，只保留描边。
     * 判断依据：当前 bg_opa 是 0 即视为调用方要纯透明。 */
    {
        lv_opa_t cur = (lv_opa_t)lv_obj_get_style_bg_opa(o, 0);
        if (cur != LV_OPA_TRANSP)
            lv_obj_set_style_bg_opa(o, on ? LV_OPA_30 : LV_OPA_10, 0);
    }
}

/**
 * @brief  套上「按钮」外观并使其可聚焦
 * @param  o  目标对象
 */
void ui_style_button(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_10, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(o, focus_style_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(o, focus_style_cb, LV_EVENT_DEFOCUSED, NULL);
    ui_make_focusable(o);
}

/**
 * @brief  建一个文本 label
 * @param  parent  父对象
 * @param  txt     文本内容
 * @param  font    字体
 * @param  color   文字颜色（0xRRGGBB）
 * @return 新建的 label
 */
lv_obj_t *ui_label(lv_obj_t *parent, const char *txt,
                   const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

/**
 * @brief  建一张卡片（深色半透明底 + 细描边 + 圆角）
 * @param  parent  父对象
 * @param  x, y    相对父对象的坐标
 * @param  w, h    尺寸
 * @return 新建的卡片
 */
lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                  lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *c = lv_obj_create(parent);

    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_10, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_10, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/**
 * @brief  建一个可按下按钮（含文案与外观）
 * @param  parent  父对象
 * @param  txt     按钮文案
 * @param  w, h    尺寸
 * @param  cb      点击回调，可为 NULL
 * @return 新建的按钮
 */
lv_obj_t *ui_button(lv_obj_t *parent, const char *txt,
                    lv_coord_t w, lv_coord_t h, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_t *l;

    ui_style_button(b);
    lv_obj_set_size(b, w, h);

    l = ui_label(b, txt, &zh18, C_TXT);
    lv_obj_center(l);

    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, NULL);
    return b;
}

/* ── 壁纸 ── */

/* 照片壁纸：由 wallpaper_get() 返回的 240x320 RGB565 常驻缓冲（WALL_BYTES），
 * 按 WALL_ZOOM 作为缩放系数铺到屏幕上（WALL_ZOOM = 1 即 1:1 不放大）。
 * 该缓冲由 wallpaper_store.c 从外置存储加载，本函数不负责释放。 */
static lv_obj_t *build_wallpaper(lv_obj_t *scr)
{
    const uint8_t *data = wallpaper_get();
    lv_obj_t *img;
    static lv_image_dsc_t dsc;      /* 必须常驻：LVGL 会长期持有指针 */

    /* 读不到图（文件系统没挂 / 还没灌图）就返回 NULL，
     * 让 ui_init 直接露出纯色背景，不至于黑屏或崩。 */
    if (data == RT_NULL)
        return RT_NULL;

    img = lv_image_create(scr);

    dsc.header.w        = WALL_SRC_W;
    dsc.header.h        = WALL_SRC_H;
    dsc.header.cf       = LV_COLOR_FORMAT_RGB565;
    dsc.header.stride   = WALL_SRC_W * 2;
    dsc.data_size       = WALL_BYTES;
    dsc.data            = data;

    lv_image_set_src(img, &dsc);
    /* LVGL 缩放系数 256 = 1.0，512 = 2.0；WALL_ZOOM 定义见 wallpaper_store.h */
    lv_image_set_scale_x(img, WALL_ZOOM * 256);
    lv_image_set_scale_y(img, 256);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    return img;
}

/* 照片上压一层极淡的暗色蒙版：
 * 图标与文字直接压在亮部（白床单）上会糊，加 25% 黑纱后对比度稳定，
 * 又不会把照片压得发灰。 */
static void build_scrim(lv_obj_t *scr)
{
    lv_obj_t *s = lv_obj_create(scr);

    g_scrim = s;                       /* 句柄存起来，供按 App 开关 */
    lv_obj_remove_style_all(s);
    lv_obj_set_pos(s, 0, 0);
    lv_obj_set_size(s, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_40, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    /* 装饰层，别吃触摸/编码器事件 */
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
}

/* ── App 网格 ── */

/* ── 焦点反馈：一个会移动的白色半透明圆角高亮框 ──
 * 图标尺寸恒定，焦点靠高亮框位置表达：框限制在 cell 内，不会溢出屏幕，
 * 也不会像放大图标那样把第一行图标顶边顶出屏幕裁平。
 * g_cell_hl 只建一个，焦点变了就挪位置，不占多份对象。 */
#define ICON_SZ         50
#define ICON_BOTTOM     57          /* 图标底边在 cell 内的 y（下方留给标签+方框留白）*/
#define TAG_GAP         4           /* 图标底 → 标签顶 的间距 */
#define TAG_LINE_H      20          /* zh18 的 line_height（见 lv_font_zh18.c）*/
/* 标签底边距 cell 底多远：算出来正好让"图标+标签"在高亮框里上下等距 */
#define TAG_BOTTOM_UP   (CELL_H - (ICON_BOTTOM + TAG_GAP + TAG_LINE_H))
#define HL_INSET        3           /* 高亮框相对 cell 四周的内缩 */
#define HL_RADIUS       16
#define HL_OPA          LV_OPA_30   /* 30% 白：压在照片亮部也看得清，又不抢主体 */

static lv_obj_t *g_cell_hl;

static void build_cell_hl(lv_obj_t *home)
{
    g_cell_hl = lv_obj_create(home);
    lv_obj_remove_style_all(g_cell_hl);
    lv_obj_set_size(g_cell_hl, CELL_W - 2 * HL_INSET, CELL_H - 2 * HL_INSET);
    lv_obj_set_style_radius(g_cell_hl, HL_RADIUS, 0);
    lv_obj_set_style_bg_color(g_cell_hl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_cell_hl, HL_OPA, 0);
    lv_obj_set_style_border_width(g_cell_hl, 0, 0);
    lv_obj_clear_flag(g_cell_hl, LV_OBJ_FLAG_SCROLLABLE);
    /* 纯装饰，别抢编码器/点击事件 */
    lv_obj_remove_flag(g_cell_hl, LV_OBJ_FLAG_CLICKABLE);
}

/* idx 是全局 App 序号（0..APP_COUNT-1），而 cell 按页内 slot 摆放
 * （build_grid 里 slot = i % APPS_PER_PAGE），所以必须先换算成页内 slot 再算坐标。
 * 不换算会把第 2 页的高亮框算到屏幕外，出现「聚焦了却看不见白框」；
 * 第 1 页 idx 与 slot 恰好一致，看不出问题。 */
static void move_cell_hl(int idx)
{
    int slot;

    if (g_cell_hl == RT_NULL)
        return;

    slot = idx % APPS_PER_PAGE;          /* 页内 slot，不能省 */
    lv_obj_set_pos(g_cell_hl,
                   (slot % 3) * CELL_W + HL_INSET,
                   GRID_Y + (slot / 3) * CELL_H + HL_INSET);
}

/* 焦点反馈：文字提亮 + 高亮框挪过去。图标尺寸不动。 */
static void cell_focus_visual(int idx, int on)
{
    lv_obj_set_style_text_color(g_tag[idx],
                                lv_color_hex(on ? C_TXT : C_TXT_DIM), 0);
    if (on)
        move_cell_hl(idx);
}

/* 网格单元事件：焦点变化时更新高亮，点击时打开对应 App。 */
static void cell_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(rt_base_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_FOCUSED)
    {
        g_home_sel = idx;
        cell_focus_visual(idx, 1);
    }
    else if (code == LV_EVENT_DEFOCUSED)
    {
        cell_focus_visual(idx, 0);
    }
    else if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED)
    {
        if (ui_click_ok()) app_open(idx);
    }
}

/* 分页相关（定义在 build_grid 之后，这里先前置声明） */
static void page_dots_update(void);
static void home_page_show(int page);
static void build_page_bar(lv_obj_t *home);
static void home_group_refill(void);
static void focus_style_cb(lv_event_t *e);

static void build_grid(lv_obj_t *home)
{
    int i;

    /* 高亮框必须先建：它要垫在所有 cell 下面（LVGL 按创建顺序绘制） */
    build_cell_hl(home);

    for (i = 0; i < APP_COUNT; i++)
    {
        int slot  = i % APPS_PER_PAGE;          /* 页内序号 0..(APPS_PER_PAGE-1) */
        int page  = i / APPS_PER_PAGE;          /* 所属页 */
        int col   = slot % 3, row = slot / 3;
        const app_def_t *a = &g_apps[i];
        lv_obj_t *c, *ic, *sym, *tag;

        c = lv_obj_create(home);
        lv_obj_remove_style_all(c);
        lv_obj_set_pos(c, col * CELL_W, GRID_Y + row * CELL_H);
        /* 非当前页的整页隐藏 —— 翻页时切 LV_OBJ_FLAG_HIDDEN */
        if (page != g_home_page)
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(c, CELL_W, CELL_H);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        /* 图标尺寸恒定、高亮框也在 cell 内，cell 无需允许子对象溢出 */
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_add_event_cb(c, cell_event_cb, LV_EVENT_ALL, (void *)(rt_base_t)i);

        ic = lv_obj_create(c);
        lv_obj_remove_style_all(ic);
        lv_obj_set_size(ic, ICON_SZ, ICON_SZ);
        lv_obj_set_pos(ic, (CELL_W - ICON_SZ) / 2, ICON_BOTTOM - ICON_SZ);
        lv_obj_set_style_radius(ic, 15, 0);
        /* 扁平实色 + 纵向微渐变（同色系下压 20%，有厚度但不像玻璃拟物） */
        lv_obj_set_style_bg_color(ic, lv_color_hex(a->color), 0);
        lv_obj_set_style_bg_grad_color(ic, lv_color_hex(col_dim(a->color, 80)), 0);
        lv_obj_set_style_bg_grad_dir(ic, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
        /* 照片背景花，图标描一圈半透明白边才立得住 */
        lv_obj_set_style_border_width(ic, 1, 0);
        lv_obj_set_style_border_color(ic, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_opa(ic, LV_OPA_40, 0);
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);

        sym = ui_label(ic, a->sym, &lv_font_montserrat_32, 0xFFFFFF);
        lv_obj_center(sym);
        /* 图标字形加投影，白字压浅色图标也有边界 */
        lv_obj_set_style_text_opa(sym, LV_OPA_COVER, 0);

        /* 标签底边相对 cell 底再抬 TAG_BOTTOM_UP，使"图标 + 标签"在高亮框里上下等距。
         *   内容高 = ICON_SZ(50) + TAG_GAP(4) + zh18.line_height(20) = 74
         *   框  高 = CELL_H(82) - 2*HL_INSET(3)                = 76  → 上下各 1 px
         * 展开后 TAG_BOTTOM_UP = 82 - (57 + 4 + 20) = 1 */
        tag = ui_label(c, a->name, &zh18, C_TXT);
        lv_obj_align(tag, LV_ALIGN_BOTTOM_MID, 0, -TAG_BOTTOM_UP);
        lv_obj_set_style_text_opa(tag, LV_OPA_COVER, 0);

        g_cell[i] = c;
        g_icon[i] = ic;
        g_tag[i]  = tag;
    }

    /* 先摆到开机默认焦点上，免得高亮框在 (0,0) 闪一下 */
    move_cell_hl(g_home_sel);
    page_dots_update();
}

/* 桌面 group 成员 = 全部 cell + 左右翻页箭头。
 * ui_init / App 返回 / 切页后三处都要重建，统一抽成此函数，
 * 避免漏加箭头导致「旋钮永远翻不到第二页」。 */
static void home_group_refill(void)
{
    lv_group_t *g = lv_group_get_default();
    int i;

    if (g == RT_NULL) return;
    lv_group_remove_all_objs(g);
    for (i = 0; i < APP_COUNT; i++)
        lv_group_add_obj(g, g_cell[i]);
    if (g_pg_prev) lv_group_add_obj(g, g_pg_prev);
    if (g_pg_next) lv_group_add_obj(g, g_pg_next);
}

/* ── 翻页 ──
 * 每页 APPS_PER_PAGE 个 App（3x3），共 APP_PAGES 页。
 * 做法：切每个 cell 的 HIDDEN 标志 + 挪高亮框。 */
static void home_page_show(int page)
{
    int i;

    if (page < 0) page = 0;
    if (page >= APP_PAGES) page = APP_PAGES - 1;
    g_home_page = page;

    /* 记住停在哪一页，开机自动回到这一页（掉电也不丢）。
     * 只在用户真的翻页时落盘：开机时 build_home 只改内存不调这里，
     * 若此处也写，会把刚读出的值又写回去，覆盖用户已存的设置。 */
    if (settings_is_dirty())
        settings_update_one('p', page);

    for (i = 0; i < APP_COUNT; i++)
    {
        int p = i / APPS_PER_PAGE;
        if (g_cell[i] == RT_NULL) continue;
        if (p == page) lv_obj_remove_flag(g_cell[i], LV_OBJ_FLAG_HIDDEN);
        else            lv_obj_add_flag(g_cell[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 高亮框要跟着当前页第一个 App 走 */
    move_cell_hl(page * APPS_PER_PAGE);
    page_dots_update();

    /* 切页只给 cell 打 HIDDEN 标志，LVGL 不会重新分配焦点。
     * 焦点若停在被隐藏的 cell 上，focus_next 会跳过它，旋钮即「失联」
     * （按 KEY 无反应、转旋钮也不动），因此这里要手动把焦点拉回当前页。 */
    home_group_refill();
    {
        lv_group_t *g = lv_group_get_default();
        lv_obj_t *f = (g) ? lv_group_get_focused(g) : RT_NULL;
        int fidx = -1;

        for (i = 0; i < APP_COUNT; i++)
            if (g_cell[i] == f) { fidx = i; break; }

        /* 焦点在隐藏页的 cell 上，或压根没有焦点 -> 拉回当前页首个 */
        if (fidx < 0 || (fidx / APPS_PER_PAGE) != page)
            lv_group_focus_obj(g_cell[page * APPS_PER_PAGE]);
        else
            lv_group_focus_obj(f);  /* 同页：重新 focus 让高亮框跟上 */
    }

    rt_kprintf("[ui] page %d/%d\n", page + 1, APP_PAGES);
}

/* g_pg_prev / g_pg_next 在文件顶部的全局区声明（group 重建要用） */

/* 翻页箭头点击：按方向切页，去抖后调用 home_page_show()。 */
static void page_arrow_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int dir = (int)(rt_base_t)lv_event_get_user_data(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED)
        return;
    if (!ui_click_ok()) return;
    home_page_show(g_home_page + dir);
}

/* 更新页码小圆点的透明度：当前页不透明，其余页半透明。 */
static void page_dots_update(void)
{
    int p;
    for (p = 0; p < APP_PAGES; p++)
    {
        if (g_page_dot[p] == RT_NULL) continue;
        lv_obj_set_style_bg_opa(g_page_dot[p],
                                (p == g_home_page) ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

/* 构建底部翻页条：左箭头 + 右箭头 + 页码小圆点。 */
static void build_page_bar(lv_obj_t *home)
{
    int p;

    /* 左箭头 */
    g_pg_prev = lv_obj_create(home);
    lv_obj_remove_style_all(g_pg_prev);
    lv_obj_set_size(g_pg_prev, 32, 26);
    /* 左下角被 LVGL 性能监视器（FPS/CPU）占着，故左箭头放到右下角，
     * 右边只放页码点 */
    lv_obj_align(g_pg_prev, LV_ALIGN_BOTTOM_RIGHT, -74, -4);
    lv_obj_set_style_bg_color(g_pg_prev, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_pg_prev, LV_OPA_20, 0);
    lv_obj_set_style_radius(g_pg_prev, 8, 0);
    lv_obj_clear_flag(g_pg_prev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_pg_prev, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(g_pg_prev, page_arrow_cb, LV_EVENT_ALL, (void *)(rt_base_t)-1);
    /* 箭头必须进 encoder group，否则旋钮只能在格子间循环，
     * 永远到不了翻页条（第二页的 cell 是隐藏的，LVGL 会跳过）。 */
    lv_obj_add_event_cb(g_pg_prev, focus_style_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(g_pg_prev, focus_style_cb, LV_EVENT_DEFOCUSED, NULL);
    ui_make_focusable(g_pg_prev);
    {
        lv_obj_t *l = ui_label(g_pg_prev, LV_SYMBOL_LEFT, &lv_font_montserrat_32, 0xFFFFFF);
        lv_obj_center(l);
        lv_obj_set_style_text_opa(l, LV_OPA_80, 0);
    }

    /* 右箭头 */
    g_pg_next = lv_obj_create(home);
    lv_obj_remove_style_all(g_pg_next);
    lv_obj_set_size(g_pg_next, 32, 26);
    lv_obj_align(g_pg_next, LV_ALIGN_BOTTOM_RIGHT, -38, -4);
    lv_obj_set_style_bg_color(g_pg_next, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_pg_next, LV_OPA_20, 0);
    lv_obj_set_style_radius(g_pg_next, 8, 0);
    lv_obj_clear_flag(g_pg_next, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_pg_next, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(g_pg_next, page_arrow_cb, LV_EVENT_ALL, (void *)(rt_base_t)1);
    lv_obj_add_event_cb(g_pg_next, focus_style_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(g_pg_next, focus_style_cb, LV_EVENT_DEFOCUSED, NULL);
    ui_make_focusable(g_pg_next);
    {
        lv_obj_t *l = ui_label(g_pg_next, LV_SYMBOL_RIGHT, &lv_font_montserrat_32, 0xFFFFFF);
        lv_obj_center(l);
        lv_obj_set_style_text_opa(l, LV_OPA_80, 0);
    }

    /* 页码小圆点（右下角排列） */
    for (p = 0; p < APP_PAGES; p++)
    {
        g_page_dot[p] = lv_obj_create(home);
        lv_obj_remove_style_all(g_page_dot[p]);
        lv_obj_set_size(g_page_dot[p], 8, 8);
        lv_obj_set_style_radius(g_page_dot[p], 4, 0);
        lv_obj_set_style_bg_color(g_page_dot[p], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(g_page_dot[p],
                                (p == g_home_page) ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_clear_flag(g_page_dot[p], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(g_page_dot[p], LV_ALIGN_BOTTOM_RIGHT,
                     -16 - (APP_PAGES - 1 - p) * 11, -8);
    }
}

/* ── 桌面状态栏 ──
 *
 * 布局（240 宽，高 SB_H = 24）：左侧时间 + 日期，右侧 WiFi 图标，下方为 App 网格。
 *
 * 数据来源：
 *   · 时间/日期 <- C3 的 NTP 对时结果（NS800_CMD_TIME 帧），
 *     存于 c3_link_state_t 的 time_unix / time_tick。
 *     每秒来一帧，这里取最新值 + 本地 tick 差值推算，丢几帧也能继续走。
 *   · 联网状态 <- wifi_state（同系统页用的那套枚举文案）。
 *
 * 时区换算只在 C3 侧一处完成：C3 发来的是已按本地时区算好的时间戳，
 * NS800 只做 tm 拆解，避免两端时区逻辑不一致。 */

/* 把 Unix 时间戳拆成年月日时分秒（不依赖 time.h 的 localtime，
 * 避免 NS800 的 newlib 时区表未配置导致偏差）。UTC+8 已在 C3 侧处理。 */
typedef struct
{
    int year, mon, day, hour, min, sec, wday;   /* wday: 0=周日 */
} sb_tm_t;

/* Unix 时间戳 -> 年月日时分秒。仅支持非负时间戳，闰年按公历规则处理。 */
static void sb_gmtime(rt_int32_t ts, sb_tm_t *out)
{
    rt_int32_t days, rem;
    int y, m;
    /* 1970-01-01 是周四 → 基准 wday=4（0=周日） */
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

    days = ts / 86400;
    rem  = ts % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }

    out->hour = (int)(rem / 3600);
    out->min  = (int)((rem % 3600) / 60);
    out->sec  = (int)(rem % 60);
    out->wday = (int)((days + 4) % 7);
    if (out->wday < 0) out->wday += 7;

    /* 年 */
    y = 1970;
    for (;;)
    {
        int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
        int dy = leap ? 366 : 365;
        if (days < dy) break;
        days -= dy;
        y++;
    }
    out->year = y;

    /* 月 */
    {
        int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
        for (m = 0; m < 12; m++)
        {
            int dm = mdays[m] + ((m == 1 && leap) ? 1 : 0);
            if (days < dm) break;
            days -= dm;
        }
        out->mon = m + 1;
        out->day = (int)days + 1;
    }
}

/* 取"当前时间戳"：以最近一次收到的时间为基准，加上之后流逝的秒数。
 * 这样即使 C3 的 TIME 帧隔了几秒才到（或被丢），显示也连续。
 * @return Unix 时间戳；尚未对到时间时返回 -1。 */
static rt_int32_t sb_now_unix(void)
{
    c3_link_state_t c3;
    rt_int32_t t;

    c3_link_get_state(&c3);
    if (!c3.time_valid)
        return -1;      /* 还没对到时间 */

    t = c3.time_unix;
    /* 加上"收到那帧之后到现在"的秒数 */
    t += (rt_int32_t)((rt_tick_get() - c3.time_tick) / RT_TICK_PER_SECOND);
    return t;
}

/* wday -> 中文星期名；越界按周日处理。w：0=周日。 */
static const char *sb_wday_name(int w)   /* 0=周日 */
{
    static const char *n[7] = { S_WD_0, S_WD_1, S_WD_2, S_WD_3, S_WD_4, S_WD_5, S_WD_6 };
    return n[(w < 0 || w > 6) ? 0 : w];
}

/* 状态栏 1 秒回调：刷新时间/日期文本与 WiFi 图标颜色。 */
static void sb_timer_cb(lv_timer_t *t)
{
    static rt_int32_t last_key = -1;      /* 上次渲染用的秒数或状态，避免每帧重排 */
    c3_link_state_t c3;
    rt_int32_t now;
    sb_tm_t tm;

    (void)t;

    /* ── 时间 / 日期 ── */
    now = sb_now_unix();
    if (now >= 0)
    {
        sb_gmtime(now, &tm);
        /* 显示到秒，去抖键必须用「整秒」而非分钟（now/60）。
         * 用 now/60 时一分钟内 last_key 不变，秒数永远不刷新，
         * 表现为时间停在 mm:ss 不动。 */
        if (now != last_key)
        {
            last_key = now;
            lv_label_set_text_fmt(g_sb_time, "%02d:%02d:%02d",
                                  tm.hour, tm.min, tm.sec);
            lv_label_set_text_fmt(g_sb_date, "%02d/%02d %s",
                                  tm.mon, tm.day, sb_wday_name(tm.wday));
        }
    }
    else
    {
        if (last_key != -2)
        {
            last_key = -2;
            lv_label_set_text(g_sb_time, S_TM_NONE);   /* 未对时 */
            lv_label_set_text(g_sb_date, "");
        }
    }

    /* ── 联网状态（WiFi 图标）── */
    {
        uint32_t col;
        uint32_t i;

        c3_link_get_state(&c3);
        if (!c3.online)
        {
            col = 0xFF6B6B;             /* 红 —— C3 都联系不上 */
        }
        else if (c3.wifi_state == NS800_WIFI_GOT_IP)
        {
            col = AC_STOPW;             /* 绿 —— 已联网 */
        }
        else
        {
            col = AC_SYS;               /* 青 —— 连接中 */
        }
        /* 三段弧 + 圆点一起改色；未联网时整体半透明，视觉上"更淡"= 不可用 */
        lv_obj_set_style_arc_color(g_sb_arc[0], lv_color_hex(col), LV_PART_MAIN);
        lv_obj_set_style_arc_color(g_sb_arc[1], lv_color_hex(col), LV_PART_MAIN);
        lv_obj_set_style_arc_color(g_sb_arc[2], lv_color_hex(col), LV_PART_MAIN);
        for (i = 0; i < 3; i++)
            lv_obj_set_style_arc_opa(g_sb_arc[i],
                                     c3.online ? LV_OPA_COVER : LV_OPA_40,
                                     LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_sb_dot, lv_color_hex(col), 0);
        lv_obj_set_style_bg_opa(g_sb_dot,
                                c3.online ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

/* ── WiFi 图标（三段同心弧 + 底部圆点）──
 *
 * 不用 LV_SYMBOL_WIFI 文字符号：该符号在 U+F1EB（lv_symbol_def.h），只存在于
 * 完整版 montserrat 字体；LVGL 内置的 lv_font_montserrat_14 是子集，
 * unicode_list_0 中不含此码点，用符号只会渲染出空白或豆腐块。
 * 为单个图标重新生成字体不划算（内部 Flash 只剩约 92 KB），
 * 故用 lv_arc 直接画：三个同心弧 + 一个点，零字体、零素材，还能随状态换色。
 *
 * lv_arc 角度约定：0° 在正右方，逆时针为正（见 lv_arc.c 的 start/end 换算）。
 * WiFi 图标开口朝上，弧段取正上方，用 lv_arc_set_bg_angles() 设置背景弧角度。
 */
static void build_wifi_icon(lv_obj_t *parent)
{
    /* 半径序列（从外到内）：8 / 6 / 4
     * 外层弧张开一点（50°..130°），内层收窄，视觉上像信号由强到弱。 */
    static const lv_coord_t r[3]     = { 8, 6, 4 };
    static const lv_coord_t w[3]     = { 2, 2, 2 };
    static const int32_t    a0[3]    = { 50, 58, 66 };
    static const int32_t    a1[3]    = { 130, 122, 114 };
    lv_obj_t *box;
    int i;

    /* 一个透明容器当画布，方便整体对齐；WiFi 整体约 18x14 */
    box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 20, 16);
    lv_obj_align(box, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (i = 0; i < 3; i++)
    {
        lv_obj_t *a = lv_arc_create(box);

        /* 弧对象本身是正方形、且要贴着容器底边中点，这样三层弧才同心 */
        lv_obj_set_size(a, r[i] * 2, r[i] * 2);
        lv_obj_align(a, LV_ALIGN_BOTTOM_MID, 0, 1);

        /* 只画背景弧，不要指示器。
         * NORMAL 模式下 lv_arc 默认 indic_angle 为 135°，会额外画一小段
         * 进度条叠在弧上；remove_style_all() 后 indicator 宽度归 0 本可不显示，
         * 但那是依赖默认值，显式清零更稳。 */
        lv_arc_set_mode(a, LV_ARC_MODE_NORMAL);
        lv_obj_remove_style_all(a);
        lv_obj_set_style_arc_width(a, 0, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);

        lv_arc_set_bg_angles(a, a0[i], a1[i]);
        /* rotation 保持 0：lv_arc.c 画弧时 start/end = bg_angle_* + rotation，
         * 而 bg_angle_* 已直接来自 lv_arc_set_bg_angles() 的原始值，
         * 就是想要的绝对角度，再叠加 rotation 反而会转歪。
         * （v8 时代的某些写法要 +90°，v9 不需要。） */
        lv_arc_set_rotation(a, 0);

        lv_obj_set_style_arc_width(a, w[i], LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(AC_SYS), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
        g_sb_arc[i] = a;
    }

    /* 底部的圆点（信号源） */
    g_sb_dot = lv_obj_create(box);
    lv_obj_remove_style_all(g_sb_dot);
    lv_obj_set_size(g_sb_dot, 3, 3);
    lv_obj_set_style_bg_color(g_sb_dot, lv_color_hex(AC_SYS), 0);
    lv_obj_set_style_bg_opa(g_sb_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_sb_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(g_sb_dot, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* 构建桌面顶部状态栏：时间 + 日期 + WiFi 图标，并起 1 秒刷新定时器。 */
static void build_status_bar(lv_obj_t *home)
{
    /* 时间（左，大字，含秒） */
    g_sb_time = ui_label(home, S_TM_NONE, &lv_font_montserrat_14, C_TXT);
    lv_obj_align(g_sb_time, LV_ALIGN_TOP_LEFT, 10, 6);

    /* 日期（时间右边，小字）。x = 78：时间显示 "14:23:59" 后宽约 16 px，
     * 放 62 会与秒位重叠。 */
    g_sb_date = ui_label(home, "", &zh18, C_TXT_DIM);
    lv_obj_align(g_sb_date, LV_ALIGN_TOP_LEFT, 78, 4);

    /* 联网状态（右）：WiFi 图标，不用文字 */
    build_wifi_icon(home);

    /* 1 秒刷新。不用 app_timer：那是 App 页定时器，只在 App 打开时跑
     * （g_app_timer 受 g_cur_app 守卫），而状态栏在桌面也要一直走，
     * 所以单开一个常驻定时器。 */
    g_sb_timer = lv_timer_create(sb_timer_cb, 1000, NULL);
    sb_timer_cb(NULL);      /* 立即刷一次，别让开机空等 1 秒 */
}

/* ── 累计运行时长落盘（断电保存）──
 *
 * 这里只负责"到点喊一声"，真正的记账/节流/防重复累加都在
 * settings.c 的 settings_uptime_flush() 里（单一出处）。
 * 必须常驻：停在桌面时也要落盘（见 ui_init() 创建处的注释）。
 * 记账已集中在 settings.c，App 页 tick 再调一次也是安全的。 */
static void uptime_flush_cb(lv_timer_t *t)
{
    (void)t;
    settings_uptime_flush();
}

/* ── 闹钟到点判定（常驻，与用户在哪个界面无关）──
 *
 * 必须放在常驻定时器里：闹钟语义是"到点了就该响"，与 App 是否打开、
 * 停在桌面还是游戏里无关；放进 app_alarm_tick()（App 页 tick）则停在桌面
 * 永远不会响。
 *
 * 判定时间基准复用 sb_now_unix()（C3 的 TIME 帧 + 本地流逝秒数），
 * 与状态栏同一口径，不会出现状态栏与闹钟对不上的情况。
 *
 * 周期 1 秒：分钟级精度下足够；alarm_check() 内部有 fired_today 去重，
 * 同一分钟内重复调用不会重复触发。 */
static void alarm_watch_cb(lv_timer_t *t)
{
    (void)t;

    if (alarm_is_ringing())
    {
        /* 正在响：不再重复判定（alarm_check 里的去重已经挡住了，
         * 这里只是省掉每秒一次的时间戳计算和遍历）。 */
        return;
    }
    alarm_check(sb_now_unix());
}

/* 构建桌面：恢复上次页码，再依次建状态栏、网格与翻页条。 */
static void build_home(void)
{
    /* 开机回到上次停住的那一页（settings.c 里存过）。
     * 必须在 build_grid 之前设：build_grid 靠 g_home_page 决定哪些 cell 打 HIDDEN。 */
    {
        rt_settings_t c;
        settings_get(&c);
        if (c.home_page >= 0 && c.home_page < APP_PAGES)
            g_home_page = c.home_page;
    }

    build_status_bar(g_layer_home);  /* 顶部状态栏：时间 / 日期 / 联网 */
    build_grid(g_layer_home);
    build_page_bar(g_layer_home);   /* 左右翻页箭头 + 页码点 */
}

/* ── App 页 ── */

/* App 页周期定时器回调：只驱动当前打开 App 的 tick。 */
static void app_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_cur_app >= 0 && g_apps[g_cur_app].tick)
        g_apps[g_cur_app].tick();
}

/* 导航栏「返回」按钮点击：去抖后关闭当前 App 页。 */
static void back_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED)
    {
        if (ui_click_ok()) app_close();
    }
}

/* 构建 App 页导航栏（返回按钮 + 标题）与内容容器 g_app_body。 */
static void build_nav(lv_obj_t *app)
{
    lv_obj_t *nav = lv_obj_create(app);
    lv_obj_t *ar, *tx;

    g_nav = nav;                        /* 句柄存起来，供按 App 开关 */
    lv_obj_remove_style_all(nav);
    lv_obj_set_pos(nav, 0, 0);
    lv_obj_set_size(nav, SCR_W, NAV_H);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_30, 0);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

    g_btn_back = lv_obj_create(nav);
    ui_style_button(g_btn_back);
    lv_obj_set_size(g_btn_back, 66, 24);
    lv_obj_align(g_btn_back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(g_btn_back, back_click_cb, LV_EVENT_ALL, NULL);

    /* 箭头 + "返回"两个字手动排一下，比 flex 少一层依赖 */
    ar = ui_label(g_btn_back, LV_SYMBOL_LEFT, &lv_font_montserrat_14, C_TXT);
    lv_obj_align(ar, LV_ALIGN_LEFT_MID, 7, 0);
    tx = ui_label(g_btn_back, S_BACK, &zh18, C_TXT);
    lv_obj_align(tx, LV_ALIGN_LEFT_MID, 23, 0);
    (void)ar;
    (void)tx;

    g_nav_title = ui_label(nav, "", &zh18, C_TXT);
    lv_obj_align(g_nav_title, LV_ALIGN_CENTER, 0, 0);

    g_app_body = lv_obj_create(app);
    lv_obj_remove_style_all(g_app_body);
    lv_obj_set_pos(g_app_body, 0, NAV_H);
    lv_obj_set_size(g_app_body, SCR_W, SCR_H - NAV_H);
    lv_obj_clear_flag(g_app_body, LV_OBJ_FLAG_SCROLLABLE);
}

/* 构建 App 层（半透明深蓝底 + 导航栏），初始隐藏。 */
static void build_app_layer(lv_obj_t *scr)
{
    g_layer_app = lv_obj_create(scr);
    lv_obj_remove_style_all(g_layer_app);
    lv_obj_set_pos(g_layer_app, 0, 0);
    lv_obj_set_size(g_layer_app, SCR_W, SCR_H);
    /* App 页：照片留着当氛围，但压到很淡 —— 读数区域必须干净 */
    lv_obj_set_style_bg_color(g_layer_app, lv_color_hex(0x141820), 0);
    lv_obj_set_style_bg_opa(g_layer_app, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(g_layer_app, 0, 0);
    lv_obj_clear_flag(g_layer_app, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_layer_app, LV_OBJ_FLAG_HIDDEN);

    build_nav(g_layer_app);
}

/* ── 全屏接管（游戏 App）──
 *
 * 只有游戏页横屏，桌面/其他 App 保持竖屏。
 * 屏方向由 lv_port_disp.c 的 lv_port_disp_set_orientation() 切；但 UI 这一侧
 * 的固定尺寸是编译期常量（SCR_W/SCR_H/NAV_H），切了方向后 layer/nav/body
 * 仍按 240x320 摆，会出现右半边空、导航栏错位，所以这里必须把 UI 尺寸
 * 也跟着当前分辨率重设一遍。
 * 横竖屏切换需三层配对：屏方向、LVGL 显示方向、本函数（UI 全屏）。
 *
 * 用 lv_display_get_horizontal_resolution() 取当前分辨率，而不是写死 320x240，
 * 这样本函数对两个方向都成立。
 *
 * 结尾的整屏 lv_obj_invalidate() 可能被已有小区域包含而静默丢弃，
 * 故切方向时应在同一帧内、无更小无效区排队的前提下调用。 */
void ui_set_fullscreen(int fullscreen)
{
    lv_display_t *disp = lv_display_get_default();
    lv_coord_t w, h;

    if (disp == NULL) return;
    w = lv_display_get_horizontal_resolution(disp);
    h = lv_display_get_vertical_resolution(disp);

    if (fullscreen)
    {
        /* 导航栏整条隐藏（游戏自己画返回提示），body 铺满整屏 */
        if (g_nav) lv_obj_add_flag(g_nav, LV_OBJ_FLAG_HIDDEN);
        if (g_layer_app) lv_obj_set_size(g_layer_app, w, h);
        if (g_btn_back) lv_obj_add_flag(g_btn_back, LV_OBJ_FLAG_HIDDEN);
        if (g_app_body)
        {
            lv_obj_set_pos(g_app_body, 0, 0);
            lv_obj_set_size(g_app_body, w, h);
        }
    }
    else
    {
        if (g_nav) lv_obj_remove_flag(g_nav, LV_OBJ_FLAG_HIDDEN);
        if (g_layer_app) lv_obj_set_size(g_layer_app, w, h);
        if (g_btn_back) lv_obj_remove_flag(g_btn_back, LV_OBJ_FLAG_HIDDEN);
        if (g_app_body)
        {
            lv_obj_set_pos(g_app_body, 0, NAV_H);
            lv_obj_set_size(g_app_body, w, h - NAV_H > 0 ? h - NAV_H : h);
        }
        /* 导航栏本身宽度也要跟着走 —— 它是在 build_nav 里按 SCR_W 建死的 */
        if (g_nav) lv_obj_set_size(g_nav, w, NAV_H);
    }
    lv_obj_invalidate(lv_screen_active());
}

/**
 * @brief  取当前 body 宽度
 * @return 当前显示分辨率的宽度，无显示设备时返回 SCR_W
 */
lv_coord_t ui_body_width(void)
{
    lv_display_t *disp = lv_display_get_default();
    return disp ? lv_display_get_horizontal_resolution(disp) : SCR_W;
}

/**
 * @brief  取当前 body 高度
 * @return 当前显示分辨率高度；导航栏可见时再减去 NAV_H
 */
lv_coord_t ui_body_height(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_coord_t h = disp ? lv_display_get_vertical_resolution(disp) : SCR_H;

    if (g_nav && !lv_obj_has_flag(g_nav, LV_OBJ_FLAG_HIDDEN))
        h -= NAV_H;
    return h;
}

/**
 * @brief  打开第 idx 个 App 页
 * @param  idx  App 序号（0..APP_COUNT-1）
 * @note   动画进行中或已有 App 打开时直接返回。只能在 LVGL 线程调用。
 */
void app_open(int idx)
{
    const app_def_t *a;
    lv_group_t *g;

    if (g_anim_lock || g_cur_app >= 0) return;

    a = &g_apps[idx];
    g_cur_app = idx;

    /* 暗纱两边都要有 —— 壁纸本身偏亮，不压暗会过曝；调色台也不例外，
     * 过曝时所有颜色被推到高光、冷暖差异被压缩，反而更难判断。
     * 这里只去掉导航栏/按钮的额外底色（叠加在暗纱之上的第二层，
     * 会让顶部比中间更暗）。 */
    if (g_scrim) lv_obj_remove_flag(g_scrim, LV_OBJ_FLAG_HIDDEN);

    if (a->flat_nav)
    {
        /* 需要以壁纸作颜色参照的页面：整层 App 底完全去掉。
         * g_layer_app 默认是 0x141820 + LV_OPA_90（90% 不透明深蓝），
         * 那是给「读数必须干净」的数据页准备的，盖在图片/调色页上等于换了背景。
         * 去掉后：壁纸 → 暗纱（常开，各页一致）→ 控件，逐层与桌面相同。 */
        if (g_nav)       lv_obj_set_style_bg_opa(g_nav, LV_OPA_TRANSP, 0);
        if (g_btn_back) lv_obj_set_style_bg_opa(g_btn_back, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(g_layer_app, LV_OPA_TRANSP, 0);
    }
    else
    {
        if (g_nav)       lv_obj_set_style_bg_opa(g_nav, LV_OPA_30, 0);
        if (g_btn_back) lv_obj_set_style_bg_opa(g_btn_back, LV_OPA_10, 0);
        lv_obj_set_style_bg_opa(g_layer_app, LV_OPA_90, 0);
    }

    /* 清掉上一次的残留内容与定时器。
     * 函数开头的 g_cur_app >= 0 提前返回保证不会在「App 里再开 App」，
     * 所以这里 g_cur_app 必定是 -1，没有"上一个 App"需要 teardown ——
     * teardown 的唯一时机是 close_done_cb（返回桌面时）。 */
    lv_obj_clean(g_app_body);
    if (g_app_timer) { lv_timer_delete(g_app_timer); g_app_timer = NULL; }
    lv_label_set_text(g_nav_title, a->name);

    /* group 先只放"返回"，App 内部的可聚焦控件由 build 过程陆续加入 */
    g = lv_group_get_default();
    if (g)
    {
        lv_group_remove_all_objs(g);
        lv_group_add_obj(g, g_btn_back);
        lv_group_focus_obj(g_btn_back);
    }

    a->build(g_app_body);
    if (a->tick)
    {
        g_app_timer = lv_timer_create(app_timer_cb, a->period, NULL);
        /* 立刻跑一次，否则进页面要空等一个周期才有数据 */
        lv_timer_ready(g_app_timer);
    }

    lv_obj_set_x(g_layer_app, SCR_W);
    lv_obj_remove_flag(g_layer_app, LV_OBJ_FLAG_HIDDEN);
    /* 必须把桌面层收起来：App 页背景是半透明的（LV_OPA_90），
     * 原本 10% 透过来的正是桌面的各个图标与文字，压在读数下面会显脏。
     * 隐藏后透上来的是壁纸，即想要的"氛围"。 */
    lv_obj_add_flag(g_layer_home, LV_OBJ_FLAG_HIDDEN);

    /* App 层瞬切到位，内容单独淡入，不用整层横滑（slide_x）：
     * 横滑只改 x 坐标会让 LVGL 重绘整个移动图层（整屏 76800 B），
     * SPI 50 MHz 刷一屏约 25 ms，240 ms 动画只跑得动约 10 帧，视觉上是"闪一下"；
     * 淡入刷屏量减半，且没有滑动时两侧露黑边的拖影。 */
    lv_obj_set_style_opa(g_layer_app, LV_OPA_COVER, 0);
    lv_obj_set_x(g_layer_app, 0);

    lv_obj_set_style_opa(g_app_body, LV_OPA_TRANSP, 0);
    lv_obj_fade_in(g_app_body, 130, 30);
    g_anim_lock = 1;
    /* 动画结束解锁 —— 淡入 130ms 后放开输入 */
    {
        static lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, g_app_body);
        lv_anim_set_exec_cb(&a, NULL);
        lv_anim_set_values(&a, 0, 1);
        lv_anim_set_time(&a, 130);
        lv_anim_set_completed_cb(&a, slide_lock_release);
        lv_anim_start(&a);
    }
}

/* 关闭动画结束回调：隐藏 App 层、清空 body、调 teardown 并恢复各层状态。 */
static void close_done_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(g_layer_app, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(g_app_body);

    /* teardown 必须在 lv_obj_clean() 之后调：App 的清理逻辑依赖
     * "对象已经没了"这个事实（例如把模块级 label 指针置 NULL，
     * 免得排队的 async 回调去解引用已释放的 label）。
     *
     * 不能用 g_cur_app 判断要调谁的 teardown —— app_close() 在启动淡出动画
     * 之前就已把 g_cur_app 置 -1，异步回调跑起来时它必然是 -1，判据永远为假，
     * teardown 会从不被调用（游戏 App 退出后仍横屏即由此暴露）。
     * 因此改由 app_close() 先把索引存进 g_closing_app。 */
    if (g_closing_app >= 0 && g_apps[g_closing_app].teardown)
        g_apps[g_closing_app].teardown();
    g_closing_app = -1;

    /* 淡出动画把 g_layer_app 的整体不透明度改成了 TRANSP，必须恢复成 COVER，
     * 否则下次打开 App 会是全透明的（只看到壁纸）。
     * 注意 bg_opa 是背景色不透明度，opa 是整个对象的不透明度，两者不同。 */
    lv_obj_set_style_opa(g_layer_app, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(g_app_body, LV_OPA_COVER, 0);

    /* 回桌面了，恢复各层底色。
     * g_layer_app 的 90% 深蓝底也必须恢复，否则再进下一个数据页时会"透壁纸"，
     * 读数不清。 */
    if (g_nav)         lv_obj_set_style_bg_opa(g_nav, LV_OPA_30, 0);
    if (g_btn_back)    lv_obj_set_style_bg_opa(g_btn_back, LV_OPA_10, 0);
    lv_obj_set_style_bg_opa(g_layer_app, LV_OPA_90, 0);
    g_anim_lock = 0;
}

/**
 * @brief  关闭当前 App 页并回到桌面
 * @note   动画进行中或已在桌面时直接返回。只能在 LVGL 线程调用；
 *         若从 msh 线程触发，需经 lv_async_call() 派发。
 */
void app_close(void)
{
    lv_group_t *g;
    int i;

    if (g_anim_lock || g_cur_app < 0) return;

    g_closing_app = g_cur_app;      /* 先存待关闭索引，teardown 要用 */
    g_cur_app = -1;
    if (g_app_timer) { lv_timer_delete(g_app_timer); g_app_timer = NULL; }

    /* group 换回桌面的格子 + 翻页箭头 */
    home_group_refill();
    g = lv_group_get_default();
    if (g) lv_group_focus_obj(g_cell[g_home_sel]);

    /* 桌面层放回来：内容淡出时桌面从底下透出来 */
    lv_obj_remove_flag(g_layer_home, LV_OBJ_FLAG_HIDDEN);

    /* 只对整个 App 层做一次淡出即可。若同时跑 g_app_body 与 g_layer_app
     * 两个不透明度动画，父子效果叠加会让淡出过快（近似硬切），且 close_done_cb
     * 的恢复时机对不上，曾出现"返回后桌面残留 App + 标题栏"。 */
    fade_to(g_layer_app, LV_OPA_TRANSP, 120, close_done_cb);
}

/* ── 入口 ── */

/**
 * @brief  构建整个桌面 UI
 * @note   由 lv_user_gui.c 在 LVGL 线程里调用；只需调用一次。
 */
void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_group_t *g;
    int i;

    lv_obj_set_style_bg_color(scr, lv_color_hex(C_WALL_MID), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_wallpaper(scr);       /* 照片（最底层） */
    build_scrim(scr);           /* 暗色蒙版，保证前景可读 */

    g_layer_home = lv_obj_create(scr);
    lv_obj_remove_style_all(g_layer_home);
    lv_obj_set_pos(g_layer_home, 0, 0);
    lv_obj_set_size(g_layer_home, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(g_layer_home, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g_layer_home, 0, 0);
    lv_obj_clear_flag(g_layer_home, LV_OBJ_FLAG_SCROLLABLE);

    build_home();
    build_app_layer(scr);

    home_group_refill();
    g = lv_group_get_default();
    if (g) lv_group_focus_obj(g_cell[0]);

    /* 累计运行时长（断电保存）的常驻落盘定时器。
     *
     * 不能放在 app_sys_tick 里：那是系统 App 的 tick（period = 500 ms），
     * 只有系统 App 打开时才跑（app_timer_cb 有 g_cur_app >= 0 的守卫）。
     * 运行时长落盘与用户在哪个界面无关，若停在那里则永远不落盘，复位后历史归零。
     * 所以必须挂在始终运行的定时器上。
     *
     * 周期 5 秒检查一次，内部再按 UPTIME_SAVE_PERIOD_S = 60 判断是否该写，
     * 真正的 flash 写在 settings_uptime_store() 内部节流，这里只是心跳。 */
    g_uptime_timer = lv_timer_create(uptime_flush_cb, 5000, NULL);
    uptime_flush_cb(NULL);

    /* 业务数据 -> C3 的周期上行（每 5 秒推 13 个字段）。
     * 同样必须常驻：停在桌面时打开 C3 网页也要能看到数据，
     * 不能只在某个 App 打开时才推。 */
    c3_report_start();

    /* 闹钟数据层初始化（互斥量 + 从 flash 载入列表）。
     * 必须在任何 App 的 build() 可能读列表之前完成。 */
    alarm_init();

    /* 闹钟到点判定 —— 1 秒一次常驻，停在桌面照样会响。 */
    lv_timer_create(alarm_watch_cb, 1000, NULL);

    rt_kprintf("[ui] home ready: %d apps, %d columns\n", APP_COUNT, 3);
}

/* ── App 自检（msh: uitest）──
 * 依次打开每个 App、跑一会儿、再返回，验证所有页面都能正常构建与刷新。
 * 必须由 LVGL 定时器驱动：LVGL 不是线程安全的，不能直接从 msh 线程调
 * app_open()/app_close()。 */
static int g_uitest_idx = -1;

static void uitest_cb(lv_timer_t *t)
{
    static int phase = 0;               /* 0 = 打开下一项，1 = 关闭当前项 */

    if (g_uitest_idx < 0)
    {
        lv_timer_delete(t);
        return;
    }
    if (g_anim_lock) return;            /* 等滑入/滑出动画走完 */

    if (g_uitest_idx >= APP_COUNT)
    {
        rt_kprintf("[uitest] all %d apps OK\n", APP_COUNT);
        g_uitest_idx = -1;
        lv_timer_delete(t);
        return;
    }

    if (phase == 0)
    {
        rt_kprintf("[uitest] open #%d (%s)\n", g_uitest_idx, g_apps[g_uitest_idx].name);
        app_open(g_uitest_idx);
        phase = 1;
    }
    else
    {
        rt_kprintf("[uitest] close #%d\n", g_uitest_idx);
        app_close();
        g_uitest_idx++;
        phase = 0;
    }
}

/* msh: uitest —— 依次打开再关闭所有 App 页，自检构建与刷新是否正常。用法：uitest */
static void uitest(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (g_uitest_idx >= 0)
    {
        rt_kprintf("[uitest] already running\n");
        return;
    }
    g_uitest_idx = 0;
    lv_timer_create(uitest_cb, 400, NULL);
    rt_kprintf("[uitest] start\n");
}
MSH_CMD_EXPORT(uitest, walk through every UI app once);

/* ── 定点开/关 App 页（msh: uiapp / uihome）──
 * 用途：远程调试 UI —— 旋钮没法远程转，用这个命令直接点开某个 App 页拍照查看。
 * LVGL 对象必须在 LVGL 线程里操作，不能从 msh 线程直接调 app_open()/app_close()；
 * 走 lv_async_call 把回调丢进 LVGL 线程执行。 */
static void ui_async_open(void *p) { app_open((int)(rt_base_t)p); }
static void ui_async_home(void *p) { (void)p; app_close(); }

/* msh: uiapp <idx> —— 打开指定序号的 App 页。不带参数时列出序号与名称。 */
static void uiapp(int argc, char **argv)
{
    int n, i;

    if (argc < 2)
    {
        rt_kprintf("usage: uiapp <idx>   (uihome = 返回桌面)\n");
        for (i = 0; i < APP_COUNT; i++)
            rt_kprintf("  %d = %s\n", i, g_apps[i].name);
        return;
    }

    n = atoi(argv[1]);
    if (n < 0 || n >= APP_COUNT)
    {
        rt_kprintf("[ui] idx 超范围，应为 0..%d\n", APP_COUNT - 1);
        return;
    }
    lv_async_call(ui_async_open, (void *)(rt_base_t)n);
    rt_kprintf("[ui] opening app %d (%s)\n", n, g_apps[n].name);
}
MSH_CMD_EXPORT(uiapp, open an app page by index (remote UI debug));

/* msh: uihome —— 关闭当前 App 页并回到桌面。用法：uihome */
static void uihome(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lv_async_call(ui_async_home, RT_NULL);
    rt_kprintf("[ui] back to home\n");
}
MSH_CMD_EXPORT(uihome, close current app page and go back to home);

/* msh: fontinfo —— 打印中文字库描述符，汉字显示成空心方块时先跑这个。
 * 最常见的两种原因：
 *   1) bitmap_format != 0（压缩位图）而 lv_conf.h 的 LV_USE_FONT_COMPRESSED = 0，
 *      重新生成字库需要带 --no-compress；
 *   2) 字库没跟上文案（ui_text.h 加了字但没重跑 gen_font_zh.sh）。 */
static void fontinfo(int argc, char **argv)
{
    const lv_font_fmt_txt_dsc_t *d;

    (void)argc;
    (void)argv;

    if (zh18.dsc == NULL)
    {
        rt_kprintf("[font] zh18.dsc is NULL !!\n");
        return;
    }

    /* lv_font_t.dsc 是 void*，要看内容得转成 fmt_txt 描述符 */
    d = (const lv_font_fmt_txt_dsc_t *)zh18.dsc;

    rt_kprintf("[font] zh18: line_h=%d base=%d cmap_num=%u bpp=%u bitmap_format=%u\n",
               (int)zh18.line_height, (int)zh18.base_line,
               (unsigned)d->cmap_num, (unsigned)d->bpp, (unsigned)d->bitmap_format);

    if (d->bitmap_format != 0)
        rt_kprintf("[font] !! bitmap_format != 0 (PLAIN): 汉字会变方块，"
                   "重新生成必须带 --no-compress\n");
    else
        rt_kprintf("[font] ok: PLAIN bitmap, LV_USE_FONT_COMPRESSED=%d\n",
                   (int)LV_USE_FONT_COMPRESSED);
}
MSH_CMD_EXPORT(fontinfo, dump zh18 font descriptor);

/* msh: lvmem —— 打印 LVGL 内存池使用量。
 * 池为 64 KB，位于 SRAM1（LV_MEM_ADR = 0x20100000，与 link.lds 的 .lvgl_pool 一致）。
 * 要往 SRAM 里塞大缓冲（如全分辨率壁纸）之前先跑这个，看池能缩到多少。 */
static void lvmem(int argc, char **argv)
{
    lv_mem_monitor_t mon;

    (void)argc;
    (void)argv;

    lv_mem_monitor(&mon);
    rt_kprintf("[lvgl] pool total = %u B (%u KB)\n",
               (unsigned)mon.total_size, (unsigned)(mon.total_size / 1024));
    rt_kprintf("[lvgl]      free  = %u B (%u KB)\n",
               (unsigned)mon.free_size, (unsigned)(mon.free_size / 1024));
    rt_kprintf("[lvgl]      used  = %u B, frag = %u%%\n",
               (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.frag_pct);
    rt_kprintf("[lvgl]      max free block = %u B\n", (unsigned)mon.free_biggest_size);
}
MSH_CMD_EXPORT(lvmem, show LVGL memory pool usage);

/* msh: fpsinfo —— 统计实际刷屏量：先执行 fpsinfo start，
 * 等约 2 秒后再执行 fpsinfo 读取结果。
 *
 * 自己数而不是读 LVGL sysmon：sysmon 的 perf_info 是 static
 * （lv_sysmon.c 中把 perf_info 声明为 static lv_sysmon_perf_info_t），没有对外
 * getter，拿不到它的 fps / flush_avg_time，故直接在 flush 回调里累加计数。
 *
 * 统计口径为实际刷出的像素数与等效整屏次数，与 SPI 带宽消耗一一对应；
 * 不用「16 次 flush = 1 屏」这类按整宽行数折算的办法 —— 脏区重绘后单次
 * flush 可能只有几百像素，该折算会严重失真。 */
volatile rt_uint32_t g_fps_flush_lines;      /* lv_port_disp.c 的 flush 回调累加 */
volatile rt_uint32_t g_fps_flush_px;         /* 同上：实际刷出去的像素总数 */
rt_tick_t g_fps_t0;

static void fpsinfo(int argc, char **argv)
{
    if (argc >= 2 && rt_strcmp(argv[1], "start") == 0)
    {
        g_fps_flush_lines = 0;
        g_fps_flush_px    = 0;
        g_fps_t0 = rt_tick_get();
        rt_kprintf("[fps] 开始计时...（等2 秒后再执行 fpsinfo）\r\n");
        return;
    }

    {
        rt_uint32_t dt = (rt_uint32_t)(rt_tick_get() - g_fps_t0);
        rt_uint32_t full = 240u * 320u;             /* 一屏像素数 */

        if (dt == 0) { rt_kprintf("[fps] 先执行 fpsinfo start\r\n"); return; }

        /* rt_kprintf 不支持 %f（会原样打印 "%f"），全部用整数并手动补小数位。 */
        rt_kprintf("[fps] %u ms | flush=%u 次 | px=%u (%u.%02u 屏) | SPI 等效 %u KB/s\r\n",
                   (unsigned)dt,
                   (unsigned)g_fps_flush_lines,
                   (unsigned)g_fps_flush_px,
                   (unsigned)(g_fps_flush_px / full),
                   (unsigned)((g_fps_flush_px % full) * 100u / full),
                   (unsigned)((double)g_fps_flush_px * 2.0 / ((double)dt / 1000.0) / 1024.0));
        rt_kprintf("[fps] 折算整屏帧率上界 = %u.%02u fps（%u px/ms 吞吐）\r\n",
                   (unsigned)(g_fps_flush_px * 1000u / full / dt),
                   (unsigned)((g_fps_flush_px * 100000u / full / dt) % 100u),
                   (unsigned)(g_fps_flush_px / (dt ? dt : 1)));

        g_fps_flush_lines = 0;
        g_fps_flush_px    = 0;
        g_fps_t0 = rt_tick_get();
    }
}
MSH_CMD_EXPORT(fpsinfo, measure real fps - run 'fpsinfo start' then again later);




/* msh: page [n] —— 切桌面分页；不带参数时打印当前页码。
 * 调试用，方便在串口直接翻页拍照。
 * 必须派发到 LVGL 线程（home_page_show 会改对象标志 + 重设焦点）。 */
static void async_page_show(void *p) { home_page_show((int)(rt_base_t)p); }

/* msh: page [n] —— 切到第 n 页；省略 n 时打印当前页。 */
static void page(int argc, char **argv)
{
    if (argc >= 2)
        lv_async_call(async_page_show, (void *)(rt_base_t)atoi(argv[1]));
    else
        rt_kprintf("[ui] 当前第 %d / %d 页\r\n", g_home_page + 1, APP_PAGES);
}
MSH_CMD_EXPORT(page, switch home page - no comma);

/* msh: focusnext / focusprev —— 模拟旋钮转动，在串口验证 group 里有哪些对象
 * （旋钮去不了第二页通常是箭头没进 group）。
 *
 * 必须经 lv_async_call 派发到 LVGL 线程：这两个命令跑在 msh 线程，
 * 而 lv_group_focus_next() 会改 LVGL 状态并 invalidate 区域，
 * 直接调等于在 LVGL 渲染到一半时从另一个线程插手，
 * 会命中 _lv_inv_area 的 "Invalidate area is not allowed during rendering." 断言。 */
static void async_focus_next(void *p) { (void)p; lv_group_focus_next(lv_group_get_default()); }
static void async_focus_prev(void *p) { (void)p; lv_group_focus_prev(lv_group_get_default()); }

/* msh: focusnext —— 焦点前移一个（模拟旋钮右转一格）。 */
static void focusnext(int argc, char **argv)
{
    lv_group_t *g = lv_group_get_default();
    if (g == RT_NULL) { rt_kprintf("[ui] no group\r\n"); return; }
    lv_async_call(async_focus_next, RT_NULL);
}
MSH_CMD_EXPORT(focusnext, move encoder focus forward);

/* msh: focusprev —— 焦点后移一个（模拟旋钮左转一格）。 */
static void focusprev(int argc, char **argv)
{
    lv_group_t *g = lv_group_get_default();
    if (g == RT_NULL) { rt_kprintf("[ui] no group\r\n"); return; }
    lv_async_call(async_focus_prev, RT_NULL);
}
MSH_CMD_EXPORT(focusprev, move encoder focus backward);

/* msh: focusdump —— 打印当前焦点落在哪个对象上，用来确认箭头确实进 group 了。 */
static void focusdump(int argc, char **argv)
{
    lv_group_t *g = lv_group_get_default();
    lv_obj_t *f;
    int i;

    if (g == RT_NULL) { rt_kprintf("[ui] no group\r\n"); return; }
    f = lv_group_get_focused(g);
    rt_kprintf("[ui] focused = %p\r\n", f);

    for (i = 0; i < APP_COUNT; i++)
        if (g_cell[i] == f)
            rt_kprintf("[ui]   -> cell %d (page %d)\r\n", i, i / APPS_PER_PAGE);

    if (f == g_pg_prev) rt_kprintf("[ui]   -> 左翻页箭头\r\n");
    if (f == g_pg_next) rt_kprintf("[ui]   -> 右翻页箭头\r\n");

    rt_kprintf("[ui] refs: cell0=%p prev=%p next=%p\r\n",
               g_cell[0], g_pg_prev, g_pg_next);
}
MSH_CMD_EXPORT(focusdump, dump encoder focus - no comma);

/* msh: uikey <enter|right|left|info> —— 给当前焦点对象发一个按键。
 *
 * 旋钮只做两件事：转（发 left/right）和按（发 enter）。有了它就能在不接旋钮的
 * 情况下完整验证那些"必须用旋钮操作"的界面。典型场景是闹钟页的时/分滑块：
 *     走到滑块上 -> uikey enter 进编辑态 -> uikey right 调值 -> uikey enter 退出。
 *
 * 不能简单用 lv_group_send_data(g, LV_KEY_ENTER) 了事：
 * "短按可编辑控件 = 进编辑态"这条逻辑在 lv_indev.c 里，不在 group 里
 * （send_data 只把 LV_EVENT_KEY 发给焦点对象）。下面按 lv_indev 的编码器分支
 * 逐条复刻，保证测出来的行为与真机一致。
 *
 * 与 focusnext 一样，必须经 lv_async_call 派发到 LVGL 线程。 */
static void async_uikey(void *p)
{
    lv_group_t *g   = lv_group_get_default();
    lv_obj_t   *foc = (g) ? lv_group_get_focused(g) : RT_NULL;
    int which = (int)(rt_base_t)p;

    if (g == RT_NULL || foc == RT_NULL) return;

    switch (which)
    {
    case 0:     /* enter */
        if (lv_obj_is_editable(foc))
        {
            /* 可编辑控件（滑块/弧）：短按进编辑态，再按退出 */
            if (!lv_group_get_editing(g))
            {
                lv_group_set_editing(g, true);
            }
            else
            {
                lv_group_set_editing(g, false);
                lv_group_send_data(g, LV_KEY_ENTER);
            }
        }
        else
        {
            /* 普通按钮：手工补一次"点击"。
             * 只写 lv_group_send_data(g, LV_KEY_ENTER) 只会发 LV_EVENT_KEY，
             * 不会产生 CLICKED —— CLICKED 是 lv_indev 在释放分支自己发出去的，
             * 不是控件发的。这里按 lv_indev 的按下 -> 释放顺序补这一串。 */
            lv_obj_send_event(foc, LV_EVENT_PRESSED,       NULL);
            lv_obj_send_event(foc, LV_EVENT_RELEASED,      NULL);
            lv_obj_send_event(foc, LV_EVENT_SHORT_CLICKED, NULL);
            lv_obj_send_event(foc, LV_EVENT_CLICKED,       NULL);
        }
        break;

    case 1:     /* right：编辑态调值，否则移焦点 */
        if (lv_group_get_editing(g)) lv_group_send_data(g, LV_KEY_RIGHT);
        else                         lv_group_focus_next(g);
        break;

    default:    /* left */
        if (lv_group_get_editing(g)) lv_group_send_data(g, LV_KEY_LEFT);
        else                         lv_group_focus_prev(g);
        break;
    }
}

/* msh: uikey enter|right|left|info —— 向当前焦点对象发一次按键；
 * info 只打印焦点状态，不发键。 */
static void uikey(int argc, char **argv)
{
    lv_group_t *g = lv_group_get_default();
    lv_obj_t *f;
    int which;

    if (g == RT_NULL) { rt_kprintf("[ui] no group\r\n"); return; }
    if (argc < 2)
    {
        rt_kprintf("用法: uikey enter|right|left\r\n");
        rt_kprintf("      相当于按一下 KEY / 往右转一格 / 往左转一格\r\n");
        return;
    }

    if      (!rt_strcmp(argv[1], "enter")) which = 0;
    else if (!rt_strcmp(argv[1], "right")) which = 1;
    else if (!rt_strcmp(argv[1], "left"))  which = 2;
    else if (!rt_strcmp(argv[1], "info"))  which = -1;   /* 只看状态，不发键 */
    else { rt_kprintf("[ui] 不认识的键: %s\r\n", argv[1]); return; }

    f = lv_group_get_focused(g);
    rt_kprintf("[ui] key=%s  focused=%p  editable=%d  editing=%d\r\n",
               argv[1], f,
               (f) ? (int)lv_obj_is_editable(f) : -1,
               (int)lv_group_get_editing(g));

    if (which < 0) return;      /* info：探完就返回 */

    lv_async_call(async_uikey, (void *)(rt_base_t)which);
}
MSH_CMD_EXPORT(uikey, send key to focused obj: uikey enter|right|left);
