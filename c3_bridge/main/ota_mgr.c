/**
 * @file    ota_mgr.c
 * @brief   C3 联网桥 OTA 升级实现（见 ota_mgr.h 的分区布局与设计说明）
 *
 * 提供两条升级路径，共用同一套状态变量与 NS800 进度回报：
 *  - HTTP 拉取：ota_mgr_start() 起独立任务，按 URL 分块下载并写 OTA 槽，
 *    完成后切换启动槽并重启。
 *  - 网页直传：ota_mgr_upload_begin / _chunk / _end，httpd 侧收包、
 *    独立任务写 flash，RAM 占用恒定 4 KB。
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"     /* esp_task_wdt_reset()：擦/写 flash 期间必须周期调用 */
#include "esp_http_client.h"
#include "esp_timer.h"

#include "ota_mgr.h"
#include "link_ns800.h"      /* ns800_link_send / ns800_link_send_text */
#include "ns800_link.h"      /* 命令码 / 载荷格式 */
#include "wifi_mgr.h"        /* wifi_mgr_is_online / wifi_mgr_wait_ip / wifi_mgr_ip */

static const char *TAG = "ota";

/* 分块大小：HTTP 收到多少就往 flash 写多少。
 * 4 KB 是 flash 扇区的整数倍，且够大，避免块间开销占比过高。 */
#define OTA_CHUNK       4096

/* 单次 HTTP 读超时（ms）。下载中卡住不能无限等待。 */
#define OTA_HTTP_TIMEOUT_MS  15000

/* 状态：模块单实例，全局共享 */

static volatile ota_state_t s_state = OTA_ST_IDLE;
static volatile int         s_pct   = 0;
static const char          *s_err   = "";
static SemaphoreHandle_t    s_lock;      /* 保护 s_state/s_pct/s_err */

/* 进度回报节流：仅在整数百分比变化时推 NS800，避免刷爆串口 */
static volatile int         s_last_reported_pct = -1;

/* ------------------------------------------------------------------ */

/**
 * @brief  更新 OTA 状态、进度与失败原因
 * @param  st   新状态
 * @param  pct  进度百分比 0..100
 * @param  err  失败原因字符串，可为 NULL（转为空串）
 * @note   取不到锁时直接写：三个变量本身为原子访问，不阻塞升级流程。
 */
static void set_state(ota_state_t st, int pct, const char *err)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        s_state = st;
        s_pct   = pct;
        s_err   = err ? err : "";
        xSemaphoreGive(s_lock);
    }
    else
    {
        /* 拿不到锁也不阻塞升级：直接写，三变量本身为原子访问 */
        s_state = st;
        s_pct   = pct;
        s_err   = err ? err : "";
    }
}

/**
 * @brief  向 NS800 回报进度（节流：仅在整数百分比变化时发送）
 * @param  pct  进度百分比，入参会被钳到 0..100
 */
