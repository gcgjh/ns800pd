/**
 * @file    wifi_mgr.c
 * @brief   ESP32-C3 WiFi STA/AP 联网管理
 *
 * 凭据来源优先级：menuconfig 中 Kconfig.projbuild 的默认值；
 * 若均未配置则进入"未配置"状态并周期重试，不阻塞启动。
 * SSID/密码不应硬编码在源码中，以免提交后泄露；
 * 通过 main/Kconfig.projbuild 暴露为配置项。
 *
 * WiFi/IP 事件在 esp_event 任务上下文中回调，不宜做重活。
 * 回调只做两件事：更新状态变量、向 NS800 发一帧状态通知；
 * 发帧本身有互斥锁与 UART 写，耗时仅数十微秒，可以接受。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi_mgr.h"
#include "wifi_cfg.h"
#include "link_ns800.h"
#include "ns800_link.h"

static const char *TAG = "wifi";

/* 事件组位 */
#define WIFI_BIT_CONNECTED  BIT0   /* 已连上 AP */
#define WIFI_BIT_GOT_IP     BIT1   /* 已取得 IP */
#define WIFI_BIT_FAIL       BIT2   /* 连续重试耗尽，暂停自动重连 */

static EventGroupHandle_t s_evt;
static bool               s_started;
static uint8_t            s_state = NS800_WIFI_IDLE;
static char               s_ip[16] = "0.0.0.0";
static int8_t             s_rssi;

/* 重试计数：连续 N 次失败后放慢重连，避免射频/日志被快速重试刷爆 */
#define WIFI_MAX_RETRY      8
static int s_retry;

/* netif 句柄：STA 和 AP 各一个，切换模式时用（不重复创建） */
static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;

/* ── 状态变化时通知 NS800 ─────────────────────────────────────
 * 这是跨板协议的唯一出口，所有状态变化都必须走这里，
 * 否则 NS800 屏上的状态与 C3 实际状态会不一致。 */
static void notify_ns800(uint8_t st)
{
    s_state = st;
    ns800_link_send(NS800_CMD_WIFI_STATE, &st, 1);
}

/* ── 事件处理 ─────────────────────────────────────────────── */
static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            notify_ns800(NS800_WIFI_IDLE);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已连上 AP，等 DHCP");
            notify_ns800(NS800_WIFI_CONNECTED);
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;

            xEventGroupClearBits(s_evt, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);
            ESP_LOGW(TAG, "断开 (reason=%d)，第 %d 次重试", d->reason, s_retry + 1);
            notify_ns800(NS800_WIFI_DISCONNECTED);

            if (s_retry++ < WIFI_MAX_RETRY)
            {
                notify_ns800(NS800_WIFI_CONNECTING);
                esp_wifi_connect();
            }
            else
            {
                /* 重试次数耗尽后不在此死等：置 FAIL 位并通知 NS800，
                 * 由上层决定后续（手动重连或重新配置）。 */
                ESP_LOGE(TAG, "连续 %d 次失败，暂停自动重连", WIFI_MAX_RETRY);
                xEventGroupSetBits(s_evt, WIFI_BIT_FAIL);
                notify_ns800(NS800_WIFI_FAIL);
            }
            break;
        }

        case WIFI_EVENT_AP_START:
            /* AP 模式起来 ≠ 有客户端连上，状态用 CONNECTED 表示"热点在广播"。
             * 真正"可用"要等 AP_STACONNECTED。 */
            ESP_LOGI(TAG, "AP 热点已启动");
            notify_ns800(NS800_WIFI_CONNECTED);
            break;

        case WIFI_EVENT_AP_STACONNECTED:
        {
            /* 此处不打印客户端 MAC：打印需额外 include esp_mac.h，
             * 对调试价值不大（AP 模式下知道"有设备接入"即可）。 */
            (void)data;
            ESP_LOGI(TAG, "有设备连上热点");
            /* 热点自己也有一路 IP（默认 192.168.4.1），视为"已联网可服务" */
            snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
            xEventGroupSetBits(s_evt, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);
            notify_ns800(NS800_WIFI_GOT_IP);
            ns800_link_send_text(NS800_CMD_TEXT, "AP 有设备接入");
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "设备断开热点");
            break;

        default:
            break;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;

        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        ESP_LOGI(TAG, "拿到 IP %s", s_ip);

        xEventGroupSetBits(s_evt, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);
        xEventGroupClearBits(s_evt, WIFI_BIT_FAIL);
        notify_ns800(NS800_WIFI_GOT_IP);

        /* 顺带把 IP 也告知 NS800（文本帧，便于屏上显示）。
         * 必须带 "ip " 前缀：NS800 侧 NS800_CMD_TEXT 分支靠
         * txt[0]=='i' && txt[1]=='p' && txt[2]==' ' 识别是否为 IP，
         * 裸 IP 会被当成普通文本忽略，导致屏上 IP 栏一直显示 "-"。 */
        {
            char msg[24];
            snprintf(msg, sizeof(msg), "ip %s", s_ip);
            ns800_link_send_text(NS800_CMD_TEXT, msg);
        }
    }
}

