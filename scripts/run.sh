#!/bin/bash

# 运行智能备份文件系统

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build"
EXECUTABLE="${BUILD_DIR}/bin/smartbackup-fs"
MOUNT_POINT="/tmp/smartbackup"

# 检查可执行文件
if [[ ! -x "${EXECUTABLE}" ]]; then
    echo "错误: 未找到可执行文件 ${EXECUTABLE}"
    echo "请先运行: ./scripts/build.sh"
    exit 1
fi

# 创建挂载点
if [[ ! -d "${MOUNT_POINT}" ]]; then
    echo "创建挂载点: ${MOUNT_POINT}"
    sudo mkdir -p "${MOUNT_POINT}"
    sudo chown $(id -u):$(id -g) "${MOUNT_POINT}"
fi

# 检查是否已挂载
if mount | grep -q "${MOUNT_POINT}"; then
    echo "检测到已挂载的文件系统，正在卸载..."
    fusermount -u "${MOUNT_POINT}"
    sleep 1
fi

# 运行参数
ARGS=()
if [[ "$1" == "-d" ]]; then
    ARGS+=("-f")  # 前台运行，显示调试信息
    shift
fi

# 添加其他参数
ARGS+=("$@")
ARGS+=("${MOUNT_POINT}")

echo "启动智能备份文件系统..."
echo "挂载点: ${MOUNT_POINT}"
echo "参数: ${ARGS[@]}"

# 运行
exec "${EXECUTABLE}" "${ARGS[@]}"