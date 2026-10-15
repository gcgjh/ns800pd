/**
 * @file    lcd_game_ui.c
 * @brief   横屏跳跃躲避游戏（猫跳石头），LVGL 纯图元绘制，不使用图片素材。
 *
 * 内部 Flash 仅剩约 92 KB，LVGL 内存池仅 64 KB，一张 320x240 的 RGB565 图片
 * 就要 150 KB，无法容纳。因此全部美术用 LVGL 图元（矩形/圆/圆角）在代码中
 * 绘制：天空、云、灌木、地面、猫、石头共十来个 lv_obj，色值取自 Scratch
 * 猫跑图配色。这样不占 Flash，尺寸与颜色可任意修改。
 *
 * 性能约束：SPI ILI9341 @50 MHz，一屏 320x240 RGB565 = 153600 B 约 25 ms，
 * 不能每帧清屏重画，只能用脏区思路：
 *   静态部分（天空/地面/云/灌木）建一次后不再改；
 *   每帧只移动猫和石头的 x/y，LVGL 只重绘这两个小 obj 的新旧位置。
 * 实测每帧刷出的像素只有几千个，帧率可达 30 以上。
 *
 * 横竖屏切换（本 App 的核心机制）：
 *   进入 build()：lv_port_disp_set_orientation(1)，再 ui_set_fullscreen(1)；
 *   退出 teardown()：lv_port_disp_set_orientation(0)，再 ui_set_fullscreen(0)。
 * 顺序不可颠倒：先切屏方向（改 LVGL 分辨率），再让 UI 层按新分辨率重排。
 * 这两个函数只能在 LVGL 线程调用，build/teardown 恰在 LVGL 线程（由 ui.c 的
 * app_open / close_done_cb 调用），满足条件。
 *
 * 操作：
 *   EC11 旋钮的 KEY（LVGL encoder 按键）= 跳；游戏结束时按 KEY 重开一局；
 *   硬件板载 KEY41（GPIO_41）单击回桌面（board_key.c 既有逻辑，无需改动）。
 */

#include "ui.h"
#include "ui_text.h"
#include "settings.h"
#include <stdio.h>              /* snprintf（rt_kprintf 不支持 %f，个位数字符串用 snprintf） */
#include <rtthread.h>
#include <rtdevice.h>           /* rt_pin_mode / rt_pin_read（读 EC11 KEY31 电平） */
#include <lvgl.h>

/* lv_port_disp.c 提供的运行时横竖屏切换 */
int  lv_port_disp_is_landscape(void);
void lv_port_disp_set_orientation(int landscape);

/* ── 游戏区尺寸 ──
 * 横屏整屏 320x240，本 App 全屏接管（导航栏隐藏），游戏区即 320x240。
 * 用宏而非运行时读取，编译期即可算好布局。 */
#define GW              320        /* 游戏区宽，横屏屏宽 */
#define GH              240        /* 游戏区高，横屏屏高 */

/* ── 配色（取自 Scratch 猫跑图截图的取色结果）── */
#define C_SKY_TOP       0x9FD8F5   /* 天蓝（上） */
#define C_SKY_BOT       0xC8ECFA   /* 天蓝（下，渐亮） */
#define C_CLOUD         0xFFFFFF   /* 云白 */
#define C_HILL          0x7CB342   /* 远处灌木绿 */
#define C_BUSH          0x5FA22E   /* 近处灌木绿（深一档） */
#define C_GROUND_TOP    0x9CCC65   /* 草地亮边 */
#define C_GROUND        0x8D6E4A   /* 土棕 */
#define C_GROUND_DARK   0x6D4C33   /* 土棕（暗纹） */
#define C_CAT           0xF5A623   /* 猫橙 */
#define C_CAT_DARK      0xD9861A   /* 猫的暗部/条纹 */
#define C_CAT_FACE      0x3E2C1C   /* 眼睛/嘴 */
#define C_ROCK          0x9E9E9E   /* 石头灰 */
#define C_ROCK_DARK     0x757575   /* 石头暗部（当前未使用） */
#define C_TXT_GAME      0xFFFFFF   /* HUD 文字白 */
#define C_TXT_SHADOW    0x000000   /* 文字阴影黑（当前未使用） */
#define C_PANEL         0x141820   /* 开始/结束面板底色 */

