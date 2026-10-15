#!/usr/bin/env python3
"""校验 ui_text.h 里的 \\xNN 转义字节与行尾注释原文是否一致。

用法：
    python3 check_text_escapes.py           # 只检查，有错返回 1
    python3 check_text_escapes.py --fix     # 自动修正（保留行尾 /* 原文 */ 注释）

背景（2026-10-11 真实踩坑）：
    手工写 UTF-8 转义时手抖，"闹钟"的闹写成 \\xE9\\x97\\xB0（应为 \\xA9），
    屏上显示成乱码，而且串口按 UTF-8 打印时落在邻近码位，
    打出来是"闰钟" —— 极像"字库缺字"，把我引到错误方向查了很久。
    字库其实完全正常，错的只有那一个字节。

    所以：**改完 ui_text.h 一定要跑这个脚本**，别去动字库。
"""
import re
import sys

PATH = 'ui_text.h'
DEF_RE = re.compile(r'^\s*#define')
TAIL_RE = re.compile(r'/\*\s*(.+?)\s*\*/\s*$')
ESC_RE = re.compile(r'\\x([0-9A-Fa-f]{2})')
ESC_SPAN_RE = re.compile(r'(?:\\x[0-9A-Fa-f]{2})+')


def check(fix=False):
    lines = open(PATH, encoding='utf-8').read().split('\n')
    bad = 0
    for i, line in enumerate(lines):
        m = TAIL_RE.search(line)
        if not m or not DEF_RE.match(line) or not ESC_RE.search(line):
            continue

        want = m.group(1).strip()
        raw = bytes(int(x, 16) for x in ESC_RE.findall(line))
        try:
            got = raw.decode('utf-8')
        except UnicodeDecodeError:
            got = None

        if got == want:
            continue

        bad += 1
        print('第 %d 行不匹配：' % (i + 1))
        print('  注释原文: %s' % want)
        print('  实际解码: %s' % (got if got else '<UTF-8 解码失败>'))
        print('  正确字节: %s' % ' '.join('\\x%02X' % c for c in want.encode('utf-8')))

        if fix:
            span = ESC_SPAN_RE.search(line)
            s, e = span.span()
            lines[i] = line[:s] + ''.join(
                '\\x%02X' % c for c in want.encode('utf-8')) + line[e:]

    if fix and bad:
        open(PATH, 'w', encoding='utf-8').write('\n'.join(lines))
        print('\n已自动修正 %d 行' % bad)
    elif bad:
        print('\n共 %d 处不匹配，加 --fix 可自动修正' % bad)
    else:
        print('全部通过')

    return bad


if __name__ == '__main__':
    sys.exit(1 if check('--fix' in sys.argv) else 0)
