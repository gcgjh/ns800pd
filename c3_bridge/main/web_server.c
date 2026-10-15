/**
 * @file    web_server.c
 * @brief   C3 联网桥 —— HTTP 管理服务实现
 *
 * 路由：
 *    GET  /               管理页（安卓风格，单页）
 *    GET  /api/status     全部状态（JSON），网页每 1 秒轮询一次
 *    GET  /api/ping       物理层活性探针（RX 引脚电平翻转计数）
 *    GET  /api/host_data  NS800 上行数据（0x88）快照
 *    POST /api/wifi       改 WiFi 配置（JSON body）
 *    POST /api/ota        按 URL 拉取固件升级（JSON body: {"url":"..."}）
 *    POST /api/ota/upload 本地固件上传（application/octet-stream）
 *    POST /api/reboot     重启设备
 *
 * 并发：所有 handler 只读全局状态、只调幂等接口，不写临界区，
 * 因此不需要额外加锁。wifi_cfg_apply() 内部自行管理 NVS 写入。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"    /* OTA 上传后延时重启期间的看门狗处理，详见 h_ota_upload 注释 */
#include "nvs_flash.h"

#include "web_server.h"
#include "web_page.h"          /* WEB_PAGE_HTML */
#include "wifi_mgr.h"
#include "wifi_cfg.h"
#include "link_ns800.h"
#include "reporter.h"
#include "time_mgr.h"
#include "ota_mgr.h"
#include "host_data.h"

static const char *TAG = "web";

/* 普通请求体上限。网页只发几十字节 JSON，1KB 足够，但必须设上限，
 * 否则任意 POST 一个 10MB body 就能把堆吃光。
 * 此上限只管普通请求；OTA 文件上传走 /api/ota/upload，其上限由
 * OTA 槽容量（1.5MB）决定，所以普通 handler 收到非 JSON 的 POST 应拒绝。 */
#define MAX_REQ_BODY   1024

/* OTA 上传分块大小。须 4KB 对齐（flash 扇区），且足够小，
 * 以保证上传过程中栈/堆占用恒定。 */
#define OTA_UP_CHUNK   4096

static bool s_started = false;

/* ── JSON 取值辅助 ────────────────────────────────────────────
 * 仅为 4 个字段引入 cJSON 需几十 KB flash，这里手写一个只识别
 * "key": "value" / "key": number 的极简提取器。够用、够小、
 * 行为可预测，比通用 JSON 库更适合嵌入式场景。 */

/**
 * @brief  在请求体中查找 "key" 之后的取值起点（跳过冒号与空白）。
 * @param  body  请求体字符串
 * @param  key   待查找的键名（不含引号）
 * @return 指向值起始位置的指针；未找到或格式不符返回 NULL。
 */
static const char *json_find(const char *body, const char *key)
{
    char pat[40];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p;

    if (n <= 0 || n >= (int)sizeof(pat)) return NULL;
    p = strstr(body, pat);
    if (!p) return NULL;
    p += n;
    while (*p == ' ' || *p == ':') p++;      /* 跳过冒号和空白 */
    return p;
}

