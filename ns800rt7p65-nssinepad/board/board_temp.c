/*
 * 内部温度传感器（TS）读取
 *
 * 走 ADCA 通道 13，绕过原厂 ADC_getTemperatureC()——后者是通用占位模型
 * （0.925V @25C + 3.1mV/C），实测偏低约 32C。本文件改用 TRM §27.12.8 的
 * 每片出厂标定值换算，实测与板载 DHT22 环境温度相差 +5C 左右，符合
 * 「TS 测的是芯片结温（含自热）、手册标称精度 ±15C」的预期。
 */
#include <rtthread.h>

/* 原厂 adc.h 与 RT-Thread 框架 adc.h 共用 __ADC_H__，须先 undef 才能拿到原厂版（同 drv_adc.h） */
#ifdef __ADC_H__
#undef __ADC_H__
#endif
#include "adc.h"
#ifdef __ADC_H__
#undef __ADC_H__
#endif

#include "NS800RT7P65x.h"

#define TEMP_SOC   ADC_SOC_NUMBER8   /* 避开 drv_adc 占用的 SOC 0~7 */
#define TEMP_CH    ADC_CH_ADCIN13    /* 内部温度传感器 TS */

/*
 * 出厂标定值（Flash 信息区，只读）
 * TRM §27.12.8：三个数值均测于 VREFHI=1.65V，单位是 ADC 码
 */
#define TS_HI    (*(volatile int16_t *)0x1FFFC204UL)   /* 125 C */
#define TS_N25   (*(volatile int16_t *)0x1FFFC208UL)   /*  25 C */
#define TS_LO    (*(volatile int16_t *)0x1FFFC20CUL)   /* -40 C */

/*
 * 增益还原
 * ANACTL 复位默认 SGAIN = 14/32（ADC_SGAIN_14DIV32 枚举值正是 0），
 * 而标定值是在 1.65V 全量程下取得的，故须把实测码值乘 32/14 才落到标定域。
 * 忘了这一步会把 30C 算成 0.6C。
 */
#define TS_GAIN_NUM  32
#define TS_GAIN_DEN  14

/* 上电初始化：使能 TS 并占一个 SOC 槽位；INIT_APP_EXPORT 自动调用 */
int board_temp_init(void)
{
    ADCA->ANACTL.WORDVAL |= ADC_ADCANACTL_TSEN_MASK;   /* 使能后需等待 >500us 才能采样 */
    rt_thread_mdelay(1);
    ADC_setupSOC(ADCA, TEMP_SOC, ADC_TRIGGER_SW_ONLY, TEMP_CH, 64);
    ADC_setInterruptSOCTrigger(ADCA, TEMP_SOC, ADC_INT_SOC_TRIGGER_NONE);
    return 0;
}
INIT_APP_EXPORT(board_temp_init);

/* 连采 3 次取平均，压掉单次抖动 */
static uint32_t ts_raw(void)
{
    uint32_t sum = 0;
    for (int i = 0; i < 3; i++)
    {
        ADC_forceMultipleSOC(ADCA, 1UL << TEMP_SOC);
        rt_thread_mdelay(1);
        sum += ADC_readResult(ADCARESULT, TEMP_SOC);   /* 结果寄存器是 ADCARESULT，不是 ADCA */
    }
    return sum / 3;
}

/*
 * 读温度（返回摄氏度）
 *   Slope = (TS_HI - TS_LO) / 165        // LSB/C
 *   Ts    = (Rs - TS_N25) / Slope + 25
 * 全程整数运算：rt_kprintf 不支持 %f，会原样打印出 "%f"。
 * 同时打印原始码与标定值，便于现场复核。
 */
float board_temp_read(void)
{
    uint32_t raw = ts_raw();
    int      hi  = TS_HI, n25 = TS_N25, lo = TS_LO;
    int      rs  = (int)(raw * TS_GAIN_NUM / TS_GAIN_DEN);   /* 还原增益后即标定域码值 */
    int      slope100;
    int      t10;

    if (hi == lo)   /* 标定区读失败时的保护：避免除零 */
    {
        rt_kprintf("TS trim invalid (hi==lo==%d)\n", hi);
        return 0.0f;
    }

    slope100 = (hi - lo) * 100 / 165;                        /* LSB/C * 100 */
    t10      = (rs - n25) * 1000 / slope100 + 250;           /* 0.1 C */

    rt_kprintf("raw=%u rs=%d trim=%d/%d/%d slope=%d.%02d\n",
               raw, rs, hi, n25, lo, slope100 / 100, slope100 % 100);
    rt_kprintf("Ts=%d.%dC\n", t10 / 10, t10 < 0 ? -t10 % 10 : t10 % 10);   /* 负温度手动拆小数点 */
    return t10 / 10.0f;
}
MSH_CMD_EXPORT(board_temp_read, read board temperature in C);