static void report_progress(int pct)
{
    uint8_t pl[2];

    if (pct == s_last_reported_pct)
        return;
    s_last_reported_pct = pct;

    /* 载荷含义见 ns800_link.h 的 NS800_CMD_OTA_PROGRESS 注释：
     *   [0] = 0xFF 表示"进度/状态更新"，[1] = 0..100
     * 这里只发进度，状态码用 0x00（进行中）。 */
    pl[0] = 0x00;
    pl[1] = (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
    ns800_link_send(NS800_CMD_OTA_PROGRESS, pl, 2);
}

/**
 * @brief  向 NS800 回报最终结果（成功或失败）
 * @param  ok       true = 成功，false = 失败
 * @param  reason   失败原因，成功时传 NULL
 * @note   失败时另发一条 TEXT 帧，屏幕可直接显示原因（限长 64 字节）。
 */
static void report_result(bool ok, const char *reason)
{
    uint8_t pl[2];

    pl[0] = ok ? 0x01 : 0x02;      /* 1 = 成功，2 = 失败 */
    pl[1] = (uint8_t)(ok ? 100 : s_pct);
    ns800_link_send(NS800_CMD_OTA_PROGRESS, pl, 2);

    if (!ok && reason)
    {
        /* 失败原因另发 TEXT 帧，屏上可直接显示（限长，防越界） */
        char msg[64];
        snprintf(msg, sizeof(msg), "OTA 失败: %s", reason);
        ns800_link_send_text(NS800_CMD_TEXT, msg);
    }
    else if (ok)
    {
        ns800_link_send_text(NS800_CMD_TEXT, "OTA 完成，即将重启");
    }
}

/* ------------------------------------------------------------------ */

/**
 * @brief  HTTP 事件回调，用于从响应头抓取 Content-Length
 * @param  evt  HTTP 客户端事件
 * @return 恒为 ESP_OK
 * @note   总长度在收到响应头之后才能取得，不能在 perform 之前调用
 *         esp_http_client_get_content_length()。
 */
static esp_err_t http_evt_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER)
    {
        if (evt->header_key && evt->header_value &&
            strcasecmp(evt->header_key, "Content-Length") == 0)
        {
            /* 把总长度存到 user_data 指向的变量里 */
            int64_t *total = (int64_t *)evt->user_data;
            if (total) *total = strtoll(evt->header_value, NULL, 10);
        }
    }
    return ESP_OK;
}

/**
 * @brief  HTTP 拉取式 OTA 任务：下载 → 写槽 → 校验 → 切启动槽 → 重启
 * @param  arg  URL 字符串（由 ota_mgr_start() strdup 出来，本任务负责 free）
 */
