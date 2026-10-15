/**
 * @file    wifi_cfg.c
 * @brief   C3 WiFi 配置的 NVS 持久化与校验
 *
 * 本模块只负责存储：读写 NVS、校验长度、提供内存缓存，不调用
 * esp_wifi_xxx；WiFi 驱动的重配由 wifi_mgr 统一执行。
 * 原因：wifi_cfg 与 wifi_mgr 若都去调 esp_wifi_set_mode / set_config /
 * start / stop，事件回调会交错，出现“设到一半被对方改了”的竞态。
 * 因此这里只把配置落到 NVS 与内存，再调 wifi_mgr_apply_cfg() 交由唯一的
 * 驱动管理方应用。
 */

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_cfg.h"
#include "wifi_mgr.h"

static const char *TAG = "wificfg";

#define NVS_NS      "wificfg"     /* NVS 命名空间 */
#define KEY_MODE    "mode"
#define KEY_STA_SSID "sta_s"
#define KEY_STA_PASS "sta_p"
#define KEY_AP_SSID  "ap_s"
#define KEY_AP_PASS  "ap_p"

static wifi_cfg_t s_cfg;
static bool       s_inited;

/* 前向声明：定义在后面，但 init 里要用 */
static esp_err_t wifi_cfg_save_only(void);

/**
 * @brief  从 NVS 读取一个字符串键，键不存在时用默认值兜底
 * @param  h       已打开的 NVS 句柄
 * @param  key     NVS 键名
 * @param  out     输出缓冲
 * @param  out_sz  缓冲大小，含结尾 '\0'
 * @param  def     键不存在时的默认值；NULL 表示置空串
 */
static void nvs_read_str(nvs_handle_t h, const char *key,
                         char *out, size_t out_sz, const char *def)
{
    size_t len = out_sz;

    if (nvs_get_str(h, key, out, &len) != ESP_OK)
    {
        /* 键不存在则用默认值；def 为 NULL 时置空串 */
        if (def) strncpy(out, def, out_sz - 1);
        else     out[0] = '\0';
        out[out_sz - 1] = '\0';
    }
}

static esp_err_t nvs_write_str(nvs_handle_t h, const char *key, const char *val)
{
    return nvs_set_str(h, key, val ? val : "");
}

/**
 * @brief  取当前生效配置（拷贝一份到 out）
 * @param  out  输出缓冲，不可为 NULL
 */
void wifi_cfg_get(wifi_cfg_t *out)
{
    if (out) *out = s_cfg;
}

/**
 * @brief  取当前工作模式（不读 NVS，直接返回内存缓存）
 * @return NS800_WIFI_MODE_STA 或 NS800_WIFI_MODE_AP
 */
uint8_t wifi_cfg_mode(void)
{
    return s_cfg.mode;
}

/**
 * @brief  初始化：从 NVS 载入配置，无则写入默认值。幂等。
 * @return ESP_OK 成功
 * @note   NVS 已在 wifi_mgr_init 中初始化，这里再调一次 nvs_flash_init()
 *         仅为幂等兜底，防止调用顺序颠倒。
 */
esp_err_t wifi_cfg_init(void)
{
    nvs_handle_t h;
    uint8_t mode = NS800_WIFI_MODE_STA;
    esp_err_t err;

    if (s_inited) return ESP_OK;

    /* NVS 已在 wifi_mgr_init 里初始化过；这里再调一次幂等，防调用顺序颠倒 */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS 命名空间打不开: %s", esp_err_to_name(err));
        return err;
    }

    /* 读模式（键不存在 → STA） */
    bool mode_key_present = (nvs_get_u8(h, KEY_MODE, &mode) == ESP_OK);
    if (!mode_key_present) mode = NS800_WIFI_MODE_STA;
    if (mode != NS800_WIFI_MODE_STA && mode != NS800_WIFI_MODE_AP) mode = NS800_WIFI_MODE_STA;
    s_cfg.mode = mode;

    /* 读四组凭据，各自带默认值兜底 */
    nvs_read_str(h, KEY_STA_SSID, s_cfg.sta_ssid, sizeof(s_cfg.sta_ssid),
                 WIFI_CFG_DEFAULT_STA_SSID);
    nvs_read_str(h, KEY_STA_PASS, s_cfg.sta_pass, sizeof(s_cfg.sta_pass),
                 WIFI_CFG_DEFAULT_STA_PASS);
    nvs_read_str(h, KEY_AP_SSID,  s_cfg.ap_ssid,  sizeof(s_cfg.ap_ssid),
                 WIFI_CFG_DEFAULT_AP_SSID);
    nvs_read_str(h, KEY_AP_PASS,  s_cfg.ap_pass,  sizeof(s_cfg.ap_pass),
                 WIFI_CFG_DEFAULT_AP_PASS);

    nvs_close(h);
    s_inited = true;

    ESP_LOGI(TAG, "配置载入: mode=%s STA=%s AP=%s",
             s_cfg.mode == NS800_WIFI_MODE_AP ? "AP" : "STA",
             s_cfg.sta_ssid, s_cfg.ap_ssid);

    /* 首次开机（键不存在）时把默认值写回 NVS，
     * 使界面显示与生效配置一致，单独修改某项也不会丢失其余项。 */
    if (!mode_key_present)
    {
        ESP_LOGI(TAG, "首次开机，写入出厂默认配置");
        wifi_cfg_save_only();
    }
    return ESP_OK;
}