/* ── 地面几何 ── */
#define GROUND_Y        196        /* 地面顶边 y（屏内像素，自上而下） */
#define GROUND_H        (GH - GROUND_Y)   /* 地面高度 */

/* ── 猫 ── */
#define CAT_W           30         /* 猫身宽，用于碰撞盒横向范围 */
#define CAT_H           28         /* 猫身高，决定站立 y 与碰撞盒纵向范围 */
#define CAT_X           44         /* 猫固定横坐标（世界滚动，猫自身不移动） */
#define CAT_GROUND_Y    (GROUND_Y - CAT_H)   /* 站在地上的 y */

/* ── 物理参数（单位：像素/帧，帧长 = 30ms）──
 * 用"每帧加多少"而非"每秒加多少乘 dt"，以保证在 30ms 定时器上行为稳定可复现，
 * 不依赖 lv_tick_elaps 的真实耗时。
 *
 * 起跳参数由判定条件反推，不可凭手感调整。猫身底边须抬到石头之上才判为越过，
 * 即至少抬高 14px；石头相对猫的危险窗口宽 32px，4px/帧滚动时持续 8 帧。因此
 * 猫保持抬高 14px 以上的帧数必须大于 8，且起跳容错窗口必须大于该危险窗口，
 * 否则数学上跳不过任何石头。现取值 JUMP_V=-22 / GRAVITY=3：滞空 15 帧、最高
 * 70px、抬高期 13 帧，容错窗口约 20px（5 帧 = 150ms），人类可操作。
 * 修改这两个值前须重算上述约束。 */
#define GRAVITY         3          /* 每帧速度增量（向下为 +），像素/帧² */
#define JUMP_V          (-22)      /* 起跳初速度（向上为 -），像素/帧 */
#define MAX_FALL        16         /* 下落速度上限，像素/帧 */

/* ── 障碍 ──
 * 间距有下限约束：猫滞空 15 帧，落地后还要能重新起跳，两颗石头之间必须留出
 * "滞空 + 落地缓冲"的距离，90px 约 22 帧，最紧的序列也来得及。
 *
 * 随机间距经压力测试（200 组随机序列 × 15000 帧，用完美起跳机器人跑）：
 *   间距 90~140  -> 0 组必死（现取值）
 *   间距 80~130  -> 0 组必死
 *   间距 46~86   -> 8 组必死（4% 的序列无论怎么跳都过不去）
 * 原先的 46~86 即存在该问题。 */
#define ROCK_W          16         /* 石头宽，像素 */
#define ROCK_H          18         /* 石头高，像素 */
#define ROCK_MIN_GAP    90         /* 两颗石头最小间距（左边缘距离，像素） */
#define ROCK_MAX_GAP    140        /* 两颗石头最大间距（左边缘距离，像素） */
#define ROCK_FIRST_CD   150        /* 开局到第一颗石头的距离（约 37 帧 = 1.1s，留给玩家反应） */

/* ── 节奏 ── */
#define TICK_MS         30         /* 游戏主循环周期，毫秒 */

/* ── 模块状态 ── */

/* 静态背景 */
static lv_obj_t *g_sky;
static lv_obj_t *g_hill;
static lv_obj_t *g_ground;
static lv_obj_t *g_bush[3];
static lv_obj_t *g_cloud[2];

/* 猫（身体 + 头 + 耳朵 + 眼睛，各一个 obj） */
static lv_obj_t *g_cat_body;
static lv_obj_t *g_cat_head;
static lv_obj_t *g_cat_ear_l;
static lv_obj_t *g_cat_ear_r;
static lv_obj_t *g_cat_eye_l;
static lv_obj_t *g_cat_eye_r; 
static lv_obj_t *g_cat_leg[2];

/* 石头（同时最多 ROCK_MAX 颗）
 * 取 4 而非 3：间距为 90~140 时屏上最多同时存在约
 * (GW + 8 + ROCK_W) / ROCK_MIN_GAP = 3.8 颗，给 3 个槽会因槽位不足导致
 * 生成失败，白白浪费一次间距。 */
#define ROCK_MAX        4
static lv_obj_t *g_rock[ROCK_MAX];       /* 石头对象，建一次后复用 */
static int       g_rock_x[ROCK_MAX];     /* 石头左边缘 x；-1 = 空闲 */
static int       g_rock_alive[ROCK_MAX]; /* 1 = 占用中 */

