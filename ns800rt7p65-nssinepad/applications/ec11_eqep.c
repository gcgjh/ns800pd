/**
 * @file    ec11_eqep.c
 * @brief   EC11 旋转编码器驱动：片内 eQEP3 硬件正交解码
 *
 * 引脚配置顺序：复位后引脚处于模拟输入态，模拟开关未断开时数字通路断开，
 * 此时即使把 MUX 设为 ALT5，信号也送不进 eQEP，现象是不报错、寄存器读数
 * 正常但不计数。必须按 rt_pin_mode() 的顺序依次调用：
 *   setPadConfig -> setAnalogMode(DISABLED) -> setPinConfig。
 *
 * 不能用 INIT_APP_EXPORT 初始化：自动初始化阶段与 LVGL/LCD(SPI) 初始化并发，
 * eQEP3 时钟刚建立尚不稳定，写入会当场成功随后被清零（QEPCTL 归 0）。
 * 现由 lv_port_indev_init() 在 LCD 之后显式调用，并回读校验加重试兜底。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#include <stdlib.h>             /* atoi，ec11raw 命令使用 */

#include "ec11_eqep.h"

/* 模块绑定：更换 eQEP 实例只需改这一段 */
#define EC11_EQEP            EQEP3                        /* 使用 eQEP3，而非 eQEP1 */
#define EC11_CLK_MASK        RCC_PCLKEN1_EQEP3EN_M        /* PCLKEN1 时钟使能位 */
#define EC11_RST_MASK        RCC_APBRST1_EQEP3RST_M       /* APBRST1 复位位 */
#define EC11_PIN_A           GPIO_6                       /* 展开为 GPIOA, PIN_6 */
#define EC11_PIN_B           GPIO_7
#define EC11_ALT_A           GPIO_6_EQEP3_A               /* 宏自带 (port, pin, ALT) */
#define EC11_ALT_B           GPIO_7_EQEP3_B
#define EC11_QPOS_MAX        0xFFFFFFFFUL                 /* 自由累加，不依赖 Z 相 */

/* QEPCTL.SWI：软件初始化位置计数器位，本芯片上不会自清，需手动清除 */
#define EC11_QEPCTL_SWI_M    0x0080UL

/* 调试命令开关：置 0 可省数百字节 Flash（ec11、ec11raw 两个命令） */
#define EC11_DEBUG_CMD       0
/* 初始化分步打印开关：置 0 静默，不影响功能 */
#define EC11_INIT_VERBOSE    0

#if EC11_INIT_VERBOSE
#define EC11_LOG(...)        rt_kprintf(__VA_ARGS__)
#else
#define EC11_LOG(...)
#endif

/* 初始化成功标志：未就绪时所有读取接口返回 0 */
static rt_bool_t g_ec11_ready = RT_FALSE;

/**
 * @brief  配置 A/B 相引脚为 eQEP 复用
 * @note   三步顺序与 rt_pin_mode() 相同，缺一不可：
 *         setPadConfig -> setAnalogMode(DISABLED) -> setPinConfig。
 */
static void ec11_pin_config(void)
{
    /* A 相：GPIO_6 -> eQEP3_A（ALT5） */
    GPIO_setPadConfig(EC11_PIN_A, GPIO_PIN_TYPE_PULLUP);
    GPIO_setAnalogMode(EC11_PIN_A, GPIO_ANALOG_DISABLED);
    GPIO_setPinConfig(EC11_ALT_A);

    /* B 相：GPIO_7 -> eQEP3_B（ALT5） */
    GPIO_setPadConfig(EC11_PIN_B, GPIO_PIN_TYPE_PULLUP);
    GPIO_setAnalogMode(EC11_PIN_B, GPIO_ANALOG_DISABLED);
    GPIO_setPinConfig(EC11_ALT_B);
}

/**
 * @brief  位置计数器清零，并手动清除 QEPCTL.SWI 位
 */
static void ec11_clear_position(void)
{
    EQEP_setPosition(EC11_EQEP, 0);
    CLEAR_BIT(EC11_EQEP->QEPCTL.WORDVAL, EC11_QEPCTL_SWI_M);
}

