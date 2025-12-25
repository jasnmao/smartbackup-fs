#!/bin/bash

echo "=== 事务日志系统完整测试 ==="

# 创建测试目录
TEST_MOUNT_POINT="/tmp/test_transaction_mount"
WAL_DIR="/tmp/smartbackup_wal"

# 清理并创建目录
rm -rf "$TEST_MOUNT_POINT" "$WAL_DIR"
mkdir -p "$TEST_MOUNT_POINT"
mkdir -p "$WAL_DIR"

echo "1. 启动文件系统..."
cd /home/jasonmao/OSExercise/smartbackup-fs
./build/bin/smartbackup-fs -f -s "$TEST_MOUNT_POINT" &
FS_PID=$!
sleep 2

echo "2. 测试事务日志功能..."

# 创建测试目录
TEST_DIR="$TEST_MOUNT_POINT/transaction_test"
mkdir -p "$TEST_DIR"

echo "3. 启用事务日志..."
setfattr -n user.transaction.enable -v 1 "$TEST_DIR"

echo "4. 执行文件操作（应该记录事务）..."

# 创建文件
echo "创建文件测试" > "$TEST_DIR/test_file.txt"

# 修改文件
echo "修改内容" >> "$TEST_DIR/test_file.txt"

# 创建另一个文件
echo "第二个文件" > "$TEST_DIR/test_file2.txt"

echo "5. 检查WAL目录是否创建..."
if [ -d "$WAL_DIR" ]; then
    echo "✅ WAL目录已创建: $WAL_DIR"
else
    echo "❌ WAL目录未创建"
fi

echo "6. 检查WAL文件是否生成..."
# 等待事务日志写入
sleep 2

if ls "$WAL_DIR"/wal_*.segment 1>/dev/null 2>&1; then
    WAL_FILES=$(ls "$WAL_DIR"/wal_*.segment | wc -l)
    echo "✅ WAL文件已生成，数量: $WAL_FILES"
    echo "WAL文件列表:"
    ls -la "$WAL_DIR"/wal_*.segment
else
    echo "❌ 未找到WAL文件"
    echo "WAL目录内容:"
    ls -la "$WAL_DIR" 2>/dev/null || echo "WAL目录不存在"
fi

echo "7. 测试崩溃恢复..."
setfattr -n user.crash.recovery -v 1 "$TEST_DIR"

echo "8. 验证文件系统操作是否正常..."
if [ -f "$TEST_DIR/test_file.txt" ] && [ -f "$TEST_DIR/test_file2.txt" ]; then
    echo "✅ 文件系统操作正常"
    echo "文件内容:"
    cat "$TEST_DIR/test_file.txt"
    cat "$TEST_DIR/test_file2.txt"
else
    echo "❌ 文件系统操作异常"
fi

echo "9. 清理..."
fusermount -u "$TEST_MOUNT_POINT" 2>/dev/null || umount "$TEST_MOUNT_POINT" 2>/dev/null
kill $FS_PID 2>/dev/null
rm -rf "$TEST_MOUNT_POINT"

echo "=== 事务日志系统测试完成 ==="

# 保留WAL文件供检查
echo "WAL文件保留在: $WAL_DIR"
echo "您可以检查WAL文件内容以验证事务记录:"
echo "ls -la $WAL_DIR"
echo "hexdump -C $WAL_DIR/wal_*.segment | head -20"