/* HUD */
static lv_obj_t *g_score_lb;             /* 本局分数 */
static lv_obj_t *g_best_lb;              /* 历史最高分 */
static lv_obj_t *g_panel;                /* 开始/结束面板 */
static lv_obj_t *g_panel_title;          /* 面板标题 */
static lv_obj_t *g_panel_hint;           /* 面板提示 */

/* 运行状态 */
static lv_timer_t *g_game_timer;         /* 30ms 主循环定时器 */
static int  g_cat_y;                     /* 猫当前 y（世界坐标） */
static int  g_cat_vy;                    /* 猫竖直速度，像素/帧 */
static int  g_on_ground;                 /* 1 = 踩在地面，可起跳 */
static int  g_score;                     /* 当前分数：躲过一颗 +5，每 6 帧 +1 */
static int  g_run;                       /* 0 = 未开始 / 已结束（待机），1 = 跑 */
static int  g_over;                      /* 1 = 刚结束（面板显示中） */
static int  g_spawn_cd;                  /* 下一颗石头的生成倒计时（剩余像素） */
static int  g_frame;                     /* 帧计数，用于计分与跑步摆动 */

/* 1 = 正在退出游戏。
 * app_close() 会让 App 层淡出 120ms，teardown 等动画结束才被调用，这期间本
 * App 的 30ms 定时器仍在运行；若不阻止 key_poll / game_tick 动作，会写入正在
 * 释放的对象或误开新局。置 1 后主循环直接空转。 */
static int  g_closing;

/* 游戏内容容器 —— rock_spawn 要在运行中新建对象，需要它的句柄 */
static lv_obj_t *g_body;

/* ── 绘图小工具 ── */

/**
 * @brief  创建一个纯色矩形（圆角可选）
 * @param  parent  父对象
 * @param  x,y,w,h 位置与尺寸，像素
 * @param  color   填充色，RGB888（0xRRGGBB）
 * @param  radius  圆角半径，像素
 * @return 新建的 lv_obj
 * @note   LVGL 9 中 remove_style_all 后必须自行设置不透明度：默认 bg_opa 为
 *         TRANSP，否则不显示。
 */
static lv_obj_t *mk_rect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h, uint32_t color,
                         lv_coord_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    /* 所有装饰 obj 必须不可点击：否则会被 encoder group 收进来导致旋钮焦点
     * 乱跑，且按下时会吃掉 KEY 事件。 */
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* 天空渐变：用若干条纯色横带叠加实现。LVGL 9 的 bg_grad 需要 lv_grad_dsc_t，
 * 配置较繁，横带方案更直接。 */
static void draw_sky(lv_obj_t *body)
{
    const uint32_t top = C_SKY_TOP, bot = C_SKY_BOT;
    const int bands = 6;
    int i;

    for (i = 0; i < bands; i++)
    {
        /* 线性插值 t = 0..255，用整数运算避免浮点 */
        int t = i * 255 / (bands - 1);
        uint32_t r = (((top >> 16) & 0xFF) * (255 - t) + ((bot >> 16) & 0xFF) * t) / 255;
        uint32_t g = (((top >>  8) & 0xFF) * (255 - t) + ((bot >>  8) & 0xFF) * t) / 255;
        uint32_t b = (( top        & 0xFF) * (255 - t) + ( bot        & 0xFF) * t) / 255;
        int y0 = i * (GROUND_Y) / bands;
        int y1 = (i + 1) * (GROUND_Y) / bands;

        mk_rect(body, 0, y0, GW, y1 - y0, (r << 16) | (g << 8) | b, 0);
    }
}

static void draw_scenery(lv_obj_t *body)
{
    /* 云：两个圆角矩形 */
    g_cloud[0] = mk_rect(body, 28, 22, 46, 12, C_CLOUD, 6);
    g_cloud[1] = mk_rect(body, 226, 40, 40, 11, C_CLOUD, 5);

    /* 远山（一个大圆角方块当丘陵剪影） */
    g_hill = mk_rect(body, 0, GROUND_Y - 26, GW, 26, C_HILL, 0);

    /* 近处灌木：几个圆角方块，只露在丘陵上面一点 */
    g_bush[0] = mk_rect(body, 12, GROUND_Y - 34, 44, 20, C_BUSH, 9);
    g_bush[1] = mk_rect(body, 132, GROUND_Y - 40, 52, 24, C_BUSH, 11);
    g_bush[2] = mk_rect(body, 262, GROUND_Y - 32, 46, 20, C_BUSH, 9);

    /* 地面：亮草边 + 土棕 + 一条暗纹 */
    g_ground = mk_rect(body, 0, GROUND_Y, GW, 4, C_GROUND_TOP, 0);
    mk_rect(body, 0, GROUND_Y + 4, GW, GROUND_H - 4, C_GROUND, 0);
    mk_rect(body, 0, GROUND_Y + 14, GW, 3, C_GROUND_DARK, 0);
}

