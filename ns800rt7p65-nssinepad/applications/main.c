/**
 * @file    main.c
 * @brief   系统入口：创建心跳灯线程，其余外设由自动初始化完成
 *
 * LED1（GPIO_68 = PC4）的模式由 UI 的 LED App 控制；
 * LED2（GPIO_69）固定作系统心跳，1 Hz 短闪，与 LED1 互不干扰，
 * 即使 LED1 被设为常亮或关闭，仍可据此判断固件是否在运行。
 * 两颗灯均为低电平点亮。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <stdlib.h>             /* atoi */

#include <dhtxx.h>              /* dhtxx 包（PKG_USING_DHTXX），数据脚 GPIO_40 */
#include <beep.h>               /* beep 包（PKG_USING_BEEP），有源蜂鸣器 GPIO_1 */

/* LED1 引脚：GPIO_68 = PC4 */
#define LED1_PIN    PIN_NUM(GPIO_68)
/* LED2 引脚：GPIO_69，board.c 的 LED_init() 已初始化为输出。
 * 用作系统心跳灯：固定 1 Hz 短闪（100 ms 亮 / 900 ms 灭），
 * 与 LED1 的 App 可控模式互不干扰。LED2 低电平点亮，与 LED1 相同。 */
#define LED2_PIN    PIN_NUM(GPIO_69)
#define BEEP_PIN    PIN_NUM(GPIO_1)     /* 有源蜂鸣器 */
#define DATA_PIN    PIN_NUM(GPIO_40)    /* DHT22 数据脚 */

/* 心跳灯：低优先级独立线程，main 只负责创建。
 * g_led_mode 为 LED1 模式，由 UI 的 LED App 改写：
 * 0 慢闪 / 1 快闪 / 2 常亮 / 3 关闭。LED1 低电平点亮。 */
rt_uint8_t g_led_mode = 0;

/* LED2 心跳开关，默认开，LED App 亦可控制 */
rt_uint8_t g_led2_hb = 1;

/* 固定 10 ms 节拍驱动，两颗灯共用同一时基，节奏可精确还原：
 *   慢闪 100 ms 亮 + 900 ms 灭 = 1.0 s   -> tick % 100 < 10
 *   快闪  60 ms 亮 + 120 ms 灭 = 0.18 s  -> tick % 18  < 6
 * 若改为按模式各自 mdelay，两颗灯会互相阻塞，无法同时闪烁。 */
#define LED_TICK_MS         10          /* 节拍周期，毫秒 */
#define LED_SLOW_TICKS      100         /* 慢闪周期 100 * 10 ms = 1 s */
#define LED_SLOW_ON         10          /* 慢闪亮 10 * 10 ms = 100 ms */
#define LED_FAST_TICKS      18          /* 快闪周期 18 * 10 ms = 180 ms */
#define LED_FAST_ON         6           /* 快闪亮 6 * 10 ms = 60 ms */

/**
 * @brief  心跳灯线程：按 10 ms 节拍驱动 LED1 与 LED2
 * @param  param  未使用
 * @note   LED1 的模式受 g_led_mode 控制，LED2 复用慢闪节奏作 1 Hz 心跳。
 */
