# LVGL 制作规范（AI 协作标准）

> 适用：RT-Thread 5.x + LVGL 9.1.0 + NS800RT7P65（nssinepad BSP）+ ILI9341 SPI 屏 + EC11 编码器。
> 来源：本项目全部实测结论，每条都被真实编译/烧录/屏幕照片验证过。
> 任何 AI 在本仓库写 LVGL/UI/字库/输入代码前，必须先读完本文档。

---

## 0. 环境与构建铁律

1. **每条 shell 命令必须显式 `cd` 到 BSP 根**：
   `/home/linlianhui/RT_Thread/rt-thread/bsp/novosns/ns800/ns800rt7p65-nssinepad`
   上一条命令的 cwd 不会保留，凭记忆省略 cd 必然翻车。
2. **编译前先加载工具链**：`cd /home/linlianhui/RT_Thread/rt-thread/bsp/novosns/ns800 && source env.sh && scons -j8`
   （env.sh 内部会自己 cd 进 BSP；在 BSP 内部 source 它反而会报错。）
3. **假烧录判据**：烧录器报的 programmed 字节数与上次完全相同 = 烧的是旧固件。
   改动后必须确认 bin 时间戳/体积变化，或强制重烧。
4. 「改了没生效」排查链（按序）：
   ① 源文件 mtime 是否晚于 rtthread.bin ② 是否真的重编译 ③ 是否假烧录
   ④ 是否改对了宏（警惕外层 `#if` 把配置整个包住，见 §2）。

---

## 2. lv_conf.h 配置规范（路径 `applications/lvgl/lv_conf.h`）

1. **`LV_USE_PERF_MONITOR` 包在 `#if LV_USE_SYSMON` 里**。
   只改 PERF_MONITOR 不开 SYSMON = 永远不生效。grep 到 define ≠ 生效，必须检查外层条件链。
   FPS 监视器三件套：`LV_USE_SYSMON 1` + `LV_USE_PERF_MONITOR 1` + `LV_PERF_MONITOR_POS LV_INDICATOR_TOP_RIGHT`。
2. `LV_DEF_REFR_PERIOD` 是**帧率上限**（当前 1ms = 不限制）。改它不会让静止画面 FPS 变高。
3. `LV_MEM_ADR` 硬编码地址（0x20100000 = SRAM1）必须与 `board/linker_scripts/link.lds` 的
   `.lvgl_pool` 段一致。两处漂移 = 内存池盖穿其他数据，极难排查。启动自检已内置于 lv_port_disp.c。
4. v9 已移除 `LV_COLOR_16_SWAP`；RGB565 字节序在 flush_cb 里调 `lv_draw_sw_rgb565_swap()` 解决。

---

## 3. 显示移植规范（`applications/lvgl/lv_port_disp.c`）

1. PARTIAL 渲染模式，缓冲 `DISP_BUF_LINES × 240`（当前 20 行）。加大行数可减少 flush 次数。
2. flush_cb 结尾**必须**调 `lv_display_flush_ready(disp)`，否则 LVGL 不发下一帧。
3. **当前显示链路无 DMA**：`drv_spi.c` 的 xfer 是 `SPI_transmitReceive` 逐字节阻塞轮询，
   NS800 StdDriver 的 spi.h 也没有 DMA API。全屏刷新极限 ~40FPS（SPI 带宽 + CPU 轮询开销）。
   优化阶梯：① xfer 换 `SPI_pollingFifoTransaction` ② SPI 16bit 宽度免 swap
   ③ 加大 DISP_BUF_LINES ④ 用 edma.h 手搓 SPI TX DMA + 异步 flush（终极方案）。
4. 帧率三要素取 max 决定每帧间隔：`LV_DEF_REFR_PERIOD`（上限）+ 主循环
   `mdelay()`（`packages/LVGL-v9.1.0/env_support/rt-thread/lv_rt_thread_port.c`，当前 1）
   + dirty 区域大小（SPI 带宽）。
5. perf monitor 的 FPS = **每秒实际重绘轮数**。静止画面不重绘 → 数字低是正常机制，不是 bug。

---

## 4. 输入设备规范

### 4.1 EC11 编码器（`applications/ec11_eqep.c` / `lv_port_indev.c`）
1. 一律用 `ec11_get_steps()`（格数，rt_int32_t，一格棘爪 = 1，顺 +1 / 逆 −1），
   **禁止**直接拿 `ec11_get_position()` 原始计数当索引/增量。
2. 回绕原理：QPOSCNT 逆时针跨 0 硬件回绕到 0xFFFFFFFF，`ec11_get_steps()` 内部
   `(rt_int32_t)` 强转后回绕值自然变负数（0xFFFFFFFC → −1 格），累计转角不可能超 ±2^31，无需补偿。
3. 每格计数换算宏 `EC11_COUNTS_PER_DETENT`（当前 4 = 2X 解码 × 每格 2 个 AB 周期，实测校准）。
   改解码分辨率（eqep.h：4X/2X/1X_RESOLUTION）时必须同步改此宏并实测。
4. 索引消费方仍需自行 `%N`（环形）或 clamp（限幅），驱动只保证符号正确。
5. `lv_port_indev.c` 的 `encoder_read_cb`：增量 = get_steps 有符号差值，
   `ENC_DIFF_CLAMP 24` 限幅防爆转；每棘爪 enc_diff = ±1。

### 4.2 编码器 KEY（`ui.c` / `ui_apps.c`）
1. LVGL 9.1 的 encoder indev 按一次 KEY 会连发 `LV_EVENT_KEY` + `LV_EVENT_SHORT_CLICKED` +
   `LV_EVENT_CLICKED`，**删掉 KEY 分支**，SHORT_CLICKED/CLICKED 两分支留着。