static void ota_task(void *arg)
{
    char *url = (char *)arg;      /* 由 ota_mgr_start strdup 出来，本任务负责 free */
    esp_http_client_handle_t http = NULL;
    const esp_partition_t   *part = NULL;
    esp_ota_handle_t         ota  = 0;
    bool  ota_begun   = false;
    char *buf = NULL;
    const char *errmsg = NULL;

    ESP_LOGI(TAG, "=== OTA 开始: %s ===", url);
    set_state(OTA_ST_DOWNLOADING, 0, "");
    s_last_reported_pct = -1;
    report_progress(0);

    /* ── ⓪ 等待网络就绪 ──
     * NS800 可能在本端刚上电或刚复位后立即下发 OTA，此时 WiFi 尚在
     * auth/assoc 阶段、netif 还未取得 IP，esp_http_client_open() 会直接
     * 报 "Host is unreachable"，表现为 OTA 一开始就失败、进度停在 0%。
     *
     * 这是时序问题而非协议问题，命令、槽选择与失败回报均正常，因此在
     * 此处主动等待，而不是要求用户先联网再点升级：正常 DHCP 在 2~5 秒内
     * 完成，给 20 秒足以覆盖首次关联重试。 */
    if (!wifi_mgr_is_online())
    {
        ESP_LOGW(TAG, "网络未就绪，等待拿到 IP（最多 20 秒）...");
        if (!wifi_mgr_wait_ip(20000))
        {
            errmsg = "网络未就绪（等 IP 超时），请确认 C3 已联网";
            goto fail;
        }
        ESP_LOGI(TAG, "网络就绪: IP=%s", wifi_mgr_ip());
    }

    /* ── ① 选择目标槽（bootloader 会选"非当前运行"那个槽）── */
    part = esp_ota_get_next_update_partition(NULL);
    if (!part)
    {
        errmsg = "找不到可用的 OTA 分区";
        goto fail;
    }
    ESP_LOGI(TAG, "目标分区: %s @0x%08lx (%lu KB)",
             part->label, (unsigned long)part->address,
             (unsigned long)(part->size / 1024));

    /* ── ② 创建 HTTP 客户端 ── */
    int64_t total_len = 0;
    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_evt_cb,
        .user_data         = &total_len,
        .timeout_ms        = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    http = esp_http_client_init(&cfg);
    if (!http)
    {
        errmsg = "HTTP 客户端创建失败";
        goto fail;
    }

    esp_err_t err = esp_http_client_open(http, 0);
    if (err != ESP_OK)
    {
        errmsg = "HTTP 连接失败";
        goto fail;
    }

    /* 拿到响应头后才能读 content-length（回调已填进 total_len） */
    int content_len = esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    if (status != 200)
    {
        ESP_LOGE(TAG, "HTTP 状态码 %d（期望 200）", status);
        errmsg = "HTTP 状态码非 200";
        goto fail;
    }
    if (content_len > 0)
        total_len = content_len;
    ESP_LOGI(TAG, "固件总长 %lld 字节", (long long)total_len);

    /* 槽容量检查：提前失败优于写到一半失败 */
    if (total_len > 0 && (uint64_t)total_len > part->size)
    {
        ESP_LOGE(TAG, "固件 %lld 字节 > 槽容量 %lu 字节，装不下",
                 (long long)total_len, (unsigned long)part->size);
        errmsg = "固件大于 OTA 槽容量";
        goto fail;
    }

    /* ── ③ 开始写入 ── */
    err = esp_ota_begin(part, total_len > 0 ? (size_t)total_len : OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        errmsg = "OTA 分区擦除失败";
        goto fail;
    }
    ota_begun = true;

    buf = malloc(OTA_CHUNK);
    if (!buf)
    {
        errmsg = "内存不足(分配缓冲)";
        goto fail;
    }

    int64_t written = 0;
    while (1)
    {
        int n = esp_http_client_read(http, buf, OTA_CHUNK);
        if (n < 0)
        {
            ESP_LOGE(TAG, "读 socket 出错");
            errmsg = "读取固件流失败";
            goto fail;
        }
        if (n == 0)
            break;      /* 读完 */

        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            errmsg = "写 flash 失败";
            goto fail;
        }
        written += n;

        if (total_len > 0)
        {
            int pct = (int)((written * 100) / total_len);
            set_state(OTA_ST_DOWNLOADING, pct, "");
            report_progress(pct);
        }

        /* 每块之间让出 CPU，否则 WiFi 任务与板间链路会被饿死，
         * 表现为升级过程中 NS800 判"离线"。 */
        vTaskDelay(1);
    }

    ESP_LOGI(TAG, "下载完成 %lld 字节，校验中…", (long long)written);
    set_state(OTA_ST_DOWNLOADING, 100, "");
    report_progress(100);

    /* ── ④ 结束并校验：esp_ota_end 校验镜像头与长度，失败说明固件不完整 ── */
    err = esp_ota_end(ota);
    ota_begun = false;      /* 无论成败，handle 已释放 */
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end: %s（固件不完整或签名不符）", esp_err_to_name(err));
        errmsg = "固件校验失败";
        goto fail;
    }

    /* ── ⑤ 设置下次启动槽 ── */
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        errmsg = "设置启动分区失败";
        goto fail;
    }

    ESP_LOGI(TAG, "=== OTA 成功，下次启动切到 %s，5 秒后重启 ===", part->label);
    set_state(OTA_ST_SUCCESS, 100, "");
    report_result(true, NULL);

    /* 留出时间让 NS800 显示"即将重启"，并让串口日志落盘 */
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (buf)      free(buf);
    if (http)     esp_http_client_cleanup(http);
    if (url)      free(url);

    esp_restart();
    return;     /* 不会到这 */

fail:
    /* 失败必须 abort：否则 OTA 槽内残留半个固件，虽然 otadata 未改不会
     * 被启动，但下次 OTA 需先擦除，多绕一圈。 */
    if (ota_begun)
    {
        esp_ota_abort(ota);
    }
    ESP_LOGE(TAG, "=== OTA 失败: %s ===", errmsg ? errmsg : "未知");
    set_state(OTA_ST_FAILED, s_pct, errmsg);
    report_result(false, errmsg);

    if (buf)  free(buf);
    if (http) esp_http_client_cleanup(http);
    if (url)  free(url);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

