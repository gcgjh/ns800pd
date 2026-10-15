/**
 * @file    ui_apps.h
 * @brief   各 App 内容区实现（ui_apps.c）的对外声明
 *
 * 约定：每个 App 只负责画内容区，顶部导航栏（返回 + 标题）由 ui.c 统一创建，
 * 因此 build() 收到的是已算好坐标的 body 容器（从 y=0 开始，属于 App 页内部）。
 * tick() 由 ui.c 按各自周期调用，不需要周期刷新的 App 不提供 tick。
 */

#ifndef __UI_APPS_H__
#define __UI_APPS_H__

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 温度页：DHT22 环境温度 + 芯片内部结温 */
void app_temp_build(lv_obj_t *body);
/* 温度页 tick，周期 2000 ms */
void app_temp_tick(void);

/* 湿度页：DHT22 相对湿度 + 舒适度判断 */
void app_humi_build(lv_obj_t *body);
/* 湿度页 tick，周期 2000 ms */
void app_humi_tick(void);

/* 编码器页：EC11 格数 / 方向 / 按 KEY 清零 */
void app_enc_build(lv_obj_t *body);
/* 编码器页 tick，周期 100 ms */
void app_enc_tick(void);

/* 系统页：版本 / 运行时长 / 内存 / 线程，下半屏为 C3 联网状态 */
void app_sys_build(lv_obj_t *body);
/* 系统页 tick，周期 500 ms */
void app_sys_tick(void);

/* 秒表页：启动-停止-清零 */
void app_stopwatch_build(lv_obj_t *body);
/* 秒表页 tick，周期 50 ms */
void app_stopwatch_tick(void);

/* 蜂鸣器页：三档测试音 */
void app_buzzer_build(lv_obj_t *body);

/* LED 页：心跳灯模式（慢闪 / 快闪 / 常亮 / 关）+ LED2 系统心跳开关 */
void app_led_build(lv_obj_t *body);

/* 背光页：屏幕背光开关 */
void app_backlight_build(lv_obj_t *body);

/* 图片页：看图（原寸 / 铺满 / 放大），数据源同桌面壁纸 */
void app_img_build(lv_obj_t *body);
/* 图片页 teardown：无 tick，需清模块级指针 */
void app_img_teardown(void);

/* 关于页：板卡与固件信息 */
void app_about_build(lv_obj_t *body);

/* 配网页：给 ESP32-C3 下发 WiFi 配置（STA 连路由 / AP 开热点），预置候选 + 旋钮选择 */
void app_net_build(lv_obj_t *body);

/* 闹钟页：最多 4 个，到点响铃。实现不在本文件，数据与持久化在 alarm.c。
 * 响铃不依赖本 App 是否打开：判定在 ui.c 的常驻定时器里，停在桌面照样响，
 * 本 App 的 tick 只画界面。 */
void app_alarm_build(lv_obj_t *body);
/* 闹钟页 tick，周期 500 ms */
void app_alarm_tick(void);
/* 闹钟页 teardown：离开页面时补一次落盘（滑块改动为去抖保存，退出需兜底） */
void app_alarm_teardown(void);

/* 游戏页：横屏跳跃躲避。实现不在本文件，在 applications/lcd_game_ui.c。
 * 它是唯一会切换屏幕方向的 App：build 里切横屏，teardown 里切回竖屏。
 * 定义必须为非 static，ui.c 通过头文件声明的函数指针调用。 */
void app_game_build(lv_obj_t *body);
void app_game_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_APPS_H__ */