/**
 * @brief  初始化 eQEP3：时钟 -> 释放复位 -> 引脚 -> 解码器 -> 计数器 -> 使能
 * @return RT_EOK 成功；-RT_ERROR 连续 3 轮校验均未生效
 * @note   每步显式延时，最后回读校验，失败则延时重试。
 */
rt_err_t ec11_eqep_init(void)
{
    int retry;

    if (g_ec11_ready)
        return RT_EOK;

    /* 1. 开模块时钟并释放复位：两者都必需，只开时钟时寄存器写不进去 */
    SET_BIT(RCC->PCLKEN1.WORDVAL, EC11_CLK_MASK);
    rt_thread_mdelay(1);                    /* 等时钟建立 */
    CLEAR_BIT(RCC->APBRST1.WORDVAL, EC11_RST_MASK);
    rt_thread_mdelay(2);                    /* 复位释放后等模块稳定 */

    /* 2. 引脚三步配置（见文件头说明） */
    ec11_pin_config();

    /* 3. 解码器：正交、2 倍频、不交换 A/B、关闭索引门控 */
    EQEP_setDecoderConfig(EC11_EQEP,
                          EQEP_CONFIG_QUADRATURE |
                          EQEP_CONFIG_2X_RESOLUTION |
                          EQEP_CONFIG_NO_SWAP |
                          EQEP_CONFIG_IGATE_DISABLE);

    /* 4. 位置计数器：MAX_POS 模式自由累加（EC11 无索引脉冲，不能用 IDX 复位） */
    EQEP_setPositionCounterConfig(EC11_EQEP,
                                  EQEP_POSITION_RESET_MAX_POS,
                                  EC11_QPOS_MAX);
    ec11_clear_position();

    /* 5. 使能并回读校验，不生效则延时重试，最多 3 轮 */
    for (retry = 0; retry < 3; retry++)
    {
        EQEP_enableModule(EC11_EQEP);
        rt_thread_mdelay(1);

        rt_uint16_t qepctl  = EC11_EQEP->QEPCTL.WORDVAL;
        rt_uint32_t qposmax = EC11_EQEP->QPOSMAX.WORDVAL;

        if ((qepctl & EQEP_QEPCTL_QPEN) && (qposmax == EC11_QPOS_MAX))
        {
            g_ec11_ready = RT_TRUE;
#if EC11_INIT_VERBOSE
            EC11_LOG("[ec11] eQEP3 ready @ GPIO_6(A)/GPIO_7(B)"
                     "  QEPCTL=0x%04x QPOSMAX=0x%08x pos=%d\n",
                     (unsigned)qepctl, (unsigned)qposmax,
                     (int)EQEP_getPosition(EC11_EQEP));
#endif
            return RT_EOK;
        }

        EC11_LOG("[ec11] verify failed (QEPCTL=0x%04x QPOSMAX=0x%08x), retry %d\n",
                 (unsigned)qepctl, (unsigned)qposmax, retry);
        rt_thread_mdelay(10);
    }

    EC11_LOG("[ec11] eQEP3 init FAILED after 3 retries\n");
    return -RT_ERROR;
}

/**
 * @brief  查询初始化是否成功
 * @return RT_TRUE 已就绪；RT_FALSE 未就绪
 */
rt_bool_t ec11_is_ready(void)
{
    return g_ec11_ready;
}

/**
 * @brief  读取原始位置计数
 * @return 位置计数；未初始化时返回 0
 */
rt_uint32_t ec11_get_position(void)
{
    if (!g_ec11_ready)
        return 0;
    return (rt_uint32_t)EQEP_getPosition(EC11_EQEP);
}

/*
 * 每格棘爪的计数增量 = 解码分辨率 × EC11 每格 AB 周期数。
 *
 *   解码 4X：每 AB 周期 4 计数（A/B 相上升沿与下降沿全计）
 *   解码 2X：每 AB 周期 2 计数（只计单相上升沿与下降沿，当前配置）
 *   解码 1X：每 AB 周期 1 计数（只计单相上升沿）
 *
 * 当前为 2X，实测本机 EC11 每转一格跳 4，即每格含 2 个 AB 周期，故取 4。
 * 若日后改用 EQEP_CONFIG_1X_RESOLUTION，此值应改为 2。
 */
