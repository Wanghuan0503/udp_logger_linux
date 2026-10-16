#!/usr/bin/env bash
# 运行主程序。自动切到工程根目录，保证 config/ 和 logs/ 的相对路径正确。
# 用法: ./scripts/run.sh [配置文件路径]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

CONFIG="${1:-config/config.yaml}"

if [ ! -x build/udp_logger ]; then
    echo "!! 还没构建，请先执行: ./scripts/build.sh"
    exit 1
fi
if [ ! -f "$CONFIG" ]; then
    echo "!! 配置文件不存在: $CONFIG"
    exit 1
fi

echo "==> 使用配置: $CONFIG"
echo '==> Ctrl+C 可优雅退出（观察日志里是否打印"所有线程已退出"）'
echo ""

# 用 exec 让本脚本的进程被主程序替换，这样 Ctrl+C 直接作用在主程序上。
exec ./build/udp_logger -f "$CONFIG"