2. **每个点击回调的第一句都过 `ui_click_ok()`**（`ui.h` 导出，150ms 时间戳去抖）。
   它比"每个回调自己写 static 去抖"省事，也避免新加的按钮忘了去抖而一次触发两次。
3. **一个对象只注册一次点击回调**：`ui_button(p, txt, w, h, cb)` 的 `cb` 传了就不再
   `lv_obj_add_event_cb`；要带 `user_data`（例如按钮档位号）就 `cb` 传 `NULL`，只留后一次注册。
   注册两次 = 一次按键触发两遍，症状是蜂鸣器连响两声、LED 模式跳两格。
4. **beep 包（packages/beep-v1.2.0）有单线程缺陷**：静态蜂鸣线程存活期（~140ms）内再次
   调 beep() 会卡死内核。规则：同一个回调里 beep() 必须放在最后一行，且不得连发。

---

## 5. 中文字库规范（`applications/lvgl/ui/lv_font_zh18.c`）

### 5.1 标准做法：一条脚本重生成（`applications/lvgl/ui/gen_font_zh.sh`）

```bash
cd <BSP> && sh applications/lvgl/ui/gen_font_zh.sh
```

脚本从 `applications/lvgl/ui/ui_text.h` 里每条 `#define ... /* 原文 */` 注释中提取**全部汉字**，
再调 lv_font_conv 生成 `lv_font_zh18.c`。**新增/改动任何中文文案后必须重跑**，否则新字显示成方块。

关键参数（脚本已内置，改脚本时别丢）：
- 拉丁字形来自 `/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf`，range `0x20-0x7E,0xB0,0x2022`
- 汉字来自 `/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf`（**单体 ttf**）
- `--lv-font-name zh18` 保持符号名不变；`--lv-include lvgl.h` 决定产物里的 include 写法
- **必须用系统 node26**（`/home/linlianhui/.local/bin/node`）；WorkBuddy 自带 node 的 WASM 会挂

### 5.2 四个坑

1. **⚠️ 必须 `--no-compress`（2026-10-06 踩实）**：lv_font_conv 1.5.3 **默认输出压缩位图**
   （产物里 `.bitmap_format = 1`），而本工程 `lv_conf.h` 的 `LV_USE_FONT_COMPRESSED = 0`。
   LVGL 的 `lv_font_get_bitmap_fmt_txt()` 在这种情况下直接
   `LV_LOG_WARN(...); return NULL;` → **屏上所有汉字变成空心方块**。
   最坑的是**字形查找其实成功了**（cmap 正常、`fontinfo` 也正常），很容易误判成"字库没生成"，
   往字号/字体/编码方向白查半天。判据：`grep bitmap_format lv_font_zh18.c` 必须是 `= 0`；
   或设备上敲 `fontinfo`。想用压缩格式（省 flash）就去把 `LV_USE_FONT_COMPRESSED` 打开，二者必须匹配。
2. **`.ttc` 字体集合不被支持**：lv_font_conv 报 `Unsupported OpenType signature ttcf`。
   系统里的 `NotoSansCJK-Regular.ttc`、`wqy-zenhei.ttc`、`uming.ttc` 都用不了，
   要么找单体 ttf，要么先用 fonttools 拆包。
3. **`-r`/`--symbols` 分属前一个 `--font`**：多字体合并时，每个 `-r`/`--symbols` 只作用于
   紧邻它之前那个 `--font`。所以"拉丁走 range、汉字走 symbols"必须按这个顺序写。
4. **`-r` 会静默丢弃 range 之外的字形**：汉字只写 `--symbols`、只写 `-r` 都不行，
   脚本里汉字单独走 `--symbols` 就是为了这个。

### 5.3 源码里的中文

- 全部文案集中在 `ui_text.h`，用 UTF-8 转义写法（如 `"\xE6\xB8\xA9\xE5\xBA\xA6"` = 温度），
  规避编辑器/工具链编码漂移；同一行尾注释写原文方便维护。
- ⚠️ C 的 `\x` 转义**会贪婪吃十六进制字符**：`"°C"` 必须写成 `"\xC2\xB0" "C"`，
  写成 `"\xC2\xB0C"` 会被解析成 `\xC2` + `\xB0C`（溢出）而编译报错或显示错字。
- `zh18` 只含 ASCII + ° + • + 汉字子集，**不含 LVGL 符号区**：用它的 label 里不要嵌
  `LV_SYMBOL_*`。图标里的符号要用 `lv_font_montserrat_32` 单独做 label。

#### 5.3.1 🔴 铁律：**`.c` 文件里不许写中文裸字面量**（2026-10-11 乱码事故）

`gen_font_zh.sh` 的汉字来源是 **只扫 `ui_text.h`**（脚本第 35-47 行，从 `#define` 行尾
`/* 原文 */` 注释里抽 CJK）—— **它完全不扫 `.c` 文件**。

于是 `.c` 里直接 `return "已获取IP";` 这类**裸中文**是字库脚本的**盲区**：
那些字根本不会进 `lv_font_zh18.c` → 屏上渲染成**空心方块**。

**真实事故（2026-10-11）**：`ui_apps.c` 的 `c3_wifi_state_text()` 把 6 条状态文案
硬编码在 `return` 里，其中只有 **「获」「断」两个字**没被别处文案覆盖到 →
屏上显示 `已□取IP` / `已□开`，被主人报成"联网状态乱码"。
（13 个用字里恰好缺这 2 个，用 Python 复现脚本提取逻辑才定位到。）

