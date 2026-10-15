/**
 * @file    web_server.h
 * @brief   C3 联网桥 —— 内置 HTTP 管理网页（安卓风格）
 *
 * 管理页以 C 字符串常量形式编译进 app 分区，而非独立文件系统分区。
 * 原因：partitions.csv 在 0x320000 之后尚余约 0.94MB 未登记空间，
 * 但其中没有 spiffs/littlefs 分区；为一张页面改动分区表，意味着必须全量
 * 串口重烧、清空 NVS 并重新配网，代价过高。页面本体约 9KB，
 * 相对 854KB 固件可忽略，且零 IO 驱动、零初始化时序风险。
 *
 * 服务器采用 esp_http_server 组件，与工程已有的 esp_http_client（OTA 用）
 * 不是同一组件，需在 CMakeLists 的 PRIV_REQUIRES 中单独声明，
 * 漏加会在链接期报 undefined symbol。
 *
 * 视觉走 Material Design 浅色主题，纯 CSS 实现、零额外素材：
 * 主色蓝 #1A73E8、强调绿 #16A34A、卡片圆角加柔和投影浮起，
 * 顶部 56px AppBar + 白色卡片分组 + 输入框聚焦变色 + Snackbar 式 Toast。
 *
 * 安全边界：本页为明文 HTTP、无鉴权，可改 WiFi、触发 OTA、重启，
 * 仅限局域网内调试使用；任何能连上同一网络的人都能操作。
 * 若需公开部署，须另加会话登录、CSRF token 与请求体大小限制，
 * 本文件不提供这些能力。
 */
#ifndef __WEB_SERVER_H__
#define __WEB_SERVER_H__

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 固件版本串（唯一定义处，单一出处）。
 *
 * 两处共用，刻意放在头文件：
 *  ① NS800_CMD_HELLO 帧上报给 NS800，用于对端 c3stat 的"固件"栏；
 *  ② HTTP 管理页的"固件版本"栏。
 * 若两处各写一份字符串，迟早出现版本不一致（网页显示 v2.0、串口报 v2.1
 * 这类难查的问题），因此改版本号只改这一行。 */
#define C3_BRIDGE_VERSION   "c3-bridge v2.0"

/**
 * @brief  启动 HTTP 管理服务（绑定 0.0.0.0:80），并注册全部路由。
 * @return ESP_OK 成功（已启动时同样返回 ESP_OK）；httpd_start 失败返回 ESP_FAIL。
 */
esp_err_t web_server_start(void);

/**
 * @brief  返回固件版本串 C3_BRIDGE_VERSION。
 * @return 指向编译期常量字符串，无需释放。
 * @note   供 web_server.c 内部使用，避免其直接引用版本宏。
 */
const char *web_version(void);

#ifdef __cplusplus
}
#endif

#endif /* __WEB_SERVER_H__ */
