/**
 * @file    reporter.c
 * @brief   周期采集芯片状态、联网上报、与 NS800 双向同步
 *
 * 单个任务按固定周期完成三件事，顺序有依赖：
 *   ① 采集：温度趋势、RSSI、空闲堆、运行时长
 *   ② 推给 NS800（本地链路，永远做）：屏上数据显示不依赖外网，
 *      断网时本机显示照常更新，这是两块板协同的意义所在
 *   ③ 联网上报（仅在配置了上报 URL 时做）
 *
 * ② 与 ③ 的顺序不能反：如果把网络请求放在前面，一旦服务器卡住（超时
 * 数十秒），NS800 屏上的数据也会一并冻结，表现为本地显示依赖公网，
 * 这是设计上的错误。
 *
 * C3 没有真正的温度传感器：ESP32-C3 不具备 S2/S3 那样的内置温度传感器
 * 外设（temperature_sensor 驱动在 C3 上不可用）。本模块以 read_temp_x100()
 * 返回 0 表示无温度数据，由 NS800 端显示 "--"。不得用堆大小、运行时长等
 * 其它量拼凑假温度：那比没有更糟，看似正常读数实则毫无意义，排查时会
 * 把人带沟里。需要真实温度时应外接 DHT22/DS18B20 到 C3 的 GPIO，
 * 用驱动读取。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"

#include "sdkconfig.h"
#include "reporter.h"
#include "wifi_mgr.h"
#include "wifi_cfg.h"
#include "ota_mgr.h"
#include "link_ns800.h"
#include "ns800_link.h"
#include "host_data.h"

static const char *TAG = "report";

static bool     s_running;
static uint32_t s_period_ms;
static uint32_t s_sent_ok;
static uint32_t s_sent_fail;

/**
 * @brief  读取环境温度（×100 摄氏度）
 * @return 固定返回 0，表示无温度数据
 * @note   ESP32-C3 没有内置温度传感器外设，driver/temperature_sensor.h 在
 *         C3 上直接编译不过（该外设只有 S2/S3 有）。上层据返回值 0 显示 "--"。
 */
static int16_t read_temp_x100(void)
{
    return 0;   /* 0 = 无数据；NS800 端据此显示 "--" */
}

/**
 * @brief  采集一次状态快照
 * @param  temp_x100  输出，温度 ×100（C3 无传感器，恒为 0）
 * @param  rssi       输出，WiFi 信号强度 dBm
 * @param  free_kb    输出，空闲堆大小 KB
 * @param  uptime_s   输出，运行时长 秒
 */
static void sample(int16_t *temp_x100, int16_t *rssi,
                   uint16_t *free_kb, uint32_t *uptime_s)
{
    *temp_x100 = read_temp_x100();
    *rssi      = wifi_mgr_rssi();
    *free_kb   = (uint16_t)(esp_get_free_heap_size() / 1024);
    *uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000LL);
}

/**
 * @brief  向配置的上报 URL 发起一次 HTTP POST
 * @param  temp_x100  温度 ×100
 * @param  rssi       WiFi 信号强度 dBm
 * @param  free_kb    空闲堆大小 KB
 * @param  uptime_s   运行时长 秒
 * @return ESP_OK 成功；ESP_ERR_NOT_SUPPORTED 未配置 URL；其余为失败
 * @note   用 esp_http_client_perform 的同步写法加超时，不用事件回调：
 *         回调版本在超时路径上容易漏 free；且 timeout_ms 必须设，默认值
 *         是"永不超时"，服务器一挂就卡死本任务。
 */