**修法与规矩**：
- 凡会渲染到屏上的中文，**一律在 `ui_text.h` 建宏**（如 `S_C3S_IP`），`.c` 里只引宏。
- 改完文案**必须重跑** `sh applications/lvgl/ui/gen_font_zh.sh`。
- 快速自查（复现脚本的提取逻辑，看某组字是否齐全）：
  ```bash
  python3 - ui_text.h <<'EOF'
  import re,sys
  txt=open(sys.argv[1],encoding='utf-8').read(); ch=set()
  for l in txt.splitlines():
      m=re.search(r'/\*\s*(.+?)\s*\*/\s*$',l)
      if m and l.lstrip().startswith('#define'):
          ch|={c for c in m.group(1) if 0x4E00<=ord(c)<=0x9FFF}
  need=set("已获取断开连接失败未联网")   # ← 换成你要核的字
  print("缺失:", ''.join(sorted(need-ch)) or "无 ✓")
  EOF
  ```
- 生成后核对字库是否真含目标字（`unicode_list_1` 是相对 `range_start=176` 的偏移）：
  ```bash
  python3 -c "
  import re;t=open('lv_font_zh18.c',encoding='utf-8').read()
  v=[int(x,16) for x in re.findall(r'0x([0-9a-fA-F]+)',re.search(r'unicode_list_1\[\] = \{(.*?)\};',t,re.S).group(1))]
  cps={176+x for x in v}
  print({c:(ord(c) in cps) for c in '获断'})"
  ```

> ⚠️ 与 5.2 的第 1 条（`--no-compress`）区分：那个是"字形在字库里但解不出来"，
> 这个是"字形压根没进字库"。二者的**表象都是空心方块**，但 `fontinfo` 只能查出前者 ——
> 所以先 `fontinfo` 排除压缩位图，再按本节查字库覆盖。

---

## 6. UI 代码规范（`applications/lvgl/ui/`）

### 6.1 当前界面结构（手机桌面式 App 网格，2026-10-06 重做）

| 文件 | 职责 |
|---|---|
| `ui.c` | 壁纸 / **桌面状态栏（时间+日期+联网，§6.7）** / App 网格（每页 3x3，可翻页） / App 页框架（导航栏+滑入滑出+group 切换）+ App 描述表 `g_apps[APP_COUNT]` + 全屏接管 `ui_set_fullscreen()` + **累计时长常驻落盘 `uptime_flush_cb()`** |
| `ui_apps.c` | 各 App 内容（温度 湿度 编码器 系统 秒表 蜂鸣器 LED 背光 **图片** 关于） |
| `lcd_tune_ui.c` | 调色 App（独立文件，用 `lv_slider`，见 §6.4） |
| `lcd_game_ui.c` | **横屏跑酷游戏**（独立文件，纯 LVGL 图元绘制，见 §6.6） |
| `ui_text.h` | 全部中文文案（UTF-8 转义，一行一条）**—— .c 里不许写裸中文，见 §5.3.1** |
| `gen_font_zh.sh` | 从 `ui_text.h` 提取汉字重建字库 |
| `ui.h` | 主题色 + 组件工厂（`ui_label`/`ui_card`/`ui_button`/`ui_make_focusable`/`ui_click_ok`） |
| `settings.c/.h` | 板级配置持久化（LittleFS `/cfg.txt`），含 `uptime_saved` 累计时长（§6.7.3） |
| `wallpaper_store.c/.h` | 240x320 RGB565 壁纸存取（外置 flash → `.wall_buf`），也是图片 App 的数据源 |

**加一个新 App 的完整清单**（4 处，缺一不可）：
1. `ui_text.h` 加名字宏 → 重跑 `sh applications/lvgl/ui/gen_font_zh.sh`
2. `ui_apps.c` 写 `app_xxx_build()`（+ 需要时 `app_xxx_teardown()`）；独立文件（如 `lcd_game_ui.c`）也行，
   `applications/SConscript` 的 `Glob('*.c')` 会自动收
3. `ui_apps.h` 声明 `build`（`teardown` 若被 `ui.c` 调用则不能是 `static`）
4. `ui.c`：`APP_COUNT` +1、`g_apps[]` 插一行（`{ 名, 图标, 颜色, build, tick, period, flat_nav, teardown }`）、
   `ui.h` 加一个 `AC_XXX` 图标色
   ⚠️ **`APP_COUNT` 必须与表项数严格相等** —— 写大了不是编译错误而是**运行期硬故障**
   （`uitest`/`uiapp` 越界读出 NULL 的 build 指针，调用即 usage fault，2026-10-11 实测踩过一次）。
   `ui.c` 里已加 `_Static_assert(sizeof(g_apps)/sizeof(g_apps[0]) == APP_COUNT, ...)` 兜底。


层次：`lv_screen_active()` → 壁纸两段渐变 + 2 团光晕 → `g_layer_home`(透明) + `g_layer_app`(不透明,初始 HIDDEN)。
App 页滑入 = `lv_obj_set_x(240→0)`；滑出后 `HIDDEN` + `lv_obj_clean(body)`。
group 跟着切换：桌面 9 个格子 ⇄ App 内的「返回 + 各按钮」，**切换前必须 `lv_group_remove_all_objs()`**。

### 6.2 硬性坑位

1. **动画完成回调必须挂上，它是唯一的解锁时机**。`app_open/close` 用 `g_anim_lock` 防重入，
   解锁只发生在 `lv_anim_set_completed_cb` 里；漏挂一次 → lock 永远为 1 → **返回键彻底失效**。
   `slide_x()` 现在对 `done == NULL` 兜底成 `slide_lock_release`，别改掉这个兜底。
2. **别用 `transform_scale` 做焦点放大**：PARTIAL 渲染模式下，被缩放对象的**渐变填充渲染不可靠**
   ——实测焦点图标整块变成灰白（同一帧里其他图标颜色正常）。改用 `lv_obj_set_size()` +
   `lv_obj_set_pos()` 一起做动画（网格图标就是这么干的：向上"生长"，不挤下方的标签）。
   （真要用 transform 的话，`transform_pivot` 默认在左上，必须显式设 `LV_PCT(50)`，否则往右下跑。）
