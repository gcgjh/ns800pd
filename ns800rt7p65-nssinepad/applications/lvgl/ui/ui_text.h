/**
 * @file    ui_text.h
 * @brief   UI 中文文案（UTF-8 转义写法）
 *
 * 用 \xHH 转义：规避编辑器与工具链编码漂移（见 LVGL_AI_STANDARD.md §5.5）。
 * 集中在一个文件：字库生成脚本 gen_font_zh.sh 从本文件 #define 行的
 * 原文注释中提取全部汉字，漏字在屏上会显示成方块，集中管理就不会漏。
 *
 * 新增文案后必须重新生成字库：sh applications/lvgl/ui/gen_font_zh.sh
 */

#ifndef __UI_TEXT_H__
#define __UI_TEXT_H__

#define S_BACK         "\xE8\xBF\x94\xE5\x9B\x9E"  /* 返回 */
#define S_HINT_NAV     "\xE6\x97\x8B\xE8\xBD\xAC\xE9\x80\x89\xE6\x8B\xA9\x20\xE2\x80\xA2\x20\xE6\x8C\x89\x20\x4B\x45\x59\x20\xE6\x89\x93\xE5\xBC\x80"  /* 旋转选择 • 按 KEY 打开 */
/* 下面这条已不再被引用（底部"按 KEY 打开"提示已移除）。
 * 暂予保留：按 / 打 / 开 三个字在 S_E_RESET、S_B_HINT、S_BL_ON 中仍在使用，
 * 删除本行不会减少字库字符数。确认无引用后可直接删行，无需重新生成字库。 */
