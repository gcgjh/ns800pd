/**
 * @file    ui_apps.c
 * @brief   桌面各 App 的内容区实现
 *
 * 本文件实现温度、湿度、编码器、系统、秒表、蜂鸣器、LED、背光、图片、
 * 关于、配网、闹钟共 12 个 App。
 *
 * 每个 App 只画内容区：顶部「返回 + 标题」导航栏由 ui.c 统一创建，
 * build() 收到的 body 已算好坐标（从 y=0 开始，属于 App 页内部）。
 * 数据来自真实硬件：DHT22 / 片内温度传感器 / eQEP3 编码器 / RT-Thread
 * 内核信息 / 蜂鸣器 / 心跳灯 GPIO / 屏幕背光。
 *
 * 交互约定：所有可「按」的控件都是可聚焦按钮（旋钮选到 + KEY 触发），
 * 点击回调首句统一经 ui_click_ok() 去重。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <lvgl.h>

#include <dhtxx.h>              /* DHT22 @ GPIO_40 */
#include <beep.h>               /* 有源蜂鸣器 @ GPIO_1 */
#include "packages/ili9341-latest/lcd_ili9341.h"   /* PKG_ILI_9341_BLK_PIN */

#include "ui.h"
#include "ui_text.h"
#include "ui_apps.h"
#include "ec11_eqep.h"
#include "board_temp.h"
#include "wallpaper_store.h"    /* 图片 App 数据源：wallpaper_get() / WALL_SRC_* */
#include "c3_link.h"            /* 系统页下半区：ESP32-C3 联网状态 */
#include "ns800_link.h"         /* NS800_WIFI_xxx 枚举，判 WiFi 状态用 */
#include "settings.h"           /* 累计运行时长断电保存（uptime_saved） */

/* 心跳灯模式，由 main.c 的 led 线程读取（0 慢闪 1 快闪 2 常亮 3 关） */
extern rt_uint8_t g_led_mode;
extern rt_uint8_t g_led2_hb;        /* LED2(GPIO_69) 系统心跳开关，0 = 关 1 = 开 */

#define DHT_PIN     PIN_NUM(GPIO_40)    /* DHT22 数据脚 */
#define BODY_W      240                 /* 内容区宽度（竖屏像素） */

/* DHT22 手册要求最小采样间隔 2 s（对应 App 的 tick 周期在 ui.c 里设为 2000 ms） */
#define DHT_PERIOD_MS   2000

/* 内核版本号拼成字符串。
 * RT-Thread 5.x 已删除旧的 RT_VERSION / RT_SUBVERSION / RT_REVISION，
 * 只剩 RT_VERSION_MAJOR / MINOR / PATCH（见 include/rtdef.h）；沿用旧宏
 * 会原样留在字符串里，屏上显示成 RT_VERSION.RT_SUBVERSION.RT_REVISION。
 * 拼接必须两级展开：_STR(x) 是 #x 不会展开参数，先经 _XSTR 才能把
 * RT_VERSION_MAJOR 变成 "5"。 */
#define _STR(x)             #x
#define _XSTR(x)            _STR(x)
#define RT_VERSION_STR      _XSTR(RT_VERSION_MAJOR) "." _XSTR(RT_VERSION_MINOR) "." _XSTR(RT_VERSION_PATCH)

#define IABS(x)             ((x) < 0 ? -(x) : (x))    /* 取绝对值（不引入 libm） */

/* ── 小工具 ── */

/**
 * @brief  建一行「左标签 + 右值」
 * @param  p    父对象
 * @param  y    相对父对象的 y 坐标
 * @param  key  左侧标签文案
 * @param  val  右侧值文案
 * @return 右侧值 label，供 tick 里更新文本
 */
static lv_obj_t *row_kv(lv_obj_t *p, lv_coord_t y, const char *key, const char *val)
{
    lv_obj_t *k = ui_label(p, key, &zh18, C_TXT_DIM);
    lv_obj_t *v = ui_label(p, val, &zh18, C_TXT);

    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 18, y);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -18, y);
    return v;
}

/**
 * @brief  建一个居中的大号数字 label
 * @param  p      父对象
 * @param  y      相对父对象的 y 坐标
 * @param  val    初值文案
 * @param  color  文字颜色（0xRRGGBB）
 * @return 新建的 label
 */
static lv_obj_t *big_value(lv_obj_t *p, lv_coord_t y, const char *val, uint32_t color)
{
    lv_obj_t *l = ui_label(p, val, &lv_font_montserrat_32, color);

    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

/**
 * @brief  取 ui_button 的文案 label
 * @param  btn  按钮对象
 * @return 按钮的第 0 个子对象（ui_button 即用它承载文案）
 */
static lv_obj_t *btn_text(lv_obj_t *btn)
{
    return lv_obj_get_child(btn, 0);
}

/**
 * @brief  在卡片左上角放置小标题
 * @param  card  卡片对象
 * @param  txt   标题文案
 */
static void card_title(lv_obj_t *card, const char *txt)
{
    lv_obj_t *l = ui_label(card, txt, &zh18, C_TXT_DIM);

    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 12, 8);
}

/* ── 1. 温度 ── */

static lv_obj_t *tm_amb_lb;              /* 环境温度值 label */
static lv_obj_t *tm_junc_lb;             /* 芯片结温值 label */
static lv_obj_t *tm_delta_lb;            /* 温差值 label */

/**
 * @brief  构建温度页：环境温度 / 芯片结温 / 两者温差
 * @param  body  内容区容器，由 ui.c 传入
 * @note   tick 周期 2000 ms；无 teardown，无模块级动态资源。
 */
void app_temp_build(lv_obj_t *body)
{
    lv_obj_t *c1 = ui_card(body, 8, 6, 224, 84);
    lv_obj_t *c2 = ui_card(body, 8, 96, 224, 84);
    lv_obj_t *c3 = ui_card(body, 8, 186, 224, 62);
    lv_obj_t *hint;

    /* 环境温度（DHT22，读的是空气） */
    card_title(c1, S_T_AMB);
    tm_amb_lb = big_value(c1, 34, "--.-", VC_JUNC);

    /* 芯片结温（片内 TS，含自热，通常比环境高几度） */
    card_title(c2, S_T_JUNC);
    tm_junc_lb = big_value(c2, 34, "--.-", VC_TEMP);

    /* 温差，并说明差值来自芯片自热，并非故障 */
    tm_delta_lb = ui_label(c3, S_T_DELTA " --.-", &zh18, C_TXT);
    lv_obj_align(tm_delta_lb, LV_ALIGN_TOP_LEFT, 14, 12);
    hint = ui_label(c3, S_T_TS, &zh18, C_TXT_DIM);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 14, -10);
}

/**
 * @brief  温度页周期刷新：重读 DHT22 与片内温度传感器，更新三处读数
 * @note   周期 2000 ms（ui.c 的 App 表）。DHT22 单次读取成功率偏低，
 *         每次重试两次，两次都失败时环境温度显示 "--.-"。
 */
void app_temp_tick(void)
{
    dht_device_t s = dht_create(DHT_PIN);
    int amb10 = 0, junc10, d10;
    int ok = 0;

    if (s)
    {
        for (int i = 0; i < 2 && !ok; i++)
            if (dht_read(s)) ok = 1;

        if (ok)
        {
            rt_int32_t t = dht_get_temperature(s);       /* ×10 定点 */
            amb10 = (int)t;
        }
        dht_delete(s);
    }

    if (ok)
        lv_label_set_text_fmt(tm_amb_lb, "%s%d.%d",
                              amb10 < 0 ? "-" : "", IABS(amb10) / 10, IABS(amb10) % 10);
    else
        lv_label_set_text(tm_amb_lb, "--.-");

    /* board_temp_read() 返回摄氏度浮点；取 0.1 度 */
    junc10 = (int)(board_temp_read() * 10.0f + 0.5f);
    if (junc10 < 0) junc10 = 0;
    lv_label_set_text_fmt(tm_junc_lb, "%d.%d", junc10 / 10, junc10 % 10);

    if (ok)
    {
        d10 = junc10 - amb10;
        lv_label_set_text_fmt(tm_delta_lb, "%s %s%d.%d",
                              S_T_DELTA, d10 < 0 ? "-" : "+",
                              IABS(d10) / 10, IABS(d10) % 10);
    }
    else
        lv_label_set_text(tm_delta_lb, S_T_DELTA " --.-");
}

/* ── 2. 湿度 ── */

static lv_obj_t *hm_val_lb;             /* 湿度值 label（源为 ×10 定点） */
static lv_obj_t *hm_state_lb;           /* 舒适度判定 label */
static lv_obj_t *hm_bar;                /* 进度条底槽 */
static lv_obj_t *hm_fill;               /* 进度条填充 */
static int       hm_last10 = -1;        /* 上次显示值，重复值不重绘 */

#define HUM_BAR_W   190                 /* 进度条宽度（px） */
#define HUM_BAR_H   10                  /* 进度条高度（px） */

/**
 * @brief  构建湿度页：相对湿度数值 + 进度条 + 舒适度判定
 * @param  body  内容区容器
 * @note   tick 周期 2000 ms；无 teardown。
 */