/** 取字符串字段；成功时拷贝到 out（截断到 63 字节）并返回 true。 */
static bool json_get_str(const char *body, const char *key,
                         char *out, size_t out_len)
{
    const char *p = json_find(body, key);

    if (!out || out_len == 0) return false;
    out[0] = '\0';
    if (!p || *p != '"') return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1)
    {
        /* 只处理基本转义；其余原样拷贝。SSID 里出现的 \ 之类按原样即可 */
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/**
 * @brief  从请求体取出整数字段，支持负数。
 * @param  body  请求体字符串
 * @param  key   键名
 * @param  out   输出，成功时写入解析到的整数
 * @return 解析成功返回 true；键缺失、非数字或 out 为空返回 false。
 */
static bool json_get_int(const char *body, const char *key, int *out)
{
    const char *p = json_find(body, key);

    if (!out || !p || (*p != '-' && (*p < '0' || *p > '9'))) return false;
    *out = (int)strtol(p, NULL, 10);
    return true;
}

/**
 * @brief  JSON 字符串转义：将 " 与 \ 转义，控制字符换成 \uXXXX。
 *
 * 必须转义：SSID 是用户经网页写入 NVS 的值，若含 " 或 \ 会破坏 JSON
 * 结构，导致前端 JSON.parse 抛异常、页面状态栏失效。
 * \b \f \n \r \t 五个控制字符也必须转义。
 * 其余 >= 0x20 的字节原样保留（UTF-8 多字节序列不能拆开）。
 */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t i = 0;

    if (!out || out_len == 0) return;
    if (!in) in = "";

    while (*in && i < out_len - 7)
    {
        unsigned char c = (unsigned char)*in;

        switch (c)
        {
        case '"':  out[i++] = '\\'; out[i++] = '"';  in++; break;
        case '\\': out[i++] = '\\'; out[i++] = '\\'; in++; break;
        case '\b': out[i++] = '\\'; out[i++] = 'b';  in++; break;
        case '\f': out[i++] = '\\'; out[i++] = 'f';  in++; break;
        case '\n': out[i++] = '\\'; out[i++] = 'n';  in++; break;
        case '\r': out[i++] = '\\'; out[i++] = 'r';  in++; break;
        case '\t': out[i++] = '\\'; out[i++] = 't';  in++; break;
        default:
            if (c < 0x20)
            {
                i += (size_t)snprintf(out + i, out_len - i, "\\u%04x", c);
                in++;
            }
            else
            {
                out[i++] = (char)c;
                in++;
            }
        }
    }
    out[i] = '\0';
}

/**
 * @brief  把秒数格式化为 "3天 02:14:05" 或 "02:14:05"。
 * @param  s    时长（秒）
 * @param  out  输出缓冲
 * @param  len  缓冲长度
 */
static void fmt_uptime(uint32_t s, char *out, size_t len)
{
    uint32_t d = s / 86400, h = (s % 86400) / 3600;
    uint32_t m = (s % 3600) / 60, ss = s % 60;

    if (d > 0)
        snprintf(out, len, "%ud %02u:%02u:%02u",
                 (unsigned)d, (unsigned)h, (unsigned)m, (unsigned)ss);
    else
        snprintf(out, len, "%02u:%02u:%02u",
                 (unsigned)h, (unsigned)m, (unsigned)ss);
}

/**
 * @brief  返回固件版本串（C3_BRIDGE_VERSION）。
 * @return 指向编译期常量字符串，无需释放。
 * @note   版本宏定义在 web_server.h，c3_bridge_main.c 亦引用它。
 */
const char *web_version(void)
{
    return C3_BRIDGE_VERSION;
}

/**
 * @brief  GET /api/status
 * @return 返回全部状态 JSON，字段含 online/wifi_state/ssid/ip/rssi/
 *         link_alive/time_valid/time_str/version/slot/uptime/heap/
 *         rep_ok/rep_fail/frames/badcrc/rx_bytes/ota_state/ota_pct/
 *         ota_err/mode/sta_ssid/ap_ssid。网页每 1 秒轮询一次。
 */
