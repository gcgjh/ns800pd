/**
 * @file    link_ns800.h
 * @brief   ESP32-C3 → NS800RT7P65 串口链路的对外接口
 *
 * 协议格式与命令码见 ns800_link.h（两块板共用的权威定义）。
 */

#ifndef __LINK_NS800_H__
#define __LINK_NS800_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ns800_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ns800_frame_t 定义在共享协议头 ns800_link.h（协议唯一出处）。
 * 两块板必须使用同一份定义，否则 payload[0] = cmd 的约定会对不上。 */

/**
 * @brief  初始化 UART 并启动接收任务
 * @return ESP_OK 成功；其他值为互斥锁/队列创建或驱动安装失败
 * @note   幂等：重复调用安全，仅首次真正执行初始化。
 */
esp_err_t ns800_link_init(void);

/**
 * @brief  发送一帧
 * @param  cmd      命令码（NS800_CMD_*）
 * @param  payload  数据段（可为 NULL），不含 cmd 本身
 * @param  len      数据段长度，最大 NS800_LINK_MAX_PAYLOAD-1
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化；
 *         ESP_ERR_INVALID_ARG 长度越界；ESP_ERR_TIMEOUT 取锁超时；
 *         ESP_FAIL 串口写失败
 * @note   发送受互斥锁保护，须仅在任务上下文调用。
 */
esp_err_t ns800_link_send(uint8_t cmd, const void *payload, uint8_t len);

/**
 * @brief  从接收队列取一帧（阻塞至多 timeout_ms）
 * @param  out         输出，解出的完整帧
 * @param  timeout_ms  等待超时（毫秒）
 * @return true 取到一帧；false 超时或链路未初始化
 */
bool ns800_link_recv(ns800_frame_t *out, uint32_t timeout_ms);

/**
 * @brief  读取接收统计
 * @param  bytes   输出，累计收到的字节数（可为 NULL）
 * @param  frames  输出，累计成功解析的帧数（可为 NULL）
 * @param  badcrc  输出，累计 CRC 校验失败的帧数（可为 NULL）
 */
void ns800_link_get_stats(uint32_t *bytes, uint32_t *frames, uint32_t *badcrc);

/**
 * @brief  物理层活性探针：绕过 UART，直读 RX 引脚电平（联调用）
 *
 * 将 LINK_RX_GPIO 临时切为普通 GPIO 输入，密集采样 @p ms 毫秒，
 * 返回高电平翻转次数。测完自动恢复 UART 配置。
 *
 * 用途：当 rx_bytes == 0（UART 计数无法区分）时，下面两种情况难以分辨：
 *   ① 引脚上根本没有波形（接线断 / 引脚号写错）
 *   ② 引脚上有波形但 UART 认不出（波特率错、电压不足、未共地、接到相邻引脚）
 * 电平探针可直接区分：flips==0 说明确无波形，flips>0 说明波形存在、问题在 UART 侧。
 *
 * @param  ms      采样时长（毫秒），建议 500~2000
 * @param  flips   翻转次数输出（高→低 或 低→高 均计入）
 * @return ESP_OK / ESP_FAIL（链路未初始化）
 */
esp_err_t ns800_link_rx_pin_probe(uint32_t ms, uint32_t *flips);

/**
 * @brief  取最近一次 ns800_link_rx_pin_probe() 采样得到的翻转次数
 * @return 翻转次数；从未采样过则为 0
 * @note   该字段历史上作为 TX 脚对照组，目前复用为 RX 脚的翻转数，
 *         保持网页字段名不变。采样得到 0 说明 RX 引脚无波形，
 *         配合其他计数可判断是物理层断线还是 UART 侧解析问题。
 */
uint32_t ns800_link_probe_tx_flips(void);

/**
 * @brief  对端（NS800）是否还在线
 * @return true 表示 30 秒内收到过至少一帧有效帧
 * @note   判据只看 CRC 通过的完整帧：仅有字节流动不代表对端存活，
 *         波特率错位时会收到一堆 CRC 失败的垃圾，此时须算作「不在线」。
 */
bool ns800_link_peer_alive(void);

/**
 * @brief  引脚路由自检：把 IO_MUX / GPIO 矩阵寄存器打到日志（只读不写）
 * @note   用途为审计：证明 uart_set_pin() 真正把 UART1 的信号接到了
 *         GPIO4/GPIO5 上，而不只是「调用过」。须在 ns800_link_init() 之后调用。
 */
void ns800_link_dump_pin_mux(void);

/* ── 便捷发送 ── */

/**
 * @brief  发送一段文本帧（自动计算长度）
 * @param  cmd   命令码（通常为 NS800_CMD_TEXT）
 * @param  text  以 '\0' 结尾的文本，最长 95 字节
 * @return 同 ns800_link_send() 的返回语义
 */
esp_err_t ns800_link_send_text(uint8_t cmd, const char *text);

/**
 * @brief  发送传感器定长载荷帧（NS800_CMD_SENSOR）
 * @param  temp_x100  芯片温度 ×100（℃）
 * @param  rssi       WiFi 信号强度（dBm，带符号）
 * @param  free_kb    空闲堆大小 /1024（KB）
 * @param  uptime_s   系统运行秒数
 * @return 同 ns800_link_send() 的返回语义
 */
esp_err_t ns800_link_send_sensor(int16_t temp_x100, int16_t rssi,
                                 uint16_t free_kb, uint32_t uptime_s);

#ifdef __cplusplus
}
#endif

#endif /* __LINK_NS800_H__ */
