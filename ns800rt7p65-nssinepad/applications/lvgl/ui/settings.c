/**
 * @file    settings.c
 * @brief   板级配置持久化（LittleFS）
 *
 * 把调色台调出的 gamma 参数、背光亮度、偏好等存入外置 flash，开机自动
 * 恢复。设计要点：
 *  1. 单文件文本、每行 "key value"，解析器只认已知 key，忽略其余行。
 *     掉电最坏情况是文件损坏，退回出厂默认值，不会解析出离谱数值。
 *  2. 写入采用临时文件加 rename。LittleFS 的 rename 是原子的，
 *     断电不会留下半个文件；直接 open/truncate/write 会留下截断文件。
 *  3. 读入的值做范围钳位，文件被改坏时不会把 gamma 设成极端值把屏幕刷花。
 *
 * 文件位置：/cfg.txt（外置 flash 的 LittleFS，SPI3）。
 */

#include <rtthread.h>
#include <dfs.h>
#include <fcntl.h>
#include <unistd.h>

#include "settings.h"
#include "spi3_flash.h"

#define CFG_PATH   "/cfg.txt"
#define CFG_TMP    "/cfg.tmp"

/* ── 出厂默认值 ──
 * 调色台三项取值 -100..+100，背光亮度 0..100。 */
static rt_settings_t g_default =
{
    /* gamma 冷暖：-100..+100。
     * 默认必须为 0：0 表示完全沿用 lcd_ili9341.c 的出厂曲线，
     * 这样"恢复默认"即回到驱动原值，而非另设一个经验值。 */
    .tune_warm   = 0,
    /* gamma 明暗 */
    .tune_bright = 0,
    /* 饱和度 */
    .tune_sat    = 0,
    /* 背光 0..100 */
    .backlight    = 80,
    /* 当前页（桌面分页） */
    .home_page    = 0,
    /* 跑酷游戏最高分 */
    .game_best    = 0,
    /* 累计运行秒数（历史，本次开机不计） */
    .uptime_saved = 0,
};

static rt_settings_t g_cfg;
static rt_bool_t     g_loaded = RT_FALSE;
/* 载入后是否被用户改动过。开机流程中 home_page_show 会调用 update_one，
 * 若不阻挡会形成"读出又写回"的循环，用出厂默认值覆盖用户已存的值。 */
static rt_bool_t     g_dirty = RT_FALSE;

/* 上次落盘时"本次已运行的秒数"，累计运行时长记账的基线。
 * 在此处声明：settings_set() 也要使用它，而该函数在文件中排得比
 * 累计运行时长一节更靠前。 */
static rt_int32_t    g_flush_now_s = 0;

/* ── 读写单个整数项 ──
 * 找不到返回 def；解析失败（非数字或超范围）也返回 def。 */
static rt_int32_t cfg_get(const char *line, const char *key,
                          rt_int32_t def, rt_int32_t lo, rt_int32_t hi)
{
    /* key 必须从行首匹配（只允许行首空白），否则 "sat" 会匹配到
     * "tune_sat" 这类子串。 */
    const char *p = line;

    while (*p == ' ' || *p == '\t') p++;
    if (rt_strncmp(p, key, rt_strlen(key)) != 0)
        return def;

    /* 跳过 key 及可能存在的 ':' / '=' / 空白 */
    p += rt_strlen(key);
    while (*p == ':' || *p == '=' || *p == ' ' || *p == '\t') p++;

    /* 此处不能预检查并吃掉负号：下面的 neg 分支还要读那个 '-'。
     * 若在此处消耗负号，-55 会被读成 +55。 */
    if (*p != '-' && (*p < '0' || *p > '9'))
        return def;

    {
        rt_int32_t v = 0;
        rt_bool_t neg = RT_FALSE;

        if (*p == '-') { neg = RT_TRUE; p++; }
        while (*p >= '0' && *p <= '9')
        {
            v = v * 10 + (*p - '0');
            p++;
            /* 防溢出：上限放到 20 亿（约 2^31），因为要存累计运行秒数。
             * 原先为 100000（约 27 小时），累计时长超过后被截断。
             * 其余项的合理范围由下面 lo/hi 钳制保证，这里只挡溢出。 */
            if (v > 2000000000) break;
        }
        if (neg) v = -v;

        /* 钳到合法范围，文件被改坏时的保护 */
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        return v;
    }
}