/* ── 上传会话（网页本地固件直传，不经过 esp_http_client）──
 *
 * 采用"管道 + 独立任务"，而非在 httpd handler 内直接写入：
 *
 *   esp_ota_begin() 会同步擦除整个 1.5 MB 的槽，在 SPI flash 上耗时以秒计
 *   （远超 5 秒）。而 IDF 的任务看门狗（CONFIG_ESP_TASK_WDT_EN，
 *   TIMEOUT_S=5）会重启任何超过 5 秒未喂狗的任务。httpd 的 server task
 *   若直接在回调里擦写，擦 flash 期间没有机会调用 esp_task_wdt_reset()，
 *   设备刚启动就被看门狗重启，表现为 POST 上传无任何响应、串口反复出现
 *   启动日志，且槽被反复擦除。
 *
 * 本文件的结构：
 *   httpd task 负责收包并投入管道（管道满时自然阻塞等待）；
 *   ota task 负责 begin(擦) → 取块 → write → end，并自行喂看门狗。
 *
 * 不采用"先收完整文件到 RAM 再写"：C3 仅约 400 KB SRAM，而固件约 1 MB，
 * 整块缓冲必然内存溢出。
 *
 * 单实例：s_up_busy 保证不会两次上传并发（并发会互相擦同一个槽）。
 */

/* 管道容量：能装 8 块 × 4 KB = 32 KB。
 * 太小则 httpd 频繁阻塞等待（浪费带宽），太大则白占 SRAM。 */
#define OTA_UP_QUEUE_BLOCKS   8

/* ota task ← httpd task 的数据管道元素。
 * 队列元素不是裸指针，而是 {ptr, len}，必须带真实长度：
 * 固件总长通常不是 4 KB 的整数倍，最后一块不满，
 * 如果按固定 OTA_CHUNK 写就会往 flash 里灌一段越界的垃圾，
 * 轻则校验失败，重则镜像尾部损坏。 */
typedef struct {
    uint8_t *ptr;   /* 数据块首地址，由 httpd 侧 malloc */
    size_t   len;   /* 本块有效字节数，末块可小于 OTA_CHUNK */
} ota_up_blk_t;

static volatile bool        s_up_busy = false;  /* 上传会话进行中标志 */
static esp_ota_handle_t     s_up_ota  = 0;
static const esp_partition_t *s_up_part = NULL;
static int64_t              s_up_total = 0;    /* 声明的总长度（<=0 = 未知） */
static int64_t              s_up_written = 0;  /* 已写入字节数 */

/* ota task ← httpd task 的数据管道 */
static QueueHandle_t        s_up_q = NULL;
/* httpd task → ota task 的结束信号 */
static volatile int         s_up_cmd = 0;      /* 0=无 1=完成 2=中途放弃 */
static volatile int         s_up_err = 0;      /* 0=ok，非 0=esp_err_t */
static int                 s_up_last_pct = 0; /* 上次上报的百分比（降频用） */

/**
 * @brief  完成上传收尾：校验镜像并切换启动槽
 * @return ESP_OK 成功；ESP_ERR_INVALID_SIZE 表示镜像不完整或无效
 */
static esp_err_t upload_finish(void)
{
    esp_err_t err = esp_ota_end(s_up_ota);

    s_up_ota = 0;
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "上传固件 esp_ota_end: %s（文件不完整或不是有效镜像）",
                 esp_err_to_name(err));
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_ota_set_boot_partition(s_up_part);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "上传固件成功 %lld 字节，下次启动切到 %s",
             (long long)s_up_written, s_up_part->label);
    return ESP_OK;
}

/**
 * @brief  上传写 flash 任务：擦槽 → 从管道取块写入 → 校验 → 切启动槽
 * @param  arg  未使用，非 NULL 仅表示"已请求上传"
 * @note   整个擦除与写入都在本任务内完成，任务看门狗由本任务自行喂。
 */