static esp_err_t h_status(httpd_req_t *req)
{
    char buf[1400];
    wifi_cfg_t  cfg;
    uint32_t ok = 0, fail = 0, bytes = 0, frames = 0, badcrc = 0;
    ota_state_t ost;
    int  opct = 0;
    const char *oerr = NULL;
    int  wifi_st;
    char up[32] = "—";
    char tstr[40] = "";
    char ssid_j[80] = "";      /* SSID 的 JSON 转义结果 */
    char sta_j[80] = "";
    char ap_j[80] = "";
    int64_t ts = 0;
    const esp_partition_t *part;

    wifi_cfg_get(&cfg);
    reporter_get_counters(&ok, &fail);
    ns800_link_get_stats(&bytes, &frames, &badcrc);
    ota_mgr_get_state(&ost, &opct, &oerr);
    wifi_st = (int)wifi_mgr_state();

    fmt_uptime((uint32_t)(esp_timer_get_time() / 1000000ULL), up, sizeof(up));

    /* time_mgr_local_unix() 已加过 +8h，这里只做拆解格式化。
     * 时区换算只在 time_mgr 一处做，本层不再叠加。 */
    ts = time_mgr_local_unix();
    if (ts > 0)
    {
        time_t t = (time_t)ts;
        struct tm tmv;
        if (gmtime_r(&t, &tmv))          /* 用 gmtime_r：时间戳已是本地时区 */
            strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", &tmv);
    }

    /* 运行槽：取 esp_ota_get_running_partition() 的 label（ota_0 / ota_1）。 */
    part = esp_ota_get_running_partition();

    /* 版本号取项目里的 C3_BRIDGE_VERSION，而非 esp_app_get_description()->version：
     * 后者是 IDF 的 PROJECT_VER，因 partitions.csv 手写分区、未在 CMake 设
     * PROJECT_VER，编译出来是 "1"，毫无信息量。C3_BRIDGE_VERSION 与 NS800 侧
     * NS800_CMD_HELLO 上报的是同一值，网页与串口日志必须一致。 */
    const char *ver = web_version();

    /* SSID 与 ota_err 必须做 JSON 转义，不能直接拼入格式串。
     * 两者均来自外部可写处：SSID 来自 NVS（网页可改），ota_err 来自
     * HTTP 响应解析。SSID 中只要出现 " 或 \ 就会破坏 JSON 结构，
     * 导致前端 JSON.parse 抛异常、整个页面失效。统一走 json_escape()。 */
    {
        const char *cur = wifi_mgr_ssid();
        json_escape(cur ? cur : "", ssid_j, sizeof(ssid_j));
    }
    json_escape(cfg.sta_ssid, sta_j, sizeof(sta_j));
    json_escape(cfg.ap_ssid,  ap_j,  sizeof(ap_j));
    /* version/slot/up/oerr 都是编译期常量或纯 ASCII 数字，天然安全 */
    char err_j[96] = "";
    json_escape(oerr ? oerr : "", err_j, sizeof(err_j));

    snprintf(buf, sizeof(buf),
        "{\"online\":%s,\"wifi_state\":%d,\"ssid\":\"%s\",\"ip\":\"%s\","
        "\"rssi\":%d,\"link_alive\":%s,\"time_valid\":%s,\"time_str\":\"%s\","
        "\"version\":\"%s\",\"slot\":\"%s\",\"uptime\":\"%s\",\"heap\":\"%lu\","
        "\"rep_ok\":%lu,\"rep_fail\":%lu,\"frames\":%lu,\"badcrc\":%lu,"
        /* bytes 是 UART 驱动层交付的原始字节数（不过状态机），联调很有用：
         * bytes=0 表示物理层无信号；bytes>0 但 frames=0 表示字节到了却解析
         * 不出来（波特率/帧格式/CRC 域问题）。只看 frames+badcrc 两者同为 0
         * 时无法区分这两种故障。 */
        "\"rx_bytes\":%lu,"
        "\"ota_state\":%d,\"ota_pct\":%d,\"ota_err\":\"%s\","
        "\"mode\":%u,\"sta_ssid\":\"%s\",\"ap_ssid\":\"%s\"}",

        wifi_mgr_is_online() ? "true" : "false",
        wifi_st,
        ssid_j,
        wifi_mgr_ip(),
        (int)wifi_mgr_rssi(),
        ns800_link_peer_alive() ? "true" : "false",
        (ts > 0) ? "true" : "false",
        tstr,
        ver,
        part ? part->label : "?",
        up,
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)ok, (unsigned long)fail,
        (unsigned long)frames, (unsigned long)badcrc,
        (unsigned long)bytes,
        (int)ost, opct, err_j,
        (unsigned)wifi_cfg_mode(),
        sta_j, ap_j);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/**
 * @brief  延后应用新 WiFi 配置（在独立任务中执行）。
 *
 * 不能直接调用 wifi_cfg_apply()：它内部会同步调 wifi_mgr_apply_cfg() 执行
 * stop + start WiFi 驱动，在 STA 模式下驱动一重启 TCP 连接立刻断开，而此时
 * HTTP 响应尚未发出，客户端会看到"保存失败"，但配置其实已写入 NVS 并生效。
 * 正确时序：写 NVS → 发响应 → 等 TCP 把响应送出 → 再断 WiFi。
 * 因此把驱动重配置拆出，延后 800ms 单独做。800ms 为经验值：WiFi 重连通常
 * 3~5 秒，而 TCP 响应只需数十毫秒即可送出，兼顾"响应必达"与"无感延迟"。
 */
