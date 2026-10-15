/**
 * @file    wifi_mgr.h
 * @brief   ESP32-C3 WiFi STA/AP 联网管理（对外接口）
 *
 * 状态变化通过 ns800_link 自动同步给 NS800（见 wifi_mgr.c 的 notify_ns800）。
 */

#ifndef __WIFI_MGR_H__
#define __WIFI_MGR_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 NVS / netif / 事件循环 / WiFi，并按 NVS 配置启动 STA 或 AP。
 * @return ESP_OK 成功；失败返回对应错误码。
 * @note   幂等：已初始化则直接返回 ESP_OK。
 */
esp_err_t wifi_mgr_init(void);

/**
 * @brief  等待取得 IP。
 * @param  timeout_ms  等待超时，单位毫秒。
 * @return true 已取得 IP；false 超时或尚未初始化。
 * @note   超时只表示该时间段内未拿到 IP，不代表连接失败。
 */
bool wifi_mgr_wait_ip(uint32_t timeout_ms);

/**
 * @brief  查询当前是否已取得 IP（即真正能联网）。
 * @return true 在线；false 未联网或尚未初始化。
 */
bool wifi_mgr_is_online(void);

/**
 * @brief  查询当前 WiFi 状态。
 * @return NS800_WIFI_* 枚举值。
 */
uint8_t wifi_mgr_state(void);

/**
 * @brief  查询当前 IP 字符串。
 * @return 未连上时为 "0.0.0.0"。
 */
const char *wifi_mgr_ip(void);

/**
 * @brief  当前实际连上的 AP 的 SSID（未连上或 AP 模式下返回 ""）。
 *
 * 与 wifi_cfg_get() 的 sta_ssid 区别：后者是"用户想连的"意图，
 * 本函数返回的是"真正连上的"结果；刚改完配置尚未连上时两者会不同。
 *
 * @note  返回值为模块内静态缓冲，非独立拷贝：下一次调用会覆盖。
 *        需要留存须自行 memcpy。不可直接返回 esp_wifi_sta_get_ap_info()
 *        的栈变量指针，该 ap 为栈对象，函数返回即失效，
 *        直接返回会在 -O2 下读到栈垃圾并触发 -Werror=return-local-addr。
 */
const char *wifi_mgr_ssid(void);

/**
 * @brief  查询当前 AP 的 RSSI。
 * @return 信号强度，单位 dBm（负值）；未连上返回 0。
 */
int8_t wifi_mgr_rssi(void);

/**
 * @brief  立即重连：清空重试计数并重新 disconnect + connect。
 */
void wifi_mgr_reconnect_now(void);

/**
 * @brief  按当前 NVS 配置重新应用 WiFi 设置。
 *
 * 由 wifi_cfg 在保存新配置后调用：会 stop → 按新模式 set_config → start。
 * 未初始化时等价于调用 wifi_mgr_init()。
 */
esp_err_t wifi_mgr_apply_cfg(void);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_MGR_H__ */