static esp_err_t http_post_sample(int16_t temp_x100, int16_t rssi,
                                  uint16_t free_kb, uint32_t uptime_s)
{
    char body[192];
    int  n;

    if (strlen(CONFIG_C3_REPORT_URL) == 0)
        return ESP_ERR_NOT_SUPPORTED;          /* 没配 URL，跳过网络 */

    n = snprintf(body, sizeof(body),
                 "{\"dev\":\"c3\",\"t\":%.2f,\"rssi\":%d,"
                 "\"free\":%u,\"up\":%lu}",
                 temp_x100 / 100.0, (int)rssi,
                 (unsigned)free_kb, (unsigned long)uptime_s);
    if (n <= 0) return ESP_FAIL;

    esp_http_client_config_t cfg = {
        .url        = CONFIG_C3_REPORT_URL,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err;

    if (!c) return ESP_FAIL;

    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, n);

    err = esp_http_client_perform(c);
    if (err == ESP_OK)
    {
        int code = esp_http_client_get_status_code(c);
        ESP_LOGI(TAG, "POST %d (%d 字节)", code, n);

        /* 把服务器回包状态也告诉 NS800 —— 屏上能显示"上报成功/失败" */
        {
            uint8_t pl[3];
            pl[0] = (uint8_t)(code & 0xFF);
            pl[1] = (uint8_t)((code >> 8) & 0xFF);
            pl[2] = (code >= 200 && code < 300) ? 1 : 0;
            ns800_link_send(NS800_CMD_SERVER_RESP, pl, 3);
        }
        if (!(code >= 200 && code < 300)) err = ESP_FAIL;
    }
    else
    {
        ESP_LOGW(TAG, "POST 失败: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(c);
    return err;
}

/**
 * @brief  上报任务：采样 → 推 NS800 → 联网上报
 * @param  arg  未使用
 * @note   等待联网但设上限，没网也要开始给 NS800 推本地数据。
 */
static void report_task(void *arg)
{
    (void)arg;

    /* 先等联网，但不无限等 —— 没网也要开始给 NS800 推本地数据 */
    wifi_mgr_wait_ip(15000);

    while (s_running)
    {
        int16_t  t, rssi;
        uint16_t free_kb;
        uint32_t up;

        sample(&t, &rssi, &free_kb, &up);

        /* ① 本地链路 —— 永远做，不管有没有网 */
        ns800_link_send_sensor(t, rssi, free_kb, up);

        /* ② 联网上报 —— 有网 且 配了 URL 才做
         *
         * 判据必须与 http_post_sample() 的早退条件对齐。这里以前只判了
         * wifi_mgr_is_online()，是个真 bug：CONFIG_C3_REPORT_URL 为空
         * （默认值就是 ""，本地调试常态）时，http_post_sample() 会走
         * strlen(url)==0 → return ESP_ERR_NOT_SUPPORTED 这条早退分支，
         * 然后被无条件记成一次失败。现象：网页上 rep_fail 每 30 秒稳定 +1
         * 永远涨，而 rep_ok 恒为 0，看着像网络一直上传失败，实际是根本没配
         * 上传地址，还容易连带误判成板间链路故障。
         */
        if (wifi_mgr_is_online() && strlen(CONFIG_C3_REPORT_URL) > 0)
        {
            if (http_post_sample(t, rssi, free_kb, up) == ESP_OK) s_sent_ok++;
            else                                                   s_sent_fail++;
        }

        ESP_LOGI(TAG, "rssi=%d free=%uKB up=%lus (网络 成%u 败%u)",
                 (int)rssi, (unsigned)free_kb, (unsigned long)up,
                 (unsigned)s_sent_ok, (unsigned)s_sent_fail);

        vTaskDelay(pdMS_TO_TICKS(s_period_ms));
    }

    vTaskDelete(NULL);
}

/* ── 收 NS800 下来的命令 ── */

/**
 * @brief  把当前 WiFi 配置打包成 NS800_CMD_WIFI_CFG 帧发回 NS800。
 *
 * 载荷格式（与 ns800_link.h 的 SET_WIFI_CFG 对称）：
 *   [0]     mode
 *   [1]     ssid_len
 *   [2..]   ssid
 *   [1+len] pass_len
 *   [..]    pass
 * 当前模式用哪个凭据：STA 模式发 sta_*，AP 模式发 ap_*。
 */
static void send_wifi_cfg(void)
{
    wifi_cfg_t c;
    const char *ssid, *pass;
    uint8_t buf[2 + NS800_WIFI_SSID_MAX + 1 + NS800_WIFI_PASS_MAX];
    uint8_t n = 0;

    wifi_cfg_get(&c);
    if (c.mode == NS800_WIFI_MODE_AP) { ssid = c.ap_ssid;  pass = c.ap_pass; }
    else                              { ssid = c.sta_ssid; pass = c.sta_pass; }

    uint8_t sl = (uint8_t)strnlen(ssid, NS800_WIFI_SSID_MAX);
    uint8_t pl = (uint8_t)strnlen(pass, NS800_WIFI_PASS_MAX);

    buf[n++] = c.mode;
    buf[n++] = sl;
    memcpy(&buf[n], ssid, sl); n += sl;
    buf[n++] = pl;
    memcpy(&buf[n], pass, pl); n += pl;

    ns800_link_send(NS800_CMD_WIFI_CFG, buf, n);
    ESP_LOGI(TAG, "已回报配置: mode=%s SSID=%s",
             c.mode == NS800_WIFI_MODE_AP ? "AP" : "STA", ssid);
}

/**
 * @brief 解析 NS800 下发的 SET_WIFI_CFG 并应用。
 * 载荷：[mode][ssid_len][ssid][pass_len][pass]（与 send_wifi_cfg 对称）
 */
static void handle_set_wifi_cfg(const ns800_frame_t *f)
{
    uint8_t mode, sl, pl;
    char ssid[NS800_WIFI_SSID_MAX + 1];
    char pass[NS800_WIFI_PASS_MAX + 1];
    const uint8_t *p = f->payload;
    uint8_t left = f->len;

    /* payload[] 是含 cmd 的原始段，len 也含 cmd：发送端把 cmd 编进了
     * payload 段首，所以 f->payload[0] 是 cmd，真正的配网数据从
     * f->payload[1] 开始，这里需要跳过 1 字节。 */
    if (left < 1 + 2) { ESP_LOGW(TAG, "SET_WIFI_CFG 太短"); return; }
    p++; left--;                       /* 跳过 cmd 字节 */

    mode = *p++;
    sl   = *p++; left -= 2;
    if (sl > NS800_WIFI_SSID_MAX || sl == 0 || left < sl + 1u) { ESP_LOGW(TAG, "SSID 长度非法"); return; }
    memcpy(ssid, p, sl); ssid[sl] = '\0'; p += sl; left -= sl;

    pl = *p++; left -= 1;
    if (pl > NS800_WIFI_PASS_MAX || left < pl) { ESP_LOGW(TAG, "密码长度非法"); return; }
    memcpy(pass, p, pl); pass[pl] = '\0';

    ESP_LOGI(TAG, "收到配网: mode=%s SSID=%s", mode == NS800_WIFI_MODE_AP ? "AP" : "STA", ssid);

    /* 只改与目标模式对应的那组凭据，另一组保留原值（传 NULL = 不改） */
    esp_err_t err;
    if (mode == NS800_WIFI_MODE_AP)
        err = wifi_cfg_apply(mode, NULL, NULL, ssid, pass);
    else
        err = wifi_cfg_apply(mode, ssid, pass, NULL, NULL);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "配网失败: %s", esp_err_to_name(err));
        ns800_link_send_text(NS800_CMD_TEXT, "配网参数非法");
    }
    else
    {
        ns800_link_send_text(NS800_CMD_TEXT, mode == NS800_WIFI_MODE_AP ? "已切到 AP" : "已切到 STA");
    }
}

