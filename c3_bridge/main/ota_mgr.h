/**
 * @file    ota_mgr.h
 * @brief   C3 联网桥 OTA 升级管理（HTTP 拉取固件 → 写 OTA 槽 → 重启切换）
 *
 * 分区布局见工程根目录 partitions.csv：ota_0 / ota_1 各 1.5 MB，外加 otadata。
 *
 * 设计要点：
 *  - 升级过程跑在独立任务里，不阻塞 NS800 链路心跳与上报任务，
 *    避免升级的十几秒里界面显示"离线"。
 *  - 升级进度经 NS800_CMD_OTA_PROGRESS 帧回报 0..100，屏上显示进度条，
 *    失败原因一并回报。
 *  - 回滚保护：新固件首次启动处于 PENDING_VERIFY，必须显式确认，
 *    由 ota_mgr_confirm_valid() 在自检通过后调用。
 *
 * 不使用 esp_https_ota 一站式接口：该接口把整个升级锁在单次调用里，
 * 拿不到细粒度进度回报 NS800，也不便与板间链路锁协调。本模块改用手动
 * esp_ota_begin / write / end 分块，既能更新进度，又能在每块间让出 CPU。
 */
#ifndef __OTA_MGR_H__
#define __OTA_MGR_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OTA 任务当前状态 */
typedef enum
{
    OTA_ST_IDLE = 0,        /* 空闲，未在升级 */
    OTA_ST_DOWNLOADING,     /* 正在下载并写 flash */
    OTA_ST_SUCCESS,         /* 下载完成，等待重启切换 */
    OTA_ST_FAILED,          /* 失败，原因见 last_err */
} ota_state_t;

/**
 * @brief  初始化 OTA 模块
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 同步原语创建失败
 * @note   只创建同步原语，不做任何网络动作。
 */
esp_err_t ota_mgr_init(void);

/**
 * @brief  触发一次 OTA 升级（异步，立即返回）
 *
 * @param  url  固件 .bin 的完整 HTTP(S) URL，例如
 *              "http://192.168.1.10:8000/c3_bridge.bin"
 * @return ESP_OK 已接受任务；ESP_ERR_INVALID_STATE 表示正在升级中
 *
 * @note   升级进行中重复调用会被拒绝（返回 ESP_ERR_INVALID_STATE），
 *         避免两个任务同时写 OTA 槽把固件写坏。
 */
esp_err_t ota_mgr_start(const char *url);

/**
 * @brief  查询当前 OTA 状态与进度
 * @param  out_st   输出，状态（可为 NULL）
 * @param  out_pct  输出，0..100 进度（可为 NULL）
 * @param  out_err  输出，最近一次失败原因字符串（可为 NULL，
 *                  返回静态常量指针，勿释放）
 */
void ota_mgr_get_state(ota_state_t *out_st, int *out_pct, const char **out_err);

/**
 * @brief  确认当前固件有效（须在自检通过后调用）
 * @note   配合 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE：若新固件启动后
 *         一直不调用本函数，下次重启会自动回滚到旧固件；未启用回滚配置时
 *         本函数为无害空操作（仅打一条日志）。
 */
void ota_mgr_confirm_valid(void);

/**
 * @brief  打印当前运行槽信息
 * @note   调试用，含是否处于 PENDING_VERIFY。
 */
void ota_mgr_dump_info(void);

/* ── 本地上传（HTTP 直传，不依赖 URL）──
 *
 * 供网页直接选择 .bin 文件升级，无需先把文件传到 HTTP 服务器。
 * 采用流式分块直写：每收到一块立刻写入 flash，RAM 占用恒定 4 KB。
 * C3 仅约 400 KB SRAM 而固件约 1 MB，"先收完整文件再写"必然内存溢出。
 *
 * 典型用法（web_server.c 的上传 handler 即如此）：
 *     ota_mgr_upload_begin(content_len);        // 先判大小，再擦槽
 *     while ((n = httpd_req_recv(req, buf, 4096)) > 0)
 *         ota_mgr_upload_chunk(buf, n);
 *     ota_mgr_upload_end();                     // 校验 + 切启动槽
 *
 * 任一步失败都必须走 ota_mgr_upload_abort()：它调用 esp_ota_abort()
 * 丢弃半个镜像。不可在写了一半时 esp_ota_end() + set_boot，否则
 * bootloader 可能跳进半个固件，设备变砖。
 */

/**
 * @brief  开始一次上传会话
 * @param  declared_size  声明的内容长度，0 表示未知（如 chunked）
 * @return ESP_OK 已接受；其余为错误码
 */
esp_err_t ota_mgr_upload_begin(int64_t declared_size);

/**
 * @brief  写一块数据
 * @param  data  数据首地址
 * @param  len   数据长度
 * @return ESP_OK 成功；其余为错误码
 */
esp_err_t ota_mgr_upload_chunk(const uint8_t *data, size_t len);

/**
 * @brief  收尾：校验镜像并设置下次启动槽
 * @return ESP_OK 成功，代表下次重启会用新固件；其余为错误码
 */
esp_err_t ota_mgr_upload_end(void);

/**
 * @brief  放弃当前上传
 * @note   丢弃已写内容，不切换启动槽。
 */
void ota_mgr_upload_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_MGR_H__ */