/* 把当前 s_cfg 落盘（内部用；wifi_cfg_apply 也复用） */
static esp_err_t wifi_cfg_save_only(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);

    if (err != ESP_OK) return err;

    nvs_set_u8(h, KEY_MODE, s_cfg.mode);
    nvs_write_str(h, KEY_STA_SSID, s_cfg.sta_ssid);
    nvs_write_str(h, KEY_STA_PASS, s_cfg.sta_pass);
    nvs_write_str(h, KEY_AP_SSID,  s_cfg.ap_ssid);
    nvs_write_str(h, KEY_AP_PASS,  s_cfg.ap_pass);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/**
 * @brief  按长度上限校验并拷贝一个字符串字段
 * @param  dst    目标缓冲
 * @param  dst_sz 目标缓冲大小
 * @param  src    源串；NULL 表示不改这一项
 * @param  max    允许的最大长度（不含 '\0'）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 超长
 * @note   src 长度 > max 或 >= dst_sz 均判为超长；先清空再拷贝以避免残留旧值。
 */
static esp_err_t copy_limited(char *dst, size_t dst_sz, const char *src, size_t max)
{
    size_t n;

    if (!src) return ESP_OK;                  /* NULL = 不改这一项 */
    n = strlen(src);
    if (n > max || n >= dst_sz) return ESP_ERR_INVALID_ARG;

    /* 先清空再拷贝，保证不残留旧值的尾巴 */
    memset(dst, 0, dst_sz);
    memcpy(dst, src, n);
    return ESP_OK;
}

esp_err_t wifi_cfg_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err;

    /* IDF v6.1 没有 nvs_flash_erase_key()，只有擦整个分区的
     * nvs_flash_erase_partition()；后者会抹掉 NVS 内所有命名空间，
     * 含 WiFi 校准数据 phy_init 等，属于“出厂维修”级手段。
     * 因此这里用 nvs_erase_key() 逐个删除本模块的键，影响范围最小且可逆。
     * 删完后把 s_inited 清掉，下一次 wifi_cfg_init() 会重走“键不存在”
     * 分支并自动写回出厂默认值。 */
    err = nvs_flash_init();
    if (err != ESP_OK) return err;

    err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "打不开 %s: %s", NVS_NS, esp_err_to_name(err));
        return err;
    }

    {
        static const char *keys[] = {
            KEY_MODE, KEY_STA_SSID, KEY_STA_PASS, KEY_AP_SSID, KEY_AP_PASS
        };
        int i;
        for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
        {
            esp_err_t e = nvs_erase_key(h, keys[i]);
            /* ESP_ERR_NVS_NOT_FOUND = 本来就没有，正好是我们想要的状态 */
            if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
                ESP_LOGW(TAG, "删键 %s 失败: %s", keys[i], esp_err_to_name(e));
        }
    }
    nvs_commit(h);
    nvs_close(h);

    s_inited = false;      /* 关键：让下一次 wifi_cfg_init() 重新载入并写默认值 */

    ESP_LOGW(TAG, "WiFi 配置已恢复出厂默认（下次 init 生效）");
    return ESP_OK;
}

/**
 * @brief  仅校验并写 NVS，不重配 WiFi 驱动
 * @param  mode     目标模式
 * @param  sta_ssid STA SSID，NULL 表示不改此项
 * @param  sta_pass STA 密码
 * @param  ap_ssid  AP 热点名
 * @param  ap_pass  AP 热点密码
 * @return ESP_OK 已写入 NVS（不代表已生效，生效需另调 wifi_mgr_apply_cfg）
 * @note   供 HTTP 管理页使用：wifi_cfg_apply() 末尾会 stop/start 驱动，
 *         在 STA 下会立即断开 TCP，导致 HTTP 响应来不及发出。
 *         故网页走本函数落盘，响应发完后再由延时任务调 wifi_mgr_apply_cfg()。
 */