/* ── 按当前配置启动 STA 或 AP（被 init 和 apply_cfg 复用）───── */
static esp_err_t wifi_start_by_cfg(void)
{
    const wifi_cfg_t *c = NULL;
    wifi_cfg_t cfg;
    wifi_config_t  wc;
    esp_err_t      err;

    wifi_cfg_get(&cfg);
    c = &cfg;

    /* 切换模式前先 stop，否则 set_mode 会返回 ESP_ERR_WIFI_STATE。 */
    esp_wifi_stop();
    xEventGroupClearBits(s_evt, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP | WIFI_BIT_FAIL);
    s_retry = 0;
    memset(s_ip, 0, sizeof(s_ip));
    strcpy(s_ip, "0.0.0.0");

    if (c->mode == NS800_WIFI_MODE_AP)
    {
        /* ── AP 模式：自己开热点 ── */
        memset(&wc, 0, sizeof(wc));
        strncpy((char *)wc.ap.ssid, c->ap_ssid, sizeof(wc.ap.ssid) - 1);
        wc.ap.ssid_len = (uint8_t)strlen((char *)wc.ap.ssid);
        strncpy((char *)wc.ap.password, c->ap_pass, sizeof(wc.ap.password) - 1);
        wc.ap.max_connection = 4;
        /* 密码为空 → 开放网络；否则 WPA2 */
        wc.ap.authmode = (c->ap_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) { ESP_LOGE(TAG, "set_mode(AP) 失败: %s", esp_err_to_name(err)); return err; }
        err = esp_wifi_set_config(WIFI_IF_AP, &wc);
        if (err != ESP_OK) { ESP_LOGE(TAG, "set_config(AP) 失败: %s", esp_err_to_name(err)); return err; }
        err = esp_wifi_start();
        if (err != ESP_OK) { ESP_LOGE(TAG, "start(AP) 失败: %s", esp_err_to_name(err)); return err; }

        s_state = NS800_WIFI_IDLE;
        ESP_LOGI(TAG, "AP 已启动: SSID=%s 密码=%s ch=1",
                 c->ap_ssid, c->ap_pass[0] ? "(已设)" : "(开放)");
    }
    else
    {
        /* ── STA 模式：连路由器 ── */
        memset(&wc, 0, sizeof(wc));
        strncpy((char *)wc.sta.ssid, c->sta_ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, c->sta_pass, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = (c->sta_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) { ESP_LOGE(TAG, "set_mode(STA) 失败: %s", esp_err_to_name(err)); return err; }
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_OK) { ESP_LOGE(TAG, "set_config(STA) 失败: %s", esp_err_to_name(err)); return err; }
        err = esp_wifi_start();
        if (err != ESP_OK) { ESP_LOGE(TAG, "start(STA) 失败: %s", esp_err_to_name(err)); return err; }

        s_state = NS800_WIFI_IDLE;
        ESP_LOGI(TAG, "STA 已启动: SSID=%s", c->sta_ssid);
    }
    return ESP_OK;
}

/**
 * @brief  按当前 NVS 配置重新应用 WiFi 设置。
 * @return ESP_OK 成功；未初始化时等价于 wifi_mgr_init()，返回其结果。
 * @note   由 wifi_cfg 在保存新配置后调用：stop → 按新模式 set_config → start。
 */
esp_err_t wifi_mgr_apply_cfg(void)
{
    if (!s_started)
    {
        /* 还没初始化过 → 走完整初始化（会读 NVS 里刚存的新配置） */
        return wifi_mgr_init();
    }
    return wifi_start_by_cfg();
}

/**
 * @brief  初始化 NVS / netif / 事件循环 / WiFi，并按 NVS 配置启动 STA 或 AP。
 * @return ESP_OK 成功；创建事件组或 netif 失败返回 ESP_ERR_NO_MEM，
 *         NVS 初始化失败返回其错误码。
 * @note   幂等：已初始化则直接返回 ESP_OK。
 */