void app_humi_build(lv_obj_t *body)
{
    lv_obj_t *c = ui_card(body, 8, 6, 224, 140);

    /* 进页面重置缓存，否则模块级缓存会让首帧停在 "--.-" */
    hm_last10 = -1;

    card_title(c, S_H_REL);
    hm_val_lb = big_value(c, 40, "--.-", VC_HUMI);

    /* 进度条：底槽 + 填充（宽度按湿度百分比调） */
    hm_bar = lv_obj_create(c);
    lv_obj_remove_style_all(hm_bar);
    lv_obj_set_size(hm_bar, HUM_BAR_W, HUM_BAR_H);
    lv_obj_align(hm_bar, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_radius(hm_bar, HUM_BAR_H / 2, 0);
    lv_obj_set_style_bg_color(hm_bar, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_bg_opa(hm_bar, LV_OPA_20, 0);
    lv_obj_clear_flag(hm_bar, LV_OBJ_FLAG_SCROLLABLE);

    hm_fill = lv_obj_create(hm_bar);
    lv_obj_remove_style_all(hm_fill);
    lv_obj_set_pos(hm_fill, 0, 0);
    lv_obj_set_size(hm_fill, 0, HUM_BAR_H);
    lv_obj_set_style_radius(hm_fill, HUM_BAR_H / 2, 0);
    lv_obj_set_style_bg_color(hm_fill, lv_color_hex(VC_HUMI), 0);
    lv_obj_set_style_bg_opa(hm_fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hm_fill, LV_OBJ_FLAG_SCROLLABLE);

    hm_state_lb = ui_label(body, "--", &zh18, C_TXT);
    lv_obj_align(hm_state_lb, LV_ALIGN_TOP_MID, 0, 158);
}

/**
 * @brief  湿度页周期刷新：重读 DHT22，值未变则不重绘
 * @note   周期 2000 ms。值为 ×10 定点：低于 300（30.0%）判干燥，高于 700
 *         （70.0%）判潮湿，其余为舒适。重试两次以应对 DHT22 读取失败。
 */
void app_humi_tick(void)
{
    dht_device_t s = dht_create(DHT_PIN);
    int h10 = 0, ok = 0;

    if (s)
    {
        for (int i = 0; i < 2 && !ok; i++)
            if (dht_read(s)) ok = 1;
        if (ok) h10 = (int)dht_get_humidity(s);
        dht_delete(s);
    }

    if (!ok || h10 == hm_last10) return;
    hm_last10 = h10;

    lv_label_set_text_fmt(hm_val_lb, "%d.%d%%", h10 / 10, h10 % 10);
    lv_obj_set_width(hm_fill, HUM_BAR_W * (h10 / 10) / 100);

    if (h10 < 300)       lv_label_set_text(hm_state_lb, S_H_DRY);
    else if (h10 > 700)  lv_label_set_text(hm_state_lb, S_H_WET);
    else                 lv_label_set_text(hm_state_lb, S_H_COMFORT);
}

/* ── 3. 编码器 ── */

static lv_obj_t *en_val_lb;             /* 格数 label */
static lv_obj_t *en_dir_lb;             /* 方向 label */
static rt_int32_t en_last_steps = 0;    /* 上次显示的格数 */
static rt_tick_t  en_last_move  = 0;    /* 最近一次转动时刻（判静止用） */
static int8_t     en_last_dir   = 0x7F; /* 上次方向；0x7F 为「无」初值 */

/**
 * @brief  「清零」按钮回调：复位编码器计数并清空页面读数
 * @param  e  LVGL 事件
 * @note   蜂鸣调用必须放在最后一行：beep 包内部是单静态线程。
 */
static void enc_clear_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    ec11_reset_position();
    en_last_steps = 0;
    lv_label_set_text(en_val_lb, "0");
    lv_label_set_text(en_dir_lb, S_E_IDLE);
    beep(1, 60, 50, 0);                 /* 蜂鸣放最后一行：包是单线程的 */
}

/**
 * @brief  构建编码器页：当前格数 + 转动方向 + 清零按钮
 * @param  body  内容区容器
 * @note   tick 周期 100 ms；无 teardown。
 */
void app_enc_build(lv_obj_t *body)
{
    lv_obj_t *c = ui_card(body, 8, 6, 224, 150);
    lv_obj_t *clarify;
    lv_obj_t *btn;

    /* 同步缓存并直接把当前计数画出来，避免"进页面显示 0 但实际不是 0" */
    en_last_steps = ec11_get_steps();
    en_last_dir   = 0x7F;
    en_last_move  = rt_tick_get();

    card_title(c, S_E_COUNT);
    en_val_lb = big_value(c, 40, "0", VC_ENC);
    lv_label_set_text_fmt(en_val_lb, "%d", (int)en_last_steps);

    clarify = ui_label(c, S_E_DIR, &zh18, C_TXT_DIM);
    lv_obj_align(clarify, LV_ALIGN_BOTTOM_LEFT, 14, -16);
    en_dir_lb = ui_label(c, S_E_IDLE, &zh18, C_TXT);
    lv_obj_align(en_dir_lb, LV_ALIGN_BOTTOM_RIGHT, -14, -16);

    btn = ui_button(body, S_W_RESET, 110, 36, enc_clear_cb);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 176);
}

/**
 * @brief  编码器页周期刷新：格数变化时更新读数与方向
 * @note   周期 100 ms；超过 1 s 无格数变化即判为静止。
 */
void app_enc_tick(void)
{
    int8_t dir;
    rt_int32_t s = ec11_get_steps();
    rt_tick_t  now = rt_tick_get();

    if (s != en_last_steps)
    {
        dir = (s > en_last_steps) ? 1 : -1;
        en_last_steps = s;
        en_last_move = now;
        lv_label_set_text_fmt(en_val_lb, "%d", (int)s);
    }
    else
    {
        /* 1 秒没有新的格数变化就认为静止 */
        dir = ((now - en_last_move) > rt_tick_from_millisecond(1000)) ? 0 : en_last_dir;
    }

    if (dir != en_last_dir)
    {
        en_last_dir = dir;
        if (dir > 0)      lv_label_set_text(en_dir_lb, S_E_CW);
        else if (dir < 0) lv_label_set_text(en_dir_lb, S_E_CCW);
        else              lv_label_set_text(en_dir_lb, S_E_IDLE);
    }
}

/* ── 4. 系统 ── */

static lv_obj_t *sy_uptime_lb;     /* 本机累计运行时长 */
static lv_obj_t *sy_mem_lb;        /* 堆占用百分比与用量 */
static lv_obj_t *sy_thread_lb;     /* 线程数 */

/* C3 联网区标签（下半屏）。变量名与实际显示内容并不一一对应，以 tick 为准： */
static lv_obj_t *sy_c3_state_lb;   /* 实际显示 RSSI（dBm 或 "--"） */
static lv_obj_t *sy_c3_rssi_lb;    /* 实际显示 C3 剩余内存（KB 或 "--"） */
static lv_obj_t *sy_c3_free_lb;    /* 实际显示 C3 运行时长 HH:MM:SS */
static lv_obj_t *sy_c3_up_lb;      /* 实际显示 WiFi 状态（在线/离线） */

/**
 * @brief  构建系统页：本机版本 / 运行时长 / 内存 / 线程 + 下半屏 C3 联网状态
 * @param  body  内容区容器
 * @note   tick 周期 500 ms；无 teardown。
 */
void app_sys_build(lv_obj_t *body)
{
    lv_obj_t *ver = ui_label(body, "RT-Thread " RT_VERSION_STR, &zh18, C_TXT_DIM);
    lv_obj_t *sep;
    lv_obj_t *hdr;

    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 8);

    sy_uptime_lb = row_kv(body, 44,  S_S_UPTIME, "00:00:00");
    sy_mem_lb    = row_kv(body, 82,  S_S_MEM,    "0%");
    sy_thread_lb = row_kv(body, 120, S_S_THREAD, "0");

    /* 分隔线：隔开 NS800 自身信息与 C3 联网信息。
     * 用 1 px 横线比留白更清楚，代价只是一个 lv_obj。 */
    sep = lv_obj_create(body);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 204, 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x3A4553), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_70, 0);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 150);

    /* 小标题：联网状态（青色调，与系统自身信息区分） */
    hdr = ui_label(body, S_S_C3_HDR, &zh18, C_TXT_DIM);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 18, 162);

    sy_c3_state_lb = row_kv(body, 188, S_S_C3_RSSI, "--");
    sy_c3_rssi_lb  = row_kv(body, 214, S_S_C3_FREE, "--");
    sy_c3_free_lb  = row_kv(body, 240, S_S_C3_UP2,  "--");

    /* WiFi 状态贴右上角，与标题同一行 */
    sy_c3_up_lb = ui_label(body, S_S_C3_DOWN, &zh18, C_TXT_DIM);
    lv_obj_align(sy_c3_up_lb, LV_ALIGN_TOP_RIGHT, -18, 162);
}

/**
 * @brief  WiFi 状态枚举转中文文案
 * @param  s  NS800_WIFI_xxx 状态码
 * @return 对应文案；未知状态返回「关闭」
 * @note   文案必须取自 ui_text.h 的宏，不要写裸中文字面量：字库脚本只扫
 *         ui_text.h，遗漏的汉字不会进字库，屏上会显示为方块。
 */