static void draw_cat(lv_obj_t *body)
{
    /* 身体（圆角方块）、头（更大圆角）、两只耳朵（用圆角方块近似三角）。
     * 全部使用相对 body 的绝对坐标，每帧只需重设 y。 */
    g_cat_body = mk_rect(body, CAT_X + 2, 0, CAT_W - 6, CAT_H - 8, C_CAT, 7);
    g_cat_head = mk_rect(body, CAT_X, 0, 20, 17, C_CAT, 8);

    g_cat_ear_l = mk_rect(body, CAT_X + 2, 0, 7, 7, C_CAT, 3);
    g_cat_ear_r = mk_rect(body, CAT_X + 12, 0, 7, 7, C_CAT, 3);

    g_cat_eye_l = mk_rect(body, CAT_X + 5, 0, 3, 3, C_CAT_FACE, 1);
    g_cat_eye_r = mk_rect(body, CAT_X + 12, 0, 3, 3, C_CAT_FACE, 1);

    /* 两条腿（跑动时交替上下，做出"跑"的感觉） */
    g_cat_leg[0] = mk_rect(body, CAT_X + 4, 0, 6, 8, C_CAT_DARK, 2);
    g_cat_leg[1] = mk_rect(body, CAT_X + 16, 0, 6, 8, C_CAT_DARK, 2);

    /* 尾巴 */
    mk_rect(body, CAT_X - 6, 0, 12, 5, C_CAT_DARK, 2);
}

/* 把猫的所有部件移动到 g_cat_y，这是每帧唯一需要改位置的元素 */
static void cat_place(int y)
{
    int leg_swing = 0;

    if (!g_cat_body) return;

    /* 腿：在地上跑才摆，跳起来就收平 */
    if (g_on_ground && g_run)
        leg_swing = (g_frame / 3) % 2 ? 2 : -2;

    lv_obj_set_y(g_cat_head, y);
    lv_obj_set_y(g_cat_ear_l, y - 5);
    lv_obj_set_y(g_cat_ear_r, y - 5);
    lv_obj_set_y(g_cat_eye_l, y + 6);
    lv_obj_set_y(g_cat_eye_r, y + 6);
    lv_obj_set_y(g_cat_body, y + 10);
    lv_obj_set_y(g_cat_leg[0], y + CAT_H - 2 + (leg_swing > 0 ? 2 : 0));
    lv_obj_set_y(g_cat_leg[1], y + CAT_H - 2 + (leg_swing < 0 ? 2 : 0));
}

/* ── 石头 ── */

/* 间距用伪随机数。
 *
 * 直接用 g_frame 的线性函数会形成固定周期序列，而石头约每 25 帧生成一颗，
 * 取到的值会反复重复，玩家很快就能背出节奏。改用 LCG 后各条序列基本不重复，
 * 同时仍满足"间距必有解"的约束。 */
static rt_uint32_t g_rand_state = 20261011u;

static rt_uint32_t game_rand(void)
{
    /* Numerical Recipes 线性同余，取中高位：低位周期短，不可用 */
    g_rand_state = g_rand_state * 1664525u + 1013904223u;
    return (g_rand_state >> 16) & 0x7FFFu;
}

