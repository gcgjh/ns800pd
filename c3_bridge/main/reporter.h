/**
 * @file    reporter.h
 * @brief   周期采集芯片状态、联网上报、与 NS800 双向同步
 *
 * 每轮采集温度趋势、RSSI、空闲堆、运行时长，先推给 NS800（本地链路，
 * 不依赖外网），再在联网且配置了上报 URL 时做 HTTP 上报。
 */

#ifndef __REPORTER_H__
#define __REPORTER_H__

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  启动上报任务与命令接收任务（幂等，重复调用安全）
 * @return ESP_OK 启动成功
 * @note   内部先初始化 host_data，再创建 report / cmd 两个任务。
 */
esp_err_t reporter_start(void);

/**
 * @brief  读取网络上报的成功 / 失败计数
 * @param  ok    输出，成功次数（可为 NULL）
 * @param  fail  输出，失败次数（可为 NULL）
 */
void reporter_get_counters(uint32_t *ok, uint32_t *fail);

#ifdef __cplusplus
}
#endif

#endif /* __REPORTER_H__ */