static void upload_task(void *arg)
{
    (void)arg;

    /* 必须自己订阅 WDT，esp_task_wdt_reset() 才会真的生效：
     * 新建任务默认不在订阅表里，此时调 reset 只会打一行
     * "task_wdt: task not found"（而且每写一块就刷一次，
     * 串口刷屏能把 1 MB 上传拖到超时），看门狗并未被喂上。
     * 真正的病根是：擦 flash 期间 CPU 全忙，idle task 饿死，
     * CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK 超时导致整机重启。 */
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "上传任务启动，开始擦除目标槽（可能数秒）…");
    esp_task_wdt_reset();

    /* ── ① 选槽并检查大小（此时还没擦，大小不合格可以白跑）── */
    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (!s_up_part)
    {
        s_up_err = ESP_FAIL;
        goto out;
    }

    /* 必须在擦槽之前判掉超大文件：擦完槽才发现装不下，
     * 设备就白白少了一个可启动槽。 */
    if (s_up_total > 0 && (uint64_t)s_up_total > s_up_part->size)
    {
        ESP_LOGE(TAG, "文件 %lld 字节 > 槽容量 %lu 字节",
                 (long long)s_up_total, (unsigned long)s_up_part->size);
        s_up_err = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    ESP_LOGI(TAG, "目标分区 %s @0x%08lx，擦除中…",
             s_up_part->label, (unsigned long)s_up_part->address);
    esp_err_t err = esp_ota_begin(s_up_part,
                        s_up_total > 0 ? (size_t)s_up_total : OTA_SIZE_UNKNOWN,
                        &s_up_ota);
    esp_task_wdt_reset();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        s_up_err = err;
        goto out;
    }

    /* ── ② 从管道逐块取出并写 flash ──
     * 每写一块就喂一次狗：擦加写整包可能十几秒，不喂必被重启。 */
    s_up_written = 0;
    set_state(OTA_ST_DOWNLOADING, 0, "");
    report_progress(0);

    while (1)
    {
        ota_up_blk_t blk = {0};

        /* 管道取一块，最多等 3 秒。
         * 超时先喂狗再判断：等 3 秒不喂就已经吃掉 WDT 预算的一半。 */
        if (xQueueReceive(s_up_q, &blk, pdMS_TO_TICKS(3000)) != pdTRUE)
        {
            esp_task_wdt_reset();

            /* 没等到数据 → 要么对端断了，要么它还在上传。
             * 这里不立刻判失败：再给一次机会，同时看看结束信号。 */
            if (s_up_cmd == 1) break;          /* 正常收完 */
            if (s_up_cmd == 2)                 /* 对端中途放弃 */
            {
                ESP_LOGW(TAG, "上传被对端放弃，已写 %lld 字节",
                         (long long)s_up_written);
                s_up_err = ESP_FAIL;
                goto abort_out;
            }
            /* 否则当作网络停顿，继续等 */
            continue;
        }

        err = esp_ota_write(s_up_ota, blk.ptr, blk.len);
        esp_task_wdt_reset();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_write(%u 字节): %s",
                     (unsigned)blk.len, esp_err_to_name(err));
            s_up_err = err;
            free(blk.ptr);
            goto abort_out;
        }
        free(blk.ptr);      /* 每块都是独立 malloc，投递即转移所有权 */
        s_up_written += (int64_t)blk.len;

        if (s_up_total > 0)
        {
            int pct = (int)((s_up_written * 100) / s_up_total);
            if (pct > 100) pct = 100;
            set_state(OTA_ST_DOWNLOADING, pct, "");
            /* 每块（4 KB）都上报 = 1 MB 固件要发 261 个 UART 帧，
             * 串口同步写会明显拖慢上传。降到每跨 10% 报一次。 */
            if (pct >= s_up_last_pct + 10 || pct == 100)
            {
                s_up_last_pct = pct;
                report_progress(pct);
            }
        }
    }

    /* ── ③ 收尾校验并切换启动槽 ── */
    if (s_up_total > 0 && s_up_written != s_up_total)
    {
        ESP_LOGE(TAG, "长度不符：收 %lld / 声明 %lld",
                 (long long)s_up_written, (long long)s_up_total);
        s_up_err = ESP_ERR_INVALID_SIZE;
        goto abort_out;
    }

    err = upload_finish();
    if (err != ESP_OK)
    {
        s_up_err = err;
        goto abort_out;
    }

    set_state(OTA_ST_SUCCESS, 100, "");
    report_result(true, NULL);
    s_up_err = ESP_OK;
    goto out;

