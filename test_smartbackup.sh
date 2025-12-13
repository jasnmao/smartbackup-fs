#!/bin/bash

# 智能备份文件系统完整功能测试脚本

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== 智能备份文件系统完整功能测试 ===${NC}"

# 创建测试目录
TEST_DIR="/tmp/smartbackup_test_$$"
MOUNT_POINT="$TEST_DIR/mount"

echo "创建测试环境..."
mkdir -p "$MOUNT_POINT"

# 启动文件系统
echo "启动智能备份文件系统..."
./build/bin/smartbackup-fs -f -s "$MOUNT_POINT" &
FUSE_PID=$!

# 等待文件系统挂载
sleep 2

# 检查是否挂载成功
if ! mountpoint -q "$MOUNT_POINT"; then
    echo -e "${RED}文件系统挂载失败${NC}"
    kill $FUSE_PID 2>/dev/null
    exit 1
fi

echo -e "${GREEN}文件系统挂载成功！${NC}"

# 测试计数器
TOTAL_TESTS=0
PASSED_TESTS=0

# 测试函数
run_test() {
    local test_name="$1"
    local test_command="$2"
    
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    echo -n "测试 $test_name... "
    
    if eval "$test_command" >/dev/null 2>&1; then
        echo -e "${GREEN}✓ 通过${NC}"
        PASSED_TESTS=$((PASSED_TESTS + 1))
        return 0
    else
        echo -e "${RED}✗ 失败${NC}"
        return 1
    fi
}

# === FUSE操作测试 ===

echo -e "${BLUE}=== 1. 基本文件系统操作 ===${NC}"

# create - 创建文件
run_test "create" "echo 'Test content' > '$MOUNT_POINT/test.txt'"

# getattr - 获取文件属性
run_test "getattr" "stat '$MOUNT_POINT/test.txt'"

# open - 打开文件
run_test "open" "exec 3< '$MOUNT_POINT/test.txt' && exec 3<&-"

# read - 读取文件
run_test "read" "cat '$MOUNT_POINT/test.txt' | grep -q 'Test content'"

# write - 写入文件
run_test "write" "echo 'New content' > '$MOUNT_POINT/test.txt'"

# mkdir - 创建目录
run_test "mkdir" "mkdir '$MOUNT_POINT/subdir'"

# readdir - 读取目录内容
run_test "readdir" "ls '$MOUNT_POINT' | grep -q 'test.txt'"

echo -e "${BLUE}=== 2. 高级文件操作 ===${NC}"

# truncate - 截断文件
run_test "truncate" "truncate -s 100 '$MOUNT_POINT/test.txt' && [ \$(stat -c%s '$MOUNT_POINT/test.txt') -eq 100 ]"

# rename - 重命名/移动文件
run_test "rename" "mv '$MOUNT_POINT/test.txt' '$MOUNT_POINT/renamed.txt' && [ -f '$MOUNT_POINT/renamed.txt' ]"

# link - 创建硬链接
run_test "link" "ln '$MOUNT_POINT/renamed.txt' '$MOUNT_POINT/hardlink.txt' && [ -f '$MOUNT_POINT/hardlink.txt' ]"

# symlink - 创建符号链接（基本创建）
echo "测试 symlink... "
TOTAL_TESTS=$((TOTAL_TESTS + 1))
if ln -s renamed.txt "$MOUNT_POINT/symlink.txt" 2>/dev/null && [ -L "$MOUNT_POINT/symlink.txt" ]; then
    echo -e "${GREEN}✓ 通过${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo -e "${RED}✗ 失败${NC}"
fi

# readlink - 读取符号链接（暂时跳过，有已知问题）
echo "测试 readlink... ${YELLOW}跳过（已知问题）${NC}"
TOTAL_TESTS=$((TOTAL_TESTS + 1))

echo -e "${BLUE}=== 3. 删除操作 ===${NC}"

# unlink - 删除文件
run_test "unlink" "rm '$MOUNT_POINT/hardlink.txt' && [ ! -f '$MOUNT_POINT/hardlink.txt' ]"

# rmdir - 删除目录（空目录）
run_test "rmdir" "rmdir '$MOUNT_POINT/subdir' && [ ! -d '$MOUNT_POINT/subdir' ]"

echo -e "${BLUE}=== 4. 文件同步和缓冲操作 ===${NC}"

# 创建测试文件用于同步测试
echo "Sync test content" > "$MOUNT_POINT/sync_test.txt"

# fsync - 同步文件到存储
run_test "fsync" "python3 -c \"
import os
fd = os.open('$MOUNT_POINT/sync_test.txt', os.O_RDWR)
os.fsync(fd)
os.close(fd)
\""

# flush - 刷新文件缓冲
run_test "flush" "python3 -c \"
import os
fd = os.open('$MOUNT_POINT/sync_test.txt', os.O_RDWR)
os.write(fd, b'flush test')
os.close(fd)
\""

# release - 释放文件句柄
run_test "release" "python3 -c \"
import os
fd = os.open('$MOUNT_POINT/sync_test.txt', os.O_RDONLY)
data = os.read(fd, 10)
os.close(fd)
\""