/* ── 从文件加载 ──
 * 返回 RT_TRUE 表示成功读到并解析了文件。 */
/**
 * @brief  从 flash 载入配置
 * @return RT_TRUE 成功读到并解析了文件；RT_FALSE 挂载失败或文件不存在
 * @note   失败或缺失的项保持出厂默认值，仍会置已载入标志。
 */
rt_bool_t settings_load(void)
{
    rt_ssize_t n;
    int fd;
    rt_settings_t c = g_default;      /* 先铺默认值，未改到的项保持默认 */

    g_cfg = g_default;                /* 失败时至少保证不是未初始化 */

    if (spi3_lfs_ensure_mounted() != RT_EOK)
    {
        rt_kprintf("[cfg] LittleFS 挂载失败，用出厂默认值\n");
        return RT_FALSE;
    }

    fd = open(CFG_PATH, O_RDONLY);
    if (fd < 0)
    {
        rt_kprintf("[cfg] 尚无 %s（首次开机），用出厂默认值\n", CFG_PATH);
        g_cfg = g_default;
        g_loaded = RT_TRUE;
        return RT_FALSE;
    }

    /* 必须按行解析，不能一次 read 进来当整行：那样 strstr("warm") 只在
     * 第一行命中，后面的 key 全部漏掉。此处按 '\n' 切分逐行交给 cfg_get。
     * 缓冲 192 字节对本文件（约 56 字节）足够，另有余量。 */
    {
        char buf[192];
        rt_size_t total = 0;

        while ((n = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
        {
            total += (rt_size_t)n;
            if (total >= sizeof(buf) - 1) break;      /* 文件异常大，截断 */
        }
        close(fd);
        buf[total] = '\0';

        {
            char *cur = buf;
            while (cur && *cur)
            {
                char *nl = strchr(cur, (int)'\n');
                if (nl) *nl = '\0';

                if (*cur != '#' && *cur != '\r')
                {
                    c.tune_warm   = cfg_get(cur, "warm",   c.tune_warm,   -100, 100);
                    c.tune_bright = cfg_get(cur, "bright", c.tune_bright, -100, 100);
                    c.tune_sat    = cfg_get(cur, "sat",    c.tune_sat,    -100, 100);
                    c.backlight   = cfg_get(cur, "bl",     c.backlight,     0, 100);
                    c.home_page   = cfg_get(cur, "page",   c.home_page,     0,   1);
                    /* 游戏最高分：0..99999（5 位显示上限） */
                    c.game_best   = cfg_get(cur, "gbest",  c.game_best,     0, 99999);
                    /* 累计运行秒数：0..20 亿（≈68 年，实际用不完） */
                    c.uptime_saved = cfg_get(cur, "uptime", c.uptime_saved, 0, 2000000000);
                }

                cur = nl ? nl + 1 : RT_NULL;
            }
        }
    }

    g_cfg = c;
    g_loaded = RT_TRUE;
    g_dirty = RT_FALSE;              /* 刚载入，还不算用户改过 */
    rt_kprintf("[cfg] 已从 %s 载入 warm=%d bright=%d sat=%d bl=%d page=%d gbest=%d uptime=%ds\n",
               CFG_PATH, c.tune_warm, c.tune_bright, c.tune_sat,
               c.backlight, c.home_page, c.game_best, (int)c.uptime_saved);
    return RT_TRUE;
}

/* ── 保存到文件 ── */

/**
 * @brief  将当前配置写回 flash
 * @return RT_TRUE 保存成功；RT_FALSE 挂载失败或写入不完整
 * @note   先写临时文件再 rename；LittleFS 的 rename 原子，断电安全。
 */
rt_bool_t settings_save(void)
{
    char buf[192];
    rt_ssize_t n;
    int fd;
    int len;

    if (spi3_lfs_ensure_mounted() != RT_EOK)
    {
        rt_kprintf("[cfg] LittleFS 未挂载，保存失败\n");
        return RT_FALSE;
    }

    /* 先删旧的临时文件：rename 在部分实现里目标已存在会失败 */
    remove(CFG_TMP);

    fd = open(CFG_TMP, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        rt_kprintf("[cfg] 打开 %s 失败 (err %d)\n", CFG_TMP, fd);
        return RT_FALSE;
    }

    len = rt_snprintf(buf, sizeof(buf),
                      "# NS800 config v1\n"
                      "warm %d\n"
                      "bright %d\n"
                      "sat %d\n"
                      "bl %d\n"
                      "page %d\n"
                      "gbest %d\n"
                      "uptime %d\n",
                      g_cfg.tune_warm, g_cfg.tune_bright, g_cfg.tune_sat,
                      g_cfg.backlight, g_cfg.home_page, g_cfg.game_best,
                      (int)g_cfg.uptime_saved);

    /* 性能诊断：LittleFS 的 erase+write 耗时数十毫秒，同步调用会阻塞
     * LVGL 线程，表现为翻页动画卡顿。打印耗时便于判断是否需要延后落盘。 */
    {
        rt_int32_t t0 = (rt_int32_t)rt_tick_get();
        n = write(fd, buf, (rt_size_t)len);
        close(fd);
        rt_kprintf("[cfg] flash 写入耗时 %d ms\r\n", (rt_int32_t)rt_tick_get() - t0);
    }

    if (n != len)
    {
        rt_kprintf("[cfg] 写入不完整 (%d / %d)\n", (int)n, len);
        remove(CFG_TMP);
        return RT_FALSE;
    }

    /* 原子替换 */
    remove(CFG_PATH);
    if (rename(CFG_TMP, CFG_PATH) < 0)
    {
        rt_kprintf("[cfg] rename 失败\n");
        return RT_FALSE;
    }

    rt_kprintf("[cfg] 已保存 warm=%d bright=%d sat=%d bl=%d page=%d gbest=%d uptime=%ds\n",
               g_cfg.tune_warm, g_cfg.tune_bright, g_cfg.tune_sat,
               g_cfg.backlight, g_cfg.home_page, g_cfg.game_best,
               (int)g_cfg.uptime_saved);
    return RT_TRUE;
}

/* ── 访问接口 ── */

/**
 * @brief  取当前配置快照
 * @param  out 输出缓冲；为 RT_NULL 时不写入
 */
void settings_get(rt_settings_t *out)
{
    if (!g_loaded) settings_load();
    if (out) *out = g_cfg;
}

/**
 * @brief  整体写入配置（只改内存，随后需调用 settings_save 落盘）
 * @param  in 输入配置；为 RT_NULL 时按出厂默认值处理
 * @note   内部对所有项统一钳位，防止外部调用传入越界值。
 *         该接口会把 uptime_saved 的记账基线清零，见下方注释。
 */
void settings_set(const rt_settings_t *in)
{
    rt_settings_t c = g_default;

    if (!g_loaded) settings_load();
    if (in)
    {
        c.tune_warm   = in->tune_warm;
        c.tune_bright = in->tune_bright;
        c.tune_sat    = in->tune_sat;
        c.backlight    = in->backlight;
        c.home_page    = in->home_page;
        c.game_best    = in->game_best;
        c.uptime_saved = in->uptime_saved;
    }

    /* 统一钳位（调色台滑块本身限了 -100..100，这里防外部调用越界） */
    if (c.tune_warm   >  100) c.tune_warm   =  100;
    if (c.tune_warm   < -100) c.tune_warm   = -100;
    if (c.tune_bright >  100) c.tune_bright =  100;
    if (c.tune_bright < -100) c.tune_bright = -100;
    if (c.tune_sat    >  100) c.tune_sat    =  100;
    if (c.tune_sat    < -100) c.tune_sat    = -100;
    if (c.backlight    >  100) c.backlight    =  100;
    if (c.backlight    <    0) c.backlight    =    0;
    if (c.home_page    >    1) c.home_page    =    1;
    if (c.home_page    <    0) c.home_page    =    0;
    if (c.game_best    > 99999) c.game_best    = 99999;
    if (c.game_best    <    0) c.game_best    =    0;
    if (c.uptime_saved <    0) c.uptime_saved =    0;
    /* 上限开放：累计运行时长可以很大（旧值 100000 约 28 小时即被截断） */

    /* uptime_saved 被外部改动时（cfgset u、恢复出厂、载入），必须把"已计入"
     * 的基线清零，否则 settings_uptime_total() 会用旧基线相减，得出负数或跳变。 */
    g_flush_now_s = 0;

    g_cfg = c;
    g_dirty = RT_TRUE;          /* 用户主动改过 */
}

/**
 * @brief  修改单项配置并立即落盘，供调色台拖动滑块时使用
 * @param  which 项标识：'w' 冷暖 'b' 明暗 's' 饱和 'l' 背光 'p' 页号
 *               'g' 游戏最高分 'u' 累计运行秒数
 * @param  val   新值
 * @return RT_TRUE 成功；RT_FALSE which 非法或写盘失败
 */
rt_bool_t settings_update_one(char which, rt_int32_t val)
{
    rt_settings_t c;

    settings_get(&c);

    switch (which)
    {
    case 'w': c.tune_warm   = val; break;
    case 'b': c.tune_bright = val; break;
    case 's': c.tune_sat    = val; break;
    case 'l': c.backlight    = val; break;
    case 'p': c.home_page    = val; break;
    case 'g': c.game_best    = val; break;
    /* 'u' 为累计运行秒数，主要用于清零（cfgset u 0）：累计时长若被异常数据
     * 污染成离谱的大数，屏上会一直显示假值，需要有归零的途径。
     * settings_set() 会把记账基线一并清零。 */
    case 'u': c.uptime_saved = (val < 0) ? 0 : val; break;
    default:  return RT_FALSE;
    }

    settings_set(&c);
    return settings_save();
}

/**
 * @brief  查询配置是否已载入
 * @return RT_TRUE 已从 flash 载入过
 */
rt_bool_t settings_is_loaded(void) { return g_loaded; }

/**
 * @brief  查询配置载入后是否被用户改动过
 * @return RT_TRUE 被用户改动过
 */
rt_bool_t settings_is_dirty(void) { return g_dirty; }

/* ── 累计运行时长（断电保存）──
 *
 * 记账只此一处：g_flush_now_s 为上次落盘时"本次已运行的秒数"，
 * 当前总时长 = uptime_saved + (now_s - g_flush_now_s)。
 * 早期由各调用点自行计算 total = uptime_saved + now_s 再写回，写回后
 * uptime_saved 已含 now_s，下次再加即重复累加，误差随调用次数平方增长；
 * 当时 ui.c 与 ui_apps.c 各持一份静态基线，进一步放大偏差。现统一入口：
 * 外部只读 settings_uptime_total()、只写 settings_uptime_flush()。
 *
 * 其他要点：
 *  1. 不经过 settings_set()：那条路径会置 g_dirty（视为用户改动），
 *     而周期落盘属于系统行为，会干扰开机流程中"刚载入不写回"的判断。
 *  2. 只在数值增长到一定程度才真写：flash 有擦写寿命，每秒写一次难以承受，
 *     由 settings_uptime_flush() 按 UPTIME_SAVE_PERIOD_S 节流。
 *  3. 只修改 uptime_saved 一个字段，不触碰其它配置项，
 *     避免把内存中可能过期的其它值一并写回。
 *
 * g_flush_now_s 已在文件顶部（g_dirty 附近）声明。
 */

/**
 * @brief  取历史累计与本次开机之和，即当前总运行秒数
 * @return 当前累计总秒数
 * @note   显示与上报统一使用本函数，不要自行累加。
 */
rt_int32_t settings_uptime_total(void)
{
    rt_int32_t now_s;

    if (!g_loaded) settings_load();
    now_s = (rt_int32_t)(rt_tick_get() / RT_TICK_PER_SECOND);

    {
        rt_int32_t d = now_s - g_flush_now_s;
        /* 保险：外部改过 uptime_saved（如 cfgset u 0）而基线还没同步时可能为负 */
        if (d < 0) d = 0;
        return g_cfg.uptime_saved + d;
    }
}

/**
 * @brief  取历史累计运行秒数（不含本次开机）
 * @return uptime_saved 字段值
 * @note   仅用于展示历史与本次的拆分。
 */
rt_int32_t settings_uptime_saved(void)
{
    if (!g_loaded) settings_load();
    return g_cfg.uptime_saved;
}

/**
 * @brief  周期落盘：距上次落盘满 UPTIME_SAVE_PERIOD_S 秒才真写一次
 * @return RT_TRUE 已写入或无需写入；RT_FALSE 写失败，下个周期重试即可
 * @note   内部自带节流与单一记账，可在任意位置以任意频率调用；ui.c 的
 *         常驻定时器与 App 页的 tick 同时调用也不会互相干扰。
 */
rt_bool_t settings_uptime_flush(void)
{
    rt_int32_t now_s, total;

    if (!g_loaded) settings_load();
    now_s = (rt_int32_t)(rt_tick_get() / RT_TICK_PER_SECOND);

    /* 节流：距上次落盘还不够久 */
    if ((now_s - g_flush_now_s) < UPTIME_SAVE_PERIOD_S)
        return RT_TRUE;

    total = g_cfg.uptime_saved + (now_s - g_flush_now_s);
    if (settings_uptime_store(total))
    {
        g_flush_now_s = now_s;      /* 记录这几秒已计入，下次不再重复累加 */
        return RT_TRUE;
    }
    return RT_FALSE;                /* 写失败：g_flush_now_s 不动，下周期重试 */
}

/**
 * @brief  把累计总秒数写回 flash
 * @param  total_s 要保存的累计总秒数（历史加本次）
 * @return RT_TRUE 已写入或无需写入；RT_FALSE 写失败
 * @note   数值未增长（含时钟回拨）时不写，减少 flash 擦写。
 *         一般不要直接调用本函数，应走 settings_uptime_flush()，后者
 *         会同步更新记账基线，直接调用会导致重复累加。
 */
rt_bool_t settings_uptime_store(rt_int32_t total_s)
{
    int fd, len;
    char buf[192];

    if (!g_loaded) settings_load();

    if (total_s < 0) total_s = 0;
    /* 无变化（或变小，例如时钟回拨）时不写，节省 flash */
    if (total_s <= g_cfg.uptime_saved)
        return RT_TRUE;

    g_cfg.uptime_saved = total_s;

    if (spi3_lfs_ensure_mounted() != RT_EOK)
        return RT_FALSE;

    remove(CFG_TMP);
    fd = open(CFG_TMP, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        rt_kprintf("[cfg] uptime 保存: 打开 %s 失败\n", CFG_TMP);
        return RT_FALSE;
    }

    /* 与 settings_save() 保持完全相同的格式，否则下次 load 会解析错 */
    len = rt_snprintf(buf, sizeof(buf),
                      "# NS800 config v1\n"
                      "warm %d\n"
                      "bright %d\n"
                      "sat %d\n"
                      "bl %d\n"
                      "page %d\n"
                      "gbest %d\n"
                      "uptime %d\n",
                      g_cfg.tune_warm, g_cfg.tune_bright, g_cfg.tune_sat,
                      g_cfg.backlight, g_cfg.home_page, g_cfg.game_best,
                      (int)g_cfg.uptime_saved);

    if (write(fd, buf, (rt_size_t)len) != len)
    {
        close(fd);
        remove(CFG_TMP);
        return RT_FALSE;
    }
    close(fd);

    remove(CFG_PATH);
    if (rename(CFG_TMP, CFG_PATH) < 0)
        return RT_FALSE;

    return RT_TRUE;
}

/* ── msh 调试命令 ── */

/* cfg —— 显示当前配置与运行时长。用法: cfg */
static void cfg(int argc, char **argv)
{
    rt_settings_t c;

    settings_get(&c);
    rt_kprintf("[cfg] 已加载=%d  文件=%s\n", g_loaded, CFG_PATH);
    rt_kprintf("      warm   = %d   (-100 冷 ~ +100 暖)\n", c.tune_warm);
    rt_kprintf("      bright = %d   (-100 暗 ~ +100 亮)\n", c.tune_bright);
    rt_kprintf("      sat    = %d   (-100灰 ~ +100 艳)\n", c.tune_sat);
    rt_kprintf("      bl= %d   (背光 0..100)\n", c.backlight);
    rt_kprintf("      page   = %d   (桌面页号)\n", c.home_page);
    rt_kprintf("      gbest  = %d   (跑酷最高分)\n", c.game_best);
    {
        /* 使用 settings_uptime_total()，不要自行累加 uptime 与本次时间，
         * 否则会重复计数（见本文件累计运行时长一节）。 */
        rt_int32_t tot = settings_uptime_total();
        rt_int32_t hist = settings_uptime_saved();
        rt_int32_t now  = settings_uptime_now_s();
        rt_kprintf("      uptime = %d 秒 = 历史 %d 秒 + 本次 %d 秒\n",
                   (int)tot, (int)hist, (int)now);
        rt_kprintf("        累计 %d 天 %d:%02d:%02d | 历史 %d:%02d:%02d | 本次 %d:%02d:%02d\n",
                   (int)(tot / 86400),   (int)((tot % 86400) / 3600),
                   (int)((tot % 3600) / 60), (int)(tot % 60),
                   (int)(hist / 3600),  (int)((hist / 60) % 60), (int)(hist % 60),
                   (int)(now / 3600),   (int)((now / 60) % 60),  (int)(now % 60));
    }
}
MSH_CMD_EXPORT(cfg, show or reload saved config);

/* cfgsave —— 将当前配置写入 flash。用法: cfgsave */
static void cfgsave(int argc, char **argv)
{
    settings_save();
}
MSH_CMD_EXPORT(cfgsave, write config to flash);

/* cfgreload —— 从 flash 重新读取配置。用法: cfgreload */
static void cfgreload(int argc, char **argv)
{
    g_loaded = RT_FALSE;
    settings_load();
}
MSH_CMD_EXPORT(cfgreload, re-read config from flash);

/* cfgset —— 修改一项配置并立即落盘。用法: cfgset <w|b|s|l|p|g|u> <值> */
static void cfgset(int argc, char **argv)
{
    if (argc != 3)
    {
        rt_kprintf("用法: cfgset <w|b|s|l|p|g|u> <值>\r\n");
        rt_kprintf("  w=冷暖 b=明暗 s=饱和 l=背光 p=页号 g=游戏最高分\r\n");
        rt_kprintf("  u=累计运行秒数（写 0 = 清零累计时长）\r\n");
        return;
    }
    if (!settings_update_one(argv[1][0], atoi(argv[2])))
        rt_kprintf("[cfg] 写入失败（键只能是 w/b/s/l/p/g/u）\r\n");
}
MSH_CMD_EXPORT(cfgset, set and save one config item);

/* cfgdefault —— 恢复出厂默认并写入 flash。用法: cfgdefault */
static void cfgdefault(int argc, char **argv)
{
    g_cfg = g_default;
    settings_save();
    rt_kprintf("[cfg] 已恢复出厂默认并写入 flash\n");
}
MSH_CMD_EXPORT(cfgdefault, restore factory defaults);