#!/bin/sh
#
# 重新生成中文子集字库 lv_font_zh18.c
#
# 用法：sh applications/lvgl/ui/gen_font_zh.sh
#       改完 ui_text.h 里的文案后必须重跑，否则新字显示成方块。
#
# 字符来源：ui_text.h 里每条 #define 行尾 /* 原文 */ 注释中的全部汉字。
#           拉丁/°(0xB0)/•(0x2022) 由拉丁字体按 range 提供，不在 symbols 里重复。
#
# 两个坑（见 LVGL_AI_STANDARD.md §5）：
#   1. 必须用系统 node26（/home/linlianhui/.local/bin/node）；WorkBuddy 自带 node 的 WASM 会挂。
#   2. .ttc 字体集合不被支持（Unsupported OpenType signature ttcf），只能用单体 ttf。
#   3.   必须 --no-compress：lv_font_conv 默认输出压缩位图（.bitmap_format = 1），
#      而本工程 lv_conf.h 的 LV_USE_FONT_COMPRESSED = 0 → LVGL 解不出位图、直接返回 NULL，
#      屏上所有汉字变成空心方块（字形查找其实是成功的，很容易误判成字库没生成）。
#      想让字库更小就去 lv_conf.h 打开 LV_USE_FONT_COMPRESSED，二者必须匹配。
#
set -e

UI_DIR=$(cd "$(dirname "$0")" && pwd)

PY=/home/linlianhui/.workbuddy/binaries/python/versions/3.13.12/bin/python3
NODE=/home/linlianhui/.local/bin/node
CONV=/home/linlianhui/.workbuddy/binaries/node/workspace/node_modules/lv_font_conv/lv_font_conv.js

#    **先校验转义字节，再生成字库**（顺序不能反）。
#   手工写 \xNN 时手抖是常事，而症状极具误导性：
#   "闹钟"的闹写成 \xB0（应为 \xA9）→ 屏上乱码，
#   串口按 UTF-8 打印时落在邻近码位，打出来是"闰钟" ——
#   **极像"字库缺字"，会把你引去查字库（我为此白查了半天）。**
#   check_text_escapes.py 直接比对字节与注释原文，不一致就报错并给出正确值。
cd "$UI_DIR" || exit 1
"$PY" check_text_escapes.py || {
    echo "!! ui_text.h 的 UTF-8 转义字节与注释原文不一致，先修好（可加 --fix 自动修）" >&2
    exit 1
}

# 拉丁字形用 Noto Sans（数字/字母）；汉字用 Droid Sans Fallback（全字库单体 ttf）
FONT_LATIN=/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf
FONT_CJK=/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf

for f in "$PY" "$NODE" "$CONV" "$FONT_LATIN" "$FONT_CJK"; do
    [ -e "$f" ] || { echo "!! 缺少依赖: $f" >&2; exit 1; }
done

SYMBOLS=$("$PY" - "$UI_DIR/ui_text.h" <<'PYEOF'
import re, sys
txt = open(sys.argv[1], encoding='utf-8').read()
chars = set()
for line in txt.splitlines():
    m = re.search(r'/\*\s*(.+?)\s*\*/\s*$', line)
    if not m or not line.lstrip().startswith('#define'):
        continue
    for ch in m.group(1):
        if 0x4E00 <= ord(ch) <= 0x9FFF:      # 只取 CJK 汉字
            chars.add(ch)
sys.stdout.write(''.join(sorted(chars)))
PYEOF
)

[ -n "$SYMBOLS" ] || { echo "!! 没提取到汉字，检查 ui_text.h" >&2; exit 1; }
echo "提取汉字 $(printf '%s' "$SYMBOLS" | wc -m) 个：$SYMBOLS"

"$NODE" "$CONV" \
    --size 18 --bpp 4 --format lvgl \
    --font "$FONT_LATIN" -r 0x20-0x7E,0xB0,0x2022 \
    --font "$FONT_CJK" --symbols "$SYMBOLS" \
    --lv-font-name zh18 \
    --lv-include lvgl.h \
    --no-compress \
    -o "$UI_DIR/lv_font_zh18.c"

echo "已生成 $UI_DIR/lv_font_zh18.c ($(wc -c < "$UI_DIR/lv_font_zh18.c") bytes)"
grep -n "symbols" "$UI_DIR/lv_font_zh18.c" | head -3
