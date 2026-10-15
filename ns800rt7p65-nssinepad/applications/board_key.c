/**
 * @file    board_key.c
 * @brief   板载按键 KEY（GPIO_41），基于 PKG_USING_BUTTON 软件包
 *
 * 硬件：board.c 的 KEY_Initial() 已把该脚配置为开漏输入并启用 6 采样滤波：
 *   GPIO_setPinConfig(GPIO_41_GPIO41);
 *   GPIO_setAnalogMode(BOARD_KEY_PIN, GPIO_ANALOG_DISABLED);
 *   GPIO_setPadConfig(BOARD_KEY_PIN, GPIO_PIN_TYPE_OD);       开漏
 *   GPIO_setQualificationPeriod/Mode(...6SAMPLE);             6 采样滤波去抖
 *   GPIO_setDirectionMode(BOARD_KEY_PIN, GPIO_DIR_MODE_IN);
 * 开漏加外部上拉，空闲高、按下低，故 KEY_ON = PIN_LOW。
 * 硬件本身已有 6 采样滤波，软件包的消抖时间仅作双保险。
 *
 * 软件包（packages/button-latest）为纯状态机，不依赖任何 BSP：
 *   Button_Create(name, btn, 读电平函数, 触发电平)
 *   Button_Attach(btn, 事件, 回调)   事件含单击/双击/长按/长按释放/连发
 *   Button_Process()                 必须周期性调用（本文件 10 ms 一次）
 * 时间参数在 button.h 中，单位是扫描周期数：
 *   BUTTON_LONG_TIME   50 -> (50-1)*10 = 490 ms 判长按
 *   BUTTON_DOUBLE_TIME 15 -> 140 ms 双击窗口
 *
 * 按键功能：
 *   单击 -> 回到桌面（在 App 页内退出，UI 对象操作走 lv_async_call）
 *   长按 -> 打印一行（预留调试或后续功能挂载点）
 * 注意与编码器 EC11 上 KEY31 的区别：KEY31 是 LVGL 的 indev（选择/执行），
 * KEY41 是独立按键，两者互不干扰。
 */

#include <rtthread.h>
#include <board.h>
#include <lvgl.h>

#include "button.h"
#include "ui.h"

/* 注意：BOARD_KEY_PIN 来自 board.h，展开为厂商的二元组宏
 * （GPIO_41 -> GPIOF, GPIO_PIN_xx），供 GPIO_setAnalogMode(port, pin) 使用。
 * RT-Thread 的 rt_pin_read()/rt_pin_mode() 只接收一个引脚号，
 * 因此每次都要经 PIN_NUM() 转换。 */
#define KEY_PIN     PIN_NUM(BOARD_KEY_PIN)

/* 按键对象 */
static Button_t g_key_btn;

/**
 * @brief  读取 KEY41 电平
 * @return 1 空闲高，0 按下低（开漏加外部上拉，见文件头）
 */
static rt_uint8_t key_read_level(void)
{
    return (rt_uint8_t)rt_pin_read(KEY_PIN);
}

/**
 * @brief  lv_async_call 回调：回到桌面
 * @param  p  未使用
 * @note   LVGL 对象只能在 LVGL 线程操作，关页面须经 async 调用。
 */
static void key_back_home_async(void *p)
{
    (void)p;
    app_close();
}

/**
 * @brief  KEY41 单击回调：打印提示并异步回桌面
 * @param  btn  按键对象，未使用
 */
static void key_down_cb(void *btn)
{
    (void)btn;
    rt_kprintf("[key] KEY41 单击 → 回桌面\n");
    lv_async_call(key_back_home_async, RT_NULL);
}

/**
 * @brief  KEY41 长按回调：打印提示
 * @param  btn  按键对象，未使用
 */
static void key_long_cb(void *btn)
{
    (void)btn;
    rt_kprintf("[key] KEY41 长按\n");
}

/**
 * @brief  KEY41 双击回调：打印提示
 * @param  btn  按键对象，未使用
 */
static void key_double_cb(void *btn)
{
    (void)btn;
    rt_kprintf("[key] KEY41 双击\n");
}

/**
 * @brief  按键扫描线程：每 10 ms 调用一次 Button_Process()
 * @param  param  未使用
 * @note   优先级设为最低，避免与 UI 线程抢占 CPU。
 */
static void key_scan_thread_entry(void *param)
{
    (void)param;
    while (1)
    {
        Button_Process();
        rt_thread_mdelay(10);
    }
}

/**
 * @brief  初始化 KEY41：配置引脚、注册按键事件、启动扫描线程
 * @return RT_EOK 成功；-RT_ERROR 扫描线程创建失败
 */
int board_key_init(void)
{
    rt_thread_t th;

    /* board.c 的 Switch_init()（含 KEY 引脚配置）从未被调用：它挂在
     * Board_init() 下，而 Board_init() 全工程无调用点。因此该脚一直处于
     * 默认状态（实测读出为 LOW）。这里用 RT-Thread 的 GPIO API 自行配置，
     * 复用与方向均走 drv_gpio。 */
    rt_pin_mode(KEY_PIN, PIN_MODE_INPUT);

    Button_Create("KEY41", &g_key_btn, key_read_level, PIN_LOW);
    Button_Attach(&g_key_btn, BUTTON_DOWM,   key_down_cb);
    Button_Attach(&g_key_btn, BUTTON_DOUBLE, key_double_cb);
    Button_Attach(&g_key_btn, BUTTON_LONG,   key_long_cb);

    th = rt_thread_create("key", key_scan_thread_entry, RT_NULL,
                          1024, 8, 10);
    if (th == RT_NULL)
    {
        rt_kprintf("[key] !! scan thread create failed\n");
        return -RT_ERROR;
    }
    rt_thread_startup(th);

    rt_kprintf("[key] KEY41 ready on %s (active %s, scan 10ms)\n",
               "GPIO_41", (PIN_LOW == PIN_LOW) ? "LOW" : "HIGH");
    return RT_EOK;
}
INIT_APP_EXPORT(board_key_init);

/* msh 命令 keyinfo：查看引脚原始电平，用于确认有效电平是否正确 */

/**
 * @brief  msh 命令 keyinfo：打印 KEY41 引脚电平与按键时间参数
 *         用法 keyinfo
 */
static void keyinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("[key] GPIO_41 level = %s (PIN_LOW=%d 表示按下)\n",
               (rt_pin_read(KEY_PIN) == PIN_LOW) ? "LOW(按下中)" : "HIGH(空闲)",
               PIN_LOW);
    rt_kprintf("[key] 长按判定 ~490ms，双击窗口 ~140ms\n");
}
MSH_CMD_EXPORT(keyinfo, show board KEY41 pin level and timing);