#define EC11_COUNTS_PER_DETENT    4

/**
 * @brief  读取当前格数（有符号）
 * @return 格数；未初始化时返回 0
 */
rt_int32_t ec11_get_steps(void)
{
    rt_int32_t counts;

    if (!g_ec11_ready)
        return 0;

    counts = (rt_int32_t)EQEP_getPosition(EC11_EQEP);

    return counts / (rt_int32_t)EC11_COUNTS_PER_DETENT;
}


/**
 * @brief  读取当前旋转方向
 * @return 1 = 正转；-1 = 反转。未初始化时返回 1
 */
rt_int16_t ec11_get_direction(void)
{
    if (!g_ec11_ready)
        return 1;
    return (rt_int16_t)EQEP_getDirection(EC11_EQEP);
}

/**
 * @brief  位置清零
 * @note   未初始化时不做任何操作。
 */
void ec11_reset_position(void)
{
    if (g_ec11_ready)
        ec11_clear_position();
}

/* 调试命令：EC11_DEBUG_CMD = 0 时整段不编译 */

#if EC11_DEBUG_CMD

/**
 * @brief  msh 命令 ec11：读取位置与方向，或清零
 *         用法 ec11 [reset]
 */
static void ec11(int argc, char **argv)
{
    if (argc >= 2 && rt_strcmp(argv[1], "reset") == 0)
    {
        ec11_reset_position();
        rt_kprintf("[ec11] position reset\n");
        return;
    }

    if (!g_ec11_ready)
    {
        rt_kprintf("[ec11] NOT initialized -- eQEP3 init failed, check boot log\n");
        return;
    }

    rt_kprintf("[ec11] pos=%d  dir=%d\n",
               (int)EQEP_getPosition(EC11_EQEP),
               (int)EQEP_getDirection(EC11_EQEP));
}
MSH_CMD_EXPORT(ec11, ec11 [reset] : read or reset eQEP3 position);

/**
 * @brief  msh 命令 ec11raw：观察 A/B 相原始电平变化，用于核对接线
 *         用法 ec11raw [ms]，默认 5000 ms
 * @note   切换到 GPIO 输入前会先配为下拉/上拉输入，结束后恢复 eQEP 复用。
 */
static void ec11raw(int argc, char **argv)
{
    int ms = 5000;
    rt_uint8_t la, lb, pla, plb;
    rt_uint32_t checked = 0;
    const rt_base_t pin_a = 6;      /* BSP pin index = GPIO_6 */
    const rt_base_t pin_b = 7;      /* BSP pin index = GPIO_7 */

    if (argc >= 2)
        ms = atoi(argv[1]);
    if (ms < 100)
        ms = 100;

    rt_pin_mode(pin_a, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(pin_b, PIN_MODE_INPUT_PULLUP);
    la = rt_pin_read(pin_a);
    lb = rt_pin_read(pin_b);
    pla = la;
    plb = lb;

    rt_kprintf("[ec11raw] watching GPIO_6(A)/GPIO_7(B) for %d ms, turn the knob now\n", ms);
    rt_kprintf("[ec11raw] idle level: A=%u B=%u\n", la, lb);

    while (checked < (rt_uint32_t)ms)
    {
        la = rt_pin_read(pin_a);
        lb = rt_pin_read(pin_b);
        if (la != pla || lb != plb)
        {
            rt_kprintf("  A=%u B=%u  (+%u ms)\n", la, lb, checked);
            pla = la;
            plb = lb;
        }
        rt_thread_mdelay(2);
        checked += 2;
    }

    rt_kprintf("[ec11raw] done, eQEP3 pins restored\n");
    ec11_pin_config();          /* 恢复 eQEP 复用（含关闭模拟模式三步） */
}
MSH_CMD_EXPORT(ec11raw, ec11raw [ms] : raw GPIO watch on A/B pins);

#endif /* EC11_DEBUG_CMD */