static void apply_wifi_later(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGI(TAG, "延迟应用新 WiFi 配置");
    esp_err_t err = wifi_mgr_apply_cfg();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "应用新配置失败: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "新配置已应用");
    vTaskDelete(NULL);
}

/**
 * @brief  POST /api/wifi
 * @return 接收 JSON body（mode/ssid/pass/apssid/appass），仅写入 NVS，
 *         返回 {"ok":bool,"msg":str}；实际 WiFi 重配置由延时任务完成。
 */
static esp_err_t h_wifi_post(httpd_req_t *req)
{
    char body[MAX_REQ_BODY + 1] = {0};
    char ssid[NS800_WIFI_SSID_MAX + 1], pass[NS800_WIFI_PASS_MAX + 1];
    char apssid[NS800_WIFI_SSID_MAX + 1], appass[NS800_WIFI_PASS_MAX + 1];
    int  mode = 0;
    int  n = req->content_len;

    if (n <= 0 || n > MAX_REQ_BODY)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"请求体大小非法\"}");
        return ESP_OK;
    }
    if (httpd_req_recv(req, body, n) != n)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"接收请求体失败\"}");
        return ESP_OK;
    }
    body[n] = '\0';

    /* 表单里留空的密码 = "不修改这一项"，传 NULL 给 wifi_cfg_apply。
     * 网页的密码框默认就是空的（见 web_page.h 的 placeholder）。 */
    memset(ssid, 0, sizeof(ssid));
    memset(pass, 0, sizeof(pass));
    memset(apssid, 0, sizeof(apssid));
    memset(appass, 0, sizeof(appass));

    json_get_str(body, "ssid",   ssid,   sizeof(ssid));
    json_get_str(body, "pass",   pass,   sizeof(pass));
    json_get_str(body, "apssid", apssid, sizeof(apssid));
    json_get_str(body, "appass", appass, sizeof(appass));
    json_get_int(body, "mode",   &mode);

    /* 用 store_only 而非 apply：apply 会在返回前 stop/start WiFi 驱动，
     * STA 下 TCP 当场断开、响应发不出去（见 apply_wifi_later 注释）。
     * 驱动重配置交给延时任务。 */
    esp_err_t err = wifi_cfg_store_only(
        (uint8_t)mode,
        ssid[0]   ? ssid   : NULL,
        pass[0]   ? pass   : NULL,
        apssid[0] ? apssid : NULL,
        appass[0] ? appass : NULL);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (err == ESP_OK)
        httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"已保存，WiFi 即将重连\"}");
    else
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"参数非法或NVS写入失败\"}");

    ESP_LOGI(TAG, "网页改 WiFi: mode=%d ssid=%s -> %s",
             mode, ssid, esp_err_to_name(err));

    /* 只在保存成功后才启动 WiFi 重配置；保存失败则不动网络。 */
    if (err == ESP_OK)
    {
        BaseType_t r = xTaskCreate(apply_wifi_later, "wifi_apply", 3072,
                                   NULL, 4, NULL);
        if (r != pdPASS)
            ESP_LOGE(TAG, "建延时任务失败，配置已存但本次不生效（下次开机会生效）");
    }
    return ESP_OK;
}

/**
 * @brief  POST /api/ota
 * @return 接收 {"url":"..."}，对 URL 拉取式 OTA 发起请求，
 *         返回 {"ok":bool,"msg":str}。ESP_ERR_INVALID_STATE 表示已有升级在进行中。
 * @note   本层仅校验 url 首字符为 'h'，完整 scheme 合法性由 ota_mgr 判定。
 */
