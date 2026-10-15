/**
 * @file    c3_report.c
 * @brief   NS800 业务数据周期上报（NS800_CMD_HOST_DATA / 0x88）
 *
 * 每 5 秒把本机状态以 "key=value" 形式经 UART2 发往 C3，
 * 由 C3 的 host_data 模块缓存，并通过 /api/host_data 提供给网页。
 * 上报逻辑独立成文件，避免 ui.c 继续膨胀；新增字段只需改本文件。
 *
 * 每 5 秒推一次全部字段：这些数据变化缓慢，5 秒一轮足以让网页保持刷新；
 * C3 侧 host_data 为覆盖语义（同 key 存最新值），推中间值没有意义。
 * 一轮约 13 帧、每帧约 15 字节，链路压力很小。
 *
 * 采用 LVGL 定时器而非独立 RT-Thread 线程：定时器回调与各 App 的 tick
 * 同处 LVGL 线程，串行执行，不会与 UI 侧同时进入 c3_link_send_kv()
 * （该函数需持发送互斥锁）；同时省下一个线程约 1 KB 的栈开销
 * （RT-Thread 堆已接近排满）。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>          /* PIN_NUM / GPIO_40 —— 缺了会报 implicit declaration */

#include "c3_report.h"
#include "c3_link.h"
#include "settings.h"

/* DHT22：与 ui_apps.c 同型号、同引脚（GPIO_40）。
 * 这里不复用 ui_apps.c 的读数：其读取函数为 static，跨文件复用需去掉 static，
 * 会使 UI 层与上报层耦合。本模块自行读取一次，代价是每轮多一次约 4ms 的
 * DHT 事务。DHT22 手册要求最小采样间隔 2s：本模块 5 秒一轮、
 * ui_apps.c 自身按 2s 节流，两边各自满足。 */
#include <dhtxx.h>
#define DHT_PIN        PIN_NUM(GPIO_40)

/* ── 整数转字符串 ── */
/* rt_snprintf 不支持 %f，这里只需整数，直接用 %d。 */
static const char *i2s(rt_int32_t v, char *buf, rt_size_t n)
{
    if (v < 0)
    {
        /* 负数：符号占 1 位，剩余给数字 */
        rt_snprintf(buf, n, "-%d", (int)(-v));
    }
    else
    {
        rt_snprintf(buf, n, "%d", (int)v);
    }
    return buf;
}

/* 把定点温度转成 "23.4" 这类一位小数的字符串。
 * 不用 %f（rt_kprintf/rt_snprintf 不支持浮点，会输出乱码）。
 * 手工拆分：t_x100 / 100 为整数部分，%100 为小数部分。 */
static const char *t_x100_to_str(rt_int32_t t_x100, char *buf, rt_size_t n)
{
    rt_int32_t ip, fp;

    if (t_x100 < 0)
    {
        ip = (-t_x100) / 100;
        fp = (-t_x100) % 100;
        rt_snprintf(buf, n, "-%d.%d", (int)ip, (int)fp);
    }
    else
    {
        ip = t_x100 / 100;
        fp = t_x100 % 100;
        rt_snprintf(buf, n, "%d.%d", (int)ip, (int)fp);
    }
    return buf;
}

/**
 * @brief  读取 DHT22 温湿度
 * @param  t_x100  输出，温度 ×100
 * @param  h_x10   输出，湿度 ×10
 * @return RT_TRUE 读取成功
 * @note   必须重试 2 次：DHT22 单次读取成功率偏低（上一帧残留电平、
 *         总线未完全释放都会导致失败），首读失败后再试一次。
 *         两次之间不加延时，DHT22 的转换等待已在 dht_read() 内部完成，
 *         额外延时只会拖慢上报。
 *         失败时输出值不可信，调用方应跳过本轮上报，
 *         不得沿用旧值（否则网页会显示已过期的温度）。
 */
static rt_bool_t read_dht(rt_int32_t *t_x100, rt_int32_t *h_x10)
{
    dht_device_t dev;
    rt_bool_t ok = RT_FALSE;

    dev = dht_create(DHT_PIN);
    if (dev == RT_NULL) return RT_FALSE;

    for (int i = 0; i < 2 && !ok; i++)
    {
        if (dht_read(dev)) ok = RT_TRUE;
    }
    if (ok)
    {
        *t_x100 = dht_get_temperature(dev) * 10;   /* dht 返回 ×10，这里统一成 ×100 */
        *h_x10  = dht_get_humidity(dev);
        ok = RT_TRUE;
    }
    dht_delete(dev);
    return ok;
}

static void report_cb(lv_timer_t *t)
{
    rt_settings_t cfg;
    rt_int32_t   t_x100 = 0, h_x10 = 0;
    char         b[24];

    (void)t;

    /* ① 配置类（来自 flash 落盘的 settings）。
     * settings_get() 内部会在未载入时自动 load，此处无需自己判断。 */
    settings_get(&cfg);

    c3_link_send_kv("backlight", i2s(cfg.backlight, b, sizeof(b)));
    c3_link_send_kv("tune_warm",   i2s(cfg.tune_warm,   b, sizeof(b)));
    c3_link_send_kv("tune_bright", i2s(cfg.tune_bright, b, sizeof(b)));
    c3_link_send_kv("tune_sat",    i2s(cfg.tune_sat,    b, sizeof(b)));
    c3_link_send_kv("game_best",   i2s(cfg.game_best,   b, sizeof(b)));
    c3_link_send_kv("uptime_s",    i2s(settings_uptime_total(), b, sizeof(b)));

    /* ② 传感器（读失败就不推，宁可少一个字段也不要推过期值） */
    if (read_dht(&t_x100, &h_x10))
    {
        c3_link_send_kv("temp_c", t_x100_to_str(t_x100, b, sizeof(b)));
        /* 湿度 dht 给的是 ×10（= 整数百分比 23.5% → 235），
         * 这里转成一位小数字符串，和温度保持同一种表达。 */
        c3_link_send_kv("humidity", t_x100_to_str(h_x10 * 10, b, sizeof(b)));
    }

    /* ③ 链路自身状态（便于网页区分数据来自屏侧主动上报） */
    {
        c3_link_state_t s;
        c3_link_get_state(&s);
        c3_link_send_kv("c3_online", s.online ? "1" : "0");
        c3_link_send_kv("c3_alive",  s.online ? "1" : "0");
        c3_link_send_kv("ns800_ver", NS800_REPORT_VER);
    }

    /* 一次 report 推 13 帧，5 秒一次约 2.6 帧/秒；115200 下每帧约 15 字节，
     * 合计不到 40 字节/秒，占空比低于 0.4%。
     *
     * C3 侧 host_data 表有 16 槽，多于 13 个字段，一轮下来不会有字段被挤掉。
     * 槽位数必须不小于单轮字段数（见 host_data.h 里 HOST_DATA_SLOTS 的注释）；
     * 这里新增字段时，需同步调大 C3 侧的 HOST_DATA_SLOTS。
     */
}

/**
 * @brief  启动业务数据周期上行
 * @return 无
 * @note   在 LVGL 线程创建 5 秒周期的 lv_timer，首次上报在创建后 5 秒触发。
 *         必须先完成 UI 初始化再调用，避免开机阶段与 LittleFS 挂载相互争用。
 */
void c3_report_start(void)
{
    lv_timer_create(report_cb, 5000, NULL);
    rt_kprintf("[c3rep] 业务数据上行已启动（5 秒/轮，共 %d 字段）\r\n", 13);
}