static const char *c3_wifi_state_text(rt_uint8_t s)
{
    switch (s)
    {
    case NS800_WIFI_CONNECTING:   return S_C3S_CONN;
    case NS800_WIFI_CONNECTED:    return S_C3S_OK;
    case NS800_WIFI_GOT_IP:       return S_C3S_IP;
    case NS800_WIFI_DISCONNECTED: return S_C3S_DOWN;
    case NS800_WIFI_FAIL:         return S_C3S_FAIL;
    default:                      return S_C3S_OFF;
    }
}

/**
 * @brief  系统页周期刷新：运行时长、内存、线程数与 C3 联网状态
 * @note   周期 500 ms；无 teardown。
 *
 * 落盘主路径在 ui.c 的常驻定时器 uptime_flush_cb()，本函数只在系统页
 * 打开时被调用，停在桌面时不会执行，因此这里只是一次额外的补充落盘。
 * 不能每 tick 落盘：LittleFS 的 erase + write 是几十毫秒级，写在 LVGL
 * 线程里会拖慢动画；落盘周期由 settings.h 的 UPTIME_SAVE_PERIOD_S 决定。
 */

void app_sys_tick(void)
{
    rt_int32_t total_s  = settings_uptime_total();   /* 累计总时长（含历史，断电保存） */
    rt_size_t total = 0, used = 0, max_used = 0;
    c3_link_state_t c3;

    /* 显示累计运行时长（HH:MM:SS，超 100 小时改显示天 + 时）。
     * 需求是「运行时长断电保存」，所以这里显示累计总时长（含历史），
     * 断电不清零；本次开机时长可在 cfg 命令里查看。
     * 必须用 settings_uptime_total()：自行累加会重复计入已落盘的部分。 */
    if (total_s < 360000)   /* < 100 小时：显示 HH:MM:SS */
    {
        lv_label_set_text_fmt(sy_uptime_lb, "%02u:%02u:%02u",
                              (unsigned)(total_s / 3600),
                              (unsigned)((total_s / 60) % 60),
                              (unsigned)(total_s % 60));
    }
    else                  /* 超 100 小时：显示 天+时，避免数字溢出格子 */
    {
        lv_label_set_text_fmt(sy_uptime_lb, "%ud %02uh",
                              (unsigned)(total_s / 86400),
                              (unsigned)((total_s % 86400) / 3600));
    }

    /* 落盘：系统页开着时额外补一次。主路径是 ui.c 的常驻定时器
     * （停在桌面也要落盘）。记账在 settings_uptime_flush() 内集中处理，
     * 两处都调不会重复累加。 */
    settings_uptime_flush();

    rt_memory_info(&total, &used, &max_used);
    if (total)
        lv_label_set_text_fmt(sy_mem_lb, "%u%% (%uK/%uK)",
                              (unsigned)(used * 100U / total),
                              (unsigned)(used / 1024U), (unsigned)(total / 1024U));

    lv_label_set_text_fmt(sy_thread_lb, "%u",
                          (unsigned)rt_object_get_length(RT_Object_Class_Thread));

    /* C3 区：先取一份快照再刷新，避免多次加锁；
     * 在线与离线用不同颜色区分。 */
    c3_link_get_state(&c3);

    if (c3.online)
    {
        lv_label_set_text(sy_c3_up_lb, c3_wifi_state_text(c3.wifi_state));
        lv_obj_set_style_text_color(sy_c3_up_lb,
                                     lv_color_hex(c3.wifi_state == NS800_WIFI_GOT_IP
                                                  ? AC_STOPW  /* 绿 = 已通 */
                                                  : AC_SYS),  /* 青 = 中间态 */
                                     0);
    }
    else
    {
        lv_label_set_text(sy_c3_up_lb, S_S_C3_DOWN);
        lv_obj_set_style_text_color(sy_c3_up_lb, lv_color_hex(0xFF6B6B), 0);
    }

    /* 信号：在线且有 RSSI 才显示，否则 "--" */
    if (c3.online && c3.rssi_dbm != 0)
        lv_label_set_text_fmt(sy_c3_state_lb, "%d dBm", c3.rssi_dbm);
    else
        lv_label_set_text(sy_c3_state_lb, "--");

    if (c3.online)
    {
        lv_label_set_text_fmt(sy_c3_rssi_lb, "%u KB", (unsigned)c3.free_kb);
        lv_label_set_text_fmt(sy_c3_free_lb, "%02u:%02u:%02u",
                              (unsigned)(c3.uptime_s / 3600U),
                              (unsigned)((c3.uptime_s / 60U) % 60U),
                              (unsigned)(c3.uptime_s % 60U));
    }
    else
    {
        lv_label_set_text(sy_c3_rssi_lb, "--");
        lv_label_set_text(sy_c3_free_lb, "--");
    }
}

/* ── 5. 秒表 ── */

static lv_obj_t *sw_val_lb;                     /* 计时读数 label */
static lv_obj_t *sw_btn;                        /* 启动/停止按钮 */
static int       sw_running = 0;                /* 1 = 正在计时 */
static uint32_t  sw_acc_ms  = 0;                /* 已累计毫秒（暂停时结算） */
static rt_tick_t sw_start   = 0;                /* 本轮启动时刻 */
static uint32_t  sw_last_shown = 0xFFFFFFFFU;   /* 上次显示值，重复值不重绘 */

/**
 * @brief  启动/停止按钮回调：切换计时状态并更新按钮文案
 * @param  e  LVGL 事件
 */
static void sw_toggle_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    if (sw_running)
    {
        sw_acc_ms += (uint32_t)((rt_tick_get() - sw_start) * 1000U / RT_TICK_PER_SECOND);
        sw_running = 0;
        lv_label_set_text(btn_text(sw_btn), S_W_START);
    }
    else
    {
        sw_start = rt_tick_get();
        sw_running = 1;
        lv_label_set_text(btn_text(sw_btn), S_W_STOP);
    }
}

/**
 * @brief  清零按钮回调：停止计时并把读数归零
 * @param  e  LVGL 事件
 */
static void sw_reset_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    sw_running = 0;
    sw_acc_ms  = 0;
    lv_label_set_text(btn_text(sw_btn), S_W_START);
    lv_label_set_text(sw_val_lb, "00:00.0");
}

/**
 * @brief  构建秒表页：读数 + 启动/停止 + 清零
 * @param  body  内容区容器
 * @note   tick 周期 50 ms；无 teardown。
 */
void app_stopwatch_build(lv_obj_t *body)
{
    lv_obj_t *c = ui_card(body, 8, 6, 224, 130);
    lv_obj_t *btn_r;

    sw_last_shown = 0xFFFFFFFFU;        /* 强制首帧刷新 */

    card_title(c, S_A_STOPW);
    sw_val_lb = big_value(c, 46, "00:00.0", VC_STOPW);

    sw_btn = ui_button(body, S_W_START, 100, 40, sw_toggle_cb);
    lv_obj_align(sw_btn, LV_ALIGN_TOP_MID, -55, 160);
    /* 秒表可能在离开页面时还在跑，回来要把按钮文案对上 */
    lv_label_set_text(btn_text(sw_btn), sw_running ? S_W_STOP : S_W_START);

    btn_r = ui_button(body, S_W_RESET, 100, 40, sw_reset_cb);
    lv_obj_align(btn_r, LV_ALIGN_TOP_MID, 55, 160);
}

/**
 * @brief  秒表页周期刷新读数（显示到 0.1 s）
 * @note   周期 50 ms；读数不变时直接返回，避免无谓重绘。
 */
void app_stopwatch_tick(void)
{
    uint32_t ms, d;

    if (sw_running)
        ms = sw_acc_ms + (uint32_t)((rt_tick_get() - sw_start) * 1000U / RT_TICK_PER_SECOND);
    else
        ms = sw_acc_ms;

    if (ms == sw_last_shown) return;    /* 值没变就别重绘（省 dirty 区域） */
    sw_last_shown = ms;

    d = (ms / 100U) % 10U;
    lv_label_set_text_fmt(sw_val_lb, "%02u:%02u.%u",
                          (unsigned)(ms / 60000U),
                          (unsigned)((ms / 1000U) % 60U), (unsigned)d);
}

/* ── 6. 蜂鸣器 ── */

/**
 * @brief  蜂鸣档位按钮回调
 * @param  e  事件，user_data 为档位号 0 / 1 / 2
 * @note   beep 包是单静态线程，同一回调内只能调用一次，且必须放在最后一行。
 */
static void buzz_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    rt_uint32_t idx = (rt_uint32_t)(rt_base_t)lv_event_get_user_data(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    /* beep 包是单静态线程：同一回调里只能调一次，且必须是最后一句 */
    if (idx == 0)      beep(1, 60,  50, 0);     /* 短响 */
    else if (idx == 1) beep(1, 320, 50, 0);     /* 长响 */
    else               beep(3, 90,  50, 0);     /* 连响 */
}

/**
 * @brief  构建蜂鸣器页：图标 + 提示 + 三档测试音按钮
 * @param  body  内容区容器
 * @note   无 tick、无 teardown。
 */