static void rock_spawn(lv_obj_t *body)
{
    int i;

    for (i = 0; i < ROCK_MAX; i++)
    {
        if (!g_rock_alive[i]) break;
    }
    if (i >= ROCK_MAX) return;

    if (!g_rock[i])
    {
        /* 石头为灰色圆角方块，只建一次，之后靠 alive 标志复用。
         * parent 传 body 而非 lv_screen_active()：body 是游戏内容容器，随
         * lv_obj_clean(g_app_body) 一起释放；挂到 screen 上会活过 teardown
         * 成为野对象。 */
        g_rock[i] = mk_rect(body, 0, 0, ROCK_W, ROCK_H, C_ROCK, 4);
    }

    g_rock_alive[i] = 1;
    g_rock_x[i]     = GW + 8;
    lv_obj_set_pos(g_rock[i], g_rock_x[i], GROUND_Y - ROCK_H);
    lv_obj_remove_flag(g_rock[i], LV_OBJ_FLAG_HIDDEN);
}

/* ── HUD ── */

/* 刷新分数与最高分标签 */
static void hud_update(void)
{
    char buf[24];
    rt_settings_t cfg;

    if (g_score_lb)
    {
        snprintf(buf, sizeof(buf), "%d", g_score);
        lv_label_set_text(g_score_lb, buf);
    }
    if (g_best_lb)
    {
        settings_get(&cfg);
        snprintf(buf, sizeof(buf), "%d", (int)cfg.game_best);
        lv_label_set_text(g_best_lb, buf);
    }
}

/* 结束本局：更新最高分并弹出结束面板 */
static void game_over(void)
{
    rt_settings_t cfg;

    g_run = 0;
    g_over = 1;

    settings_get(&cfg);
    if (g_score > cfg.game_best)
    {
        cfg.game_best = g_score;
        settings_update_one('g', g_score);
        rt_kprintf("[game] 新纪录 %d\n", g_score);
    }
    else
    {
        rt_kprintf("[game] 结束，本次 %d（最高 %d）\n", g_score, (int)cfg.game_best);
    }

    if (g_panel) lv_obj_remove_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    hud_update();
}

/* 每 30ms 推进一帧：猫的垂直物理、石头滚动与碰撞、生成控制与计分 */
static void game_tick(void)
{
    int i;

    if (!g_run) return;

    g_frame++;

    /* ── 猫的垂直物理 ── */
    g_cat_vy += GRAVITY;
    if (g_cat_vy > MAX_FALL) g_cat_vy = MAX_FALL;
    g_cat_y += g_cat_vy;
    if (g_cat_y >= CAT_GROUND_Y)
    {
        g_cat_y = CAT_GROUND_Y;
        g_cat_vy = 0;
        g_on_ground = 1;
    }
    else
    {
        g_on_ground = 0;
    }
    cat_place(g_cat_y);

    /* ── 石头滚动 + 碰撞 ── */
    for (i = 0; i < ROCK_MAX; i++)
    {
        if (!g_rock_alive[i]) continue;

        g_rock_x[i] -= 4;                   /* 恒定速度，方便预判跳跃 */
        if (g_rock_x[i] < -ROCK_W)
        {
            g_rock_alive[i] = 0;
            lv_obj_add_flag(g_rock[i], LV_OBJ_FLAG_HIDDEN);
            g_score += 5;                   /* 躲过一颗加分 */
        }
        else
        {
            lv_obj_set_x(g_rock[i], g_rock_x[i]);
        }

        /* AABB 碰撞：猫身判定盒略收窄，使手感更宽容 */
        {
            int cat_l = CAT_X + 5, cat_r = CAT_X + CAT_W - 5;
            int cat_t = g_cat_y + 4, cat_b = g_cat_y + CAT_H - 2;
            int rk_l = g_rock_x[i] + 2, rk_r = g_rock_x[i] + ROCK_W - 2;
            int rk_t = GROUND_Y - ROCK_H + 2, rk_b = GROUND_Y;

            if (cat_r > rk_l && cat_l < rk_r && cat_b > rk_t && cat_t < rk_b)
            {
                game_over();
                return;
            }
        }
    }

    /* 生成控制：按距离倒计时，保证间距落在 [ROCK_MIN_GAP, ROCK_MAX_GAP]。
     * 只有真正生成成功才重置倒计时：若无论成功与否都重置，槽位满时这次生成
     * 会被丢弃，表现为偶尔少一颗石头，间距也随之不可控。 */
    g_spawn_cd -= 4;
    if (g_spawn_cd <= 0)
    {
        int before = 0, after = 0, k;

        for (k = 0; k < ROCK_MAX; k++) if (g_rock_alive[k]) before++;
        rock_spawn(g_body);
        for (k = 0; k < ROCK_MAX; k++) if (g_rock_alive[k]) after++;

        if (after > before)
        {
            g_spawn_cd = ROCK_MIN_GAP +
                         (int)(game_rand() % (rt_uint32_t)(ROCK_MAX_GAP - ROCK_MIN_GAP));
        }
        else
        {
            g_spawn_cd = 8;     /* 未生成成功：过几帧再试，不视为已排期 */
        }
    }

    /* 分数：跑得越久越高 */
    if ((g_frame % 6) == 0) { g_score++; hud_update(); }
}