#define S_HINT_OPEN    "\xE6\x8C\x89\x20\x4B\x45\x59\x20\xE6\x89\x93\xE5\xBC\x80"  /* 按 KEY 打开 */
#define S_READING      "\xE8\xAF\xBB\xE5\x8F\x96\xE4\xB8\xAD"  /* 读取中 */
#define S_FAIL         "\xE8\xAF\xBB\xE5\x8F\x96\xE5\xA4\xB1\xE8\xB4\xA5"  /* 读取失败 */
#define S_A_TEMP       "\xE6\xB8\xA9\xE5\xBA\xA6"  /* 温度 */
#define S_A_HUMI       "\xE6\xB9\xBF\xE5\xBA\xA6"  /* 湿度 */
#define S_A_ENC        "\xE7\xBC\x96\xE7\xA0\x81\xE5\x99\xA8"  /* 编码器 */
#define S_A_SYS        "\xE7\xB3\xBB\xE7\xBB\x9F"  /* 系统 */
#define S_A_STOPW      "\xE7\xA7\x92\xE8\xA1\xA8"  /* 秒表 */
#define S_A_BUZZ       "\xE8\x9C\x82\xE9\xB8\xA3\xE5\x99\xA8"  /* 蜂鸣器 */
#define S_A_BL         "\xE8\x83\x8C\xE5\x85\x89"  /* 背光 */
#define S_A_ABOUT      "\xE5\x85\xB3\xE4\xBA\x8E"  /* 关于 */
#define S_A_TUNE       "\xE8\xB0\x83\xE8\x89\xB2"  /* 调色 */
#define S_T_AMB        "\xE7\x8E\xAF\xE5\xA2\x83\xE6\xB8\xA9\xE5\xBA\xA6"  /* 环境温度 */
#define S_T_JUNC       "\xE8\x8A\xAF\xE7\x89\x87\xE7\xBB\x93\xE6\xB8\xA9"  /* 芯片结温 */
#define S_T_DELTA      "\xE6\xB8\xA9\xE5\xB7\xAE"  /* 温差 */
#define S_T_TS         "\xE5\x86\x85\xE9\x83\xA8\xE4\xBC\xA0\xE6\x84\x9F\xE5\x99\xA8"  /* 内部传感器 */
#define S_H_REL        "\xE7\x9B\xB8\xE5\xAF\xB9\xE6\xB9\xBF\xE5\xBA\xA6"  /* 相对湿度 */
#define S_H_COMFORT    "\xE8\x88\x92\xE9\x80\x82"  /* 舒适 */
#define S_H_DRY        "\xE5\x81\x8F\xE5\xB9\xB2"  /* 偏干 */
#define S_H_WET        "\xE5\x81\x8F\xE6\xB9\xBF"  /* 偏湿 */
#define S_E_COUNT      "\xE8\xAE\xA1\xE6\x95\xB0"  /* 计数 */
#define S_E_DIR        "\xE6\x96\xB9\xE5\x90\x91"  /* 方向 */
#define S_E_CW         "\xE9\xA1\xBA\xE6\x97\xB6\xE9\x92\x88"  /* 顺时针 */
#define S_E_CCW        "\xE9\x80\x86\xE6\x97\xB6\xE9\x92\x88"  /* 逆时针 */
#define S_E_IDLE       "\xE9\x9D\x99\xE6\xAD\xA2"  /* 静止 */
#define S_E_RESET      "\xE6\x8C\x89\x20\x4B\x45\x59\x20\xE6\xB8\x85\xE9\x9B\xB6"  /* 按 KEY 清零 */
#define S_S_VER        "\xE7\x89\x88\xE6\x9C\xAC"  /* 版本 */
#define S_S_UPTIME     "\xE8\xBF\x90\xE8\xA1\x8C\xE6\x97\xB6\xE9\x95\xBF"  /* 运行时长 */
#define S_S_MEM        "\xE5\x86\x85\xE5\xAD\x98"  /* 内存 */
#define S_S_USED       "\xE5\xB7\xB2\xE7\x94\xA8"  /* 已用 */
#define S_S_THREAD     "\xE7\xBA\xBF\xE7\xA8\x8B"  /* 线程 */
#define S_S_FPS        "\xE5\xB8\xA7\xE7\x8E\x87"  /* 帧率 */
#define S_W_START      "\xE5\x90\xAF\xE5\x8A\xA8"  /* 启动 */
#define S_W_STOP       "\xE5\x81\x9C\xE6\xAD\xA2"  /* 停止 */
#define S_W_RESET      "\xE6\xB8\x85\xE9\x9B\xB6"  /* 清零 */
#define S_B_SHORT      "\xE7\x9F\xAD\xE5\x93\x8D"  /* 短响 */
#define S_B_LONG       "\xE9\x95\xBF\xE5\x93\x8D"  /* 长响 */
#define S_B_TRIPLE     "\xE8\xBF\x9E\xE5\x93\x8D"  /* 连响 */
#define S_B_HINT       "\xE6\x8C\x89\x20\x4B\x45\x59\x20\xE8\xA7\xA6\xE5\x8F\x91"  /* 按 KEY 触发 */
#define S_L_SLOW       "\xE6\x85\xA2\xE9\x97\xAA"  /* 慢闪 */
#define S_L_FAST       "\xE5\xBF\xAB\xE9\x97\xAA"  /* 快闪 */
#define S_L_ON         "\xE5\xB8\xB8\xE4\xBA\xAE"  /* 常亮 */
#define S_L_OFF        "\xE5\x85\xB3\xE9\x97\xAD"  /* 关闭 */
#define S_L_MODE       "\xE5\xBD\x93\xE5\x89\x8D\xE6\xA8\xA1\xE5\xBC\x8F"  /* 当前模式 */
#define S_BL_ON        "\xE6\x89\x93\xE5\xBC\x80"  /* 打开 */
#define S_BL_OFF       "\xE5\x85\xB3\xE9\x97\xAD"  /* 关闭 */
#define S_BL_STATE     "\xE7\x8A\xB6\xE6\x80\x81"  /* 状态 */
#define S_AB_BOARD     "\xE5\xBC\x80\xE5\x8F\x91\xE6\x9D\xBF"  /* 开发板 */
#define S_AB_CHIP      "\xE8\x8A\xAF\xE7\x89\x87"  /* 芯片 */
#define S_AB_FREQ      "\xE4\xB8\xBB\xE9\xA2\x91"  /* 主频 */
#define S_AB_SCR       "\xE5\xB1\x8F\xE5\xB9\x95"  /* 屏幕 */
#define S_AB_INPUT     "\xE8\xBE\x93\xE5\x85\xA5"  /* 输入 */
#define S_AB_BUILD     "\xE6\x9E\x84\xE5\xBB\xBA"  /* 构建 */
#define S_AB_LVGL      "\xE5\x9B\xBE\xE5\xBD\xA2\xE5\xBA\x93"  /* 图形库 */
#define S_AB_RTOS      "\xE7\xB3\xBB\xE7\xBB\x9F"  /* 系统 */