void app_buzzer_build(lv_obj_t *body)
{
    lv_obj_t *ring = lv_obj_create(body);
    lv_obj_t *sym, *hint;
    lv_coord_t y = 10;
    int i;

    /* 圆形大图标 */
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 92, 92);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ring, lv_color_hex(VC_BUZZ), 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_20, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(VC_BUZZ), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_60, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    sym = ui_label(ring, LV_SYMBOL_BELL, &lv_font_montserrat_32, VC_BUZZ);
    lv_obj_center(sym);

    hint = ui_label(body, S_B_HINT, &zh18, C_TXT_DIM);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, y + 104);

    for (i = 0; i < 3; i++)
    {
        static const char *txt[3];
        lv_obj_t *b;

        txt[0] = S_B_SHORT; txt[1] = S_B_LONG; txt[2] = S_B_TRIPLE;
        /* ui_button 的 cb 传 NULL：回调只在这里注册一次，user_data 带上档位号 */
        b = ui_button(body, txt[i], 68, 38, NULL);
        lv_obj_add_event_cb(b, buzz_cb, LV_EVENT_ALL, (void *)(rt_base_t)i);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8 + i * 78, 168);
    }
}

/* ── 7. LED ── */

static lv_obj_t *led_state_lb;      /* LED1 模式文本 */
static lv_obj_t *led2_state_lb;     /* LED2 开关文本 */

/**
 * @brief  LED2（系统心跳）开关回调
 * @param  e  事件，user_data 为 0 = 关 1 = 开
 */
static void led2_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    rt_uint32_t on = (rt_uint32_t)(rt_base_t)lv_event_get_user_data(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    g_led2_hb = (rt_uint8_t)on;
    lv_label_set_text_fmt(led2_state_lb, "LED2 %s",
                          g_led2_hb ? S_L_ON : S_L_OFF);
}

/**
 * @brief  LED1 模式按钮回调
 * @param  e  事件，user_data 为模式号 0 慢闪 / 1 快闪 / 2 常亮 / 3 关
 */
static void led_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    rt_uint32_t mode = (rt_uint32_t)(rt_base_t)lv_event_get_user_data(e);
    static const char *names[4];

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    g_led_mode = (rt_uint8_t)mode;
    names[0] = S_L_SLOW; names[1] = S_L_FAST; names[2] = S_L_ON; names[3] = S_L_OFF;
    lv_label_set_text_fmt(led_state_lb, "%s %s", S_L_MODE, names[mode]);
}

/**
 * @brief  构建 LED 页：LED1 四档模式 + LED2 心跳开关
 * @param  body  内容区容器
 * @note   无 tick、无 teardown。
 */
void app_led_build(lv_obj_t *body)
{
    static const char *names[4];
    lv_obj_t *hint, *b2on, *b2off;
    int i;

    names[0] = S_L_SLOW; names[1] = S_L_FAST; names[2] = S_L_ON; names[3] = S_L_OFF;

    /* 以下 y 均为相对 body 的偏移，body 从屏幕 y=34（NAV_H）开始，
     * 屏高 320，故 body 可用高度只有 286；新增控件时需据此核算，避免溢出屏外。 */

    led_state_lb = ui_label(body, "", &zh18, C_TXT);
    lv_obj_align(led_state_lb, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text_fmt(led_state_lb, "%s %s", S_L_MODE,
                          names[g_led_mode > 3 ? 0 : g_led_mode]);

    hint = ui_label(body, "LED", &lv_font_montserrat_32, AC_LED);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 44);

    for (i = 0; i < 4; i++)
    {
        /* cb 传 NULL，回调在下面注册一次（避免一次按键触发两遍） */
        lv_obj_t *b = ui_button(body, names[i], 104, 40, NULL);
        lv_obj_add_event_cb(b, led_cb, LV_EVENT_ALL, (void *)(rt_base_t)i);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8 + (i % 2) * 120, 90 + (i / 2) * 48);
    }

    /* LED2(GPIO_69) 为系统心跳，独立于 LED1 的模式：
     * LED1 设为常亮或关闭时，仍可用它判断固件是否运行。 */
    led2_state_lb = ui_label(body, "", &zh18, C_TXT);
    lv_obj_align(led2_state_lb, LV_ALIGN_TOP_MID, 0, 190);
    lv_label_set_text_fmt(led2_state_lb, "LED2 %s", g_led2_hb ? S_L_ON : S_L_OFF);

    b2on = ui_button(body, S_L_ON, 104, 36, NULL);
    lv_obj_add_event_cb(b2on, led2_cb, LV_EVENT_ALL, (void *)(rt_base_t)1);
    lv_obj_align(b2on, LV_ALIGN_TOP_LEFT, 8, 220);

    b2off = ui_button(body, S_L_OFF, 104, 36, NULL);
    lv_obj_add_event_cb(b2off, led2_cb, LV_EVENT_ALL, (void *)(rt_base_t)0);
    lv_obj_align(b2off, LV_ALIGN_TOP_LEFT, 128, 220);
}

/* ── 8. 背光 ── */

static lv_obj_t *bl_state_lb;   /* 背光状态文本 */
static int        bl_on = 1;    /* 1 = 亮 0 = 灭 */

/**
 * @brief  设置背光并同步页面文本
 * @param  on  1 = 点亮 0 = 熄灭
 * @note   背光引脚低电平点亮。
 */
static void bl_set(int on)
{
    rt_pin_write(PKG_ILI_9341_BLK_PIN, on ? PIN_LOW : PIN_HIGH);   /* 低电平点亮 */
    bl_on = on;
    lv_label_set_text_fmt(bl_state_lb, "%s %s", S_BL_STATE,
                          on ? S_BL_ON : S_BL_OFF);
}

/**
 * @brief  背光开关按钮回调
 * @param  e  事件，user_data 为 1 = 开 0 = 关
 */
static void bl_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    rt_uint32_t on = (rt_uint32_t)(rt_base_t)lv_event_get_user_data(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    bl_set((int)on);
}

/**
 * @brief  构建背光页：图标 + 状态 + 开/关按钮
 * @param  body  内容区容器
 * @note   无 tick、无 teardown。
 */
void app_backlight_build(lv_obj_t *body)
{
    lv_obj_t *sym, *b_on, *b_off;

    sym = ui_label(body, LV_SYMBOL_EYE_OPEN, &lv_font_montserrat_32, AC_BL);
    lv_obj_align(sym, LV_ALIGN_TOP_MID, 0, 30);

    bl_state_lb = ui_label(body, "", &zh18, C_TXT);
    lv_obj_align(bl_state_lb, LV_ALIGN_TOP_MID, 0, 84);
    lv_label_set_text_fmt(bl_state_lb, "%s %s", S_BL_STATE,
                          bl_on ? S_BL_ON : S_BL_OFF);

    b_on = ui_button(body, S_BL_ON, 100, 42, NULL);
    lv_obj_add_event_cb(b_on, bl_cb, LV_EVENT_ALL, (void *)(rt_base_t)1);
    lv_obj_align(b_on, LV_ALIGN_TOP_MID, -55, 140);

    b_off = ui_button(body, S_BL_OFF, 100, 42, NULL);
    lv_obj_add_event_cb(b_off, bl_cb, LV_EVENT_ALL, (void *)(rt_base_t)0);
    lv_obj_align(b_off, LV_ALIGN_TOP_MID, 55, 140);
}

/* ── 10. 图片 ──
 *
 * 看图 App：直接显示 wallpaper_store 里那张 240x320 RGB565 原图，并提供三种视图。
 * 数据源与桌面壁纸共用同一管线（外置 flash 的 /wallpaper.bin → SRAM2 的
 * .wall_buf），因此「能换桌面壁纸」即「能换这张图」，无需另建存储。
 *
 * 用 lv_image 而非把图当背景贴到 body：body 高度只有 286（320 - 34 导航栏），
 * 1:1 原图放不下会被裁掉底边，故用一个 240x286 的容器作取景框，图片对象
 * 放在其中按视图移动。
 *
 * 三种视图（默认原寸）：
 *   orig  1:1 不缩放，靠取景框裁切（默认）
 *   fit   按 286/320 缩放，完整显示整图（纵向压约 10.6%，失真极小）
 *   zoom  1.5 倍，用于查看局部纹理
 *
 * 三个视图只做几何变换，不改颜色：不加 recolor、暗纱、蒙版，也不改 opa。
 * 桌面壁纸 build_wallpaper() 同样只用 lv_image_set_scale_*()，两边像素源
 * 都是同一个 .wall_buf 指针，因此只要这里不改色，屏上颜色必然一致。
 * 原有的 recolor 冷灰滤镜视图已删除。
 *
 * 换图后必须重建 dsc.data 指针：wallpaper_invalidate() 会让
 * wallpaper_get() 重新读文件；本 App 每次 build 都重新调用 wallpaper_get()。
 */

#define IMG_VIEW_ORIG   0                       /* 原寸（默认） */
#define IMG_VIEW_FIT    1                       /* 铺满 */
#define IMG_VIEW_ZOOM   2                       /* 放大 */
#define IMG_VIEW_COUNT  3                       /* 视图数量 */
#define IMG_VIEW_DEF    IMG_VIEW_ORIG           /* 进页面默认视图 */

