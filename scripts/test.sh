#!/bin/bash

# 测试脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build"
MOUNT_POINT="/tmp/smartbackup"

# 检查文件系统是否运行
if ! mount | grep -q "${MOUNT_POINT}"; then
    echo "文件系统未运行，请先运行: ./scripts/run.sh"
    exit 1
fi

echo "开始测试文件系统..."

# 测试目录
TEST_DIR="${MOUNT_POINT}/test_$(date +%s)"
echo "创建测试目录: ${TEST_DIR}"
mkdir -p "${TEST_DIR}"

# 测试1: 创建文件
echo "测试1: 创建文件"
echo "Hello, Smart Backup FS!" > "${TEST_DIR}/test1.txt"
ls -la "${TEST_DIR}/test1.txt"
cat "${TEST_DIR}/test1.txt"

# 测试2: 创建目录
echo -e "\n测试2: 创建目录"
mkdir "${TEST_DIR}/subdir"
echo "File in subdirectory" > "${TEST_DIR}/subdir/file.txt"
ls -la "${TEST_DIR}/subdir/"

# 测试3: 复制文件
echo -e "\n测试3: 复制文件"
cp "${TEST_DIR}/test1.txt" "${TEST_DIR}/test1_copy.txt"
diff "${TEST_DIR}/test1.txt" "${TEST_DIR}/test1_copy.txt"
if [[ $? -eq 0 ]]; then
    echo "文件复制成功"
fi

# 测试4: 重命名
echo -e "\n测试4: 重命名文件"
mv "${TEST_DIR}/test1_copy.txt" "${TEST_DIR}/renamed.txt"
if [[ -f "${TEST_DIR}/renamed.txt" ]]; then
    echo "重命名成功"
fi

# 测试5: 删除文件
echo -e "\n测试5: 删除文件"
rm "${TEST_DIR}/renamed.txt"
if [[ ! -f "${TEST_DIR}/renamed.txt" ]]; then
    echo "删除成功"
fi

# 测试6: 权限测试
echo -e "\n测试6: 权限测试"
chmod 644 "${TEST_DIR}/test1.txt"
stat -c "%A %n" "${TEST_DIR}/test1.txt"

# 测试7: 时间戳测试
echo -e "\n测试7: 时间戳测试"
touch "${TEST_DIR}/test1.txt"
stat -c "%y %n" "${TEST_DIR}/test1.txt"

# 测试8: 大文件测试（可选）
echo -e "\n测试8: 大文件测试"
dd if=/dev/urandom of="${TEST_DIR}/large.bin" bs=1M count=10 status=progress
ls -lh "${TEST_DIR}/large.bin"

echo -e "\n测试完成!"
echo "清理测试目录..."
rm -rf "${TEST_DIR}"
echo "测试目录已清理"