/**
 * @file    ui.h
 * @brief   UI 层对外接口、主题色与组件工厂
 *
 * 相关文件：
 *   ui.c            桌面（状态栏 / App 网格 / 导航）与 App 页面框架
 *   ui_apps.c       各 App 的内容实现
 *   ui_text.h       全部中文文案（UTF-8 转义写法）
 *   gen_font_zh.sh  从 ui_text.h 提取汉字重新生成字库
 *
 * 上层只调用 ui_init()，由 lv_user_gui.c 在 LVGL 线程里调用。
 */

#ifndef __UI_H__
#define __UI_H__

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 中文字库（lv_font_conv 生成，不含 LVGL 符号区，不要往里塞 LV_SYMBOL_*）── */
extern const lv_font_t zh18;

/* ── 主题色：彩色图标 + 照片壁纸 ──
 * 底色由照片提供（见 wallpaper_store.c，从外置 flash 的 /wallpaper.bin
 * 读进 SRAM12 的 .wall_buf 段），这里只管文字与控件。 */
#define C_TXT        0xFFFFFF   /* 主文字色（照片上需要高对比） */
#define C_TXT_DIM    0xE4E7F0   /* 次文字色 */
#define C_ACCENT     0x8FD8FF   /* 焦点强调色（亮青） */

/* 壁纸兜底色：照片未就绪时的纵向渐变 */
#define C_WALL_MID   0x1B1236   /* 渐变中段 */
#define C_WALL_BOT   0x0B0718   /* 渐变底端 */

/* 文字在照片上的投影色：保证任何底色上都读得清 */
#define C_TEXT_SHADOW 0x000000

/* 卡片/按钮底色与描边（白色低透明叠加，省得给每处调色） */
#define C_SURFACE    0xFFFFFF
#define C_LINE       0xFFFFFF

/* ── App 图标主色（明快但成组协调）── */
#define AC_TEMP      0xFF9F5A   /* 橙 —— 温度 */
#define AC_HUMI      0x38BDF8   /* 天蓝 —— 湿度 */
#define AC_ENC       0xA78BFA   /* 紫 —— 编码器 */
#define AC_SYS       0x22D3EE   /* 青 —— 系统 */
#define AC_STOPW     0x34D399   /* 绿 —— 秒表 */
#define AC_BUZZ      0xFBBF24   /* 黄 —— 蜂鸣器 */
#define AC_LED       0x4ADE80   /* 亮绿 —— LED */
#define AC_BL        0xF472B6   /* 粉 —— 背光 */
#define AC_IMG       0x60A5FA   /* 亮蓝 —— 图片 */
#define AC_ABOUT     0x94A3B8   /* 灰蓝 —— 关于 */
#define AC_GAME      0xF87171   /* 红 —— 游戏 */
#define AC_NET       0x38BDF8   /* 天蓝 —— 配网（同湿度色系，WiFi 感） */
#define AC_ALARM     0xFB923C   /* 橙红 —— 闹钟 */

/* 数值强调色（App 内大字号读数） */
#define VC_TEMP      0xFF9F5A   /* 温度读数 */
#define VC_JUNC      0xF472B6   /* 结温读数 */
#define VC_HUMI      0x38BDF8   /* 湿度读数 */
#define VC_ENC       0xA78BFA   /* 编码器读数 */
#define VC_STOPW     0x34D399   /* 秒表读数 */
#define VC_BUZZ      0xFBBF24   /* 蜂鸣器读数 */

/* ── 组件工厂（ui.c 提供，ui_apps.c 复用）── */

/**
 * @brief  让对象可聚焦并加入默认 group（encoder 旋钮能选到它）
 * @param  o  目标对象
 */
void ui_make_focusable(lv_obj_t *o);

/**
 * @brief  套上「按钮」外观：半透明底 + 描边 + 焦点时青色高亮
 * @param  o  目标对象
 * @note   内部已含 ui_make_focusable()。
 */
void ui_style_button(lv_obj_t *o);

/**
 * @brief  建一个文本 label
 * @param  parent  父对象
 * @param  txt     文本内容
 * @param  font    字体
 * @param  color   文字颜色（0xRRGGBB）
 * @return 新建的 label
 */
lv_obj_t *ui_label(lv_obj_t *parent, const char *txt,
                   const lv_font_t *font, uint32_t color);

/**
 * @brief  建一张卡片（深色半透明底 + 细描边 + 圆角）
 * @param  parent  父对象
 * @param  x, y    相对父对象的坐标
 * @param  w, h    尺寸
 * @return 新建的卡片
 */
lv_obj_t *ui_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                  lv_coord_t w, lv_coord_t h);

/**
 * @brief  建一个可按下按钮（含文案与外观）
 * @param  parent  父对象
 * @param  txt     按钮文案
 * @param  w, h    尺寸
 * @param  cb      点击回调，可为 NULL
 * @return 新建的按钮
 */
lv_obj_t *ui_button(lv_obj_t *parent, const char *txt,
                    lv_coord_t w, lv_coord_t h, lv_event_cb_t cb);

/**
 * @brief  一次按键只放行一次的边沿判定
 * @return 1 = 本次可响应；0 = 距上次过近，应忽略
 * @note   LVGL encoder indev 按一次 KEY 会连发多个事件（KEY / SHORT_CLICKED /
 *         CLICKED），所有点击回调都要先过这道闸，否则一次按键触发多次动作。
 */
int ui_click_ok(void);

/**
 * @brief  构建并显示界面
 * @note   由 lv_user_gui.c 在 LVGL 线程里调用。
 */
void ui_init(void);

/**
 * @brief  全屏接管（游戏 App 用）
 * @param  fullscreen  1 = 进入全屏；0 = 退出并恢复标准布局
 * @note   fullscreen = 1 时隐藏顶部导航栏（返回按钮 + 标题），body 从 y=0 铺满
 *         整屏，并把 App 层 / body 重设成当前显示分辨率（横屏时 320x240）；
 *         fullscreen = 0 恢复标准布局（导航栏 34 px，尺寸 240x320）。
 *         只在 App 的 build() 里调一次、teardown() 里调 0 恢复。
 *         必须与 lv_port_disp_set_orientation() 配对使用：切方向在前，
 *         本函数在后（本函数读的是切换后的分辨率）。
 *         只能在 LVGL 线程调用（build/teardown 就在 LVGL 线程里，满足）。
 */
void ui_set_fullscreen(int fullscreen);

/**
 * @brief  取当前 body 宽度
 * @return 全屏后 = 横屏 320，否则 240
 * @note   给游戏算布局用。
 */
lv_coord_t ui_body_width(void);

/**
 * @brief  取当前 body 高度
 * @return 全屏后 = 横屏 240，否则 320 减导航栏高度
 * @note   给游戏算布局用。
 */
lv_coord_t ui_body_height(void);

/**
 * @brief  回到桌面：在 App 页里就关掉它，已在桌面则什么都不做
 * @note   内部会操作 LVGL 对象，必须通过 lv_async_call() 在 LVGL 线程里调用：
 *             lv_async_call(ui_back_home_async, RT_NULL);
 *         main.c 的板载 KEY 单击就走这条路，见 applications/board_key.c。
 */
void app_close(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_H__ */