#define IMG_FIT_W       240                     /* 取景框宽（px） */
#define IMG_FIT_H       286                     /* 取景框高 = body 高 = 320 - NAV_H(34) */
#define IMG_ZOOM_NUM    3                       /* 放大 1.5 倍 = 384/256 */
#define IMG_ZOOM_DEN    2                       /* 放大系数分母 */

static lv_obj_t   *s_img_obj;                   /* lv_image 对象 */
static lv_obj_t   *s_img_box;                   /* 取景框（固定 240x286，裁剪用） */
static lv_obj_t   *s_img_btn[IMG_VIEW_COUNT];   /* 三个视图按钮 */
static lv_obj_t   *s_img_note;                  /* 无图时的提示标签 */
static lv_image_dsc_t s_img_dsc;                /* 必须常驻：LVGL 长期持有指针 */
static int         s_img_view = IMG_VIEW_DEF;   /* 当前视图 */

/**
 * @brief  重填图像描述符，把像素源换成 p（几何参数不变）
 * @param  p  指向 .wall_buf 的像素数据，不能为 NULL
 */
static void img_dsc_set_data(const uint8_t *p)
{
    s_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;   /* 必填：lv_image_src_get_type
                                                        * 靠它判定「这是 lv_image_dsc_t」，
                                                        * 缺失会按文件路径去解析。 */
    s_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w      = WALL_SRC_W;
    s_img_dsc.header.h      = WALL_SRC_H;
    s_img_dsc.header.stride = WALL_SRC_W * 2;
    s_img_dsc.data_size     = WALL_BYTES;
    s_img_dsc.data          = p;
}

/**
 * @brief  把视图应用到图片对象，只改几何参数，不改颜色
 * @param  view  IMG_VIEW_ORIG / FIT / ZOOM
 */
static void img_apply_view(int view)
{
    if (s_img_obj == RT_NULL) return;

    switch (view)
    {
    case IMG_VIEW_ORIG:
        /* 原寸：1:1，居中偏移，靠取景框裁切。
         * 不加描边，否则边框会覆盖图边缘像素，导致色值不一致。 */
        lv_image_set_scale(s_img_obj, 256);
        lv_obj_set_pos(s_img_obj,
                       (IMG_FIT_W - WALL_SRC_W) / 2,
                       (IMG_FIT_H - WALL_SRC_H) / 2);
        break;

    case IMG_VIEW_ZOOM:
        /* 放大 1.5 倍，从中心看 */
        lv_image_set_scale(s_img_obj, 256 * IMG_ZOOM_NUM / IMG_ZOOM_DEN);
        lv_obj_set_pos(s_img_obj,
                       (IMG_FIT_W - WALL_SRC_W * IMG_ZOOM_NUM / IMG_ZOOM_DEN) / 2,
                       (IMG_FIT_H - WALL_SRC_H * IMG_ZOOM_NUM / IMG_ZOOM_DEN) / 2);
        break;

    case IMG_VIEW_FIT:
    default:
        /* 铺满：横向 1.0，纵向 286/320 ≈ 0.894（scale_y = 229/256） */
        lv_image_set_scale_x(s_img_obj, 256);
        lv_image_set_scale_y(s_img_obj, 256 * IMG_FIT_H / WALL_SRC_H);
        lv_obj_set_pos(s_img_obj, 0, 0);
        break;
    }
}

/**
 * @brief  视图切换按钮回调
 * @param  e  事件，user_data 为目标视图号
 */
static void img_view_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int v = (int)(rt_base_t)lv_event_get_user_data(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;
    if (s_img_obj == RT_NULL || v == s_img_view) return;

    s_img_view = v;

    /* 视图只改几何，无需更换像素源；重建 dsc 是幂等操作，顺手执行。 */
    img_dsc_set_data(wallpaper_get());
    img_apply_view(s_img_view);
    lv_image_set_src(s_img_obj, &s_img_dsc);    /* 让 LVGL 重新算区域 */
    lv_obj_invalidate(s_img_box);               /* 只刷取景框这块，别整屏 */
    rt_kprintf("[img] 视图 → %d\r\n", s_img_view);
}

/**
 * @brief  构建图片页：取景框 + 原图 + 三个视图按钮
 * @param  body  内容区容器
 * @note   无 tick；teardown 需清模块级指针。数据源为 wallpaper_get()。
 */
void app_img_build(lv_obj_t *body)
{
    const uint8_t *data;
    lv_obj_t *b;
    int i;

    /* 每次进页面重置模块级指针：对象是新建的，异步回调可能仍持有旧指针。 */
    s_img_obj = RT_NULL;
    s_img_box = RT_NULL;
    s_img_note = RT_NULL;
    for (i = 0; i < IMG_VIEW_COUNT; i++) s_img_btn[i] = RT_NULL;
    s_img_view = IMG_VIEW_DEF;                  /* 默认原寸 */

    data = wallpaper_get();

    /* 取景框：240x286 固定尺寸，超出部分裁掉。
     * 底色用透明而非黑色：三种视图都盖满取景框，正常看不到底色；
     * 若边缘留有缝隙，透出的是桌面背景，不会出现黑边。 */
    s_img_box = lv_obj_create(body);
    lv_obj_remove_style_all(s_img_box);
    lv_obj_set_size(s_img_box, IMG_FIT_W, IMG_FIT_H);
    lv_obj_align(s_img_box, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_img_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_img_box, LV_OBJ_FLAG_SCROLLABLE);
    /* 默认子对象超出父容器会被裁掉，正是取景框所需；
     * 不要添加 LV_OBJ_FLAG_OVERFLOW_VISIBLE，否则图会溢出到导航栏。 */

    if (data == RT_NULL)
    {
        /* 没图：给个明确提示，不放空白框 */
        s_img_note = ui_label(s_img_box, S_I_NONE, &zh18, C_TXT);
        lv_obj_center(s_img_note);
        rt_kprintf("[img] 无图片（wallpaper_get 返回 NULL）\r\n");
        return;
    }

    img_dsc_set_data(data);

    s_img_obj = lv_image_create(s_img_box);
    lv_image_set_src(s_img_obj, &s_img_dsc);
    lv_obj_clear_flag(s_img_obj, LV_OBJ_FLAG_SCROLLABLE);
    img_apply_view(s_img_view);

    /* 底部三个视图按钮，顺序与 IMG_VIEW_* 常量一致。
     * 取景框占满 0..286，按钮叠在其下缘之上，因此按钮必须建在 s_img_box
     * 内（或在其后创建），才能位于图片上层。 */
    {
        static const char *names[IMG_VIEW_COUNT];
        names[IMG_VIEW_ORIG] = S_I_ORIG;
        names[IMG_VIEW_FIT]  = S_I_FIT;
        names[IMG_VIEW_ZOOM] = S_I_ZOOM;

        for (i = 0; i < IMG_VIEW_COUNT; i++)
        {
            b = ui_button(s_img_box, names[i], 68, 34, NULL);
            lv_obj_add_event_cb(b, img_view_cb, LV_EVENT_ALL, (void *)(rt_base_t)i);
            /* x：6 + i*76 → 6/82/158，各宽 68 → 最右 226，右留 14px */
            lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 6 + i * 76, -8);
            s_img_btn[i] = b;
        }
    }

    rt_kprintf("[img] 图片页就绪：%dx%d RGB565（默认原寸；颜色同桌面壁纸）\r\n",
               WALL_SRC_W, WALL_SRC_H);
}

/**
 * @brief  图片页清理钩子
 * @note   图片与取景框随 lv_obj_clean 释放，但模块级指针必须置空：
 *         可能仍有排队中的 lv_async_call 回调会解引用它们。
 *         dsc.data 指向 wallpaper_store 的常驻 .wall_buf，不能 free。
 */
void app_img_teardown(void)
{
    s_img_obj  = RT_NULL;
    s_img_box  = RT_NULL;
    s_img_note = RT_NULL;
    s_img_view = IMG_VIEW_DEF;
}

/* msh 命令 imgview：打印图片页状态，供调试与验收 */
static void imgview(int argc, char **argv)
{
    const char *vname[IMG_VIEW_COUNT];
    (void)argc;
    (void)argv;

    vname[IMG_VIEW_ORIG] = "原寸"; vname[IMG_VIEW_FIT] = "铺满"; vname[IMG_VIEW_ZOOM] = "放大";
    rt_kprintf("[img] 数据源 %s, %ux%u RGB565\n",
               wallpaper_get() ? "已载入" : "缺失",
               (unsigned)WALL_SRC_W, (unsigned)WALL_SRC_H);
    rt_kprintf("[img] 当前视图 = %d (%s), 默认 = %d\n",
               s_img_view, vname[s_img_view], IMG_VIEW_DEF);
    rt_kprintf("[img] 图片对象 %p（纯几何变换，无任何调色）\n",
               (void *)s_img_obj);
}
MSH_CMD_EXPORT(imgview, picture app: current view state);

/* ── 9. 关于 ── */

/**
 * @brief  构建关于页：板卡与固件信息
 * @param  body  内容区容器
 * @note   均为静态信息，无 tick、无 teardown。
 */
