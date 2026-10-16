#!/usr/bin/env bash
# 一键构建脚本。
# 用法: ./scripts/build.sh [Debug|Release]
set -euo pipefail

# 切到工程根目录（本脚本位于 scripts/ 下），这样相对路径不会出错。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

BUILD_TYPE="${1:-Release}"
echo "==> 工程目录: $PROJECT_DIR"
echo "==> 构建类型: $BUILD_TYPE"

# 依赖检查：缺库时给出明确的 apt 命令，而不是让 CMake 报一堆看不懂的错。
missing=0
for pkg in libgoogle-glog-dev libyaml-cpp-dev; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        echo "!! 缺少依赖: $pkg"
        missing=1
    fi
done
if ! command -v cmake >/dev/null 2>&1; then
    echo "!! 缺少 cmake"
    missing=1
fi
if [ "$missing" -ne 0 ]; then
    echo ""
    echo "请先安装依赖:"
    echo "  sudo apt update"
    echo "  sudo apt install -y build-essential cmake libgoogle-glog-dev libyaml-cpp-dev"
    exit 1
fi

# 打印依赖版本，便于记录"这次是在哪个版本上编过的"。
echo "==> 依赖版本:"
dpkg-query -W -f='    ${Package} = ${Version}\n' libgoogle-glog-dev libyaml-cpp-dev 2>/dev/null || true

cmake -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build build -j"$(nproc)"

echo ""
echo "==> 构建完成:"
ls -lh build/udp_logger build/udp_sender 2>/dev/null || true
echo ""
echo "运行主程序 (在工程根目录执行，保证配置和日志路径正确):"
echo "  ./build/udp_logger -f config/config.yaml"
echo "或直接用:"
echo "  ./scripts/run.sh"