esp_err_t wifi_cfg_store_only(uint8_t mode,
                              const char *sta_ssid, const char *sta_pass,
                              const char *ap_ssid,  const char *ap_pass)
{
    /* 校验与写 NVS 的规则与 wifi_cfg_apply 逐条一致，改一处须同步改两处。 */
    if (mode != NS800_WIFI_MODE_STA && mode != NS800_WIFI_MODE_AP)
        return ESP_ERR_INVALID_ARG;

    if (copy_limited(s_cfg.sta_ssid, sizeof(s_cfg.sta_ssid), sta_ssid, NS800_WIFI_SSID_MAX) != ESP_OK ||
        copy_limited(s_cfg.sta_pass, sizeof(s_cfg.sta_pass), sta_pass, NS800_WIFI_PASS_MAX) != ESP_OK ||
        copy_limited(s_cfg.ap_ssid,  sizeof(s_cfg.ap_ssid),  ap_ssid,  NS800_WIFI_SSID_MAX) != ESP_OK ||
        copy_limited(s_cfg.ap_pass,  sizeof(s_cfg.ap_pass),  ap_pass,  NS800_WIFI_PASS_MAX) != ESP_OK)
        return ESP_ERR_INVALID_ARG;

    s_cfg.mode = mode;
    if (s_cfg.sta_ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_cfg.ap_ssid[0]  == '\0') return ESP_ERR_INVALID_ARG;

    return wifi_cfg_save_only();
}

/**
 * @brief  保存并应用一份新配置
 * @param  mode     目标模式
 * @param  sta_ssid STA SSID，NULL 表示不改此项
 * @param  sta_pass STA 密码
 * @param  ap_ssid  AP 热点名
 * @param  ap_pass  AP 热点密码
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法
 * @note   流程：校验长度，写 NVS，重配 WiFi 驱动（wifi_mgr_apply_cfg）。
 */
esp_err_t wifi_cfg_apply(uint8_t mode,
                         const char *sta_ssid, const char *sta_pass,
                         const char *ap_ssid,  const char *ap_pass)
{
    if (mode != NS800_WIFI_MODE_STA && mode != NS800_WIFI_MODE_AP)
        return ESP_ERR_INVALID_ARG;

    if (copy_limited(s_cfg.sta_ssid, sizeof(s_cfg.sta_ssid), sta_ssid, NS800_WIFI_SSID_MAX) != ESP_OK ||
        copy_limited(s_cfg.sta_pass, sizeof(s_cfg.sta_pass), sta_pass, NS800_WIFI_PASS_MAX) != ESP_OK ||
        copy_limited(s_cfg.ap_ssid,  sizeof(s_cfg.ap_ssid),  ap_ssid,  NS800_WIFI_SSID_MAX) != ESP_OK ||
        copy_limited(s_cfg.ap_pass,  sizeof(s_cfg.ap_pass),  ap_pass,  NS800_WIFI_PASS_MAX) != ESP_OK)
    {
        ESP_LOGE(TAG, "配置过长（SSID≤%u / 密码≤%u）",
                 (unsigned)NS800_WIFI_SSID_MAX, (unsigned)NS800_WIFI_PASS_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg.mode = mode;

    /* STA 模式下 SSID 不能为空（AP 模式允许空密码，但 SSID 也不能空） */
    if (s_cfg.sta_ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_cfg.ap_ssid[0]  == '\0') return ESP_ERR_INVALID_ARG;

    esp_err_t err = wifi_cfg_save_only();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "写 NVS 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "配置已保存: mode=%s",
             mode == NS800_WIFI_MODE_AP ? "AP" : "STA");

    /* 交由唯一的驱动管理方 wifi_mgr 应用配置 */
    return wifi_mgr_apply_cfg();
}

/**
 * @brief  切换工作模式并持久化（其余字段保持不变）
 * @param  mode 目标模式
 * @return ESP_OK 成功
 * @note   其余字段以 NULL 传入，由 copy_limited 跳过，只更新模式。
 */
esp_err_t wifi_cfg_set_mode(uint8_t mode)
{
    if (mode != NS800_WIFI_MODE_STA && mode != NS800_WIFI_MODE_AP)
        return ESP_ERR_INVALID_ARG;
    return wifi_cfg_apply(mode, NULL, NULL, NULL, NULL);
}