void app_about_build(lv_obj_t *body)
{
    lv_obj_t *title = ui_label(body, "NS800RT7P65", &lv_font_montserrat_18, C_TXT);
    lv_coord_t y = 44;

    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    row_kv(body, y,      S_AB_BOARD, "NSSinePad");
    row_kv(body, y + 30, S_AB_CHIP,  "NS800RT7P65D");
    row_kv(body, y + 60, S_AB_SCR,   "ILI9341 240x320");
    row_kv(body, y + 90, S_AB_INPUT, "EC11 + KEY41");
    row_kv(body, y + 120, S_AB_LVGL, "LVGL 9.1.0");
    row_kv(body, y + 150, S_AB_RTOS, "RT-Thread " RT_VERSION_STR);
    row_kv(body, y + 180, S_AB_BUILD, __DATE__);
}

/* ── 10. 配网 ──
 *
 * 通过板间链路给 ESP32-C3 下发 WiFi 配置（STA 连路由器 / AP 开热点）。
 *
 * 不做软键盘：LVGL 内存池只有约 64K，软键盘 + 输入框 + 编辑缓冲要占几 K，
 * 且旋钮逐字选字母体验差。改为预置候选 + 旋钮选择：
 *   STA 模式：预置若干常用 SSID，旋钮选一个后按「应用」下发；
 *   AP 模式：预置若干热点名，同样选择。
 * 新增候选只需在下方 s_sta_presets / s_ap_presets 表里加一行。
 *
 * 配置最终存放在 C3 的 NVS（掉电不丢），本 App 只负责选择与下发。
 */

/* STA 模式预置候选表：{ SSID, 密码 } */
static const struct { const char *ssid; const char *pass; } s_sta_presets[] = {
    { "gpm",         "12345678" },   /* 默认候选 */
    { "nssine",      "12345678" },
    { "TP-LINK",     "12345678" },
};
/* AP 模式预置候选表：{ 热点名, 密码 } */
static const struct { const char *ssid; const char *pass; } s_ap_presets[] = {
    { "NSSinePad-AP", "nssinepad" }, /* 出厂默认 */
    { "NS800-Setup",  "12345678"  },
    { "NS800-Hot",    "88888888"  },
};
#define STA_N  ((int)(sizeof(s_sta_presets) / sizeof(s_sta_presets[0])))   /* STA 候选数 */
#define AP_N   ((int)(sizeof(s_ap_presets)  / sizeof(s_ap_presets[0])))    /* AP 候选数 */

static lv_obj_t *net_mode_lb;      /* "模式 STA/AP" */
static lv_obj_t *net_list_lb;      /* 选中项的名称与密码 */
static lv_obj_t *net_status_lb;    /* 下发结果 */
static lv_obj_t *net_item_btn[3];  /* 候选按钮，最多 3 个 */

static int s_net_mode = 0;         /* 0 = STA 1 = AP */
static int s_net_sel  = 0;         /* 当前选中的候选下标 */

/**
 * @brief  刷新列表区显示，模式或选中项变化后调用
 */
