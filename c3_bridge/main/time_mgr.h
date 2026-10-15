/**
 * @file    time_mgr.h
 * @brief   C3 侧 NTP 对时，并向 NS800 每秒下发本地时间
 *
 * 时间为何在 C3 侧对再下发给 NS800：
 *   NS800 没有网络能力（联网全靠 C3 这颗“网卡”），NTP 客户端只能跑在 C3。
 *   对时完成后经板间链路（NS800_CMD_TIME 帧）把 Unix 时间戳推给 NS800，
 *   NS800 只做 tm 拆解、不碰时区；时区换算只在本文件一处完成。
 *
 * 时区：固定 UTC+8（中国标准时间）。不引入 setenv("TZ")/localtime_r，
 *   避免多一处全局状态依赖，且 newlib 时区表在 IDF 中需额外配置。
 *   直接对 UTC 时间戳加 8*3600，更直白，也便于 NS800 侧复现。
 */

#ifndef __TIME_MGR_H__
#define __TIME_MGR_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 东八区偏移（秒）。要换时区只改这一个常量。 */
#define TIME_MGR_TZ_OFFSET_SEC   (8 * 3600)

/**
 * @brief  启动 NTP 客户端，并每秒向 NS800 下发时间的任务。幂等。
 * @param  server  NTP 服务器域名；NULL 用默认（ntp.aliyun.com）
 * @return ESP_OK 已启动
 * @note   可在 WiFi 尚未连上时调用：内部任务会等拿到 IP 再对时，
 *         启动顺序因此简单（无需在 IP 事件回调里回调它）。
 *         本函数会阻塞到首次对时完成或失败（等 IP 最长 60 秒 +
 *         等 SNTP 最长 5×3 秒），故不要在 app_main 中直接调用，
 *         应经 time_mgr_task 起独立任务。
 */
esp_err_t time_mgr_start(const char *server);

/**
 * @brief  FreeRTOS 任务入口：先调 time_mgr_start()（等 IP + 首次对时），
 *         再每秒构造 NS800_CMD_TIME 帧下发。
 * @note   直接喂给 xTaskCreate，不要在别的任务中调用（死循环不返回）。
 *         即使 NTP 对时失败，任务也会继续运行并周期重试对时，
 *         避免“开机时网络慢一点”导致时间功能永久失效。
 */
void time_mgr_task(void *arg);

/**
 * @brief  当前是否已对到时间。
 */
bool time_mgr_is_synced(void);

/**
 * @brief  取当前本地（UTC+8）Unix 时间戳；未对时返回 0。
 */
int64_t time_mgr_local_unix(void);

#ifdef __cplusplus
}
#endif

#endif /* __TIME_MGR_H__ */