static void led_thread_entry(void *param)
{
    rt_uint32_t tick = 0;

    rt_pin_mode(LED1_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(LED2_PIN, PIN_MODE_OUTPUT);

    while (1)
    {
        /* LED1：App 可控的四种模式 */
        switch (g_led_mode)
        {
        case 2:                                     /* 常亮 */
            rt_pin_write(LED1_PIN, PIN_LOW);
            break;

        case 3:                                     /* 关闭 */
            rt_pin_write(LED1_PIN, PIN_HIGH);
            break;

        case 1:                                     /* 快闪 */
            rt_pin_write(LED1_PIN, (tick % LED_FAST_TICKS < LED_FAST_ON) ? PIN_LOW : PIN_HIGH);
            break;

        default:                                    /* 慢闪：1s 周期 */
            rt_pin_write(LED1_PIN, (tick % LED_SLOW_TICKS < LED_SLOW_ON) ? PIN_LOW : PIN_HIGH);
            break;
        }

        /* LED2：系统心跳，复用慢闪节奏，1 Hz */
        rt_pin_write(LED2_PIN,
                     (g_led2_hb && (tick % LED_SLOW_TICKS < LED_SLOW_ON)) ? PIN_LOW : PIN_HIGH);

        tick++;
        rt_thread_mdelay(LED_TICK_MS);
    }
}

/* DHT22（GPIO_40）：msh 命令 */

/**
 * @brief  读取一次 DHT22 并打印温湿度
 * @note   DHT22 单次读取成功率偏低，内部最多重试 5 次，每次间隔 100 ms。
 */
static void cat_dhtxx(void)
{
    dht_device_t sensor = dht_create(DATA_PIN);
    rt_int8_t try_count = 5;
    rt_bool_t ok = RT_FALSE;

    if (sensor == RT_NULL)
    {
        rt_kprintf("dhtxx: alloc failed\n");
        return;
    }

    while (try_count-- > 0)
    {
        if (dht_read(sensor))
        {
            ok = RT_TRUE;
            break;
        }
        rt_kprintf("Read dht sensor failed.\n");
        rt_thread_mdelay(100);
    }

    if (ok)
    {
        rt_int32_t ti, td, hi, hd, flag;

        flag = split_int(dht_get_temperature(sensor), &ti, &td, 10);
        rt_kprintf("Temp: %s%d.%d\t", flag > 0 ? "-" : "", ti, td);

        flag = split_int(dht_get_humidity(sensor), &hi, &hd, 10);
        rt_kprintf("Hum: %s%d.%d\n", flag > 0 ? "-" : "", hi, hd);
    }

    dht_delete(sensor);         /* 成功或失败均需释放 */
}

/**
 * @brief  msh 命令 dhx22：周期读取 DHT22 温湿度
 *         用法 dhx22 <times> [delay_ms]
 * @param  times     读取次数，须大于 0
 * @param  delay_ms  两次读取的间隔，毫秒，默认 2000
 */
static void dhx22(int argc, char **argv)
{
    int         times    = 1;
    rt_int32_t  delay_ms = 2000;    /* 默认 2 s，符合 DHT22 最小采样间隔 */

    if (argc >= 2) times    = atoi(argv[1]);    /* argc >= 2 时才有 argv[1] */
    if (argc >= 3) delay_ms = atoi(argv[2]);

    if (times <= 0)
    {
        rt_kprintf("usage: dhx22 <times> [delay_ms]\n");
        return;
    }
    if (delay_ms < 0)
    {
        rt_kprintf("Invalid delay, use 2000ms\n");
        delay_ms = 2000;
    }

    while (times-- > 0)
    {
        cat_dhtxx();
        if (times > 0) rt_thread_mdelay(delay_ms);  /* 最后一次不用等待 */
    }
}
MSH_CMD_EXPORT(dhx22, dhx22 <times> [delay_ms]);

/* 有源蜂鸣器（beep 包）：上电自检响一声。
 * beep 包自带按需线程（响完即退出），此处只做一次初始化。 */

/**
 * @brief  蜂鸣器上电自检：初始化 GPIO_1 并短鸣提示
 * @return RT_EOK
 */
static int beep_hw_init(void)
{
    beep_init(BEEP_PIN, PIN_HIGH);
    beep(1, 150, 100, 0);
    return RT_EOK;
}
INIT_APP_EXPORT(beep_hw_init);

/* 板间链路：ESP32-C3，UART2 = PB6/PB7。
 * C3 负责 WiFi 联网与数据上报，NS800 负责 UI 与业务，
 * 两者按 c3_link.h 定义的帧协议通信。
 * 放在 INIT_APP_EXPORT 而非 main()：UI 可能在 main 返回前启动，
 * 提前初始化可保证 UI 首次刷新即读到有效状态。 */
#include "c3_link.h"

/**
 * @brief  板间链路初始化，由 INIT_APP_EXPORT 在启动阶段调用
 * @return RT_EOK
 */
static int c3_link_hw_init(void)
{
    c3_link_init();
    return RT_EOK;
}
INIT_APP_EXPORT(c3_link_hw_init);

/* main：只创建线程后返回 */

/**
 * @brief  系统入口：创建心跳灯线程
 * @return 0
 * @note   其余外设由各模块的自动初始化（INIT_*_EXPORT）完成。
 */
int main(void)
{
    rt_thread_t t;

    t = rt_thread_create("led", led_thread_entry, RT_NULL, 512, 25, 10);
    if (t) rt_thread_startup(t);

    return 0;
}
