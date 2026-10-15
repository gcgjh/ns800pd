/**
 * @file    c3_report.h
 * @brief   NS800 业务数据周期上报接口（NS800_CMD_HOST_DATA / 0x88）
 *
 * 协议定义见 ns800_link.h 的 NS800_CMD_HOST_DATA，
 * C3 侧接收端为 host_data.c，网页出口为 GET /api/host_data。
 */

#ifndef __C3_REPORT_H__
#define __C3_REPORT_H__

#include <rtthread.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 上报固件版本标识，随 ns800_ver 字段上行 */
#define NS800_REPORT_VER   "nssinepad-1"

/**
 * @brief  启动业务数据周期上行（每 5 秒一轮）
 *
 * @note   必须在 LVGL 线程里调用（内部创建 lv_timer）。
 *         调用点：ui.c 的 ui_init()，与常驻的 uptime/sb 定时器并列。
 */
void c3_report_start(void);

#ifdef __cplusplus
}
#endif

#endif /* __C3_REPORT_H__ */