abort_out:
    /* 必须 abort，不能 end() + set_boot：在只写了一半的镜像上调用
     * esp_ota_end() 再切启动槽，bootloader 会跳进半个固件，设备变砖。 */
    if (s_up_ota)
    {
        esp_ota_abort(s_up_ota);
        s_up_ota = 0;
    }
    /* 状态必须显式落到 FAILED：
     * ota_mgr_upload_begin() 的并发守卫会查 s_state，
     * 漏掉此步则 s_state 永远卡在 DOWNLOADING，
     * 之后任何上传都会被判为"已有升级在进行中"，直到重启设备。 */
    set_state(OTA_ST_FAILED, 0,
              s_up_err == ESP_ERR_INVALID_SIZE ? "固件无效或过大"
                                               : "写 flash 失败");
    report_result(false, s_up_err == ESP_ERR_INVALID_SIZE ? "固件无效或过大"
                                                         : "写 flash 失败");

out:
    s_up_busy = false;
    /* 退订后再删任务，否则会留下一个指向已释放 TCB 的 WDT 订阅项 */
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

/**
 * @brief  httpd 侧发起上传：建管道并启动写 flash 任务，立即返回
 * @param  declared_size  声明的总长度，0 表示未知（如 chunked）
 * @return ESP_OK 已接受；ESP_ERR_INVALID_STATE 表示已有升级在进行中；
 *         ESP_ERR_INVALID_SIZE 表示文件大于槽容量；ESP_ERR_NO_MEM 资源不足
 */
esp_err_t ota_mgr_upload_begin(int64_t declared_size)
{
    if (s_up_busy || s_state == OTA_ST_DOWNLOADING)
        return ESP_ERR_INVALID_STATE;   /* 已有升级在进行中 */

    s_up_total   = declared_size > 0 ? declared_size : 0;
    s_up_written = 0;
    s_up_cmd     = 0;
    s_up_err     = ESP_OK;
    s_up_last_pct = 0;
    s_up_ota     = 0;
    s_up_part    = NULL;

    /* 超大文件在这里就拒掉，不建任务也不擦槽。 */
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p) return ESP_FAIL;
    if (s_up_total > 0 && (uint64_t)s_up_total > p->size)
    {
        ESP_LOGE(TAG, "文件 %lld 字节 > 槽容量 %lu 字节",
                 (long long)s_up_total, (unsigned long)p->size);
        set_state(OTA_ST_FAILED, 0, "固件文件大于 OTA 槽容量");
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_up_q)
    {
        s_up_q = xQueueCreate(OTA_UP_QUEUE_BLOCKS, sizeof(ota_up_blk_t));
        if (!s_up_q)
        {
            set_state(OTA_ST_FAILED, 0, "内存不足(建管道)");
            return ESP_ERR_NO_MEM;
        }
    }
    /* 上一次残留的块要清掉（否则会被当成本次数据写进去） */
    { ota_up_blk_t junk = {0};
      while (xQueueReceive(s_up_q, &junk, 0) == pdTRUE)
      {
          free(junk.ptr);
          junk.ptr = NULL;
      } }

    s_up_busy = true;
    BaseType_t r = xTaskCreate(upload_task, "ota_up", 4096, NULL, 4, NULL);
    if (r != pdPASS)
    {
        s_up_busy = false;
        set_state(OTA_ST_FAILED, 0, "建 OTA 任务失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "开始接收上传固件（声明 %lld 字节，槽 %s）",
             (long long)s_up_total, p->label);
    return ESP_OK;
}

/**
 * @brief  向当前上传会话投递一块数据
 * @param  data  数据首地址
 * @param  len   长度，取值 1..OTA_CHUNK
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 无进行中的会话；
 *         ESP_ERR_INVALID_ARG 长度非法；ESP_ERR_NO_MEM 分配失败；
 *         ESP_ERR_TIMEOUT 管道塞满超时
 * @note   内部 malloc 一块并拷贝，投递成功即把所有权转移给写 flash 任务。
 */
