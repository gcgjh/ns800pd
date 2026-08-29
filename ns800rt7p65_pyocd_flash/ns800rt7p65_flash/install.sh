#!/usr/bin/env bash
# -*- coding: utf-8 -*-
# NS800RT7P65 pyocd 烧录支持部署脚本
# 用法：bash install.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYOCD_DIR="${HOME}/.pyocd"
FLASH_ALGO_DIR="${PYOCD_DIR}/flash_algo"
SVD_DIR="${PYOCD_DIR}/svd"

echo "=============================================="
echo " NS800RT7P65 pyocd 烧录支持 - 部署脚本"
echo "=============================================="

# 1. 检查 pyocd
if ! command -v pyocd >/dev/null 2>&1; then
    echo "❌ 未检测到 pyocd，请先安装："
    echo "   pip3 install pyocd"
    echo "   （国内加速）pip3 install -i https://mirrors.aliyun.com/pypi/simple/ pyocd"
    exit 1
fi
echo "✅ 检测到 pyocd：$(pyocd --version)"

# 2. 复制 FLM 算法文件
echo ""
echo "📦 部署 Flash 算法（FLM）到 ${FLASH_ALGO_DIR} ..."
mkdir -p "${FLASH_ALGO_DIR}"
cp -v "${SCRIPT_DIR}/flash_algo/"*.FLM "${FLASH_ALGO_DIR}/" 2>/dev/null || {
    echo "⚠️  未找到 FLM 文件，请确认 flash_algo/ 目录完整"
}

# 3. 复制 SVD 文件（可选，用于调试时显示寄存器）
echo ""
echo "📦 部署 SVD 寄存器描述（可选）到 ${SVD_DIR} ..."
if ls "${SCRIPT_DIR}/svd/"*.svd >/dev/null 2>&1; then
    mkdir -p "${SVD_DIR}"
    cp -v "${SCRIPT_DIR}/svd/"*.svd "${SVD_DIR}/" 2>/dev/null || true
else
    echo "⚠️  未找到 SVD 文件（不影响烧录，仅影响调试时寄存器显示）"
fi

# 4. 提示复制 pyocd_user.py 到 bsp 目录
echo ""
echo "=============================================="
echo " ✅ 部署完成！"
echo "=============================================="
echo ""
echo "最后一步：把 pyocd_user.py 复制到你的 RT-Thread bsp 目录："
echo ""
echo "  cp \"${SCRIPT_DIR}/pyocd_user.py\" \\"
echo "     ~/RT_Thread/rt-thread/bsp/novosns/ns800/ns800rt7p65-nssinepad/"
echo ""
echo "然后在该 bsp 目录下执行："
echo ""
echo "  pyocd list                              # 确认识别 NS800RT7P65"
echo "  pyocd flash -t ns800rt7p65 rtthread.elf # 烧录"
echo ""
echo "如果 pyocd list 报 'No available debug probes'，"
echo "请先解决 USB HID 权限（见 README.md 排查章节）。"