/* ── 调色 App（lcd_tune_ui.c）──────────────────────────────
 * 字库脚本 gen_font_zh.sh 只从本文件 #define 行尾的原文注释中提取汉字，
 * 下方每条 define 行尾注释里的字必须列全，否则屏上显示为空心方块。 */
#define S_TN_WARM    "\xE5\x86\xB7\xE6\x9A\x96"  /* 冷暖 */
#define S_TN_GAMMA   "\xE6\x98\x8E\xE6\x9A\x97"  /* 明暗 */
#define S_TN_SAT     "\xE9\xA5\xB1\xE5\x92\x8C"  /* 饱和 */
#define S_TN_H_COLD  "\xE5\xB7\xA6\xE5\x86\xB7"  /* 左冷 */
#define S_TN_H_WARM  "\xE5\x8F\xB3\xE6\x9A\x96"  /* 右暖 */
#define S_TN_H_DARK  "\xE5\xB7\xA6\xE6\x9A\x97"  /* 左暗 */
#define S_TN_H_LIGHT "\xE5\x8F\xB3\xE4\xBA\xAE"  /* 右亮 */
#define S_TN_H_GRAY  "\xE5\xB7\xA6\xE7\x81\xB0"  /* 左灰 */
#define S_TN_H_VIVID "\xE5\x8F\xB3\xE8\x89\xB3"  /* 右艳 */
#define S_TN_RESET   "\xE6\x81\xA2\xE5\xA4\x8D\xE9\xBB\x98\xE8\xAE\xA4"  /* 恢复默认 */

/* ── 图片 App（ui_apps.c 的 app_img_build）──────────────────────
 * 看图工具：原寸（默认）/ 铺满 / 放大 三视图切换。
 * 三个视图只做几何变换、不改颜色，像素须与桌面壁纸保持一致。 */
#define S_A_IMG      "\xE5\x9B\xBE\xE7\x89\x87"  /* 图片 */
#define S_I_ORIG     "\xE5\x8E\x9F\xE5\xAF\xB8"  /* 原寸 */
#define S_I_FIT      "\xE9\x93\xBA\xE6\xBB\xA1"  /* 铺满 */
#define S_I_ZOOM     "\xE6\x94\xBE\xE5\xA4\xA7"  /* 放大 */
#define S_I_NONE     "\xE6\x97\xA0\xE5\x9B\xBE\xE7\x89\x87"  /* 无图片 */

/* ── 游戏 App（lcd_game_ui.c 的 app_game_build）────────────────────
 * 横屏跑酷：猫跳石头。文案只有三处，但字库脚本要从行尾注释提字，
 * 因此每条 define 行尾注释里的汉字须逐个列全，否则屏上显示空心方块。 */
#define S_A_GAME     "\xE6\xB8\xB8\xE6\x88\x8F"              /* 游戏 */
#define S_G_BEST     "\xE6\x9C\x80\xE9\xAB\x98"              /* 最高 */
#define S_G_HINT     "\xE6\x8C\x89\xE9\x94\xAE\xE5\xBC\x80\xE5\xA7\x8B"  /* 按键开始 */
#define S_G_TIP      "\xE8\xB7\xB3\xE9\x81\xBF\xE9\x9A\x9C\xE7\xA2\x8D"  /* 跳避障碍 */

/* ── ESP32-C3 联网状态（ui_apps.c 的 app_sys_build 下半区）──────────
 * 系统页里嵌一块"C3 联网"信息区。文案用字：
 *   联 网 状 态 在 线 离 线 信 号 剩 余 内 存 运 行 上 报
 *   （数值本身是数字/英文，不占字库） */
