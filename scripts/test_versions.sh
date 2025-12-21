#!/bin/bash
# 测试版本管理：创建文件并通过重命名/删除触发版本，验证 filename@versions 列表

set -e

MOUNT_POINT="/tmp/smartbackup"

echo "确保文件系统已挂载到 ${MOUNT_POINT}（使用 ./scripts/run.sh -d）"
if ! mountpoint -q "${MOUNT_POINT}"; then
    echo "错误：未检测到挂载点 ${MOUNT_POINT}"
    exit 1
fi

TESTFILE="${MOUNT_POINT}/version_test.txt"

echo "创建文件并写入内容..."
echo "version 1" > "$TESTFILE"

echo "重命名触发版本快照（rename） - 1..."
mv "$TESTFILE" "${MOUNT_POINT}/version_test_renamed1.txt"
mv "${MOUNT_POINT}/version_test_renamed1.txt" "$TESTFILE"

echo "修改并重命名触发版本快照（rename） - 2..."
echo "version 2" > "$TESTFILE"
mv "$TESTFILE" "${MOUNT_POINT}/version_test_renamed2.txt"
mv "${MOUNT_POINT}/version_test_renamed2.txt" "$TESTFILE"

echo "列出版本（在删除前）..."
echo "版本列表内容："
ls -la "${MOUNT_POINT}/version_test.txt@versions" || true
echo "测试访问最新版本："
cat "${MOUNT_POINT}/version_test.txt@latest" || echo "无法访问latest版本"

echo "删除触发版本快照（unlink）..."
rm -f "$TESTFILE"

echo "完成。若看到 v1 v2 等项，说明版本管理基本工作。删除后可再次 ls 但可能找不到基准文件。"