static esp_err_t h_ota_post(httpd_req_t *req)
{
    char body[MAX_REQ_BODY + 1] = {0};
    char url[256] = "";
    int  n = req->content_len;

    if (n <= 0 || n > MAX_REQ_BODY)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"请求体大小非法\"}");
        return ESP_OK;
    }
    if (httpd_req_recv(req, body, n) != n)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"接收请求体失败\"}");
        return ESP_OK;
    }
    body[n] = '\0';

    json_get_str(body, "url", url, sizeof(url));
    if (url[0] != 'h')
    {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"URL 必须以 http:// 开头\"}");
        return ESP_OK;
    }

    esp_err_t err = ota_mgr_start(url);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (err == ESP_OK)
        httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"已开始\"}");
    else if (err == ESP_ERR_INVALID_STATE)
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"已有升级在进行中\"}");
    else
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"启动失败\"}");

    ESP_LOGI(TAG, "网页触发 OTA: %s -> %s", url, esp_err_to_name(err));
    return ESP_OK;
}

/**
 * @brief  POST /api/ota/upload（本地文件上传）
 *
 * 请求体为整个 .bin 文件，Content-Type: application/octet-stream。
 * 浏览器端用 XMLHttpRequest 上传以获得真实进度（fetch 难以获取）。
 * 必须流式处理：固件约 1MB，而 C3 仅 400KB SRAM，整块缓冲会导致
 * malloc 失败甚至重启；每收到一块即 esp_ota_write()，RAM 占用恒定 4KB。
 * 中途出错必须调用 ota_mgr_upload_abort()（内部 esp_ota_abort 丢弃半个
 * 镜像）；绝不可在写了半截镜像时 end()+set_boot，否则 bootloader 跳入
 * 半个固件，设备变砖。
 */
static esp_err_t h_ota_upload(httpd_req_t *req)
{
    /* 此处不可声明 char buf[4096] 这类大局部数组：本函数运行在 httpd 的
     * server task 中，其栈默认仅 CONFIG_ESP_HTTPD_TASK_STACK_SIZE = 4096
     * 字节，局部数组铺满即爆栈，表现为接口连不上/返回空但不崩溃。
     * 因此缓冲分配到堆上，退出前释放。 */
    char  *buf;
    size_t total = 0;
    esp_err_t err;

    err = ota_mgr_upload_begin(req->content_len);
    if (err != ESP_OK)
    {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        if (err == ESP_ERR_INVALID_STATE)
            httpd_resp_sendstr(req,
                "{\"ok\":false,\"msg\":\"已有升级在进行中，请等它结束\"}");
        else if (err == ESP_ERR_INVALID_SIZE)
            httpd_resp_sendstr(req,
                "{\"ok\":false,\"msg\":\"固件文件太大，装不进 1.5MB 的槽\"}");
        else
            httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"无法开始，请看串口日志\"}");
        /* 客户端还在发 body，这里不等它发完 —— httpd 会自己收尾。
         * 不 return 的话 keep-alive 会错位，浏览器看到的是空响应。 */
        return ESP_OK;
    }

    buf = malloc(OTA_UP_CHUNK);
    if (!buf)
    {
        ota_mgr_upload_abort();
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"内存不足\"}");
        return ESP_OK;
    }

    /* ② 流式收包 → 投进 OTA 管道（擦/写都在 ota task 里做）。
     *    recv 返回 0 = 收完；<0 = 出错。 */
    while (1)
    {
        int n = httpd_req_recv(req, (char *)buf, sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;          /* 只是这一拍没数据，继续等（别当失败） */
        if (n <= 0)
        {
            if (n < 0)
            {
                ESP_LOGE(TAG, "上传中断：recv 返回 %d", n);
                err = ESP_FAIL;
            }
            break;             /* n==0 = 正常收完 */
        }

        err = ota_mgr_upload_chunk((const uint8_t *)buf, (size_t)n);
        if (err != ESP_OK)
            break;             /* 内部已经 abort + set_state */

        total += (size_t)n;
    }

    /* ③ 收尾 */
    if (err == ESP_OK)
        err = ota_mgr_upload_end();

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "上传完成 %u 字节", (unsigned)total);
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req,
            "{\"ok\":true,\"msg\":\"固件上传完成，5 秒后自动重启\"}");
        /* 让 NS800 先把"即将重启"显示出来，再切换。
         * 此处不调 esp_task_wdt_reset()：httpd task 默认不在 WDT 订阅表内，
         * 调用只会刷出 "task not found" 日志。这 5 秒为纯 vTaskDelay
         * （阻塞态），idle 任务正常运行，idle WDT 不会超时，无需额外喂狗。 */
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        free(buf);           /* 实际走不到（esp_restart 不返回），仅为对称 */
        return ESP_OK;
    }

    /* 失败路径：确保半个镜像被丢掉 */
    ota_mgr_upload_abort();
    ESP_LOGE(TAG, "上传失败: %s", esp_err_to_name(err));
    free(buf);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req,
        "{\"ok\":false,\"msg\":\"上传失败，固件无效或槽已满（设备不会变砖）\"}");
    return ESP_OK;
}

