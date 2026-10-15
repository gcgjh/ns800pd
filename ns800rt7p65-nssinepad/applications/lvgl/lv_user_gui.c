/**
 * @file    lv_user_gui.c
 * @brief   用户界面入口，由 env_support/rt-thread/lv_rt_thread_port.c 在 LVGL 线程中调用
 *
 * 当前内容：加载 applications/lvgl/ui/ 中的占位界面（ui_init）。
 * 它同时用于集成自检：旋转编码器应使焦点移到滑块，按下 KEY 进入编辑模式，
 * 一次性验证 LVGL、编码器与按键是否正常。
 *
 * 后续以 EEZ Studio 导出产物覆盖 ui/ 后，本文件无需改动。
 */

#include <rtthread.h>
#include <lvgl.h>
#include "ui.h"
#include "settings.h"

/**
 * @brief  初始化用户界面
 * @note   由 LVGL 线程调用；先载入配置再建界面，因为桌面要读取 home_page
 *         决定停在哪一页。settings_load 幂等，重复调用无副作用。
 */
void lv_user_gui_init(void)
{
    rt_kprintf("[lvgl] start user UI (applications/lvgl/ui)\n");

    /* 必须先载入配置再调用 ui_init()：桌面要读取 home_page 决定停在哪一页。
     * settings_load 幂等，重复调用无副作用。 */
    settings_load();

    ui_init();

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
    /* 打印 LVGL 内存池占用（TLSF）。max_used 为历史峰值，
     * 是判断内存池是否充足的唯一硬指标：接近 total 即应扩池。 */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    rt_kprintf("[lvgl] pool after ui: total=%uK free=%uK max_used=%uK used=%u%% frag=%u%%\n",
               (unsigned)(mon.total_size / 1024U), (unsigned)(mon.free_size / 1024U),
               (unsigned)(mon.max_used / 1024U), (unsigned)mon.used_pct, (unsigned)mon.frag_pct);
#endif
}
