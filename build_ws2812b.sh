#!/bin/bash
# ESP32-S3 WS2812B LED Board Build Script
# 用于编译 bread-compact-wifi-ws2812b 板级项目

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================="
echo "ESP32-S3 WS2812B LED Board Build Script"
echo "=========================================="

# 检查ESP-IDF环境
if [ -z "$IDF_PATH" ]; then
    echo "错误: ESP-IDF未配置"
    echo "请先运行: . /path/to/esp-idf/export.sh"
    exit 1
fi

cd "$PROJECT_DIR"

# 清理旧构建
echo "清理旧构建..."
rm -rf build/

# 设置目标芯片
echo "设置目标芯片为 ESP32-S3..."
idf.py set-target esp32s3

# 配置项目
echo "配置项目参数..."
idf.py menuconfig

# 编译
echo "开始编译..."
idf.py build

# 检查编译结果
if [ -f "build/xiaozhi.bin" ]; then
    echo ""
    echo "=========================================="
    echo "编译成功!"
    echo "=========================================="
    echo "固件位置: build/xiaozhi.bin"
    echo ""
    ls -lh build/xiaozhi.bin
    echo ""
    echo "烧录命令: idf.py -p /dev/ttyUSB0 flash monitor"
else
    echo "编译失败，请检查错误信息"
    exit 1
fi