/**
 * @brief  GET /api/ping
 * @return 物理层活性探针：绕过 UART 直读 RX 引脚电平（原理见 link_ns800.c），
 *         返回 {"ok":true,"flips_rx":uint,"ms":1000,"verdict":str}。
 *         flips==0 表示引脚上无波形（查接线/引脚号/短接线未拔）；
 *         flips>0 表示波形存在，问题在 UART 侧（波特率/共地/接错相邻脚）。
 *         副作用：调用期间（1 秒）UART RX 短暂失效，测完自动恢复。
 */
static esp_err_t h_ping(httpd_req_t *req)
{
    uint32_t flips = 0;
    char buf[256];

    if (ns800_link_rx_pin_probe(1000, &flips) != ESP_OK)
    {
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"链路未初始化\"}");
        return ESP_OK;
    }
    /* 判定：TX-RX 短路时 flips_rx 必然 > 0（TX 波形经短路出现在 RX 上）。
     *   所以这个接口本身就是自检的，不需要额外对照组：
     *     flips_rx  > 0 → RX 脚有电平活动（短路时属正常；不短路时说明外部有波形）
     *     flips_rx == 0 → RX 脚上恒定电平，没有任何波形 */
    const char *verdict = (flips > 0)
        ? "RX 脚有电平翻转：引脚是活的（短路时应如此；已接 NS800 时说明物理层通）"
        : "RX 脚无翻转：引脚恒定电平。请确认 TX-RX 是否已短接 —— 若已短接则探针异常，若未短接则外部无波形";

    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"flips_rx\":%lu,\"ms\":1000,\"verdict\":\"%s\"}",
             (unsigned long)flips, verdict);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/**
 * @brief  POST /api/reboot
 * @return 返回 {"ok":true,"msg":"正在重启"}，随后延时 300ms 调用 esp_restart()。
 */
static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"正在重启\"}");
    ESP_LOGI(TAG, "网页触发重启");
    /* 延时重启：先把响应发出去，否则浏览器会以为请求失败了 */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/**
 * @brief  GET / 返回 HTML 管理页（WEB_PAGE_HTML）。
 */
static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, WEB_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * @brief  GET /api/host_data
 * @return NS800 → C3 通用数据上行通道（NS800_CMD_HOST_DATA 0x88）的落地结果，
 *         返回 {"n":uint,"rx":uint,"last_raw":str,"kv":[{"k","v","seq"}]}。
 *         key/value 均来自串口（外部输入），必须经 json_escape，否则
 *         若 NS800 推了带引号的 key，前端 JSON.parse 会失败。
 */