static void net_refresh(void)
{
    int i, n = (s_net_mode == 0) ? STA_N : AP_N;

    lv_label_set_text_fmt(net_mode_lb, "%s %s", S_N_MODE,
                          s_net_mode == 0 ? "STA" : "AP");

    /* 候选按钮文案 = SSID；选中项高亮由 group 焦点自动体现 */
    for (i = 0; i < 3; i++)
    {
        if (i < n)
        {
            const char *ssid = (s_net_mode == 0) ? s_sta_presets[i].ssid : s_ap_presets[i].ssid;
            lv_obj_clear_flag(net_item_btn[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(btn_text(net_item_btn[i]), ssid);
        }
        else
        {
            lv_obj_add_flag(net_item_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 底部显示当前选中项的名称与密码（明文，便于调试） */
    if (s_net_sel < n)
    {
        const char *ssid = (s_net_mode == 0) ? s_sta_presets[s_net_sel].ssid : s_ap_presets[s_net_sel].ssid;
        const char *pass = (s_net_mode == 0) ? s_sta_presets[s_net_sel].pass : s_ap_presets[s_net_sel].pass;
        lv_label_set_text_fmt(net_list_lb, "%s: %s\n%s: %s",
                              S_N_SSID, ssid, S_N_PASS, pass);
    }
    else
    {
        lv_label_set_text(net_list_lb, S_N_SET);
    }
}

/**
 * @brief  候选项按钮回调：选中该候选
 * @param  e  事件，user_data 为候选下标
 */
static void net_item_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    s_net_sel = (int)(rt_base_t)lv_event_get_user_data(e);
    net_refresh();
}

/**
 * @brief  模式切换按钮回调
 * @param  e  事件，user_data 为 0 = STA 1 = AP
 */
static void net_mode_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    s_net_mode = (int)(rt_base_t)lv_event_get_user_data(e);
    s_net_sel = 0;
    net_refresh();
    lv_label_set_text(net_status_lb, "");
}

/**
 * @brief  「应用」按钮回调：把选中候选下发到 C3
 * @param  e  LVGL 事件
 */
static void net_apply_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    rt_err_t err;
    int n = (s_net_mode == 0) ? STA_N : AP_N;

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;
    if (s_net_sel >= n) return;

    if (s_net_mode == 0)
    {
        const char *s = s_sta_presets[s_net_sel].ssid, *p = s_sta_presets[s_net_sel].pass;
        err = c3_link_set_wifi_cfg(s, p);
    }
    else
    {
        const char *s = s_ap_presets[s_net_sel].ssid, *p = s_ap_presets[s_net_sel].pass;
        err = c3_link_set_ap_cfg(s, p);
    }

    if (err == RT_EOK)
        lv_label_set_text(net_status_lb, S_N_SAVED);
    else
        lv_label_set_text(net_status_lb, S_N_FAIL);

    /* 下发后顺手请求回报，把 C3 实际配置读回来填快照 */
    if (err == RT_EOK) c3_link_get_wifi_cfg();
}

/**
 * @brief  构建配网页：模式切换 + 候选列表 + 应用按钮
 * @param  body  内容区容器
 * @note   无 tick、无 teardown；进入时异步拉取一次 C3 实际配置。
 */
void app_net_build(lv_obj_t *body)
{
    lv_obj_t *b_sta, *b_ap, *b_apply;
    int i;

    /* 顶部：当前模式 */
    net_mode_lb = ui_label(body, "", &zh18, C_ACCENT);
    lv_obj_align(net_mode_lb, LV_ALIGN_TOP_MID, 0, 8);

    /* 模式切换两个按钮 */
    b_sta = ui_button(body, S_N_STA, 104, 34, NULL);
    lv_obj_add_event_cb(b_sta, net_mode_cb, LV_EVENT_ALL, (void *)(rt_base_t)0);
    lv_obj_align(b_sta, LV_ALIGN_TOP_LEFT, 8, 36);

    b_ap = ui_button(body, S_N_AP, 104, 34, NULL);
    lv_obj_add_event_cb(b_ap, net_mode_cb, LV_EVENT_ALL, (void *)(rt_base_t)1);
    lv_obj_align(b_ap, LV_ALIGN_TOP_LEFT, 128, 36);

    /* 候选列表（3 行） */
    for (i = 0; i < 3; i++)
    {
        net_item_btn[i] = ui_button(body, "", 208, 30, NULL);
        lv_obj_add_event_cb(net_item_btn[i], net_item_cb, LV_EVENT_ALL, (void *)(rt_base_t)i);
        lv_obj_align(net_item_btn[i], LV_ALIGN_TOP_MID, 0, 80 + i * 34);
    }

    /* 选中项详情（名称 / 密码） */
    net_list_lb = ui_label(body, "", &zh18, C_TXT);
    lv_obj_align(net_list_lb, LV_ALIGN_TOP_LEFT, 18, 192);

    /* 下发结果提示 */
    net_status_lb = ui_label(body, "", &zh18, C_TXT_DIM);
    lv_obj_align(net_status_lb, LV_ALIGN_TOP_RIGHT, -18, 192);

    /* 应用按钮 */
    b_apply = ui_button(body, S_N_APPLY, 208, 40, NULL);
    lv_obj_add_event_cb(b_apply, net_apply_cb, LV_EVENT_ALL, NULL);
    lv_obj_align(b_apply, LV_ALIGN_TOP_MID, 0, 240);

    /* 首次进入：拉一次 C3 的实际配置（异步，几秒后回填快照） */
    c3_link_get_wifi_cfg();

    net_refresh();
    rt_kprintf("[net] 配网 App 就绪: 模式=%s, %d 个候选\n",
               s_net_mode == 0 ? "STA" : "AP", (s_net_mode == 0) ? STA_N : AP_N);
}

/* ── 闹钟 ──
 *
 * 旋钮只有一个 KEY，因此不做二级页面。焦点在「列表项 4 个 / 时滑块 /
 * 分滑块 / 删除 / 添加」之间移动：
 *   列表项：KEY 开/关该闹钟
 *   滑块  ：KEY 进入调节态，此时旋钮改数值，再按一次退出
 *   删除  ：删掉当前选中项
 *   添加  ：用滑块上的时/分新建一个闹钟
 *
 * 滑块可「旋钮调值」的原理：lv_slider 类带 editable = TRUE +
 * group_def = TRUE，创建时自动进默认 group；焦点在其上短按 KEY 会让
 * lv_indev 调 lv_group_set_editing(g, true)（不发 CLICKED），之后旋钮的
 * enc_diff 才转成 LV_KEY_LEFT/RIGHT 发给它。列表项需要的是 CLICKED，
 * 两者互斥，故列表项不能用滑块，只能用可聚焦普通按钮。
 *
 * 调节只写 RAM，不每格写 flash：LittleFS 一次 erase + write 是几十毫秒，
 * 每转一格写一次会明显卡顿并无谓磨损 flash。改动由 tick 里的去抖计时器
 * 统一落盘，退出页面时 teardown 兜底再存一次。
 *
 * 响铃不依赖本 App 是否打开：判定在 ui.c 的常驻定时器 alarm_watch_cb()
 * 里，停在桌面照样会响；本 App 的 tick 只负责把「正在响」的状态画出来。
 */
#include "app_alarm.h"

#define VC_ALARM   0xFB923C   /* 橙红，闹钟强调色 */

/* 布局（内容区 240x286）*/
#define AL_ROW_Y0     44      /* 第 1 个列表槽的 y */
#define AL_ROW_DY     30      /* 槽间距 */
#define AL_ROW_H      26      /* 槽高 */
#define AL_SL_Y0     168      /* 第 1 个滑块行的 y（顺序：时、分）*/
#define AL_SL_DY      30      /* 滑块行间距 */
#define AL_SL_X       40      /* 滑轨左端 x */
#define AL_SL_W      118      /* 滑轨宽 */
#define AL_VAL_X     172      /* 数值文本左端 x */

static lv_obj_t *al_list[ALARM_MAX];    /* 每个槽位一个可聚焦按钮 */
static lv_obj_t *al_time_lb;            /* 选中项的大号时间 */
static lv_obj_t *al_cnt_lb;             /* 右上角「开/总数」 */
static lv_obj_t *al_del_btn;            /* 删除按钮 */
static lv_obj_t *al_add_btn;            /* 添加按钮 */
static lv_obj_t *al_sl[2];              /* 0 = 时 1 = 分 */
static lv_obj_t *al_val_lb[2];          /* 两个滑块的数值文本 */
static int       al_sel = 0;            /* 当前选中下标 */
static int       al_dirty;              /* 有改动尚未落盘 */
static int       al_settle;             /* 停手计数（tick 数）*/

/**
 * @brief  绘制第 i 个列表槽
 * @param  i  槽下标，越界直接返回
 * @note   仅在文本确实变化时才 set_text，减少重绘区域。
 */
static void al_paint_slot(int i)
{
    alarm_item_t list[ALARM_MAX];
    lv_obj_t *txt;
    char buf[32];

    if (i < 0 || i >= ALARM_MAX) return;

    if (i >= (int)alarm_count())
    {
        /* 隐藏而非删除：按钮已在 focus group 里，删除会让 group 持有的
         * 指针悬空，下次旋转聚焦到野指针。 */
        lv_obj_add_flag(al_list[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    alarm_get(list);
    lv_obj_clear_flag(al_list[i], LV_OBJ_FLAG_HIDDEN);

    txt = lv_obj_get_child(al_list[i], 0);
    rt_snprintf(buf, sizeof(buf), "%02d:%02d  %s",
                (int)list[i].hour, (int)list[i].min,
                list[i].enabled ? S_AL_ON : S_AL_OFF);
    if (rt_strcmp(lv_label_get_text(txt), buf) != 0)
    {
        lv_label_set_text(txt, buf);
        /* 开 = 强调色，关 = 灰色，便于分辨哪些会响 */
        lv_obj_set_style_text_color(txt,
                                    lv_color_hex(list[i].enabled ? VC_ALARM : C_TXT_DIM),
                                    0);
    }
}

/**
 * @brief  绘制左上角的大号时间
 * @note   字体必须随内容切换：有闹钟时内容是 "07:30"（纯 ASCII），可用
 *         montserrat_32；无闹钟时内容是中文「无闹钟」，montserrat 是拉丁
 *         子集渲染不了汉字，会显示方块，必须用 zh18。
 *         LVGL 的字体是 label 的属性，可在运行时直接修改。
 */
static void al_paint_bigtime(void)
{
    alarm_item_t list[ALARM_MAX];
    int n = (int)alarm_count();

    if (al_sel >= 0 && al_sel < n)
    {
        alarm_get(list);
        lv_obj_set_style_text_font(al_time_lb, &lv_font_montserrat_32, 0);
        lv_label_set_text_fmt(al_time_lb, "%02d:%02d",
                              (int)list[al_sel].hour, (int)list[al_sel].min);
    }
    else
    {
        lv_obj_set_style_text_font(al_time_lb, &zh18, 0);
        lv_label_set_text(al_time_lb, S_AL_NONE);
    }
}

/**
 * @brief  把两个滑块同步为当前选中闹钟的时间
 * @note   没有闹钟时不动滑块，保留上次的值，作为「添加」时的初值。
 */
static void al_sync_sliders(void)
{
    alarm_item_t list[ALARM_MAX];
    int n = (int)alarm_count();
    int h, m;

    if (al_sel < 0 || al_sel >= n) return;

    alarm_get(list);
    h = list[al_sel].hour;
    m = list[al_sel].min;

    /* lv_slider_set_value() 不发 VALUE_CHANGED，因此这里不会再次触发
     * al_slider_cb，不会形成回环；这也是「程序改值」与「旋钮改值」的区分依据。 */
    lv_slider_set_value(al_sl[0], h, LV_ANIM_OFF);
    lv_slider_set_value(al_sl[1], m, LV_ANIM_OFF);
    lv_label_set_text_fmt(al_val_lb[0], "%02d", h);
    lv_label_set_text_fmt(al_val_lb[1], "%02d", m);
}

/**
 * @brief  刷新整个列表区
 * @note   build、增删改、焦点变化后都需调用。
 */
static void al_refresh(void)
{
    int i;

    lv_label_set_text_fmt(al_cnt_lb, "%d/%d", (int)alarm_active_count(), ALARM_MAX);
    for (i = 0; i < ALARM_MAX; i++) al_paint_slot(i);
    al_paint_bigtime();
    al_sync_sliders();
}

/**
 * @brief  滑块被旋钮调节的回调，修改当前选中闹钟的时间
 * @param  e  事件，user_data 为 0 = 时 1 = 分
 * @note   此处不调用 alarm_save()：每转一格就写一次 flash 会明显卡顿并
 *         磨损 flash。改动只写 RAM 并置脏标志，由 app_alarm_tick 的去抖
 *         计时器统一落盘，退出页面时 app_alarm_teardown 再兜底一次。
 */
static void al_slider_cb(lv_event_t *e)
{
    int which = (int)(rt_base_t)lv_event_get_user_data(e);
    alarm_item_t list[ALARM_MAX];
    int v;

    if (which != 0) which = 1;
    v = (int)lv_slider_get_value(lv_event_get_target(e));

    lv_label_set_text_fmt(al_val_lb[which], "%02d", v);

    /* 还没有闹钟：滑块只代表「添加时用的初值」，不碰数据 */
    if (al_sel < 0 || al_sel >= (int)alarm_count()) return;

    alarm_get(list);
    if (which == 0) alarm_set_time((rt_uint8_t)al_sel, (rt_uint8_t)v, list[al_sel].min);
    else            alarm_set_time((rt_uint8_t)al_sel, list[al_sel].hour, (rt_uint8_t)v);

    al_dirty  = 1;
    al_settle = 0;

    /* 只重画受影响的那一槽 + 大号时间，不整表重刷 */
    al_paint_slot(al_sel);
    al_paint_bigtime();
}

/**
 * @brief  计算「当前时间 +1 分钟」的时/分
 * @param  h  输出，小时 0..23
 * @param  m  输出，分钟 0..59
 * @note   跨天/跨小时必须回绕：23:59 + 1 分钟应得 00:00。直接
 *         (ts + 60) / 3600 会得到 hour = 24，闹钟永不触发；先对 86400
 *         （一天的秒数）取模可把小时限制在 0..23。
 */
static void al_next_minute(rt_uint8_t *h, rt_uint8_t *m)
{
    c3_link_state_t c3;
    rt_int32_t ts, rem;

    c3_link_get_state(&c3);
    if (!c3.time_valid)
    {
        *h = 7;      /* 未对时：给固定默认值 07:00 */
        *m = 0;
        return;
    }

    ts  = c3.time_unix +
          (rt_int32_t)((rt_tick_get() - c3.time_tick) / RT_TICK_PER_SECOND);
    rem = (ts + 60) % 86400;
    if (rem < 0) rem += 86400;      /* 时间戳为负（C3 还没对上）时防一手 */
    *h = (rt_uint8_t)(rem / 3600);
    *m = (rt_uint8_t)((rem % 3600) / 60);
}

/**
 * @brief  列表项回调：焦点进入即选中，按键切换开关
 * @param  e  事件，user_data 为槽下标
 */
static void al_item_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(rt_base_t)lv_event_get_user_data(e);

    if (idx < 0 || idx >= ALARM_MAX) return;
    if (idx >= (int)alarm_count()) return;

    /* 焦点移到某一槽时它成为「正在编辑的那条」，滑块跟着走。
     * 不要在 FOCUSED 里做 toggle，否则只是转过去看一眼就会开关掉闹钟。 */
    if (code == LV_EVENT_FOCUSED)
    {
        al_sel = idx;
        al_sync_sliders();
        return;
    }

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    al_sel = idx;
    alarm_toggle((rt_uint8_t)idx);
    al_paint_slot(idx);
    lv_label_set_text_fmt(al_cnt_lb, "%d/%d", (int)alarm_active_count(), ALARM_MAX);
}

/**
 * @brief  删除按钮回调：删除当前选中闹钟
 * @param  e  LVGL 事件
 */
static void al_del_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    if (al_sel < 0 || al_sel >= (int)alarm_count()) return;
    if (!alarm_remove((rt_uint8_t)al_sel)) return;

    if (al_sel > 0) al_sel--;       /* 避免选中项指向已消失的槽位 */
    al_refresh();
}

/**
 * @brief  添加按钮回调：用滑块上的时/分新建一个闹钟
 * @param  e  LVGL 事件
 */
static void al_add_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int idx;

    if (code != LV_EVENT_SHORT_CLICKED && code != LV_EVENT_CLICKED) return;
    if (!ui_click_ok()) return;

    /* 取滑块当前值。没有闹钟时滑块放的是「当前时间 +1 分钟」（见 build
     * 末尾），因此直接按添加会新建一个一分钟后响的闹钟。 */
    idx = alarm_add((rt_uint8_t)lv_slider_get_value(al_sl[0]),
                    (rt_uint8_t)lv_slider_get_value(al_sl[1]));
    if (idx < 0) return;            /* 已满 */

    if (al_dirty) { alarm_save(); al_dirty = 0; }   /* 顺手把之前的改动落盘 */

    al_sel = idx;
    al_refresh();
}

/**
 * @brief  构建闹钟页：大号时间 + 计数 + 4 个列表槽 + 时/分滑块 + 删/增
 * @param  body  内容区容器
 * @note   tick 周期 500 ms；teardown 需兜底落盘并清模块级指针。
 */
void app_alarm_build(lv_obj_t *body)
{
    lv_obj_t *hint, *lb;
    rt_uint8_t h, m;
    int i;

    /* 顶部：选中项的大号时间（左）+ 计数（右）。
     * 数字用 montserrat_32（已启用）。不要改用 28：lv_conf.h 里
     * MONTSERRAT_28=0，未启用的字体没有编译进去，引用会报 undeclared；
     * 内部 FLASH 已用约 93%，为一个字号再增一份字库不划算。
     * 空状态用 zh18：montserrat 是拉丁子集，渲染不了「无闹钟」会显示方块。 */
    al_time_lb = ui_label(body, S_AL_NONE, &zh18, VC_ALARM);
    lv_obj_align(al_time_lb, LV_ALIGN_TOP_LEFT, 14, 2);

    al_cnt_lb = ui_label(body, "", &zh18, C_TXT_DIM);
    lv_obj_align(al_cnt_lb, LV_ALIGN_TOP_RIGHT, -14, 8);

    /* ── 列表 4 槽 ── */
    for (i = 0; i < ALARM_MAX; i++)
    {
        al_list[i] = ui_button(body, "--:--", 212, AL_ROW_H, NULL);
        lv_obj_add_event_cb(al_list[i], al_item_cb, LV_EVENT_ALL,
                            (void *)(rt_base_t)i);
        lv_obj_align(al_list[i], LV_ALIGN_TOP_LEFT, 14, AL_ROW_Y0 + i * AL_ROW_DY);
    }

    /* ── 调值提示（放在列表和滑块之间）── */
    hint = ui_label(body, S_AL_EDIT, &zh18, C_TXT_DIM);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 151);

    /* 时 / 分 两个滑块：属「可编辑控件」，焦点在其上按 KEY 会让 LVGL 进入
     * 编辑态，之后旋钮只改数值、不再移动焦点（详见本段开头的设计说明）。 */
    for (i = 0; i < 2; i++)
    {
        lb = ui_label(body, i == 0 ? S_AL_HOUR : S_AL_MIN, &zh18, C_TXT);
        lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 14, AL_SL_Y0 + i * AL_SL_DY + 2);

        al_sl[i] = lv_slider_create(body);
        lv_obj_set_size(al_sl[i], AL_SL_W, 10);
        lv_obj_align(al_sl[i], LV_ALIGN_TOP_LEFT, AL_SL_X, AL_SL_Y0 + i * AL_SL_DY + 7);
        lv_slider_set_range(al_sl[i], 0, i == 0 ? 23 : 59);
        lv_slider_set_value(al_sl[i], i == 0 ? 7 : 0, LV_ANIM_OFF);

        /* 旋钮控制不需要「点击」，但必须保持可聚焦：清掉 CLICK_FOCUSABLE
         * 会导致控件进不了 group，旋钮无法选中。 */
        lv_obj_clear_flag(al_sl[i], LV_OBJ_FLAG_CLICKABLE);

        /* 轨道做细条：LVGL 9 没有 bg_width，用 pad_ver 压出来 */
        lv_obj_set_style_pad_ver(al_sl[i], 2, LV_PART_MAIN);
        lv_obj_set_style_radius(al_sl[i], 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(al_sl[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(al_sl[i], LV_OPA_30, LV_PART_MAIN);

        /* 已选段用闹钟强调色，便于看出当前值位置 */
        lv_obj_set_style_bg_color(al_sl[i], lv_color_hex(VC_ALARM), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(al_sl[i], LV_OPA_80, LV_PART_INDICATOR);
        lv_obj_set_style_radius(al_sl[i], 3, LV_PART_INDICATOR);

        /* 把手 */
        lv_obj_set_style_bg_color(al_sl[i], lv_color_hex(VC_ALARM), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(al_sl[i], LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_pad_all(al_sl[i], 8, LV_PART_KNOB);
        lv_obj_set_style_radius(al_sl[i], LV_RADIUS_CIRCLE, LV_PART_KNOB);

        lv_obj_add_event_cb(al_sl[i], al_slider_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(rt_base_t)i);

        al_val_lb[i] = ui_label(body, "00", &zh18, C_TXT);
        lv_obj_align(al_val_lb[i], LV_ALIGN_TOP_LEFT, AL_VAL_X, AL_SL_Y0 + i * AL_SL_DY + 2);
    }

    /* ── 底部：删除（左） / 添加（右） ── */
    al_del_btn = ui_button(body, S_AL_DEL, 100, 34, NULL);
    lv_obj_add_event_cb(al_del_btn, al_del_cb, LV_EVENT_ALL, NULL);
    lv_obj_align(al_del_btn, LV_ALIGN_BOTTOM_LEFT, 12, -2);

    al_add_btn = ui_button(body, S_AL_ADD, 100, 34, NULL);
    lv_obj_add_event_cb(al_add_btn, al_add_cb, LV_EVENT_ALL, NULL);
    lv_obj_align(al_add_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -2);

    /* ── 列表操作提示 ── */
    hint = ui_label(body, S_AL_HINT, &zh18, C_TXT_DIM);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 227);

    al_sel = 0;
    al_dirty  = 0;
    al_settle = 0;
    al_refresh();

    /* 没有任何闹钟时给滑块一个有意义的初值：当前时间 +1 分钟，
     * 这样进来直接按添加会新建一个一分钟后响的闹钟。 */
    if (alarm_count() == 0)
    {
        al_next_minute(&h, &m);
        lv_slider_set_value(al_sl[0], h, LV_ANIM_OFF);
        lv_slider_set_value(al_sl[1], m, LV_ANIM_OFF);
        lv_label_set_text_fmt(al_val_lb[0], "%02d", (int)h);
        lv_label_set_text_fmt(al_val_lb[1], "%02d", (int)m);
    }
}

/**
 * @brief  闹钟页周期刷新：响铃状态变化时重画，并负责滑块改动的去抖落盘
 * @note   周期 500 ms；去抖约 1 s（2 个 tick）落盘一次。
 */
void app_alarm_tick(void)
{
    static rt_bool_t last = RT_FALSE;
    rt_bool_t now = alarm_is_ringing();

    if (now != last)
    {
        last = now;
        al_refresh();
    }

    /* 去抖落盘：停手约 1 s 后才写一次 flash。不在 al_slider_cb 里直接写，
     * 否则每转一格写一次会让界面卡顿。 */
    if (al_dirty && ++al_settle >= 2)
    {
        alarm_save();
        al_dirty  = 0;
        al_settle = 0;
    }
}

/**
 * @brief  离开闹钟页：兜底落盘并清空模块级指针
 * @note   这次落盘不能省：用户调完滑块立刻返回时，tick 的去抖尚未到点，
 *         省掉会丢失本次改动。本函数非 static，ui.c 的 App 表会引用它。
 */
void app_alarm_teardown(void)
{
    int i;

    if (al_dirty)
    {
        alarm_save();
        al_dirty  = 0;
        al_settle = 0;
    }

    /* 清空模块级缓存：下次进页面是全新的一批对象 */
    al_time_lb = NULL;
    al_cnt_lb  = NULL;
    al_del_btn = NULL;
    al_add_btn = NULL;
    al_val_lb[0] = al_val_lb[1] = NULL;
    al_sl[0]     = al_sl[1]     = NULL;
    for (i = 0; i < ALARM_MAX; i++) al_list[i] = NULL;
}