esp_err_t ota_mgr_upload_chunk(const uint8_t *data, size_t len)
{
    ota_up_blk_t item;

    if (!s_up_busy) return ESP_ERR_INVALID_STATE;
    if (len == 0 || len > OTA_CHUNK) return ESP_ERR_INVALID_ARG;

    item.ptr = malloc(len);
    if (!item.ptr) return ESP_ERR_NO_MEM;
    memcpy(item.ptr, data, len);
    item.len = len;      /* 末块不满时这里是真实长度，不是 OTA_CHUNK */

    /* 队列满 → 阻塞等待 ota task 消化。超 10 秒认为对端已死。 */
    if (xQueueSend(s_up_q, &item, pdMS_TO_TICKS(10000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "上传管道塞不进去（写 flash 太慢或对端已断）");
        free(item.ptr);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/**
 * @brief  httpd 侧通知数据发送完毕，并等待上传任务收尾
 * @return 上传任务的最终 esp_err_t 结果
 * @note   等待上限 60 秒（擦/写 1.5 MB 可能十几秒）。此处 httpd task 仅为
 *         vTaskDelay（阻塞态），idle 正常运行，idle WDT 不会超时；且 httpd
 *         task 不在 WDT 订阅表里，这里调 esp_task_wdt_reset() 只会刷
 *         "task not found" 噪声。
 */
esp_err_t ota_mgr_upload_end(void)
{
    if (!s_up_busy) return ESP_ERR_INVALID_STATE;

    s_up_cmd = 1;   /* 通知 ota task 收尾 */

    int waited = 0;
    while (s_up_busy && waited < 60000)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    return s_up_err;
}

/**
 * @brief  放弃当前上传（丢弃已写内容，不切换启动槽）
 * @note   置结束信号后等待片刻，让上传任务自行清理，避免在它写入时插手。
 */
void ota_mgr_upload_abort(void)
{
    if (!s_up_busy) return;
    s_up_cmd = 2;   /* 让 ota task 走 abort 分支 */
    /* 给它一点时间自己清理，不要在它还在写的时候插手 */
    vTaskDelay(pdMS_TO_TICKS(300));
}

/**
 * @brief  初始化 OTA 模块（只创建同步原语，不做任何网络动作）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 互斥锁创建失败
 */
esp_err_t ota_mgr_init(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "OTA 模块就绪（槽 1.5MB × 2，回滚保护=%s）",
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
             "开"
#else
             "关"
#endif
            );
    return ESP_OK;
}

/**
 * @brief  触发一次 HTTP 拉取式 OTA 升级（异步，立即返回）
 * @param  url  固件 .bin 的完整 URL
 * @return ESP_OK 任务已接受；ESP_ERR_INVALID_ARG url 为空；
 *         ESP_ERR_INVALID_STATE 已有升级在进行中；ESP_ERR_NO_MEM 资源不足
 * @note   内部 strdup 一份 URL 交给任务，任务结束时释放。
 */
esp_err_t ota_mgr_start(const char *url)
{
    if (!url || !*url)
        return ESP_ERR_INVALID_ARG;

    if (s_state == OTA_ST_DOWNLOADING)
    {
        ESP_LOGW(TAG, "已有升级在进行中，忽略本次请求");
        return ESP_ERR_INVALID_STATE;
    }

    char *u = strdup(url);
    if (!u)
        return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(ota_task, "ota", 8192, u, 4, NULL);
    if (ok != pdPASS)
    {
        free(u);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief  查询当前 OTA 状态与进度
 * @param  out_st   输出，状态（可为 NULL）
 * @param  out_pct  输出，0..100 进度（可为 NULL）
 * @param  out_err  输出，最近一次失败原因字符串（可为 NULL）
 * @note   取不到锁时直接读，三个变量为原子访问。
 */
void ota_mgr_get_state(ota_state_t *out_st, int *out_pct, const char **out_err)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        if (out_st)  *out_st  = s_state;
        if (out_pct) *out_pct = s_pct;
        if (out_err) *out_err = s_err;
        xSemaphoreGive(s_lock);
    }
    else
    {
        if (out_st)  *out_st  = s_state;
        if (out_pct) *out_pct = s_pct;
        if (out_err) *out_err = s_err;
    }
}

/**
 * @brief  确认当前固件有效（须在自检通过后调用）
 * @note   配合 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE：若新固件启动后一直
 *         不调用本函数，下次重启会自动回滚到旧固件；未启用回滚配置时本
 *         函数为无害空操作（仅打一条日志）。
 */
void ota_mgr_confirm_valid(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;

    if (esp_ota_get_state_partition(running, &st) != ESP_OK)
    {
        ESP_LOGW(TAG, "读不到运行分区状态，跳过确认");
        return;
    }

    if (st == ESP_OTA_IMG_PENDING_VERIFY)
    {
        ESP_LOGW(TAG, "当前固件处于 PENDING_VERIFY —— 自检通过，确认有效（取消回滚）");
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        if (e == ESP_OK)
            ESP_LOGI(TAG, "已确认有效 ✔");
        else
            ESP_LOGE(TAG, "确认失败: %s", esp_err_to_name(e));
    }
    else
    {
        /* 常见于串口直接烧录：bootloader 首次启动会直接把 app 标为 VALID，
         * 不走 PENDING_VERIFY，所以这里是正常的，不是错误。
         * 只有 OTA 方式刷进来的固件才会进 PENDING_VERIFY。 */
        ESP_LOGI(TAG, "固件状态 %u（非 PENDING_VERIFY），无需确认",
                 (unsigned)st);
    }
#else
    ESP_LOGI(TAG, "回滚保护未启用，confirm_valid 为空操作");
#endif
}

/**
 * @brief  打印当前运行槽、启动槽与待写槽信息
 * @note   调试用，含镜像状态，可判断是否处于 PENDING_VERIFY。
 */
void ota_mgr_dump_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot    = esp_ota_get_boot_partition();
    const esp_partition_t *next    = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t   st      = ESP_OTA_IMG_UNDEFINED;

    ESP_LOGI(TAG, "── OTA 分区信息 ──");
    ESP_LOGI(TAG, "运行槽  : %s @0x%08lx",
             running ? running->label : "?",
             (unsigned long)(running ? running->address : 0));
    ESP_LOGI(TAG, "启动槽  : %s",
             boot ? boot->label : "?");
    ESP_LOGI(TAG, "下次可写: %s %lu KB",
             next ? next->label : "?",
             (unsigned long)(next ? next->size / 1024 : 0));

    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK)
    {
        /* 此映射表的顺序必须严格对应 esp_flash_partitions.h 的枚举值：
         *   NEW=0, PENDING_VERIFY=1, VALID=2, INVALID=3, ABORTED=4,
         *   UNDEFINED=0xFFFFFFFF
         * 枚举值映射务必对着头文件核对，顺序错位会把状态打印成错误名称。 */
        const char *n;
        switch ((uint32_t)st)
        {
        case ESP_OTA_IMG_NEW:            n = "NEW";            break;
        case ESP_OTA_IMG_PENDING_VERIFY: n = "PENDING_VERIFY"; break;
        case ESP_OTA_IMG_VALID:          n = "VALID";          break;
        case ESP_OTA_IMG_INVALID:        n = "INVALID";        break;
        case ESP_OTA_IMG_ABORTED:        n = "ABORTED";        break;
        case ESP_OTA_IMG_UNDEFINED:      n = "UNDEFINED";      break;
        default:                         n = "?";              break;
        }
        ESP_LOGI(TAG, "槽状态  : %s (%u)", n, (unsigned)st);
        if (st == ESP_OTA_IMG_PENDING_VERIFY)
            ESP_LOGW(TAG, "           ↑ 待验证：需调用 ota_mgr_confirm_valid()，否则下次重启回滚");
    }
}