3. **被放大的子对象要 `LV_OBJ_FLAG_OVERFLOW_VISIBLE`**：否则父容器（cell）会把放大后的图标裁掉。
4. **`LV_OPA_15` 不存在**：LVGL 只有 0/10/20/…/100/COVER 这几档，写 15 直接编译报错。
5. **LVGL 不是线程安全的**：窗口内所有 LVGL 调用只能在 LVGL 线程做。msh 命令里想驱动界面，
   必须通过 `lv_timer` 转进 LVGL 线程（参考 `uitest`）。
6. **进 App 后要立刻出数据**：`lv_timer_create()` 首次触发要等一个 period，页面会空一段。
   建完 `lv_timer_ready(t)` 让它在下一帧就跑一次。
7. **module 级缓存进页面时要重置**：tick 里用 `static`/文件级变量做「值变化才重绘」的缓存，
   重进页面时 label 已经重建（回到占位文本），但缓存还是旧值 → **页面永远停在占位文本**。
   所有这类缓存都要在 `build()` 里重置/同步（见 `hm_last10`、`en_last_steps`、`sw_last_shown`）。
8. **`teardown()` 差点永远不被调用**（2026-10-11 做游戏 App 时才发现，属于既有隐患）：
   `close_done_cb` 原来用 `if (g_cur_app >= 0 && ...teardown)` 判断该调谁的清理钩子，
   但 `app_close()` 在**启动淡出动画之前**就已经把 `g_cur_app = -1` 了 —— 等这个异步回调
   跑起来时判据恒为假，**teardown 从来没被调用过**。图片/调色 App 没暴露是因为它们的指针
   清理在 `build()` 里也做了一遍；游戏 App 必须在 teardown 里"切回竖屏"才露馅。
   现已改为 `app_close()` 先存 `g_closing_app = g_cur_app`，`close_done_cb` 用它。
   ⚠️ 以后凡是"离开页面必须收尾"的逻辑（停 timer、还硬件、切方向）都要确认 teardown 真的跑了。
9. **别用 `lv_indev_get_state()` 做按键沿检测**（2026-10-11 实测）：`encoder_read_cb`
   每 30ms 才被调用一次，`data->state` 只在那一刻刷新；切换分辨率/重排布局时 LVGL 主循环
   被拖住，read_cb 延后 → 读到**陈旧状态** → 同一个"按下"被连续判成多个上升沿
   （实测屏幕在横竖屏之间一秒抖十几次）。**正确做法：直接读引脚电平**
   （`rt_pin_read(31)`，EC11 的 KEY31，上拉、按下为低），自己做"连续 N 次同电平才认账"的消抖。

### 6.3 其它

1. 定时器刷新遵守「**值变化才重绘**」：先 diff 再 set_text，减小 dirty 区域。
2. perf monitor 定制（透明/纵排/去标题）改在
   `packages/LVGL-v9.1.0/src/others/sysmon/lv_sysmon.c`：
   `lv_sysmon_create()` 里 `bg_opa = LV_OPA_TRANSP`、`pad_all = 0`；
   `perf_observer_cb()` fmt 改为 `"%"LV_PRIu32" FPS\n%"LV_PRIu32"%% CPU"`。
   改包内文件必须留 `/* NS800 */` 注释标记，防止包更新时丢失。
   **位置**在 `lv_conf.h` 的 `LV_USE_PERF_MONITOR_POS`：右上角已被自绘状态栏占用，现设 `LV_ALIGN_BOTTOM_LEFT`。
3. 桌面网格焦点反馈 = 图标 `transform_scale` 256↔300 动画（160ms）+ 标签变色 + 底部提示换字。

### 6.5 内存预算实录：别再想"再分配一块 150K"（2026-10-11，图片 App 踩坑）

> 起因：做图片 App 的"灰度"视图，思路是 `lv_malloc(153600)` 再来一块缓冲做像素级灰度。
> **结论：这块内存根本不存在，分配必然失败。** 写代码前先看这三个数字：

| 区域 | 容量 | 实际占用 | 备注 |
|---|---|---|---|
| LVGL 池 `LV_MEM_SIZE` | **64 K** | 峰值 ~16K | `lv_malloc` 从这里出，150K 想都别想 |
| SRAM12 | 256 K | **214 K (83.6%)** | `.lvgl_pool` 64K + `.wall_buf` 150K，已排满 |
| RT-Thread 堆 `RT_HEAP` | 56 K | 链接期即 100% | `rt_malloc` 也拿不出 150K |
| DTCM | 71 K | 47.7 K (65.6%) | 剩 ~24K，同样不够 |

**可用的"大内存"只有两处**：`.wall_buf`（150K，一张图的专属缓冲，被 `wall_buf` 独占）、
外置 LittleFS（15.8 MB，但 LVGL 需要可寻址内存，不能直接显示）。

**正确做法**：需要"改图效果"时优先用 LVGL 原生样式，而不是自己备一份像素：
- 色调/灰度滤镜 → `lv_obj_set_style_image_recolor(obj, color, 0)` +
  `lv_obj_set_style_image_recolor_opa(obj, LV_OPA_70, 0)`（按像素混合，**零额外内存**）
  ⚠️ 切回无滤镜时必须把 `recolor_opa` 置 `LV_OPA_TRANSP`，否则色调残留。
- 几何变换（缩放/裁切）→ `lv_image_set_scale_x/y()` + 父容器裁切，零内存。
- 真要逐像素处理 → 只能在 `.wall_buf` 上**原地**改（它就是那份唯一的缓冲），
  而且要能接受"改了以后桌面壁纸也变了"。

**Flash 侧同理**：`FLASH 430K/512K (82.1%)`，剩 ~92K。字库从 113 字扩到 125 字就让
`lv_font_zh18.c` 涨了 ~1.2K —— 加中文文案前先算账（§5）。