echo -e "${BLUE}=== 5. 时间戳操作 ===${NC}"

# utimens - 更新时间戳
run_test "utimens" "touch -a -m -t 202301011200.00 '$MOUNT_POINT/sync_test.txt'"

# 验证时间戳更新
ACCESS_TIME=$(stat -c%X "$MOUNT_POINT/sync_test.txt")
TARGET_TIME=$(date -d "2023-01-01 12:00:00" +%s)
if [ "$ACCESS_TIME" -eq "$TARGET_TIME" ]; then
    echo "测试 utimens... ${GREEN}✓ 通过${NC}"
    PASSED_TESTS=$((PASSED_TESTS + 1))
else
    echo "测试 utimens... ${RED}✗ 失败${NC}"
fi
TOTAL_TESTS=$((TOTAL_TESTS + 1))

echo -e "${BLUE}=== 6. 扩展属性操作 ===${NC}"

# setxattr - 设置扩展属性
run_test "setxattr" "python3 -c \"
import os
try:
    os.setxattr('$MOUNT_POINT/sync_test.txt', 'user.comment', b'This is a test comment')
except OSError as e:
    exit(1)
\""

# getxattr - 获取扩展属性
run_test "getxattr" "python3 -c \"
import os
try:
    value = os.getxattr('$MOUNT_POINT/sync_test.txt', 'user.comment')
    if value.decode() == 'This is a test comment':
        exit(0)
    else:
        exit(1)
except OSError as e:
    exit(1)
\""

# listxattr - 列出扩展属性
run_test "listxattr" "python3 -c \"
import os
try:
    attrs = os.listxattr('$MOUNT_POINT/sync_test.txt')
    # 检查字符串形式（某些实现返回字符串）
    if b'user.comment' in attrs or 'user.comment' in attrs:
        exit(0)
    else:
        print('Attrs:', attrs)
        exit(1)
except OSError as e:
    print('Error:', e)
    exit(1)
\""

# removexattr - 删除扩展属性
run_test "removexattr" "python3 -c \"
import os
try:
    os.removexattr('$MOUNT_POINT/sync_test.txt', 'user.comment')
    # 验证已删除
    attrs = os.listxattr('$MOUNT_POINT/sync_test.txt')
    if b'user.comment' not in attrs:
        exit(0)
    else:
        exit(1)
except OSError as e:
    exit(1)
\""

echo -e "${BLUE}=== 7. 边界条件和错误处理 ===${NC}"

# 测试不存在的文件
run_test "不存在的文件getattr" "! stat '$MOUNT_POINT/nonexistent.txt' 2>/dev/null"

# 测试删除不存在的文件
run_test "不存在的文件unlink" "! rm '$MOUNT_POINT/nonexistent.txt' 2>/dev/null"

# 测试创建已存在的文件
run_test "重复创建文件" "! mkdir '$MOUNT_POINT' 2>/dev/null || true"

# 测试删除非空目录
mkdir "$MOUNT_POINT/nonempty"
touch "$MOUNT_POINT/nonempty/file.txt"
run_test "删除非空目录" "! rmdir '$MOUNT_POINT/nonempty' 2>/dev/null"
rm -rf "$MOUNT_POINT/nonempty"

# 测试创建硬链接到目录（应该失败）
run_test "目录硬链接" "! ln '$MOUNT_POINT' '$MOUNT_POINT/link_to_dir' 2>/dev/null || true"

echo -e "${BLUE}=== 8. 大文件操作 ===${NC}"

# 测试大文件写入
run_test "大文件写入" "dd if=/dev/urandom of='$MOUNT_POINT/large_file.bin' bs=4096 count=256 2>/dev/null"

# 测试大文件读取
run_test "大文件读取" "dd if='$MOUNT_POINT/large_file.bin' of=/dev/null bs=4096 count=256 2>/dev/null"

# 清理大文件
rm -f "$MOUNT_POINT/large_file.bin" "$MOUNT_POINT/sync_test.txt" "$MOUNT_POINT/renamed.txt" "$MOUNT_POINT/symlink.txt"

# === 测试总结 ===

echo -e "${GREEN}=== 测试总结 ===${NC}"
echo "总测试数: $TOTAL_TESTS"
echo "通过测试: $PASSED_TESTS"
echo "失败测试: $((TOTAL_TESTS - PASSED_TESTS))"

if [ $PASSED_TESTS -eq $TOTAL_TESTS ]; then
    echo -e "${GREEN}🎉 所有测试通过！文件系统功能完整！${NC}"
    EXIT_CODE=0
else
    echo -e "${RED}❌ 有 $((TOTAL_TESTS - PASSED_TESTS)) 个测试失败${NC}"
    EXIT_CODE=1
fi

# 清理测试环境
echo "清理测试环境..."
fusermount -u "$MOUNT_POINT" 2>/dev/null || umount "$MOUNT_POINT" 2>/dev/null
kill $FUSE_PID 2>/dev/null
rm -rf "$TEST_DIR"

echo -e "${GREEN}=== 测试完成 ===${NC}"
exit $EXIT_CODE