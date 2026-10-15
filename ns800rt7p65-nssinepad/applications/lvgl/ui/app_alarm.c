/**
 * @file    app_alarm.c
 * @brief   闹钟数据模型、持久化与到点判定
 *
 * 界面与逻辑分离：ui_apps.c 只负责绘制，闹钟的存储与触发判定集中在
 * 本文件，便于独立验证，也可由 msh 命令复用。
 * 持久化沿用 settings.c 的做法：单文件文本加临时文件 rename，
 * 存放于外置 flash 的 LittleFS，路径 /alarm.txt。
 */

#include <rtthread.h>
#include <dfs.h>
#include <fcntl.h>
#include <unistd.h>
/* 头文件路径用 <msh.h>，不用 <finsh/msh.h>：finsh 组件只把
 * components/finsh 这一层加入 include 路径，写成 finsh/msh.h 会报
 * "No such file or directory"。缺少该头时 MSH_CMD_EXPORT 不展开，
 * 报错为 "'alist' undeclared here (not in a function)"。 */
#include <msh.h>

#include "app_alarm.h"
#include "spi3_flash.h"
#include "c3_link.h"      /* acheck 命令用它取当前时间戳 */

/* beep 包：响铃使用，单静态线程 */
#include <beep.h>

/* 持久化路径：先写临时文件再 rename，避免掉电留下截断文件 */
#define ALARM_PATH    "/alarm.txt"
#define ALARM_TMP     "/alarm.tmp"

/* 一天秒数，用于把时间戳折算成日序号 */
#define DAY_SEC       86400

/* 闹钟列表与状态：s_count 为实际占用槽位数，s_loaded 表示是否已载入 */
static alarm_item_t s_list[ALARM_MAX];
static rt_uint8_t   s_count;
static rt_bool_t    s_loaded;

/* 响铃状态：s_last_idx 为正在响铃的闹钟下标，-1 表示无 */
static rt_bool_t s_ringing;
static rt_int32_t s_last_idx = -1;

/* 上次判定时的日序号（now / DAY_SEC），用于跨天清除 fired_today */
static rt_int32_t s_last_day  = -1;

/* 保护列表的互斥量：写入均在 LVGL 线程，但 msh 命令（aadd/adel）在
 * 另一线程执行，可能与 App 的操作交错。
 * 优先级参数用 RT_IPC_FLAG_PRIO（与 c3_link.c 一致），不是 RT_IPRIO_CURR，
 * 后者表示线程优先级，用在 mutex 上无法编译。 */
static struct rt_mutex s_lock;
static rt_bool_t s_lock_ready;

/* ── 时间工具：只拆出日/时/分，不需要完整的 gmtime ──
 *
 * 不复用 ui.c 的 sb_gmtime()：它是 static，且输入同样是已换算到
 * UTC+8 的时间戳。这里用相同口径拆分：ts 直接相除，不做时区偏移。
 *
 * @param  ts   本地时间戳（秒）
 * @param  day  输出，日序号（ts / DAY_SEC）
 * @param  h    输出，小时 0..23
 * @param  m    输出，分钟 0..59
 */
static void split_time(rt_int32_t ts, rt_int32_t *day, rt_uint8_t *h, rt_uint8_t *m)
{
    rt_int32_t days = ts / DAY_SEC;
    rt_int32_t rem  = ts % DAY_SEC;

    if (rem < 0) { rem += DAY_SEC; days -= 1; }
    *day = days;
    *h = (rt_uint8_t)(rem / 3600);
    *m = (rt_uint8_t)((rem % 3600) / 60);
}

static void lock(void)
{
    if (s_lock_ready) rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
}

static void unlock(void)
{
    if (s_lock_ready) rt_mutex_release(&s_lock);
}

/* ── 持久化 ── */