**`lv_image` + `lv_image_dsc_t` 的三个坑**（图片 App 实测）：
1. `header.magic` **必填** `LV_IMAGE_HEADER_MAGIC`（0x19）。漏了 `lv_image_src_get_type()`
   认不出这是描述符，会把它当成**文件路径字符串**去解析。
2. LVGL 9.1 的 `lv_scale_t` 是**结构体**（不是整数）→ `lv_image_set_scale_x/y()` 直接传整数，
   别写 `(lv_scale_t)(256*286/320)`，会报 "conversion to non-scalar type requested"。
   `lv_image_set_scale(obj, 256)` 仍收整数（内部转结构体），1.0 = 256。
3. `dsc` 结构体**必须常驻**（`static`）：LVGL 长期持有指针，不能放栈上。
   裁切靠父容器（子对象超出父 bounds 默认不画），**别**给父容器加
   `LV_OBJ_FLAG_OVERFLOW_VISIBLE`。

**想让页面里的图"和桌面壁纸一个颜色"** → 该 App 必须 `flat_nav = 1`。
原因：`g_layer_app` 默认带 `0x141820 @ LV_OPA_90`（90% 不透明深蓝）当读数页背景，
**它会整个盖住壁纸**。`app_open()` 里 `flat_nav=1` 会把这层 bg_opa 置 TRANSP，
于是层级变成「壁纸 → 暗纱（40% 黑，两页一致）→ 控件」，颜色才和桌面相同。
调色台（要拿背景当颜色参照）和图片 App 都是这个理由。

### 6.4 脏区重绘与"拖滑块卡顿"优化（2026-10-11，实测全套结论）

> 症状：**调色页拖滑块一顿一顿**。根因不是 SPI 慢，而是每步做了两件不该做的事。
> 完整链条 + 四个坑都在这，改任何"拖动/实时预览"类页面照抄即可。

**① 拖拽中不要写硬件。** 原来 `slider_cb` 每步调 `apply_tuning()` 写 gamma 寄存器
（`0xE0/0xE1` 各 16 字节）。ILI9341 收到 gamma 写会**内部重扫整屏**（几十 ms，
与 SPI 带宽无关的固有延迟）→ 每格卡一下。改为：拖拽中只更新 LVGL 侧，在
`LV_EVENT_RELEASED` 里写一次。肉眼只看停住那刻的值，过程零收益。
同理 `settings_save()`（LittleFS erase/write 几十 ms）也必须挪到松手后，且只存改动项。

**② 拖拽中不要整屏 invalidate。** 详见下面坑 2。

**③ 坑 1：`lv_slider_set_value()` 不会触发 `LV_EVENT_VALUE_CHANGED`。**
读源码确认：`lv_slider_set_value` → `lv_bar_set_value` → `lv_bar_set_value_with_anim`，
走 `LV_ANIM_OFF` 分支时**只 `lv_obj_invalidate()`，不发 VALUE_CHANGED**。
VALUE_CHANGED 只在 `lv_slider_event` 的 `LV_EVENT_KEY` / `LV_EVENT_ROTARY` 分支里
`lv_obj_send_event()`。结论：**只有"旋钮/按键"这条输入路径才会进 `slider_cb`**，
程序里直接 set_value 是死路（A/B 压测第一次就是这么测出"两组数字一模一样"的）。

**④ 坑 2：`lv_obj_invalidate(lv_scr_act())` 会被静默丢弃。**
`_lv_inv_area()`（lv_refr.c:253）有个合并守卫：
```c
for (i = 0; i < disp->inv_p; i++)
    if (_lv_area_is_in(&com_area, &disp->inv_areas[i], 0)) return;  // 被包含 → 丢弃
```
slider 自己改值时就已 `lv_obj_invalidate(sl)` 把小矩形塞进 `inv_areas`；
之后整屏 invalidate **包含**那个小矩形 → 命中守卫 → 整屏这次被丢掉。
**所以"整屏 invalidate"必须先做、且只做这一个动作**（不要再 invalidate 子对象），
否则假全屏（实测假全屏只有 0.62 屏，和脏区没差别）。`LV_INV_BUF_SIZE=32`。

**⑤ 坑 3：`lv_obj_send_event(obj, LV_EVENT_KEY, &key)` 发不到控件。**
`lv_slider_event()` 第一句是 `lv_obj_event_base(MY_CLASS, e)` —— **父类 KEY 处理先跑**，
把 `LV_KEY_RIGHT` 当焦点导航吃掉并返回，滑块自己的 KEY 分支到不了。

**⑥ 坑 4：正解 = `lv_group_send_data()`，且必须先聚焦。**
看 `lv_indev.c` 的 encoder 派发（约 1012 行）：**只有 group 在 edit 模式**时，
旋钮 diff 才通过 `lv_group_send_data(g, LV_KEY_RIGHT)` 下发。
`lv_group_send_data` 会把 key 转成 `LV_EVENT_KEY` 发给 **group 的 focused 对象**
（`lv_group.c:306`）。⚠️ 进 App 时焦点在「返回」按钮上，滑块只是 `CLICK_FOCUSABLE`
—— 不先 `lv_group_focus_obj(slider)`，key 全被返回按钮吃掉。
**真机三步缺一不可：`lv_group_focus_obj` → `lv_group_set_editing(g, true)` → `lv_group_send_data`。**

**⑦ 坑 5：压测节拍器不能阻塞 LVGL 线程。**
`rt_thread_mdelay()` 在 `lv_async_call` 回调里睡的就是 LVGL 线程本身 → 主循环停摆
（实测 40 格全冻结、flush=5）。更坑的是 `app_open()` 的 `g_anim_lock` 靠
130ms 淡入动画的 `lv_anim_set_completed_cb` 解锁，不渲染就永不回调
→ 之后 `app_open/close` 被静默拒绝。**正确做法：一格 = 一次 `lv_async_call`**，
或用一个 `lv_timer`（跑在 LVGL 线程内）每 period 走一格。