/* ── 开始一局 ── */
static void game_start(void)
{
    int i;

    g_run = 1; g_over = 0;
    g_score = 0; g_frame = 0;
    g_cat_y = CAT_GROUND_Y; g_cat_vy = 0; g_on_ground = 1;
    g_spawn_cd = ROCK_FIRST_CD;
    for (i = 0; i < ROCK_MAX; i++)
    {
        g_rock_alive[i] = 0;
        if (g_rock[i]) lv_obj_add_flag(g_rock[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (g_panel) lv_obj_add_flag(g_panel, LV_OBJ_FLAG_HIDDEN);
    hud_update();
    rt_kprintf("[game] start\n");
}

/* ── 读 KEY 并做按下沿检测 ──
 *
 * 不使用 lv_indev_get_state() 做沿检测：lv_port_indev.c 的 encoder_read_cb
 * 每 30ms 才被 LVGL 调用一次，data->state 只在该刻更新；而本游戏 tick 同为
 * 30ms 但两者不同相，切换屏方向或重排布局时 LVGL 主循环被拖住，read_cb 延后，
 * lv_indev_get_state() 会返回陈旧值，同一次按下被连续判定为多个上升沿，导致
 * game_start() 反复触发、屏幕在横竖屏之间高频抖动。
 *
 * 改为直接读 EC11 上 KEY 的引脚电平（GPIO31，lv_port_indev.c 已配为
 * INPUT_PULLUP，即 LVGL encoder 的按键），自行做"连续 N 次读到低才认按下"的
 * 消抖，与引脚真实状态同步，不受 LVGL 主循环节奏影响。按下为低电平（上拉，
 * 见 lv_port_indev.c 的反向判定）。 */
#define GAME_KEY_PIN        31      /* EC11 按键引脚，亦为 LVGL encoder 按键 */
#define GAME_KEY_DEBOUNCE   2       /* 连续 2 个 tick（约 60ms）同电平才确认 */

static void key_poll(void)
{
    static rt_uint8_t stable = 1;   /* 1 = 松开，0 = 按下 */
    static rt_uint8_t cnt    = 0;
    rt_uint8_t raw = (rt_pin_read(GAME_KEY_PIN) == PIN_HIGH) ? 1 : 0;

    if (raw == stable)
    {
        cnt = 0;
    }
    else if (++cnt >= GAME_KEY_DEBOUNCE)
    {
        rt_uint8_t prev = stable;

        stable = raw;
        cnt = 0;

        /* 上升沿（松开）不处理；只在"刚按下"这一沿动作 */
        if (stable == 0 && prev == 1)
        {
            if (g_run)
            {
                if (g_on_ground)            /* 只有踩在地上才能起跳 */
                {
                    g_cat_vy = JUMP_V;
                    g_on_ground = 0;
                }
            }
            else
            {
                game_start();               /* 待机 / 已结束 → 按一下开一局 */
            }
        }
    }
}

/* ── 主循环 ── */

/* 未使用 ui.c 的 app_timer 机制（其周期不适用 30ms 高频），本 App 自建
 * lv_timer，teardown 中可明确删除，生命周期清晰。 */
static void game_loop(lv_timer_t *t)
{
    (void)t;
    if (g_closing) return;              /* 正在退出，不再改动任何状态（见 g_closing 注释） */
    key_poll();
    game_tick();
}

/* ── 页面构建 ── */

/**
 * @brief  构建横屏游戏页（由 ui.c 在打开 App 时于 LVGL 线程调用）
 * @param  body  游戏内容容器
 * @note   先切横屏并全屏接管，再构建静态背景、猫、HUD 与开始面板，最后创建
 *         30ms 主循环定时器；该定时器由 app_game_teardown() 删除并切回竖屏。
 */
void app_game_build(lv_obj_t *body)
{
    lv_obj_t *lb;
    int i;

    /* 先切屏方向（改 LVGL 分辨率），再让 UI 层按新分辨率重排 */
    lv_port_disp_set_orientation(1);
    ui_set_fullscreen(1);

    g_body = body;

    /* EC11 的 KEY（GPIO31）确保是上拉输入。lv_port_indev.c 已配置过，
     * 这里重复设为同值无害（幂等），仅为不依赖初始化顺序。 */
    rt_pin_mode(GAME_KEY_PIN, PIN_MODE_INPUT_PULLUP);

    /* 必须清空 group，否则按 KEY 进不了游戏。
     *
     * ui.c 的 app_open() 中执行 lv_group_focus_obj(g_btn_back)，进任何 App 时焦点
     * 默认落在返回按钮上。LVGL 的 encoder indev 在检测到按下（GPIO31 变低）时会向
     * 焦点对象发 LV_KEY_ENTER，而本页的跳/开始又由 key_poll() 直读同一引脚。按一次
     * KEY 会同时触发 game_start() 与 back_click_cb（app_close），先后取决于两个
     * 30ms 定时器的相位，多数情况下后者先执行，表现为按下即退回桌面。
     *
     * 本页全屏接管、没有可聚焦控件，所有交互都走 key_poll()，本就不应有焦点。清空
     * group 后 indev 找不到焦点对象，不发 ENTER，按键由 key_poll() 独占。退出后焦点
     * 由 app_close() 中的 home_group_refill() 恢复。 */
    {
        lv_group_t *g = lv_group_get_default();
        if (g) lv_group_remove_all_objs(g);
    }

    /* 状态复位 */
    g_run = 0; g_over = 0;
    g_score = 0; g_frame = 0;
    g_cat_y = CAT_GROUND_Y; g_cat_vy = 0; g_on_ground = 1;
    g_spawn_cd = ROCK_FIRST_CD;
    g_closing = 0;
    for (i = 0; i < ROCK_MAX; i++) { g_rock[i] = NULL; g_rock_alive[i] = 0; g_rock_x[i] = -1; }
    for (i = 0; i < 3; i++) g_bush[i] = NULL;
    for (i = 0; i < 2; i++) g_cloud[i] = NULL;
    g_cat_body = g_cat_head = g_cat_ear_l = g_cat_ear_r = NULL;
    g_cat_eye_l = g_cat_eye_r = NULL;
    g_cat_leg[0] = g_cat_leg[1] = NULL;
    g_score_lb = g_best_lb = g_panel = g_panel_title = g_panel_hint = NULL;

    /* ── 背景（静态，只画一次）── */
    draw_sky(body);
    draw_scenery(body);

    /* ── 猫 ── */
    draw_cat(body);
    cat_place(g_cat_y);

    /* ── HUD：分数（左上）与最高分（右上）──
     * 数字用 montserrat，不占中文字库。 */
    lb = ui_label(body, "0", &lv_font_montserrat_32, C_TXT_GAME);
    lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 8, 6);
    g_score_lb = lb;

    lb = ui_label(body, S_G_BEST, &zh18, C_TXT_GAME);
    lv_obj_align(lb, LV_ALIGN_TOP_RIGHT, -52, 12);
    lb = ui_label(body, "0", &lv_font_montserrat_18, C_TXT_GAME);
    lv_obj_align(lb, LV_ALIGN_TOP_RIGHT, -8, 12);
    g_best_lb = lb;

    /* ── 开始面板（半透明黑 + 标题 + 提示）── */
    g_panel = mk_rect(body, GW / 2 - 90, GH / 2 - 46, 180, 92, C_PANEL, 10);
    lv_obj_set_style_bg_opa(g_panel, LV_OPA_80, 0);

    g_panel_title = ui_label(g_panel, S_A_GAME, &zh18, C_TXT_GAME);
    lv_obj_align(g_panel_title, LV_ALIGN_TOP_MID, 0, 12);

    g_panel_hint = ui_label(g_panel, S_G_HINT, &zh18, 0x9FD8F5);
    lv_obj_align(g_panel_hint, LV_ALIGN_TOP_MID, 0, 44);

    /* 让面板可接收 KEY：设为可点击以兼容 encoder group；真正的开始逻辑仍由
     * key_poll() 的按键沿检测负责。 */
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_CLICKABLE);

    hud_update();

    /* ── 30ms 主循环 ── */
    if (g_game_timer) { lv_timer_delete(g_game_timer); g_game_timer = NULL; }
    g_game_timer = lv_timer_create(game_loop, TICK_MS, NULL);

    rt_kprintf("[game] 横屏 %dx%d 就绪（KEY 开始/跳）\n", GW, GH);
}

/**
 * @brief  清理游戏页（由 ui.c 的 close_done_cb 在淡出动画结束后调用）
 * @note   先置 g_closing 阻止主循环动作，再删除 30ms 定时器；随后切回竖屏并取消
 *         全屏，最后清空对象指针。方向切换顺序与 build 相反。
 */
void app_game_teardown(void)
{
    g_closing = 1;                      /* 先置位阻止主循环动作，再清理（防 120ms 淡出窗口内误动作） */
    if (g_game_timer) { lv_timer_delete(g_game_timer); g_game_timer = NULL; }

    /* 顺序与 build 相反，且不能简单镜像：必须先切回竖屏，再让 UI 层按竖屏分辨率
     * 重排。若先 ui_set_fullscreen(0) 再切方向，ui_set_fullscreen 读到的仍是横屏
     * 的 320 宽，会把导航栏设为 320 宽；方向切回 240 宽后导航栏溢出屏外。 */
    lv_port_disp_set_orientation(0);
    ui_set_fullscreen(0);

    g_run = 0; g_over = 0;
    g_run = 0;
    g_score_lb = NULL; g_best_lb = NULL; g_panel = NULL;
    g_panel_title = NULL; g_panel_hint = NULL;
    g_cat_body = g_cat_head = g_cat_ear_l = g_cat_ear_r = NULL;
    g_cat_eye_l = g_cat_eye_r = NULL;
    g_cat_leg[0] = g_cat_leg[1] = NULL;
    g_sky = g_hill = g_ground = NULL;
    g_body = NULL;
}

/* msh gamestate: 打印游戏运行状态与存活石头位置，供联调与验收。用法 gamestate */
static void gamestate(int argc, char **argv)
{
    int i;
    char rocks[64];
    int n = 0;

    (void)argc;
    (void)argv;

    /* 一并打印存活石头的位置，联调与自动跳测试据此判断起跳时机。
     * 起跳窗口为 rock_x ∈ [64, 84]（由文件头 JUMP_V 的推算得出）。 */
    for (i = 0; i < ROCK_MAX; i++)
    {
        if (!g_rock_alive[i]) continue;
        n += rt_snprintf(rocks + n, sizeof(rocks) - n, " %d", g_rock_x[i]);
        if (n >= (int)sizeof(rocks) - 8) break;
    }
    if (n == 0) { rocks[0] = '-'; rocks[1] = '\0'; }

    rt_kprintf("[game] run=%d over=%d score=%d 猫y=%d vy=%d 地面=%d frame=%d\n",
               g_run, g_over, g_score, g_cat_y, g_cat_vy, g_on_ground, g_frame);
    rt_kprintf("[game] rocks:%s\n", rocks);
    rt_kprintf("[game] 方向=%s 屏=%dx%d\n",
               lv_port_disp_is_landscape() ? "横屏" : "竖屏",
               (int)lv_display_get_horizontal_resolution(lv_display_get_default()),
               (int)lv_display_get_vertical_resolution(lv_display_get_default()));
}
MSH_CMD_EXPORT(gamestate, runner game: state dump);

/* async 回调：在 LVGL 线程执行跳或开局 */
static void gamejump_async(void *p)
{
    (void)p;
    if (g_run && g_on_ground)
    {
        g_cat_vy = JUMP_V;
        g_on_ground = 0;
        rt_kprintf("[game] 远程跳\n");
    }
    else if (!g_run)
    {
        game_start();
    }
}
/* msh gamejump: 远程触发一次跳，或在待机时开一局（旋钮无法远程旋转，调试用）。
 * 用法 gamejump。本命令跑在 msh 线程，须经 lv_async_call 进入 LVGL 线程，
 * 因为改猫的 y 与调用 lv_obj_set_* 都属于 LVGL 操作。 */
static void gamejump(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lv_async_call(gamejump_async, RT_NULL);
}
MSH_CMD_EXPORT(gamejump, runner game: jump / start a round);
