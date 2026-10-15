/**
 * @file    time_mgr.c
 * @brief   C3 侧 NTP 对时，并每秒向 NS800 下发本地时间
 *
 * 流程（均在 time_mgr_task 任务内）：
 *   1. 等待 WiFi 拿到 IP（用 wifi_mgr 的事件组，不自轮询）
 *   2. 初始化 SNTP，等待首次对时完成（失败则周期重试）
 *   3. 每秒把本地时间戳打成 NS800_CMD_TIME 帧下发
 *
 * 每秒下发一次：状态栏需每秒刷新；帧很小（9 字节），115200 下约 0.8 ms，
 * 链路无压力。NS800 收到后只更新“基准时间 + 基准 tick”，显示时用 tick
 * 差值推算，偶尔丢帧也不会跳变。
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "time_mgr.h"
#include "wifi_mgr.h"
#include "link_ns800.h"
#include "ns800_link.h"

static const char *TAG = "time";

/* 默认 NTP 服务器（阿里云，国内可达性好） */
#define TIME_MGR_DEFAULT_SERVER  "ntp.aliyun.com"

/* 对时重试：首次可能因为路由/阻塞失败，重试几次 */
#define TIME_SYNC_RETRY_MAX   5
#define TIME_SYNC_RETRY_MS    3000

/* 对时成功后，多久再对一次（SNTP 内部也有自己的 1 小时轮询，
 * 这里只是兜底：万一内部轮询因重启网络被拉断，我们主动补一次）。 */
#define TIME_RESYNC_PERIOD_MS (30 * 60 * 1000)

static bool     s_sntp_inited = false;
static bool     s_synced      = false;

esp_err_t time_mgr_start(const char *server)
{
    const char *srv = (server && server[0]) ? server : TIME_MGR_DEFAULT_SERVER;

    /* 等待 WiFi 拿到 IP：不在 WiFi 事件回调里启动（耦合太深），这里自己等。
     * 用 wifi_mgr_wait_ip，最长 60 秒（首次连路由器 + DHCP 可能要几秒）。 */
    ESP_LOGI(TAG, "等待网络就绪后开始 NTP 对时（服务器 %s）...", srv);
    if (!wifi_mgr_wait_ip(60000))
    {
        ESP_LOGW(TAG, "等 IP 超时 —— 无法对时，时间功能不可用");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "网络就绪 (%s)，开始 SNTP", wifi_mgr_ip());

    /* 初始化 SNTP（只做一次） */
    if (!s_sntp_inited)
    {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(srv);
        /* 显式关闭默认配置的自动 start：自己控制开始时机并轮询判完成，
         * 逻辑更易追踪。 */
        cfg.start   = false;
        cfg.sync_cb = NULL;      /* 用轮询判完成，比回调简单 */

        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
            return err;
        }
        s_sntp_inited = true;
    }

    esp_netif_sntp_start();

    /* 等待首次对时完成 */
    {
        int retry = 0;
        while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_RETRY_MS)) != ESP_OK)
        {
            if (++retry >= TIME_SYNC_RETRY_MAX)
            {
                ESP_LOGW(TAG, "NTP 对时失败（重试 %d 次）—— 稍后由 time 任务重试",
                         retry);
                return ESP_FAIL;
            }
            ESP_LOGW(TAG, "NTP 未就绪，重试 %d/%d", retry, TIME_SYNC_RETRY_MAX);
        }
    }

    s_synced = true;
    {
        time_t now = time(NULL);
        struct tm tmv;
        gmtime_r(&now, &tmv);
        ESP_LOGI(TAG, "✔ NTP 对时完成（UTC）: %04d-%02d-%02d %02d:%02d:%02d",
                 1900 + tmv.tm_year, 1 + tmv.tm_mon, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    }

    return ESP_OK;
}

bool time_mgr_is_synced(void) { return s_synced; }

int64_t time_mgr_local_unix(void)
{
    time_t now;

    if (!s_synced)
        return 0;

    time(&now);
    /* UTC → UTC+8：只在这里加一次偏移，NS800 拿到的就是"本地时间戳" */
    return (int64_t)now + TIME_MGR_TZ_OFFSET_SEC;
}

/* 常驻任务：先对时，成功后每秒下发一次。
 * 单独成任务而非并入 reporter：reporter 周期是 30 秒（上报），
 * 时间需 1 秒一次，节奏不同，混在一起会互相拖累。
 * 对时失败不退出，每 TIME_RESYNC_PERIOD_MS 重试一次：
 * 开机时路由器未起或 DHCP 慢属常态，若就此放弃，状态栏会永远停在 "--:--"。 */
void time_mgr_task(void *arg)
{
    (void)arg;

    TickType_t next_resync = 0;

    for (;;)
    {
        if (!s_synced)
        {
            /* 到点（或首次）就试着对时 */
            if (xTaskGetTickCount() >= next_resync)
            {
                time_mgr_start(NULL);
                /* 成功后 30 分钟兜底重对；失败则 1 分钟后再试 */
                next_resync = xTaskGetTickCount() +
                              (s_synced ? pdMS_TO_TICKS(TIME_RESYNC_PERIOD_MS)
                                        : pdMS_TO_TICKS(60000));
            }
        }
        else
        {
            /* 已对时：每秒把本地时间戳推给 NS800 */
            int64_t ts = time_mgr_local_unix();
            uint8_t b[4];

            /* 小端 4 字节，与 ns800_link.h 中 NS800_CMD_TIME 的载荷约定一致 */
            b[0] = (uint8_t)(ts & 0xFF);
            b[1] = (uint8_t)((ts >> 8) & 0xFF);
            b[2] = (uint8_t)((ts >> 16) & 0xFF);
            b[3] = (uint8_t)((ts >> 24) & 0xFF);

            ns800_link_send(NS800_CMD_TIME, b, 4);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