**⑧ 量化收益（`tune drag 40` @20Hz，FPS 计 8.2s 窗口，slider_cb 均触发 40 次）**

| 模式 | 实刷像素 | 折算整屏 | SPI 等效带宽 |
|---|---|---|---|
| 整屏 invalidate（旧） | 3,044,303 px | 39.63 屏 | 724 KB/s |
| 脏区重绘（新） | 1,263,873 px | 16.45 屏 | **300 KB/s** |

**像素流量 −58.5%，SPI 带宽 −59%**。剩余 16.45 屏主要是"数值大字 + 三条滑块"
每次都必须重绘（它们每格都变），属正常开销。

**⑨ 配套：`vi_click_ok()` 别复用给非点击事件。** `ui_click_ok()` 是全局 150ms 节流，
一次按键会连发 KEY/SHORT_CLICKED/CLICKED，用它去抖是对的；但**不能**把它塞进
需要高频触发的回调（例如每格都该响应的拖动），否则会被节流吞掉。

**⑩ 调试命令**：`tune fullscreen 0|1`（切两种模式）· `tune rate HZ`（旋钮转速，
默认 20 格/秒）· `tune drag N`（走真机路径拖 N 格）· `tune status`（看模式 +
`slider_cb` 触发次数，**用来验证回调是否真的被调到**）。
`fpsinfo start` / `fpsinfo` 报实刷像素/整屏数/SPI 带宽。

---

## 6.6 横屏 App 与运行时切方向（2026-10-11，游戏 App）

需求原文：「**进入游戏切横屏，主页还是原样**」——只有游戏页横屏，桌面和其他 App 全竖屏。

### 6.6.1 机制：三件事必须配对

| 层 | 做什么 | 在哪 |
|---|---|---|
| 屏控制器 | `LCD_direction(0\|1)` 改 MADCTL（硬件旋转，**零 CPU 开销**） | `lcd_ili9341.c` |
| LVGL | `lv_display_set_resolution()` + 重调 `lv_display_set_buffers()` | `lv_port_disp.c` → `lv_port_disp_set_orientation(1\|0)` |
| UI 层 | 把 `g_layer_app`/`g_nav`/`g_app_body` 按**新分辨率**重设尺寸 | `ui.c` → `ui_set_fullscreen(1\|0)` |

**调用顺序**（错了就错位，实测过）：
- 进页面：`lv_port_disp_set_orientation(1)` → `ui_set_fullscreen(1)`
- 出页面：`lv_port_disp_set_orientation(0)` → `ui_set_fullscreen(0)`
  （先切方向，UI 层才读得到正确的当前分辨率）

⚠️ **缓冲必须按两个方向里最宽的（320）分配**。`lv_display_set_buffers()` 是用当前分辨率
算 stride/行数的（`lv_display.c:413`，`h = buf_size/stride`）；若按竖屏 240 宽只分 480 B/行，
横屏时 LVGL 会按 640 B/行写 → 溢出踩内存。

⚠️ **导航栏宽度也要重设**。`build_nav()` 是按 `SCR_W`(240) 建死的，横屏不重设就只有 240 宽。
反过来更隐蔽：`ui_set_fullscreen()` 用"当前分辨率"算宽度，所以**退出时若不先把方向切回竖屏**，
会把导航栏设成 320 宽，切回竖屏后就溢出屏外了。

### 6.6.2 美术：程序化绘制，别想放图片

Flash 只剩 ~92 KB、LVGL 池只有 64 KB —— 一张 320×240 RGB565 图要 **153600 B**，放不下。
所以天空/云/丘陵/灌木/地面/猫/石头全部用 `lv_obj` 纯色圆角矩形画出来：
- `lv_obj_remove_style_all()` 后**必须自己设 `bg_opa`**（默认 TRANSP，什么都不显示）；
- 所有装饰 obj 要 `lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE)`，否则被 encoder group 收走、焦点乱跑；
- 天空渐变用 6 条横带做线性插值（**别碰 float**，这块芯片 `rt_kprintf` 都没 `%f`）。

### 6.6.3 性能：静态部分只画一次

一屏 153600 B @ SPI 50MHz ≈ 25 ms。**绝对不能每帧清屏重画**：
静态背景建一次就不动，每帧只 `lv_obj_set_x/y` 移动猫和石头 —— LVGL 脏区机制
只重绘这两个小 obj 的旧/新位置，每帧实刷几千像素，30+ FPS 轻松。

### 6.6.4 游戏侧参数（`lcd_game_ui.c`）

- 游戏区 320×240，全屏接管（导航栏隐藏），`flat_nav = 1`
- 30 ms 自建 `lv_timer` 驱动（比 `app_def_t.period` 直观，teardown 里能明确停掉）
- 物理用"每帧增量"而非 dt 换算（30ms 固定节拍下行为可复现）：重力 +3/帧、起跳 -14
- 最高分存 `settings` 的 `game_best`（`cfgset g <值>` 可手改，`cfg` 能看）
- 调试命令：`gamestate`（状态+方向）、`gamejump`（远程跳/开局，走 `lv_async_call` 进 LVGL 线程）

---

## 6.7 桌面状态栏 + 累计运行时长持久化（2026-10-11）

### 6.7.1 状态栏（`ui.c` 的 `build_status_bar()` / `sb_timer_cb()`）

顶部 24 px 一条：`14:23  10/11 周日  ● 已联网`。时间来自 **C3 的 NTP 对时**
（C3 走 `NS800_CMD_TIME` 帧每秒下发"本地时区时间戳"，详见 §6.7.2）。

