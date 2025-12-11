#!/bin/bash

# 智能备份文件系统构建脚本

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

echo "项目目录: ${PROJECT_DIR}"
echo "构建目录: ${BUILD_DIR}"

# 检查依赖
echo "检查依赖..."
pkg-config --exists fuse3 || {
    echo "错误: 未找到FUSE3库"
    echo "安装依赖: sudo apt-get install libfuse3-dev"
    exit 1
}

pkg-config --exists libzstd || {
    echo "警告: 未找到zstd库"
    echo "安装: sudo apt-get install libzstd-dev"
}

pkg-config --exists liblz4 || {
    echo "警告: 未找到lz4库"
    echo "安装: sudo apt-get install liblz4-dev"
}

# 创建构建目录
mkdir -p "${BUILD_DIR}"

# 配置构建类型
BUILD_TYPE="${1:-Debug}"
if [[ "$BUILD_TYPE" != "Debug" && "$BUILD_TYPE" != "Release" ]]; then
    echo "用法: $0 [Debug|Release]"
    exit 1
fi

echo "构建类型: ${BUILD_TYPE}"

# 运行CMake
cd "${BUILD_DIR}"
cmake "${PROJECT_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev

# 构建
echo "开始构建..."
make -j$(nproc)

echo "构建完成!"
echo "可执行文件: ${BUILD_DIR}/bin/smartbackup-fs"