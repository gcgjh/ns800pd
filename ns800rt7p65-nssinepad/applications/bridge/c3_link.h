/**
 * @file    c3_link.h
 * @brief   NS800 侧与 ESP32-C3 的板间链路接口（UART2）
 *
 * 硬件：UART2（PB6 = TX / PB7 = RX，ALT1），115200 8N1
 * 接线：NS800 PB6 -> C3 GPIO5(RX)，NS800 PB7 <- C3 GPIO4(TX)，GND 共地
 * 协议：见同目录 ns800_link.h，两块板共用同一份定义，改动必须两边同步。
 *
 * 设计要点：
 *   - 只做收帧、解析、落地到共享状态，不在中断里跑业务；
 *   - 共享状态由一把互斥锁保护，UI 线程随时可读快照；
 *   - 发送走互斥锁串行化，避免多线程交错把帧撕开。
 */

#ifndef __C3_LINK_H__
#define __C3_LINK_H__

#include <rtthread.h>
#include "ns800_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* C3 上报给 NS800 的运行时状态快照（整体加锁拷贝，避免读到一半被改写） */
typedef struct
{
    rt_uint8_t  online;          /* 最近一次收帧在有效期内，1 = 在线 */
    rt_uint8_t  wifi_state;      /* C3 WiFi 状态，取值 NS800_WIFI_xxx */
    rt_int16_t  rssi_dbm;        /* 来自 SENSOR 帧，带符号（如 -30） */
    rt_int16_t  temp_x100;       /* 摄氏度 ×100；C3 无温度传感器，恒为 0 */
    rt_uint16_t free_kb;         /* C3 剩余堆（KB） */
    rt_uint32_t uptime_s;        /* C3 运行秒数 */
    char        ip[20];          /* 点分十进制 IPv4，未拿到时为 "" */
    char        c3_ver[24];      /* C3 固件版本，来自 HELLO */
    /* ── 配网（由 WIFI_CFG 帧回填，配网 App 读这里） ── */
    rt_uint8_t  wifi_mode;       /* NS800_WIFI_MODE_STA / _AP */
    rt_uint8_t  cfg_valid;       /* 1 = 已从 C3 拿到过配置快照 */
    char        cfg_ssid[NS800_WIFI_SSID_MAX + 1];
    char        cfg_pass[NS800_WIFI_PASS_MAX + 1];
    /* ── OTA（由 OTA_PROGRESS 帧回填） ── */
    rt_uint8_t  ota_status;      /* NS800_OTA_ST_RUNNING / _OK / _FAIL */
    rt_uint8_t  ota_percent;     /* 0..100 */
    rt_uint8_t  ota_active;      /* 1 = 正在升级（收到过 RUNNING），用于 UI 判断 */
    /* ── 时间（由 TIME 帧回填；C3 走 NTP 对时后下发） ──
     * time_valid = 0 时其余字段无意义（C3 还没对到时间）。 */
    rt_uint8_t  time_valid;      /* 1 = 已收到过有效时间 */
    rt_int32_t  time_unix;       /* Unix 时间戳，已换算到本地时区 UTC+8 */
    rt_uint32_t time_tick;       /* 收到该时间时的 NS800 tick（用于本地推算秒数） */
    /* 统计 */
    rt_uint32_t rx_frames;       /* 累计收帧数 */
    rt_uint32_t rx_badcrc;       /* 累计 CRC 错帧数 */
    rt_int16_t  last_http_code;  /* C3 最近一次上报的 HTTP 状态码，-1 表示未上报过 */
    rt_uint8_t  last_http_ok;    /* 1=成功 0=失败 */
    rt_uint32_t last_rx_tick;    /* 最近一次收帧的系统 tick */
} c3_link_state_t;

/**
 * @brief  初始化板间链路（打开 uart2、启动接收线程）
 * @return RT_EOK 成功；-RT_ERROR 串口未找到或配置失败；-RT_ENOMEM 线程创建失败
 * @note   重复调用直接返回 RT_EOK。
 */
rt_err_t c3_link_init(void);

/**
 * @brief  发送一帧到 C3
 * @param  cmd     命令码（NS800_CMD_xxx）
 * @param  payload 负载（不含 cmd），可为 RT_NULL
 * @param  len     负载长度（不含 cmd），最大 NS800_LINK_MAX_PAYLOAD-1
 * @return RT_EOK 成功，其他为失败
 * @note   发送全程持发送互斥锁，可从任意线程调用。
 */