- **布局常量要跟着调**：加状态栏后 `GRID_Y` 12 → `(SB_H+12)=36`、`CELL_H` 88 → 82，
  否则网格会顶到屏幕底部、和页码条（y 290..316）相撞。
  `TAG_BOTTOM_UP` 由 `CELL_H` 推导，会自动适配，不用动。
- **自包含 `sb_gmtime()`**：自己算 Y/M/D/H/M/S/wday，**不依赖 newlib 的 `localtime()`**
  —— BSP 的时区表没配，用 newlib 会算错。UTC+8 已在 C3 侧加过，这里只做 tm 拆解。
- **`sb_now_unix()` = 最近一帧时间 + (now_tick - 收到帧时的 tick)**：
  这样即使 C3 的帧隔了几秒才到或丢了几帧，显示也连续、不跳秒。
- **1 s 定时器**：`g_sb_timer` 单开。⚠️ 别复用 App 页的 `g_app_timer`
  —— 那个只在 App 打开时跑（桌面状态是停的）。
- wday：`1970-01-01` 是**周四** → `wday = (days + 4) % 7`（0 = 周日）。

### 6.7.2 时间来源（C3 侧，`~/ESP32_workspace/c3_bridge/main/time_mgr.c`）

NS800 无网络，NTP 只能在 C3 跑；对到时间后走板间链路 `NS800_CMD_TIME(0x08)` 下发
**小端 4 字节 Unix 时间戳**（已换算到 UTC+8）。

- **时区只在一处换算**（C3 的 `time_mgr_local_unix()` = `time(NULL) + 8*3600`），
  NS800 只拆 tm —— 避免两边各算一次对不上。
- ⚠️ **不要在 `app_main` 里同步调 `time_mgr_start()`**：它阻塞最多 75 s
  （等 IP 60 s + 首次对时 15 s），会把心跳任务和 `ota_mgr_confirm_valid()`（回滚确认）
  一起推迟；期间若复位，新 OTA 固件会被 bootloader 回滚。→ 起独立任务 `time_mgr_task`。
- 对时失败**不退出**：每 60 s 重试，否则"开机时路由器慢"会让时间功能永久失效。

### 6.7.3 累计运行时长持久化（`settings.c`）

- 字段 `uptime_saved`（累计总秒数）；`cfg_get()` 的溢出保护必须放大到 **2000000000**
  （原来的 100000 ≈ 27 h，对 uptime 太小会静默截断）。
- 落盘走独立 `settings_uptime_store()`，**绕过 `settings_set()`**
  —— 后者会置 `g_dirty`（"用户改过"），干扰开机"刚载入不回写"的守卫。
- 🔴 **落盘定时器必须常驻**：**别只放在 `app_sys_tick()` 里** ——
  那是"系统 App"的 tick（`app_timer_cb` 有 `g_cur_app >= 0` 守卫），
  **只有系统 App 打开时才跑**。结果就是"停在桌面时 uptime 永不落盘"，
  `cfg` 看到的内存在涨、`/cfg.txt` 里却根本没这一行，复位后历史归零（实测踩过）。
  → 在 `ui_init()` 挂常驻 `lv_timer`，与界面无关。
- `UPTIME_SAVE_PERIOD_S`（60 s）定义在 `settings.h`（两处调用点共用，防改一处忘一处）。

#### 🔴🔴 6.7.3.1 "历史 + 本次"型累加器：**记账必须单一出处**（2026-10-11 踩过）

**典型错误写法**：每个调用点自己算
```c
total = uptime_saved + rt_tick_get()/RT_TICK_PER_SECOND;   /* ❌ */
if (过了 60s) settings_uptime_store(total);                 /* 写回 uptime_saved */
```
**为什么错**：`store(total)` 之后 `uptime_saved` 里**已经含了 now_s**，
下次再 `+ now_s` 就**加了两次**：
```
S 为历史值
 t=60s  → 存 S+60
 t=120s → 算 (S+60)+120 = S+180   ← 真值 S+120，多算 60
 t=180s → 算 (S+180)+180 = S+360   ← 真值 S+240，多算 120
```
误差按 **n²/2 × 周期** 增长 → 表现为"运行时间跑得比真实时间快，且越来越快"。
实测：13 分钟内从 435 秒涨到 101158 秒（真实只过了十几分钟）。

**更糟的放大器**：当时 `ui.c` 和 `ui_apps.c` **各有一份** `static last_saved`，
两个写者互相把对方的落盘值当基线 → 增速再翻倍。

**正确做法：把记账收进一个函数，外部只读/只喊"落盘"**
```c
static rt_int32_t g_flush_now_s = 0;   /* 上次落盘时"本次已运行秒数" */

rt_int32_t settings_uptime_total(void)          /* 读 */
{
    rt_int32_t now_s = settings_uptime_now_s();
    rt_int32_t d = now_s - g_flush_now_s;
    if (d < 0) d = 0;                          /* 外部改过 uptime_saved 时的保险 */
    return g_cfg.uptime_saved + d;              /* 减去已计入的那段，才不重复加 */
}

rt_bool_t settings_uptime_flush(void)           /* 写（自带节流，可高频调用） */
{
    rt_int32_t now_s = settings_uptime_now_s();
    if ((now_s - g_flush_now_s) < UPTIME_SAVE_PERIOD_S) return RT_TRUE;
    if (settings_uptime_store(g_cfg.uptime_saved + (now_s - g_flush_now_s)))
        { g_flush_now_s = now_s; return RT_TRUE; }   /* ← 关键：记住"这几秒已计入" */
    return RT_FALSE;                            /* 失败则基线不动，下周期重试 */
}
```
- 多个调用点都调 `flush()` 是安全的（内部单一记账，不会互相干扰）。
- **外部改 `uptime_saved`（`cfgset u` / 恢复出厂 / 载入）时必须 `g_flush_now_s = 0`**，
  否则用旧基线去减会算出负数或跳变。