/**
 * @brief  从 flash 载入闹钟列表
 * @return RT_TRUE 读取成功（含文件不存在的正常首启，此时列表为空）；
 *         RT_FALSE LittleFS 挂载失败
 * @note   文件格式为每行 "A <hour> <min> <enabled>"，以 # 开头的行忽略。
 *         文件不存在或损坏均不报错，一律退化为空列表，不阻塞开机。
 */
rt_bool_t alarm_load(void)
{
    char buf[192];
    int fd;
    rt_ssize_t n;
    rt_size_t total = 0;
    rt_int32_t i = 0;
    char *cur;

    s_count = 0;
    memset(s_list, 0, sizeof(s_list));
    /* 此处不置 s_loaded：挂载失败时应允许上层稍后重试，
     * 只有成功读完（含文件不存在的正常首启）才置位。 */

    if (spi3_lfs_ensure_mounted() != RT_EOK)
    {
        rt_kprintf("[alarm] LittleFS 挂载失败，闹钟列表为空\n");
        return RT_FALSE;
    }

    fd = open(ALARM_PATH, O_RDONLY);
    if (fd < 0)
    {
        rt_kprintf("[alarm] 尚无 %s（首次开机）\n", ALARM_PATH);
        s_loaded = RT_TRUE;
        return RT_FALSE;
    }

    /* 必须整块读入后按 '\n' 切分，不能把一次 read 当作一行：
     * LittleFS(DFS) 的 read() 语义与 stdio 的 fgets 不同，逐行读会读不到内容
     * （文件里有 "A 7 30 1"，却报已载入 0 个）。settings.c 与 wallpaper_store.c
     * 均采用整块读加自行切行的写法，保持一致。
     *
     * 文件格式：每行 "A <hour> <min> <enabled>"。
     * 解析必须容错：坏行跳过继续处理，不因一行损坏作废整个文件，
     * 避免掉电写 flash 时后面的闹钟一起丢失。 */
    while ((n = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    {
        total += (rt_size_t)n;
        if (total >= sizeof(buf) - 1) break;        /* 文件异常大，截断 */
    }
    close(fd);
    buf[total] = '\0';

    for (cur = buf; cur && *cur; )
    {
        int h, m, e;
        char *nl = strchr(cur, (int)'\n');
        if (nl) *nl = '\0';

        if (*cur == 'A' && sscanf(cur, "A %d %d %d", &h, &m, &e) == 3 &&
            h >= 0 && h <= 23 && m >= 0 && m <= 59 && i < (rt_int32_t)ALARM_MAX)
        {
            s_list[i].hour    = (rt_uint8_t)h;
            s_list[i].min     = (rt_uint8_t)m;
            s_list[i].enabled = (e != 0) ? 1 : 0;
            i++;
        }

        if (!nl) break;
        cur = nl + 1;
    }

    s_count = (rt_uint8_t)i;
    s_loaded = RT_TRUE;
    rt_kprintf("[alarm] 已载入 %d 个闹钟（文件 %d 字节）\n", (int)s_count, (int)total);
    return RT_TRUE;
}

/**
 * @brief  将闹钟列表写回 flash
 * @return RT_TRUE 保存成功（列表为空时删除文件同样算成功）；RT_FALSE 失败
 * @note   采用临时文件写入后 rename 的方式，掉电不会留下截断文件。
 */
rt_bool_t alarm_save(void)
{
    char buf[ALARM_MAX * 24];
    rt_ssize_t n;
    int fd, len = 0, i;

    lock();

    if (spi3_lfs_ensure_mounted() != RT_EOK)
    {
        unlock();
        rt_kprintf("[alarm] LittleFS 未挂载，保存失败\n");
        return RT_FALSE;
    }

    /* 列表为空时删除文件而非写空文件：省一次 flash 写，
     * 也避免"有文件但无闹钟"的歧义状态。 */
    if (s_count == 0)
    {
        remove(ALARM_PATH);
        unlock();
        rt_kprintf("[alarm] 列表已清空，删除 %s\n", ALARM_PATH);
        return RT_TRUE;
    }

    remove(ALARM_TMP);
    fd = open(ALARM_TMP, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        unlock();
        rt_kprintf("[alarm] 打开 %s 失败 (err %d)\n", ALARM_TMP, fd);
        return RT_FALSE;
    }

    len = rt_snprintf(buf, sizeof(buf), "# NS800 alarms v1\n");
    for (i = 0; i < (int)s_count; i++)
    {
        len += rt_snprintf(buf + len, sizeof(buf) - len, "A %d %d %d\n",
                           (int)s_list[i].hour, (int)s_list[i].min,
                           (int)s_list[i].enabled);
    }

    /* len 是 rt_snprintf 的返回值（本应写入的长度），内容超出缓冲时
     * 会大于缓冲实际长度，此处显式钳位。ALARM_MAX=4 时内容约 100 字节，
     * 缓冲够用，但扩到 8 个闹钟时即会越界。 */
    if (len < 0)            len = 0;
    if (len > (int)sizeof(buf)) len = (int)sizeof(buf);

    n = write(fd, buf, (rt_size_t)len);
    close(fd);

    if (n != len)
    {
        rt_kprintf("[alarm] 写入不完整 (%d / %d)\n", (int)n, len);
        remove(ALARM_TMP);
        unlock();
        return RT_FALSE;
    }

    remove(ALARM_PATH);
    if (rename(ALARM_TMP, ALARM_PATH) < 0)
    {
        rt_kprintf("[alarm] rename 失败\n");
        unlock();
        return RT_FALSE;
    }

    unlock();
    rt_kprintf("[alarm] 已保存 %d 个闹钟\n", (int)s_count);
    return RT_TRUE;
}

/* ── 列表操作 ── */

/**
 * @brief  取一份闹钟列表快照
 * @param  out 输出缓冲，容量需容纳 ALARM_MAX 个元素；为 RT_NULL 时直接返回
 */
void alarm_get(alarm_item_t *out)
{
    if (out == RT_NULL) return;
    if (!s_loaded) alarm_load();
    lock();
    memcpy(out, s_list, sizeof(s_list));
    unlock();
}

/**
 * @brief  当前实际占用的槽位数（与开关状态无关）
 * @return 0..ALARM_MAX
 * @note   界面据此决定显示几行，不能靠 hour/min/enabled 是否非零来判断，
 *         否则 00:00 的合法闹钟会被误判成空槽。
 */
rt_uint8_t alarm_count(void)
{
    if (!s_loaded) alarm_load();
    lock();
    {
        rt_uint8_t n = s_count;
        unlock();
        return n;
    }
}

/**
 * @brief  追加一个闹钟
 * @param  hour 小时，越界时钳到 0..23
 * @param  min  分钟，越界时钳到 0..59
 * @return 槽位下标 0..ALARM_MAX-1；列表已满返回 -1
 * @note   新建默认 enabled=1。
 */
int alarm_add(rt_uint8_t hour, rt_uint8_t min)
{
    int idx;

    if (!s_loaded) alarm_load();

    lock();
    if (s_count >= ALARM_MAX)
    {
        unlock();
        rt_kprintf("[alarm] 已满 %d 个，加不了\n", ALARM_MAX);
        return -1;
    }

    /* 越界钳位：上层（msh 命令、UI 旋钮）都可能给出越界值，
     * 直接拒绝会让用户觉得操作无响应，钳位更符合直觉。 */
    if (hour > 23) hour = 23;
    if (min  > 59) min  = 59;

    idx = (int)s_count++;
    s_list[idx].hour        = hour;
    s_list[idx].min         = min;
    s_list[idx].enabled     = 1;      /* 新建默认开启 */
    s_list[idx].fired_today = 0;
    unlock();

    alarm_save();
    return idx;
}

/**
 * @brief  删除指定闹钟，其后的元素前移一位
 * @param  idx 槽位下标
 * @return RT_TRUE 删除成功；RT_FALSE 下标越界
 * @note   若删除的正是当前响铃项，立即停止响铃。
 */
rt_bool_t alarm_remove(rt_uint8_t idx)
{
    if (!s_loaded) alarm_load();

    lock();
    if (idx >= s_count) { unlock(); return RT_FALSE; }

    /* 删除的是当前响铃项，立即停止响铃 */
    if (s_ringing && s_last_idx == (rt_int32_t)idx)
    {
        beep_stop();
        s_ringing = RT_FALSE;
        s_last_idx = -1;
    }

    /* 后面的往前挪一格 */
    {
        int i;
        for (i = idx; i < (int)s_count - 1; i++)
        {
            s_list[i] = s_list[i + 1];
        }
        s_count--;
        s_list[s_count].hour = s_list[s_count].min = 0;
        s_list[s_count].enabled = s_list[s_count].fired_today = 0;
    }
    unlock();

    alarm_save();
    return RT_TRUE;
}

/**
 * @brief  切换指定闹钟的开关状态
 * @param  idx 槽位下标
 * @return RT_TRUE 成功；RT_FALSE 下标越界
 * @note   重新开启时清除 fired_today，使当天仍可再响。
 */
rt_bool_t alarm_toggle(rt_uint8_t idx)
{
    if (!s_loaded) alarm_load();

    lock();
    if (idx >= s_count) { unlock(); return RT_FALSE; }
    s_list[idx].enabled = s_list[idx].enabled ? 0 : 1;
    /* 重新开启时清 fired_today，让当天仍能触发 */
    if (s_list[idx].enabled) s_list[idx].fired_today = 0;
    unlock();

    alarm_save();
    return RT_TRUE;
}

/**
 * @brief  修改指定闹钟的时间
 * @param  idx  槽位下标
 * @param  hour 小时 0..23，越界返回 RT_FALSE
 * @param  min  分钟 0..59，越界返回 RT_FALSE
 * @return RT_TRUE 成功；RT_FALSE 参数越界或下标越界
 * @note   只改 RAM 不落盘，供旋钮连续调节使用；调用方事后必须自行调用
 *         alarm_save()。每转一格就写 flash 会因 LittleFS 的 erase+write
 *         耗时数十毫秒而阻塞 LVGL 线程，并加快 flash 磨损。
 */
rt_bool_t alarm_set_time(rt_uint8_t idx, rt_uint8_t hour, rt_uint8_t min)
{
    if (hour > 23 || min > 59) return RT_FALSE;
    if (!s_loaded) alarm_load();

    lock();
    if (idx >= s_count) { unlock(); return RT_FALSE; }

    if (s_list[idx].hour == hour && s_list[idx].min == min)
    {
        unlock();
        return RT_TRUE;         /* 无变化，不置脏标志 */
    }

    s_list[idx].hour = hour;
    s_list[idx].min  = min;

    /* 必须清除 fired_today：否则"今天已响过"的标记会落在新时间点上，
     * 把闹钟从 07:30 改到 09:00 后当天不再触发。 */
    s_list[idx].fired_today = 0;
    unlock();

    return RT_TRUE;
}

/**
 * @brief  当前处于开启状态的闹钟数量
 * @return 0..s_count
 */
rt_uint8_t alarm_active_count(void)
{
    rt_uint8_t n = 0;
    int i;

    if (!s_loaded) alarm_load();

    lock();
    for (i = 0; i < (int)s_count; i++)
    {
        if (s_list[i].enabled) n++;
    }
    unlock();
    return n;
}

/* ── 到点判定 ── */

/**
 * @brief  检查是否有闹钟到点，到点则启动响铃
 * @param  now_unix 当前本地时间戳（UTC+8），小于 0 表示未对时，直接返回 0
 * @return 本次触发返回 1，否则 0
 * @note   去重依赖 fired_today：只比较时和分时，同一分钟内每次调用都会
 *         重复触发，响铃会被反复重启。每个闹钟每天只触发一次，日序号
 *         变化时统一清除 fired_today。
 * @note   响铃由 beep() 包负责，其内部为单静态线程，本函数同一时刻只能
 *         被调用一次；beep() 是阻塞调用，必须在解锁之后执行。
 */
int alarm_check(rt_int32_t now_unix)
{
    rt_int32_t day;
    rt_uint8_t h, m;
    int i, ret = 0;

    if (now_unix < 0) return 0;      /* 未对时，无从判定 */
    if (!s_loaded) alarm_load();

    split_time(now_unix, &day, &h, &m);

    lock();

    /* 跨天检测：日序号发生变化时清除全部 fired_today。
     * 日序号在一整天内保持不变，同一日内多次调用不会重复清零。 */
    if (s_last_day >= 0 && day != s_last_day)
    {
        for (i = 0; i < (int)s_count; i++) s_list[i].fired_today = 0;
    }
    s_last_day = day;

    for (i = 0; i < (int)s_count; i++)
    {
        if (!s_list[i].enabled)        continue;
        if (s_list[i].fired_today)      continue;
        if (s_list[i].hour != h)       continue;
        if (s_list[i].min  != m)       continue;

        s_list[i].fired_today = 1;
        s_last_idx = i;
        ret = 1;
        break;      /* 同一分钟多个闹钟只响一次，报第一个 */
    }

    if (ret)
    {
        s_ringing = RT_TRUE;
        rt_kprintf("[alarm] ⏰ 到点！%02d:%02d 闹钟 %d 开始响铃\n",
                   (int)h, (int)m, s_last_idx);
        /* beep 包为单静态线程，一次只能排一个序列。这里用连响 10 次、
         * 每次 200 ms 作为闹钟节奏，与蜂鸣器 App 的三档测试音区分。
         * beep 是阻塞式调用，必须放在解锁之后，且作为最后一句执行。 */
        unlock();
        beep(10, 200, 50, 0);
        return 1;
    }

    unlock();
    return 0;
}

/**
 * @brief  停止响铃（用户按 KEY 确认）
 * @return RT_TRUE 此前正在响铃（本次确实停止）；RT_FALSE 本来未响
 */
rt_bool_t alarm_stop(void)
{
    rt_bool_t was;

    lock();
    was = s_ringing;
    if (was)
    {
        beep_stop();
        s_ringing = RT_FALSE;
    }
    unlock();

    if (was) rt_kprintf("[alarm] 已停止响铃\n");
    return was;
}

/**
 * @brief  查询当前是否正在响铃
 * @return RT_TRUE 正在响铃
 */
rt_bool_t alarm_is_ringing(void)
{
    return s_ringing;
}

/**
 * @brief  取上次响铃的闹钟下标
 * @return 槽位下标；无有效值时返回 -1
 */
int alarm_last_index(void)
{
    return (int)s_last_idx;
}

/* ── msh 命令 ── */

/* alist —— 列出全部闹钟。用法: alist */
static void alist(void)
{
    rt_int32_t i;

    if (!s_loaded) alarm_load();

    if (s_count == 0)
    {
        rt_kprintf("[alarm] 还没有闹钟\n");
        return;
    }
    for (i = 0; i < (int)s_count; i++)
    {
        rt_kprintf("[alarm] %d: %02d:%02d  %s%s\n", (int)i,
                   (int)s_list[i].hour, (int)s_list[i].min,
                   s_list[i].enabled ? "开" : "关",
                   s_ringing && s_last_idx == i ? "  ←正在响" : "");
    }
}
MSH_CMD_EXPORT(alist, 查看闹钟列表);

/* aadd —— 新增闹钟。用法: aadd <时> <分>  例: aadd 7 30 */
static int aadd(int argc, char **argv)
{
    int idx;

    if (argc < 3)
    {
        rt_kprintf("用法: aadd <时> <分>   例: aadd 7 30\n");
        return 1;
    }
    idx = alarm_add((rt_uint8_t)atoi(argv[1]), (rt_uint8_t)atoi(argv[2]));
    if (idx < 0) rt_kprintf("添加失败（列表已满 %d 个）\n", ALARM_MAX);
    else         rt_kprintf("已添加 %02d:%02d，槽位 %d\n",
                            atoi(argv[1]) % 24, atoi(argv[2]) % 60, idx);
    return (idx < 0) ? 1 : 0;
}
MSH_CMD_EXPORT(aadd, 添加闹钟: aadd <时> <分>);

/* adel —— 删除指定序号的闹钟。用法: adel <序号> */
static int adel(int argc, char **argv)
{
    if (argc < 2) { rt_kprintf("用法: adel <序号>\n"); return 1; }
    if (alarm_remove((rt_uint8_t)atoi(argv[1])))
        rt_kprintf("已删除 %s\n", argv[1]);
    else
        rt_kprintf("删除失败（序号越界）\n");
    return 0;
}
MSH_CMD_EXPORT(adel, 删除闹钟: adel <序号>);

/* acheck —— 立即按当前时间执行一次到点判定，用于自检。用法: acheck
 * 命令名不能用 atop：beep 包已导出同名命令，msh 命令表重名时行为不确定。 */
static int acheck(int argc, char **argv)
{
    rt_int32_t ts;
    c3_link_state_t c3;

    (void)argc; (void)argv;
    c3_link_get_state(&c3);
    if (!c3.time_valid) { rt_kprintf("[alarm] 还没对时，无法自检\n"); return 1; }

    ts = c3.time_unix +
         (rt_int32_t)((rt_tick_get() - c3.time_tick) / RT_TICK_PER_SECOND);
    rt_kprintf("[alarm] 自检用时间 %02d:%02d\n",
               (int)((ts % 86400) / 3600), (int)((ts % 3600) / 60));
    rt_kprintf("[alarm] 结果: %s\n", alarm_check(ts) ? "触发了" : "没触发");
    return 0;
}
MSH_CMD_EXPORT(acheck, 立刻按当前时间跑一次到点判定);

/* aon —— 开关指定闹钟。用法: aon <序号> [0|1]；省略第二个参数则取反 */
static int aon(int argc, char **argv)
{
    if (argc < 2) { rt_kprintf("用法: aon <序号> [0|1]\n"); return 1; }

    rt_int32_t idx = atoi(argv[1]);
    if (!s_loaded) alarm_load();
    if (idx < 0 || idx >= (rt_int32_t)s_count)
    { rt_kprintf("[alarm] 序号 %d 越界（共 %d 个）\n", (int)idx, (int)s_count); return 1; }

    if (argc >= 3)
    {
        rt_bool_t want = (argv[2][0] == '1' || argv[2][0] == 'o' || argv[2][0] == 'O');
        if (want != s_list[idx].enabled) alarm_toggle((rt_uint8_t)idx);
    }
    else
    {
        alarm_toggle((rt_uint8_t)idx);
    }
    rt_kprintf("[alarm] %d: %02d:%02d  %s\n", (int)idx,
               (int)s_list[idx].hour, (int)s_list[idx].min,
               s_list[idx].enabled ? "开" : "关");
    return 0;
}
MSH_CMD_EXPORT(aon, 开关闹钟: aon <序号> [0|1]);

/**
 * @brief  初始化互斥量并载入闹钟列表
 * @return RT_EOK
 * @note   必须在 ui_init() 中早于 App 表构建调用：闹钟 App 的 build()
 *         会读取列表，不能撞上未初始化的状态。互斥量创建失败时不加锁运行。
 */
int alarm_init(void)
{
    if (!s_lock_ready)
    {
        if (rt_mutex_init(&s_lock, "alm", RT_IPC_FLAG_PRIO) == RT_EOK)
        {
            s_lock_ready = RT_TRUE;
        }
        else
        {
            rt_kprintf("[alarm] 互斥量初始化失败（将无锁运行）\n");
        }
    }
    alarm_load();
    return RT_EOK;
}
