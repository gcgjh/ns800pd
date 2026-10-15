/**
 * @file    settings.h
 * @brief   板级配置持久化接口
 *
 * 配置存于外置 flash 的 LittleFS（/cfg.txt），掉电不丢。开机自动载入，
 * 改动通过 settings_update_one() 或 settings_save() 落盘。
 */

#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 累计运行时长落盘周期 ──
 *
 * 每隔多少秒把"历史加本次开机"的总时长写一次 flash，掉电最多丢失最近
 * 这么长的时间，换取不频繁擦写 flash。
 * 放在头文件而非某个 .c 内部：ui.c 的常驻定时器与 ui_apps.c 的系统 App
 * tick 都要使用同一个值，各写一份容易改一处忘一处。
 */
#define UPTIME_SAVE_PERIOD_S   60

/* 存盘的配置项 */
typedef struct
{
    rt_int32_t tune_warm;      /* gamma 冷暖 -100..100（<0 冷，>0 暖） */
    rt_int32_t tune_bright;    /* gamma 明暗 -100..100               */
    rt_int32_t tune_sat;       /* 饱和度     -100..100               */
    rt_int32_t backlight;      /* 背光        0..100                 */
    rt_int32_t home_page;      /* 桌面当前页  0..1                   */
    rt_int32_t game_best;      /* 跑酷游戏最高分 0..99999            */
    /* ── 累计运行时长（断电保存）──
     * 语义为历史累计开机秒数，本次开机的时间不计入，运行时另行相加。
     * 需要持久化是因为屏上"运行时长"原先只统计本次开机，断电即归零。
     * 落盘策略见 ui.c 的 uptime_flush_cb()（常驻定时器，与界面无关）：
     * 每 UPTIME_SAVE_PERIOD_S 秒刷新一次，掉电最多丢最近 60 秒。 */
    rt_int32_t uptime_saved;   /* 上次落盘时累计的开机秒数 */
} rt_settings_t;

/**
 * @brief  从 flash 载入配置
 * @return RT_TRUE 成功读到并解析；RT_FALSE 挂载失败或文件不存在
 * @note   失败时铺出厂默认值，仍会置已载入标志（首机返回 RT_FALSE）。
 */
rt_bool_t settings_load(void);

/**
 * @brief  将配置写回 flash
 * @return RT_TRUE 保存成功；RT_FALSE 失败
 * @note   临时文件加 rename，掉电安全。
 */
rt_bool_t settings_save(void);

/**
 * @brief  取配置快照
 * @param  out 输出缓冲；为 RT_NULL 时不写入
 */
void settings_get(rt_settings_t *out);

/**
 * @brief  整体写入配置（只改内存）
 * @param  in 输入配置；为 RT_NULL 时按出厂默认值处理
 * @note   内部统一钳位；会把累计运行时长的记账基线清零。
 */
void settings_set(const rt_settings_t *in);

/**
 * @brief  修改单项配置并立即落盘
 * @param  which 'w' 冷暖 'b' 明暗 's' 饱和 'l' 背光 'p' 页号
 *               'g' 游戏最高分 'u' 累计运行秒数
 * @param  val   新值
 * @return RT_TRUE 成功；RT_FALSE which 非法或写盘失败
 */
rt_bool_t settings_update_one(char which, rt_int32_t val);

/**
 * @brief  取本次开机已运行的秒数（不含历史累计）
 * @return 本次开机秒数
 */
#define settings_uptime_now_s()  ((rt_int32_t)(rt_tick_get() / RT_TICK_PER_SECOND))

/**
 * @brief  取当前累计总秒数（历史加本次）
 * @return 当前累计总秒数
 * @note   不要自行写 settings_uptime_saved() 加本次时间：会把已落盘的
 *         部分重复计算，得到越涨越快的假值。记账逻辑只此一份（见 settings.c）。
 */
rt_int32_t settings_uptime_total(void);

/**
 * @brief  周期落盘，内部按 UPTIME_SAVE_PERIOD_S 节流，可高频调用
 * @return RT_TRUE 已写入或无需写入；RT_FALSE 写失败
 * @note   单一记账点，ui.c 的常驻定时器与 App 页的 tick 同时调用亦不干扰。
 */
rt_bool_t settings_uptime_flush(void);

/**
 * @brief  取历史累计运行秒数（不含本次开机）
 * @return 历史累计秒数
 * @note   仅用于展示历史与本次的拆分。
 */
rt_int32_t settings_uptime_saved(void);

/**
 * @brief  把累计总秒数写回 flash，供掉电前或周期落盘使用
 * @param  total_s 要保存的累计总秒数（历史加本次）
 * @return RT_TRUE 已写入或无需写入；RT_FALSE 写失败
 * @note   内部判断数值是否增长，未增长则不写，以省 flash 寿命。
 * @note   一般不要直接调用：应走 settings_uptime_flush()，后者会同步
 *         更新记账基线，直接调用本函数会导致重复累加。
 */
rt_bool_t settings_uptime_store(rt_int32_t total_s);

/**
 * @brief  查询配置是否已载入
 * @return RT_TRUE 已从 flash 载入过
 */
rt_bool_t settings_is_loaded(void);

/**
 * @brief  查询配置载入后是否被用户改动过
 * @return RT_TRUE 被用户改动过
 * @note   开机流程中 home_page_show 会调用 update_one，用它挡掉"刚载入就
 *         写回"造成的默认值覆盖。
 */
rt_bool_t settings_is_dirty(void);

/* msh 命令：cfg / cfgsave / cfgreload / cfgset / cfgdefault
 *   cfg            显示当前配置
 *   cfgset w -30   把冷暖改成 -30 并存盘
 *   cfgdefault     恢复出厂并存盘
 */

#ifdef __cplusplus
}
#endif

#endif /* __SETTINGS_H__ */