- 显示语义：主人的要求是"运行时间要断电保存" → **屏上显示的就是累计总时长**。
  若只显示"本次开机"，一断电屏上就归零，等于没保存。历史/本次拆分放 `cfg` 命令里看。

**验证方法（别只看"数值在涨"，那正是 bug 的表象）**：**测增速**。
记 t0 的累计值，等 2~3 个落盘周期后再读，比对"增量 / 实际经过时间"是否 ≈ 1.0。
接近 1.0 才算对；明显 > 1 就是重复累加。
清零用 `cfgset u 0`（`'u'` 分支别忘了在 `settings_update_one()` 里实现）。

### 6.7.4 验证命令（新增）

| 命令 | 用途 |
|---|---|
| `c3time` | 显示 NS800 当前时间 + "收到 N 秒前"（验证时间同步与刷新） |
| `c3stat` | 链路快照，含 `时间 : <unix> (unix, UTC+8)` 行 |
| `cfg` | 配置快照，`uptime` 行显示 `历史` 与 `本次` 拆分 |
| `cfgset u 0` | **清零累计运行时长**（污染后归零重来） |

---

## 7. 验证流程（每个 UI 改动的闭环）

1. `source env.sh && scons -j22` 全绿、零 warning
2. 确认 bin 时间戳/体积变化（防假烧录）
3. 烧录 + 复位（烧录命令形状见 `FLASHING_AI_STANDARD.md`）
4. **串口跑 `fontinfo`**：打印 zh18 的 `cmap_num/bpp/bitmap_format`。汉字变方块时第一个看它
   （`bitmap_format` 必须是 0），见 §5.2 坑 1。
   ⚠️ `fontinfo` **只能查出"压缩位图解不出来"**；如果是"字形压根没进字库"（§5.3.1 的
   `.c` 裸中文盲区），它也是正常的 —— 那种要按 §5.3.1 核对 `unicode_list_1` 覆盖。
5. **串口跑 `uitest`**：依次打开/关闭全部 App，正常结尾是 `[uitest] all N apps OK`（N = 当前 `APP_COUNT`）。
   它能在没有相机的情况下自动抓出"某个页面 build 崩了 / 返回键失效"这类问题
   （`g_anim_lock` 泄漏就是被它抓到的）。**改完 UI 必跑**。
   跑完再看一次 `free`：`used` 应与跑之前相同 —— 不同就是 App 有泄漏。
6. **改"拖动/实时刷新"类页面，必跑 `tune drag N` + `fpsinfo`**（见 §6.4⑩）：
   先 `tune status` 确认 `slider_cb` 触发次数 == N（**这一步不做，压测等于没测** ——
   我们就在"回调根本没进"的状态下白测了两轮），再看 `fpsinfo` 的实刷像素对比。
7. 看 `[lvgl] pool after ui:` 的 `max_used`：接近 total 就该扩池（LV_MEM_SIZE + link.lds）
8. 让主人拍屏幕照片反馈（文字渲染、布局、配色、焦点位置）
9. **改"掉电保存"类功能（如 uptime），必做断电/复位复测**：
   `cat /cfg.txt` 确认字段真落盘 → 复位 → `cfg` 看 `历史` 是否保留（见 §6.7.3）。
   ⚠️ 读串口时若用 pyserial，**`open()` 前要设 `dtr=False, rts=False, hupcl=False`**，
   否则一读就把板子复位，把要验证的状态清零了。
10. **累计时长类"累加器"要测增速、不能只看"在涨"**（见 §6.7.3.1）：
   间隔 2~3 个落盘周期读两次，比对 `增量/实际经过时间` 是否 ≈ 1.0。
   ⚠️ "数值在涨"恰恰是重复累加 bug 的表象 —— 必须看**比值**。
11. 回到 1

验证要点：中文是否方块、图标是否居中、焦点框是否跟着旋钮走、每棘爪是否步进 1、返回是否顺畅。

---

## 8. 硬件事实速查（NS800RT7P65）

- 无 RTC 日历外设、无 VBAT 引脚、无 LSE；有 BackupLDO + BKPSRAM 4K（0x400B7000，
  链接脚本已留 `.bkupsram` 段）。掉电走时唯一方案：BSP_USING_RTC + 外挂 DS1307（I2C1 @ 0x68）。
- EQEP 解码分辨率：`EQEP_CONFIG_4X/2X/1X_RESOLUTION`（eqep.h:266-268），改档必须同步
  `EC11_COUNTS_PER_DETENT` 并实测每棘爪计数。
- 心跳灯 LED1 = GPIO_68（低电平点亮），模式变量 `g_led_mode`（`applications/main.c` 定义，
  非 static，UI 的 LED App 会改它）：0 慢闪 / 1 快闪 / 2 常亮 / 3 关闭。
- 屏背光 = GPIO22（`PKG_ILI_9341_BLK_PIN`，**低电平点亮**，只能开关、无 PWM 调光）；
  DHT22 = GPIO_40；蜂鸣器 = GPIO_1；编码器 A/B = eQEP3，KEY = GPIO31。
- `QEPCTL.SWI` 写入后不自清，需手动清（见 ec11_eqep.c）。
- 板上资源：双组 BoosterPack 排针、板载 CAN 收发器、EQEP 座、I2C EEPROM、板载调试器（STM32）。
- 高级外设待玩：EMATH/MMATH（硬件 FFT/三角）、CLB×6（类 FPGA）、HRPWM 124ps、
  ADC 4.375MSPS + DAC、CAN-FD、TRNG/HASH-AES。RT-Thread 已适配 drv_adc/can/ecap/epwm/spi/i2c/qspi。

---

*维护约定：新踩的坑必须回写本文档对应章节；文档中的行号/数值改动后同步更新。*