static esp_err_t h_host_data(httpd_req_t *req)
{
    host_data_kv_t tbl[HOST_DATA_SLOTS];
    char body[768];
    size_t off = 0;
    uint32_t n, rx;

    n  = host_data_snapshot(tbl, HOST_DATA_SLOTS);
    rx = host_data_rx_count();

    off += (size_t)snprintf(body + off, sizeof(body) - off,
                            "{\"n\":%lu,\"rx\":%lu,\"last_raw\":\"",
                            (unsigned long)n, (unsigned long)rx);
    {
        char raw_j[160];
        json_escape(host_data_last_raw(), raw_j, sizeof(raw_j));
        off += (size_t)snprintf(body + off, sizeof(body) - off, "%s", raw_j);
    }
    off += (size_t)snprintf(body + off, sizeof(body) - off, "\",\"kv\":[");

    for (uint32_t i = 0; i < n && off < sizeof(body) - 160; i++)
    {
        char k_j[96], v_j[96];
        json_escape(tbl[i].key, k_j, sizeof(k_j));
        json_escape(tbl[i].val, v_j, sizeof(v_j));
        off += (size_t)snprintf(body + off, sizeof(body) - off,
                                "%s{\"k\":\"%s\",\"v\":\"%s\",\"seq\":%lu}",
                                (i ? "," : ""), k_j, v_j,
                                (unsigned long)tbl[i].seq);
    }
    off += (size_t)snprintf(body + off, sizeof(body) - off, "]}");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/**
 * @brief  启动 HTTP 管理服务并注册全部路由；幂等。
 * @return ESP_OK 成功（含已启动的情形）；httpd_start 失败返回 ESP_FAIL。
 */
esp_err_t web_server_start(void)
{
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t h      = NULL;
    httpd_uri_t    uri;

    if (s_started) return ESP_OK;

    /* 页面要能在 WiFi 切 AP 模式时仍被访问，所以最大连接数给 4 足够 */
    cfg.max_uri_handlers = 12;   /* 覆盖全部 8 个路由并留余量 */
    cfg.max_open_sockets  = 4;
    /* 栈给到 6144：默认 4096 过紧，handler 中任何稍大的局部数组都可能
     * 踩边界（踩了不崩，仅表现为返回空/连不上，难以排查）。 */
    cfg.stack_size       = 6144;
    /* 单次 recv 等待上限 5 秒；请求体大小改由 MAX_REQ_BODY 在 handler 内限制 */
    cfg.recv_wait_timeout = 5;
    cfg.lru_purge_enable  = true;

    if (httpd_start(&h, &cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start 失败");
        return ESP_FAIL;
    }

    memset(&uri, 0, sizeof(uri));
    uri.uri      = "/";
    uri.method   = HTTP_GET;
    uri.handler  = h_root;
    httpd_register_uri_handler(h, &uri);

    memset(&uri, 0, sizeof(uri));
    uri.uri      = "/api/status";
    uri.method   = HTTP_GET;
    uri.handler  = h_status;
    httpd_register_uri_handler(h, &uri);

    memset(&uri, 0, sizeof(uri));
    uri.uri      = "/api/ping";      /* 物理层活性探针，见 h_ping 注释 */
    uri.method   = HTTP_GET;
    uri.handler  = h_ping;
    httpd_register_uri_handler(h, &uri);

    memset(&uri, 0, sizeof(uri));
    uri.uri      = "/api/host_data";  /* NS800 上行数据（0x88）的可视化出口 */
    uri.method   = HTTP_GET;
    uri.handler  = h_host_data;
    httpd_register_uri_handler(h, &uri);

    memset(&uri, 0, sizeof(uri));
    uri.uri      = "/api/wifi";
    uri.method   = HTTP_POST;
    uri.handler  = h_wifi_post;
    httpd_register_uri_handler(h, &uri);

    memset(&uri, 0, sizeof(uri));
    uri.uri      = "/api/ota";
    uri.method   = HTTP_POST;
    uri.handler  = h_ota_post;
    httpd_register_uri_handler(h, &uri);

    memset(&uri, 0, sizeof(uri));
    uri.uri      = "/api/ota/upload";
    uri.method   = HTTP_POST;
    uri.handler  = h_ota_upload;
    httpd_register_uri_handler(h, &uri);

    memset(&uri, 0, sizeof(uri));
    uri.uri      = "/api/reboot";
    uri.method   = HTTP_POST;
    uri.handler  = h_reboot;
    httpd_register_uri_handler(h, &uri);

    s_started = true;
    ESP_LOGI(TAG, "HTTP 管理页已启动: http://<本机IP>/ (端口 80，无鉴权，仅限局域网)");
    return ESP_OK;
}