#define S_S_C3_HDR   "\xE8\x81\x94\xE7\xBD\x91\xE7\x8A\xB6\xE6\x80\x81"  /* 联网状态 */
#define S_S_C3_UP    "\xE5\x9C\xA8\xE7\xBA\xBF"                          /* 在线 */
#define S_S_C3_DOWN  "\xE7\xA6\xBB\xE7\xBA\xBF"                          /* 离线 */
#define S_S_C3_RSSI  "\xE4\xBF\xA1\xE5\x8F\xB7"                          /* 信号 */
#define S_S_C3_FREE  "\xE5\x89\xA9\xE4\xBD\x99\xE5\x86\x85\xE5\xAD\x98"  /* 剩余内存 */
#define S_S_C3_UP2   "\xE8\xBF\x90\xE8\xA1\x8C"                          /* 运行 */

/* ── WiFi 状态文案（ui_apps.c 的 c3_wifi_state_text）────────────────
 * 这组文案原先是在 ui_apps.c 里直接写的裸中文字面量，而字库脚本
 * 只扫描 ui_text.h，导致其中两个汉字未进字库，屏上显示为空心方块。
 * 因此约定：凡会渲染到屏上的中文一律走本文件的宏，.c 中不写裸中文字面量。 */
#define S_C3S_CONN   "\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD"              /* 连接中 */
#define S_C3S_OK     "\xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5"              /* 已连接 */
#define S_C3S_IP     "\xE5\xB7\xB2\xE8\x8E\xB7\xE5\x8F\x96\x49\x50IP"            /* 已获取IP */
#define S_C3S_DOWN   "\xE5\xB7\xB2\xE6\x96\xAD\xE5\xBC\x80"              /* 已断开 */
#define S_C3S_FAIL   "\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xA4\xB1\xE8\xB4\xA5"  /* 连接失败 */
#define S_C3S_OFF    "\xE6\x9C\xAA\xE8\x81\x94\xE7\xBD\x91"              /* 未联网 */

/* ── 桌面状态栏（ui.c 的 build_status_bar / sb_timer_cb）──────────────
 * 顶部一行：时间 + 日期 + 联网状态。
 * 时间/日期来自 C3 的 NTP 对时（NS800_CMD_TIME 帧）。
 * 星期几用到 7 个汉字：周 一 二 三 四 五 六 日，须逐字列全。 */
#define S_TM_NONE    "--:--"                                    /* 未对时（占位，全 ASCII） */
#define S_WD_0       "\xE5\x91\xA8\xE6\x97\xA5"                 /* 周日 */
#define S_WD_1       "\xE5\x91\xA8\xE4\xB8\x80"                 /* 周一 */
#define S_WD_2       "\xE5\x91\xA8\xE4\xBA\x8C"                 /* 周二 */
#define S_WD_3       "\xE5\x91\xA8\xE4\xB8\x89"                 /* 周三 */
#define S_WD_4       "\xE5\x91\xA8\xE5\x9B\x9B"                 /* 周四 */
#define S_WD_5       "\xE5\x91\xA8\xE4\xBA\x94"                 /* 周五 */
#define S_WD_6       "\xE5\x91\xA8\xE5\x85\xAD"                 /* 周六 */

/* 状态栏右侧的联网状态词（比系统页那套更短，一行放得下） */
#define S_SB_ONLINE  "\xE5\xB7\xB2\xE8\x81\x94\xE7\xBD\x91"     /* 已联网 */
#define S_SB_LINKING "\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD"     /* 连接中 */

/* ── 配网 App（ui_apps.c 的 app_net_build）──────────────────────────
 * 用汉字：配 网 模 式 连 接 路 由 开 热 点 名 称 密 码 保 存 当 前 已 失 败
 *        应 用 清 除 放 未 设 置
 * （STA / AP 是 ASCII，不占字库） */