esp_err_t wifi_mgr_init(void)
{
    if (s_started) return ESP_OK;

    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;

    /* NVS 是 WiFi 驱动存校准数据的必需依赖；"无空闲页/版本不匹配"时擦掉重建 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    /* 载入用户配置（首次开机写默认值）。必须在读 CFG 之前。 */
    err = wifi_cfg_init();
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* STA 与 AP 的 netif 都需创建（两种模式都要能切换），但只建一次。
     * create_default 的 AP 版本在某些 IDF 版本中带 DHCP server 默认配置，
     * 未配 SSID 也能起来，正好合用。 */
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap  = esp_netif_create_default_wifi_ap();
    if (!s_netif_sta || !s_netif_ap) return ESP_ERR_NO_MEM;

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    /* 省电模式关掉：开了之后 DHCP/上报的延迟会明显变大 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_started = true;

    /* 按 NVS 里的配置（默认 STA → gpm/12345678）启动 */
    err = wifi_start_by_cfg();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "WiFi 就绪，模式=%s",
             wifi_cfg_mode() == NS800_WIFI_MODE_AP ? "AP" : "STA");
    return ESP_OK;
}

/**
 * @brief  等待取得 IP。
 * @param  timeout_ms  等待超时，单位毫秒。
 * @return true 已取得 IP；false 超时或尚未初始化。
 * @note   超时只表示该时间段内未拿到 IP，不代表连接失败。
 */
bool wifi_mgr_wait_ip(uint32_t timeout_ms)
{
    EventBits_t b;

    if (!s_started) return false;
    b = xEventGroupWaitBits(s_evt, WIFI_BIT_GOT_IP,
                            pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (b & WIFI_BIT_GOT_IP) != 0;
}

/**
 * @brief  查询当前是否已取得 IP（即可联网）。
 * @return true 在线；false 未联网或尚未初始化。
 */
bool wifi_mgr_is_online(void)
{
    if (!s_started) return false;
    return (xEventGroupGetBits(s_evt) & WIFI_BIT_GOT_IP) != 0;
}

/** @brief 当前 WiFi 状态（NS800_WIFI_* 枚举值）。 */
uint8_t wifi_mgr_state(void)  { return s_state; }
/** @brief 当前 IP 字符串；未连上时为 "0.0.0.0"。 */
const char *wifi_mgr_ip(void) { return s_ip; }

/* 静态缓冲：esp_wifi_sta_get_ap_info() 经调用者提供的栈变量返回记录，
 * 函数返回后 ap.ssid 即失效，直接返回其指针在 -O2 下会读到栈垃圾，
 * 因此拷贝到此缓冲。本函数非重入。 */
static char s_ssid[33];       /* 802.11 SSID 最长 32 字节，+1 放 '\0' */

/**
 * @brief  返回当前实际连上的 AP 的 SSID；未连上或 AP 模式下为空串。
 * @return 指向模块内静态缓冲，下一次调用即被覆盖；需留存请自行 memcpy。
 * @note   区别于 wifi_cfg 的 sta_ssid（用户想连的意图），本函数返回真正连上的结果，
 *         刚改完配置尚未连上时两者会不同。
 */
const char *wifi_mgr_ssid(void)
{
    wifi_ap_record_t ap;

    s_ssid[0] = '\0';
    if (wifi_mgr_state() == NS800_WIFI_MODE_AP) return s_ssid;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return s_ssid;
    memcpy(s_ssid, ap.ssid, sizeof(ap.ssid));
    s_ssid[sizeof(ap.ssid)] = '\0';   /* ap.ssid 是定长 32 字节，可能没终止符 */
    return s_ssid;
}

/**
 * @brief  查询当前 AP 的 RSSI。
 * @return 信号强度，单位 dBm（负值）；未连上或查询失败返回 0。
 */
int8_t wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;

    if (!wifi_mgr_is_online()) return 0;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    s_rssi = (int8_t)ap.rssi;
    return s_rssi;
}

/**
 * @brief  立即重连：清空重试计数并重新 disconnect + connect。
 * @note   未初始化时直接返回；断开后由事件回调驱动后续重连流程。
 */
void wifi_mgr_reconnect_now(void)
{
    if (!s_started) return;

    ESP_LOGI(TAG, "手动重连");
    s_retry = 0;
    xEventGroupClearBits(s_evt, WIFI_BIT_FAIL);
    notify_ns800(NS800_WIFI_CONNECTING);
    esp_wifi_disconnect();
    esp_wifi_connect();
}