rt_err_t c3_link_send(rt_uint8_t cmd, const void *payload, rt_uint8_t len);

/**
 * @brief  发送纯文本帧（自动计算长度，超长截断）
 * @param  cmd   命令码（通常 NS800_CMD_TEXT）
 * @param  text  UTF-8 文本，以 '\0' 结尾，可为 RT_NULL（返回 -RT_EINVAL）
 * @return RT_EOK 成功，其他为失败
 */
rt_err_t c3_link_send_text(rt_uint8_t cmd, const char *text);

/**
 * @brief  取一份当前状态快照（加锁拷贝，可随时调用）
 * @param  out 输出结构体，调用方分配
 * @return 无
 */
void c3_link_get_state(c3_link_state_t *out);

/**
 * @brief  请求 C3 重新联网（发 NS800_CMD_REQ_WIFI，无负载）
 * @return RT_EOK 成功，其他为失败
 */
rt_err_t c3_link_request_wifi(void);

/**
 * @brief  设置 C3 的上报周期（发 NS800_CMD_SET_REPORT，2 字节小端秒数）
 * @param  seconds 上报周期，单位秒
 * @return RT_EOK 成功，其他为失败
 * @note   秒数有效范围 1..3600（协议规定，C3 侧会拒绝越界值）。
 */
rt_err_t c3_link_set_report_period(rt_uint32_t seconds);

/**
 * @brief  下发 STA 配置并让 C3 连接（发 NS800_CMD_SET_WIFI_CFG，mode = STA）
 * @param  ssid 目标 AP 名称，长度 1..NS800_WIFI_SSID_MAX
 * @param  pass 密码，长度 0..NS800_WIFI_PASS_MAX
 * @return RT_EOK 成功，-RT_EINVAL 参数非法或超长
 */
rt_err_t c3_link_set_wifi_cfg(const char *ssid, const char *pass);

/**
 * @brief  下发 AP 配置并让 C3 开热点（发 NS800_CMD_SET_WIFI_CFG，mode = AP）
 * @param  ssid 热点名称，长度 1..NS800_WIFI_SSID_MAX
 * @param  pass 密码，可为空串表示开放
 * @return RT_EOK 成功，-RT_EINVAL 参数非法或超长
 */
rt_err_t c3_link_set_ap_cfg(const char *ssid, const char *pass);

/**
 * @brief  请求 C3 回报当前 WiFi 配置（发 NS800_CMD_GET_WIFI_CFG，无负载）
 * @return RT_EOK 成功，其他为失败
 * @note   结果异步回填到 c3_link_state_t 的 cfg_* 字段。
 */
rt_err_t c3_link_get_wifi_cfg(void);

/**
 * @brief  触发 C3 执行 OTA（发 NS800_CMD_OTA_START，payload = 固件 URL）
 * @param  url  固件 .bin 的完整 URL，如 "http://192.168.1.100:8000/c3_bridge.bin"
 * @return RT_EOK 已发出；RT_EINVAL URL 为空或超长（> NS800_LINK_MAX_PAYLOAD-1）
 *
 * @note   本函数是异步的：只负责把 URL 帧发出去，C3 在后台任务里下载并写 flash。
 *         进度通过 NS800_CMD_OTA_PROGRESS 帧异步回填到 s_state.ota_* 字段。
 */
rt_err_t c3_link_ota_start(const char *url);

/**
 * @brief  向 C3 上报一条自定义数据（通用通道 NS800_CMD_HOST_DATA 0x88）
 *
 * @param  key   键名，ASCII，建议不超过 16 字符（如 "backlight"、"game_score"）
 * @param  value 值的文本形式（如 "80"、"1280"、"on"），可为 RT_NULL（按空串处理）
 * @return RT_EOK 已发出；-RT_EINVAL key 为空或总长超限
 *
 * @note   这是 NS800 -> C3 方向的通用数据入口。C3 侧不认识该 key 也会原样保存，
 *         网页 /api/host_data 可以看到，因此新增数据种类无需改协议、无需同步头文件。
 *
 * @warning 传输开销：key + value + '=' 合计上限 95 字节，超长时返回 -RT_EINVAL，
 *          不做静默截断。超长请在调用方裁剪。
 */
rt_err_t c3_link_send_kv(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* __C3_LINK_H__ */