#define S_A_NET      "\xE9\x85\x8D\xE7\xBD\x91"                          /* 配网 */
#define S_N_MODE     "\xE6\xA8\xA1\xE5\xBC\x8F"                          /* 模式 */
#define S_N_STA      "\xE8\xBF\x9E\xE6\x8E\xA5\xE8\xB7\xAF\xE7\x94\xB1"  /* 连接路由 */
#define S_N_AP       "\xE5\xBC\x80\xE7\x83\xAD\xE7\x82\xB9"              /* 开热点 */
#define S_N_SSID     "\xE5\x90\x8D\xE7\xA7\xB0"                          /* 名称 */
#define S_N_PASS     "\xE5\xAF\x86\xE7\xA0\x81"                          /* 密码 */
#define S_N_SAVE     "\xE4\xBF\x9D\xE5\xAD\x98"                          /* 保存 */
#define S_N_CUR      "\xE5\xBD\x93\xE5\x89\x8D"                          /* 当前 */
#define S_N_SAVED    "\xE5\xB7\xB2\xE4\xBF\x9D\xE5\xAD\x98"              /* 已保存 */
#define S_N_FAIL     "\xE4\xBF\x9D\xE5\xAD\x98\xE5\xA4\xB1\xE8\xB4\xA5"  /* 保存失败 */
#define S_N_APPLY    "\xE5\xBA\x94\xE7\x94\xA8"                          /* 应用 */
#define S_N_CLR      "\xE6\xB8\x85\xE9\x99\xA4"                          /* 清除 */
#define S_N_OPEN     "\xE5\xBC\x80\xE6\x94\xBE"                          /* 开放 */
#define S_N_SET      "\xE6\x9C\xAA\xE8\xAE\xBE\xE7\xBD\xAE"              /* 未设置 */

/* ── 闹钟 App（ui_apps.c 的 app_alarm_build）──────────────────────
 * 用到汉字：闹 钟 新 建 添 加 删 除 开 关 无 铃 响 已 停 止 待 定 数
 *           最 多 个 到 点 满 旋 钮 左 右 选 择 确 认 取 消 状 态 时 分 值
 *           （数字、冒号、开关符号为 ASCII，不占字库）
 * 字库脚本只从本文件 #define 行尾注释提取汉字，新增文案后需重跑 gen_font_zh.sh。 */
#define S_A_ALARM    "\xE9\x97\xB9\xE9\x92\x9F"                          /* 闹钟 */
#define S_AL_NONE    "\xE6\x97\xA0\xE9\x97\xB9\xE9\x92\x9F"              /* 无闹钟 */
#define S_AL_ADD     "\xE6\xB7\xBB\xE5\x8A\xA0"                          /* 添加 */
#define S_AL_DEL     "\xE5\x88\xA0\xE9\x99\xA4"                          /* 删除 */
#define S_AL_ON      "\xE5\xBC\x80\xE5\x90\xAF"                          /* 开启 */
#define S_AL_OFF     "\xE5\x85\xB3\xE9\x97\xAD"                          /* 关闭 */
#define S_AL_RING    "\xE9\x93\x83\xE5\x93\x8D"                          /* 铃响 */
#define S_AL_TIP     "\xE6\x97\xB6\xE9\x97\xB4\xE5\x88\xB0\xE6\x8E\xA5\xE5\x93\x8D"  /* 时间到接响 */
#define S_AL_FULL    "\xE6\x9C\x80\xE5\xA4\x9A"                          /* 最多 */
#define S_AL_STOP    "\xE5\x81\x9C\xE6\xAD\xA2"                          /* 停止 */
#define S_AL_LIST    "\xE9\x97\xB9\xE9\x92\x9F\xE5\x88\x97\xE8\xA1\xA8"  /* 闹钟列表 */
#define S_AL_HINT    "\xE6\x97\x8B\xE8\xBD\xAC\xE9\x80\x89\x20\xE6\x8C\x89\x4B\x45\x59\xE5\xBC\x80\xE5\x85\xB3"  /* 旋转选 按KEY开关 */
#define S_AL_NOTIME  "\xE6\x9C\xAA\xE5\xAF\xB9\xE6\x97\xB6"              /* 未对时 */
#define S_AL_FIRED   "\xE5\xB7\xB2\xE5\x93\x8D\xE9\x93\x83"              /* 已响铃 */
#define S_AL_HOUR    "\xE6\x97\xB6"                                      /* 时 */
#define S_AL_MIN     "\xE5\x88\x86"                                      /* 分 */
#define S_AL_EDIT    "\xE6\x8C\x89\x4B\x45\x59\xE8\xB0\x83\xE5\x80\xBC"  /* 按KEY调值 */

#endif /* __UI_TEXT_H__ */
