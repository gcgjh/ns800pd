/**
 * @file    lv_port_indev.c
 * @brief   LVGL v9.1.0 输入移植层：eQEP3 旋转编码器与 EC11 自带按键（GPIO_31）
 *
 * 本文件只做两件事：
 *   1. 调用 ec11_eqep_init()，必须在 LCD 初始化之后（本函数由 LVGL 线程在
 *      lv_port_disp_init() 之后调用），原因见 ec11_eqep.c 文件头说明；
 *   2. 注册 LVGL encoder indev：位置增量写入 enc_diff，按键写入按键状态。
 * lv_group_set_default() 不可遗漏，否则控件不入 group，旋转编码器无响应。
 *
 * 注意：这里用 EC11 编码器自带的按键（GPIO_31），板载独立按键 KEY41
 * 由 board_key.c 单独处理，两者互不干扰。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <lvgl.h>

#include "ec11_eqep.h"

/* EC11 按键 = GPIO31。PIN_NUM() 展开为函数调用，不能用于静态初始化，
 * 故此处使用纯数字，真正的引脚模式在 lv_port_indev_init() 中配置。 */
#define EC11_KEY_PIN        31

/* 两次读取之间允许的最大增量：防止快速旋转被当作噪声累加，手感异常时调整此值 */
#define ENC_DIFF_CLAMP      24

/* 按键消抖：连续 N 次读到相同电平才确认（LVGL 默认每 30 ms 读取一次） */
#define ENC_KEY_DEBOUNCE_CNT 2

static lv_indev_t *g_indev_encoder = RT_NULL;   /* encoder indev 对象 */
static rt_int32_t  g_last_pos     = 0;   /* 上次读走时的格数快照（get_steps 内部已处理回绕） */
static rt_base_t   g_key_pin       = EC11_KEY_PIN;   /* 编码器按键引脚号 */

/**
 * @brief  LVGL encoder 读取回调：上报旋钮增量与按键状态
 * @param  indev  输入设备
 * @param  data   输出，enc_diff 与 state
 */
static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static rt_uint8_t key_stable = 1;     /* 1=松开 0=按下 */
    static rt_uint8_t key_cnt    = 0;

    /* 位置增量：格数（get_steps 内部已处理环形回绕），一格棘爪 = ±1 */
    rt_int32_t cur  = ec11_get_steps();
    rt_int32_t diff = cur - g_last_pos;
    g_last_pos = cur;

    if (diff >  ENC_DIFF_CLAMP) diff =  ENC_DIFF_CLAMP;
    if (diff < -ENC_DIFF_CLAMP) diff = -ENC_DIFF_CLAMP;
    data->enc_diff = (int16_t)diff;

    /* 按键消抖 */
    rt_uint8_t lv = (rt_pin_read(g_key_pin) == PIN_HIGH) ? 1 : 0;
    if (lv == key_stable)
    {
        key_cnt = 0;
    }
    else if (++key_cnt >= ENC_KEY_DEBOUNCE_CNT)
    {
        key_stable = lv;
        key_cnt = 0;
    }
    data->state = key_stable ? LV_INDEV_STATE_RELEASED : LV_INDEV_STATE_PRESSED;
}

/**
 * @brief  初始化 EC11 与按键并注册 encoder indev
 * @note   由 LVGL 线程在 lv_port_disp_init() 之后调用。
 */
void lv_port_indev_init(void)
{
    /* eQEP3 初始化放在此处而非 INIT_APP_EXPORT：开机自动初始化阶段与
     * LCD(SPI)/LVGL 并发，寄存器写入会被吞掉（详见 ec11_eqep.c） */
    ec11_eqep_init();
    if (!ec11_is_ready())
    {
        rt_kprintf("[lvgl] WARNING: eQEP3 not ready, encoder indev dead\n");
    }

    rt_pin_mode(g_key_pin, PIN_MODE_INPUT_PULLUP);

    g_indev_encoder = lv_indev_create();
    lv_indev_set_type(g_indev_encoder, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(g_indev_encoder, encoder_read_cb);

    /* 默认组：之后创建的可聚焦控件自动入组 */
    lv_group_t *g = lv_group_create();
    lv_indev_set_group(g_indev_encoder, g);
    lv_group_set_default(g);

    rt_kprintf("[lvgl] encoder indev ready (eQEP + KEY%u)\n", (unsigned)g_key_pin);
}
