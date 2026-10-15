/**
 * @file    app_alarm.h
 * @brief   闹钟数据模型、持久化与到点判定的接口
 *
 * 与 UI 分离：ui_apps.c 只负责绘制，存储与触发判定集中在 app_alarm.c，
 * 便于独立验证，也可由 msh 命令复用。
 *
 * 持久化沿用 settings.c 的做法：单文件文本加临时文件 rename（掉电安全），
 * 存放于外置 flash 的 LittleFS，路径 /alarm.txt。不并入 /cfg.txt：后者
 * 每行为固定 key，而闹钟是变长列表，混在一起会在改一个闹钟时重写整份
 * 配置，而写 flash 有数十毫秒卡顿。
 */

#ifndef __APP_ALARM_H__
#define __APP_ALARM_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 闹钟数量上限
 * @note  取 4：列表一屏可显示；更多条目需引入滚动与额外内存，实际用不到。
 */
#define ALARM_MAX     4

typedef struct
{
    rt_uint8_t hour;        /* 小时，0..23 */
    rt_uint8_t min;         /* 分钟，0..59 */
    rt_uint8_t enabled;     /* 开关，0=关 1=开 */
    rt_uint8_t fired_today; /* 当天是否已触发，0/1，用于同一分钟内去重 */
} alarm_item_t;

/* ── 持久化 ── */

/**
 * @brief  从 flash 载入闹钟列表
 * @return RT_TRUE 读取成功（含文件不存在的首启，此时列表为空且各字段为 0）；
 *         RT_FALSE LittleFS 挂载失败
 * @note   文件不存在或损坏均不报错，一律退化为空列表；闹钟丢失只是不便，
 *         不应因此阻塞开机。挂载失败时不置已载入标志，允许上层稍后重试。
 */
rt_bool_t alarm_load(void);

/**
 * @brief  将闹钟列表写回 flash
 * @return RT_TRUE 保存成功；RT_FALSE 失败
 * @note   临时文件写入后 rename；列表为空时删除文件而非写空文件。
 */
rt_bool_t alarm_save(void);

/* ── 列表操作 ── */

/**
 * @brief  取一份闹钟列表快照（结构体很小，整体拷贝）
 * @param  out 输出缓冲；为 RT_NULL 时直接返回
 */
void alarm_get(alarm_item_t *out);

/**
 * @brief  当前实际占用的槽位数（与开关状态无关）
 * @return 0..ALARM_MAX
 * @note   界面据此决定显示几行，不能靠 hour/min/enabled 是否非零判断，
 *         否则 00:00 的合法闹钟会被误判成空槽，应由数据层给出真实个数。
 */
rt_uint8_t alarm_count(void);

/**
 * @brief  追加一个闹钟
 * @param  hour 小时，越界时钳到 0..23
 * @param  min  分钟，越界时钳到 0..59
 * @return 槽位下标 0..ALARM_MAX-1；列表已满返回 -1
 * @note   新建默认 enabled=1，用户主动添加即为使用。
 */
int alarm_add(rt_uint8_t hour, rt_uint8_t min);

/**
 * @brief  删除第 idx 个闹钟，其后元素前移一位
 * @param  idx 槽位下标
 * @return RT_TRUE 成功；RT_FALSE 下标越界
 */
rt_bool_t alarm_remove(rt_uint8_t idx);

/**
 * @brief  切换第 idx 个闹钟的开关状态
 * @param  idx 槽位下标
 * @return RT_TRUE 成功；RT_FALSE 下标越界
 */
rt_bool_t alarm_toggle(rt_uint8_t idx);

/**
 * @brief  修改某个闹钟的时间
 * @param  idx  槽位下标
 * @param  hour 小时 0..23
 * @param  min  分钟 0..59
 * @return RT_TRUE 成功；RT_FALSE 参数或下标越界
 * @note   只改 RAM 不落盘，供旋钮连续调节使用。每转一格就写一次 flash
 *         不可行：LittleFS 的 erase+write 耗时数十毫秒，写在 LVGL 线程里
 *         会造成界面卡顿，也加快 flash 磨损。调用方在调节结束后必须自行
 *         调用一次 alarm_save()。其余 alarm_xxx 接口内部均自行落盘。
 */
rt_bool_t alarm_set_time(rt_uint8_t idx, rt_uint8_t hour, rt_uint8_t min);

/**
 * @brief  当前处于开启状态的闹钟数量
 * @return 0..s_count
 */
rt_uint8_t alarm_active_count(void);

/* ── 到点判定 ── */

/**
 * @brief  检查是否有闹钟到点，到点则启动响铃
 * @param  now_unix 当前本地时间戳（UTC+8）；小于 0 表示未对时，直接返回 0
 * @return 本次触发返回 1，否则 0
 * @note   去重依赖 fired_today：只比较时和分时，同一分钟内每次调用都会
 *         重复触发，响铃被反复重启而无法停止。每个闹钟每天只触发一次，
 *         日序号变化时统一清除 fired_today。
 * @note   响铃由 beep() 包负责，其内部为单静态线程，本函数同一时刻只能
 *         被调用一次。beep() 是阻塞调用，必须在解锁之后执行。
 */
int alarm_check(rt_int32_t now_unix);

/**
 * @brief  停止响铃（用户按 KEY 确认）
 * @return RT_TRUE 此前正在响铃且已停止；RT_FALSE 本来未响
 */
rt_bool_t alarm_stop(void);

/**
 * @brief  查询当前是否正在响铃
 * @return RT_TRUE 正在响铃
 */
rt_bool_t alarm_is_ringing(void);

/**
 * @brief  取上次响铃的闹钟下标
 * @return 槽位下标；无有效值时返回 -1
 */
int alarm_last_index(void);

/* ── 生命周期 ── */

/**
 * @brief  初始化互斥量并从 flash 载入
 * @return RT_EOK
 * @note   必须在 ui_init() 中先于 App 表构建调用：闹钟 App 的 build()
 *         会读取列表，不能撞上未初始化的状态。
 */
int alarm_init(void);

/* ── msh 命令 ──
 *   alist          显示全部闹钟
 *   aadd <h> <m>   新增闹钟
 *   adel <i>       删除第 i 个（从 0 开始）
 *   acheck         立即按当前时间判定一次，用于验证逻辑
 *
 * 注意：本段块注释必须显式收尾。若漏掉收尾符，注释会延伸到文件末尾
 * include guard 的注释处，把那里的收尾符当成自己的，导致开头的 #ifndef
 * 永不闭合，GCC 报 "unterminated #ifndef"，而 #if/#endif 计数看起来却
 * 仍然一致。排查时分别统计两种定界符的数量，二者应相等。
 */

#ifdef __cplusplus
}
#endif

#endif /* __APP_ALARM_H__ */