/**
 * @brief  接收 NS800 命令并分发处理
 * @param  arg  未使用
 */
static void cmd_task(void *arg)
{
    ns800_frame_t f;

    (void)arg;

    while (1)
    {
        if (!ns800_link_recv(&f, 500)) continue;

        /* 收帧必打一行 —— 联调期"到底收到 NS800 的命令没有"是第一手信息。
         * GET_WIFI_CFG 下发后 NS800 侧毫无反应时，仅凭两端日志分不清是
         * 下行没到 C3，还是到了但没被处理。加这行后一眼就能区分；而且带上
         * 了 payload 长度，顺便能验证无负载帧（len=1，只有 cmd）是否被正确
         * 解析。用 DEBUG 级别：排障时把日志级别调低即可看到，日常不刷屏。 */
        ESP_LOGD(TAG, "RX 命令 0x%02X (pl_len=%u)", f.cmd, (unsigned)f.len);

        switch (f.cmd)
        {
        case NS800_CMD_PING:
            ESP_LOGI(TAG, "收到 NS800 ping");
            ns800_link_send_text(NS800_CMD_TEXT, "pong");
            break;

        case NS800_CMD_REQ_WIFI:
            ESP_LOGI(TAG, "NS800 请求重连 WiFi");
            wifi_mgr_reconnect_now();
            break;

        case NS800_CMD_SET_REPORT:
            if (f.len >= 3)
            {
                uint16_t sec = (uint16_t)(f.payload[1] | (f.payload[2] << 8));

                if (sec >= 1 && sec <= 3600)
                {
                    s_period_ms = (uint32_t)sec * 1000u;
                    ESP_LOGI(TAG, "上报周期改为 %u 秒", (unsigned)sec);
                }
            }
            break;

        case NS800_CMD_SET_WIFI_CFG:
            handle_set_wifi_cfg(&f);
            break;

        case NS800_CMD_GET_WIFI_CFG:
            ESP_LOGI(TAG, "NS800 请求回报 WiFi 配置");
            send_wifi_cfg();
            break;

        case NS800_CMD_WIFI_FACTORY_RST:
            /* 恢复出厂 WiFi 配置。必须先告诉 NS800 再动作 ——
             * 后面要 esp_restart()，串口会被打断，用户就不知道发生了什么。 */
            ESP_LOGW(TAG, "NS800 请求恢复出厂 WiFi 配置");
            wifi_cfg_factory_reset();
            ns800_link_send_text(NS800_CMD_TEXT,
                                 "WiFi 已恢复出厂配置，设备重启中...");
            vTaskDelay(pdMS_TO_TICKS(200));    /* 等这帧发完再重启 */
            esp_restart();
            break;

        case NS800_CMD_HOST_DATA:
        {
            /* 通用数据上行通道（0x88）。payload 是 "key=value" 的 UTF-8 文本，
             * 不带结尾 '\0'，长度 = f.len - 1（f.len 含 cmd 那一个字节）。
             *
             * 和上面 OTA_START 同样的坑：不能把 f.payload+1 直接当 C 字符串，
             * 它没终止符。这里靠 host_data_put(text, len) 的显式长度参数，
             * 内部自己裁剪并补 '\0'，全程不依赖 strlen 越界读。
             */
            uint32_t n = (f.len > 1) ? (uint32_t)(f.len - 1) : 0;

            if (n == 0)
            {
                ESP_LOGW(TAG, "HOST_DATA 载荷为空，忽略");
                break;
            }
            host_data_put((const char *)(f.payload + 1), n);
            ESP_LOGI(TAG, "收到主机数据: %.*s", (int)n, (const char *)(f.payload + 1));
            break;
        }

        case NS800_CMD_OTA_START:
        {
            /* 载荷 = URL 字符串，不带 '\0'，长度由 f.len-1 决定（f.len 含 cmd）。
             * 必须拷进本地缓冲区再补 '\0' —— 直接把 f.payload+1 当 C 字符串
             * 用会读到 payload 数组的残留字节，URL 尾巴上多出垃圾字符。 */
            char url[NS800_LINK_MAX_PAYLOAD];
            uint8_t n = (f.len > 1) ? (uint8_t)(f.len - 1) : 0;

            if (n == 0)
            {
                ESP_LOGW(TAG, "OTA_START 载荷为空，忽略");
                ns800_link_send_text(NS800_CMD_TEXT, "OTA 失败: URL 为空");
                break;
            }
            if (n >= sizeof(url))
                n = sizeof(url) - 1;

            memcpy(url, f.payload + 1, n);
            url[n] = '\0';

            ESP_LOGI(TAG, "NS800 请求 OTA: %s", url);

            esp_err_t e = ota_mgr_start(url);
            if (e != ESP_OK)
            {
                ESP_LOGW(TAG, "OTA 启动失败: %s", esp_err_to_name(e));
                ns800_link_send_text(NS800_CMD_TEXT,
                                     (e == ESP_ERR_INVALID_STATE)
                                         ? "OTA 已在升级中"
                                         : "OTA 启动失败");
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "未知命令 0x%02X（C3 未实现）", f.cmd);
            break;
        }
    }
}

/**
 * @brief  启动上报任务与命令接收任务（幂等，重复调用安全）
 * @return ESP_OK 启动成功
 * @note   内部先初始化 host_data，再创建 report / cmd 两个任务。
 */
esp_err_t reporter_start(void)
{
    if (s_running) return ESP_OK;

    /* KV 表要在 cmd_task 起来之前清好，否则理论上可能有一帧
     * HOST_DATA 先到、撞上未初始化的 s_lock（NULL 时下面会跳过加锁，
     * 不会崩，但那一帧可能被后续 init 抹掉）。 */
    host_data_init();

    s_period_ms = (uint32_t)CONFIG_C3_REPORT_PERIOD_S * 1000u;
    s_running   = true;

    xTaskCreate(report_task, "report", 4096, NULL, 5, NULL);
    xTaskCreate(cmd_task,    "cmd",    3072, NULL, 6, NULL);

    ESP_LOGI(TAG, "上报任务启动，周期 %d 秒，URL=%s",
             CONFIG_C3_REPORT_PERIOD_S,
             strlen(CONFIG_C3_REPORT_URL) ? CONFIG_C3_REPORT_URL : "(未配置，仅本地)");
    return ESP_OK;
}

/**
 * @brief  读取网络上报的成功 / 失败计数
 * @param  ok    输出，成功次数（可为 NULL）
 * @param  fail  输出，失败次数（可为 NULL）
 */
void reporter_get_counters(uint32_t *ok, uint32_t *fail)
{
    if (ok)   *ok   = s_sent_ok;
    if (fail) *fail = s_sent_fail;
}
