/**
 * @file    wifi_cfg.h
 * @brief   C3 的 WiFi 配置持久化（NVS）与模式定义
 *
 * 持久化只存“用户意图”（想连谁 / 想开什么热点），不存连接结果，
 * 结果每次开机重算，避免存下过期状态。
 * 配置存于 NVS 命名空间 "wificfg"，掉电不丢。
 * 首次开机（NVS 空）用内置默认值兜底：
 *   STA → SSID="gpm" / 密码="12345678"
 *   AP  → SSID="NSSinePad-AP" / 密码="nssinepad"（可在配网界面修改）
 * 默认凭据是出厂预置的产品行为，非硬编码泄露：用户改完存 NVS，
 * 源码中这组仅作首次开机初值。
 */

#ifndef __WIFI_CFG_H__
#define __WIFI_CFG_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "ns800_link.h"      /* NS800_WIFI_MODE_* / SSID_MAX / PASS_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/* 出厂默认值：STA 与 AP 的初始凭据，均可在配网界面修改 */
#define WIFI_CFG_DEFAULT_STA_SSID   "gpm"
#define WIFI_CFG_DEFAULT_STA_PASS   "12345678"
#define WIFI_CFG_DEFAULT_AP_SSID    "NSSinePad-AP"
#define WIFI_CFG_DEFAULT_AP_PASS    "nssinepad"

/** 一份完整的 WiFi 配置（STA 与 AP 的凭据各存一份，互不覆盖）。 */
typedef struct
{
    uint8_t mode;                                  /* NS800_WIFI_MODE_STA / _AP */
    char    sta_ssid[NS800_WIFI_SSID_MAX + 1];     /* 以 '\0' 结尾，方便打印 */
    char    sta_pass[NS800_WIFI_PASS_MAX + 1];      /* 以 '\0' 结尾 */
    char    ap_ssid[NS800_WIFI_SSID_MAX + 1];       /* 以 '\0' 结尾 */
    char    ap_pass[NS800_WIFI_PASS_MAX + 1];       /* 以 '\0' 结尾 */
} wifi_cfg_t;

/** 初始化：从 NVS 载入配置；无则写入默认值。幂等。 */
esp_err_t wifi_cfg_init(void);

/** 取当前生效配置（拷贝一份）。 */
void wifi_cfg_get(wifi_cfg_t *out);

/** 切换工作模式（持久化）。返回 ESP_OK 成功。 */
esp_err_t wifi_cfg_set_mode(uint8_t mode);

/**
 * @brief  保存并应用一份新配置。
 *
 * @param mode      目标模式
 * @param sta_ssid  STA 的 SSID（NULL 表示不改这一项）
 * @param sta_pass  STA 密码
 * @param ap_ssid   AP 热点名
 * @param ap_pass   AP 热点密码
 *
 * 流程：校验长度，写 NVS，重配 WiFi 驱动，按新模式重启。
 */
esp_err_t wifi_cfg_apply(uint8_t mode,
                         const char *sta_ssid, const char *sta_pass,
                         const char *ap_ssid,  const char *ap_pass);

/**
 * @brief  只校验 + 写 NVS，不重配 WiFi 驱动。
 *
 * 存在的唯一理由是 HTTP 管理页：wifi_cfg_apply() 末尾会 stop/start
 * 驱动，STA 下这会让 TCP 当场断开，HTTP 响应来不及发出去 ——
 * 用户点"保存"看到失败，实际配置已生效。
 * 所以网页调本函数落盘，响应发完后再异步调 wifi_mgr_apply_cfg()。
 *
 * 校验规则与 wifi_cfg_apply 完全一致（那份是逐条复制过来的，
 * 改一处必须改两处，否则会出现"网页能存串口不能存"的不一致）。
 *
 * @return ESP_OK 已写入 NVS（不代表已生效，生效需另调 wifi_mgr_apply_cfg）
 */
esp_err_t wifi_cfg_store_only(uint8_t mode,
                              const char *sta_ssid, const char *sta_pass,
                              const char *ap_ssid,  const char *ap_pass);

/**
 * @brief  恢复出厂 WiFi 配置（擦掉 "wificfg" 命名空间，下次 init 写回默认值）。
 * @note   用于网页把自身配断后的恢复：若 STA 的 SSID 填成不存在的 AP，
 *         设备再也连不上网、网页也失联，此时只能靠串口调本函数后重启。
 *         若没有本函数，就只能用 esptool 擦整个 NVS 分区，那是“出厂维修”
 *         级手段，日常调试不应依赖。
 */
esp_err_t wifi_cfg_factory_reset(void);

/** 当前模式（不读 NVS，取内存缓存）。 */
uint8_t wifi_cfg_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_CFG_H__ */
