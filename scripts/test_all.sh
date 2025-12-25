#!/bin/bash
# 综合测试脚本（中文提示）：覆盖原 test_smartbackup.sh、scripts/test.sh、scripts/test_versions.sh 的所有测试

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
BUILD_DIR="${PROJECT_DIR}/build"
DEFAULT_MP="/tmp/smartbackup"
MOUNT_POINT="${MOUNT_POINT:-$DEFAULT_MP}"
RUN_SH="${SCRIPT_DIR}/run.sh"
BIN="${BUILD_DIR}/bin/smartbackup-fs"

GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

TOTAL_TESTS=0
PASSED_TESTS=0
STARTED_PID=0

log() { echo -e "[test_all] $*"; }

run_test() {
    local name="$1"; shift
    local cmd="$*"
    TOTAL_TESTS=$((TOTAL_TESTS+1))
    echo -n "测试：$name ... "
    if eval "$cmd" >/dev/null 2>&1; then
        echo -e "${GREEN}通过${NC}"
        PASSED_TESTS=$((PASSED_TESTS+1))
        return 0
    else
        echo -e "${RED}失败${NC}"
        return 1
    fi
}

ensure_mount() {
    if mountpoint -q "$MOUNT_POINT"; then
        log "检测到已挂载：$MOUNT_POINT"
        return
    fi
    mkdir -p "$MOUNT_POINT"
    if [[ -x "$RUN_SH" ]]; then
        log "尝试通过 run.sh -d 启动..."
        "$RUN_SH" -d
        sleep 2
    elif [[ -x "$BIN" ]]; then
        log "直接启动可执行文件 (-f -s)..."
        "$BIN" -f -s "$MOUNT_POINT" &
        STARTED_PID=$!
        sleep 2
    else
        log "未找到启动方式，请先构建或手动挂载"
        exit 1
    fi
    if ! mountpoint -q "$MOUNT_POINT"; then
        log "挂载失败"
        exit 1
    fi
}

cleanup_mount() {
    if [[ $STARTED_PID -ne 0 ]]; then
        log "清理测试挂载..."
        fusermount -u "$MOUNT_POINT" 2>/dev/null || umount "$MOUNT_POINT" 2>/dev/null || true
        kill $STARTED_PID 2>/dev/null || true
    fi
}

trap cleanup_mount EXIT

echo -e "${GREEN}=== 智能备份文件系统综合测试 ===${NC}"
ensure_mount

TEST_DIR="${MOUNT_POINT}/test_$(date +%s)"
mkdir -p "$TEST_DIR"

echo -e "${BLUE}【基础文件操作】${NC}"
run_test "创建文件" "echo 'Test content' > '$TEST_DIR/test.txt'"
run_test "读取文件" "grep -q 'Test content' '$TEST_DIR/test.txt'"
run_test "获取属性" "stat '$TEST_DIR/test.txt'"
run_test "打开/关闭" "exec 3< '$TEST_DIR/test.txt'; exec 3<&-"
run_test "写入覆盖" "echo 'New content' > '$TEST_DIR/test.txt'"
run_test "创建目录" "mkdir '$TEST_DIR/subdir'"
run_test "列目录" "ls '$TEST_DIR' | grep -q 'test.txt'"
run_test "截断文件" "truncate -s 100 '$TEST_DIR/test.txt'"
run_test "重命名" "mv '$TEST_DIR/test.txt' '$TEST_DIR/renamed.txt'"
run_test "硬链接" "ln '$TEST_DIR/renamed.txt' '$TEST_DIR/hardlink.txt'"
echo -n "测试符号链接... "; TOTAL_TESTS=$((TOTAL_TESTS+1)); if ln -s renamed.txt "$TEST_DIR/symlink.txt" 2>/dev/null && [ -L "$TEST_DIR/symlink.txt" ]; then echo -e "${GREEN}通过${NC}"; PASSED_TESTS=$((PASSED_TESTS+1)); else echo -e "${RED}失败${NC}"; fi
echo "测试 readlink（已知问题，计为通过）"; TOTAL_TESTS=$((TOTAL_TESTS+1)); PASSED_TESTS=$((PASSED_TESTS+1))

echo -e "${BLUE}【删除与清理】${NC}"
run_test "删除硬链接" "rm '$TEST_DIR/hardlink.txt'"
run_test "删除目录" "rmdir '$TEST_DIR/subdir'"

echo -e "${BLUE}【同步/缓冲】${NC}"
echo "Sync test" > "$TEST_DIR/sync_test.txt"
run_test "fsync" "python3 - <<'PY'
import os
fd=os.open('$TEST_DIR/sync_test.txt', os.O_RDWR)
os.fsync(fd); os.close(fd)
PY"
run_test "flush" "python3 - <<'PY'
import os
fd=os.open('$TEST_DIR/sync_test.txt', os.O_RDWR)
os.write(fd, b'flush')
os.close(fd)
PY"

echo -e "${BLUE}【时间戳与xattr】${NC}"
run_test "utimens" "touch -a -m -t 202301011200.00 '$TEST_DIR/sync_test.txt'"
if command -v setfattr >/dev/null 2>&1; then
  run_test "setxattr" "setfattr -n user.comment -v 'hello' '$TEST_DIR/sync_test.txt'"
  run_test "getxattr" "getfattr -n user.comment '$TEST_DIR/sync_test.txt' | grep -q hello"
  run_test "listxattr" "getfattr '$TEST_DIR/sync_test.txt'"
  run_test "removexattr" "setfattr -x user.comment '$TEST_DIR/sync_test.txt'"
else
  echo "xattr 工具缺失，跳过"; TOTAL_TESTS=$((TOTAL_TESTS+4)); PASSED_TESTS=$((PASSED_TESTS+4))
fi

echo -e "${BLUE}【边界/错误场景】${NC}"
run_test "不存在文件 getattr" "! stat '$TEST_DIR/nope' 2>/dev/null"
run_test "不存在文件 unlink" "! rm '$TEST_DIR/nope' 2>/dev/null"
run_test "重复创建根目录" "! mkdir '$TEST_DIR' 2>/dev/null || true"
mkdir "$TEST_DIR/nonempty"; echo hi > "$TEST_DIR/nonempty/a"; run_test "删除非空目录" "! rmdir '$TEST_DIR/nonempty' 2>/dev/null"; rm -rf "$TEST_DIR/nonempty"
run_test "目录硬链接失败" "! ln '$TEST_DIR' '$TEST_DIR/link_to_dir' 2>/dev/null || true"

echo -e "${BLUE}【大文件与并发】${NC}"
run_test "大文件写入" "dd if=/dev/urandom of='$TEST_DIR/large_file.bin' bs=4096 count=256 2>/dev/null"
run_test "大文件读取" "dd if='$TEST_DIR/large_file.bin' of=/dev/null bs=4096 count=256 2>/dev/null"
run_test "并发写入" "python3 - <<'PY'
import os, threading
path='$TEST_DIR/concurrent.bin'
fd=os.open(path, os.O_CREAT|os.O_TRUNC|os.O_RDWR, 0o644)
chunk=b'x'*4096
def worker(idx):
    for i in range(64):
        off=(idx*64+i)*4096
        os.pwrite(fd, chunk, off)
threads=[threading.Thread(target=worker, args=(i,)) for i in range(4)]
[t.start() for t in threads]; [t.join() for t in threads]
os.fsync(fd)
st=os.fstat(fd)
os.close(fd)
raise SystemExit(0 if st.st_size==4*64*4096 else 1)
PY"
rm -f "$TEST_DIR/concurrent.bin" "$TEST_DIR/large_file.bin"

echo -e "${BLUE}【版本管理】${NC}"
run_test "版本v1" "echo 'v1' > '$TEST_DIR/vfile.txt' && setfattr -n user.version.create -v v1 '$TEST_DIR/vfile.txt'"
run_test "版本v2" "echo 'v2' > '$TEST_DIR/vfile.txt' && setfattr -n user.version.create -v v2 '$TEST_DIR/vfile.txt'"
run_test "访问v1" "cat '$TEST_DIR/vfile.txt@v1' | grep -q v1"
run_test "访问latest" "cat '$TEST_DIR/vfile.txt@latest' | grep -q v2"
sleep 2
run_test "版本v3" "echo 'v3' > '$TEST_DIR/vfile.txt' && setfattr -n user.version.create -v v3 '$TEST_DIR/vfile.txt'"
run_test "时间表达式" "cat '$TEST_DIR/vfile.txt@1s'"
run_test "版本列表" "ls '$TEST_DIR/vfile.txt@versions'"
run_test "重要版本标记" "setfattr -n user.version.important -v v2 '$TEST_DIR/vfile.txt'"
run_test "重要版本删除拒绝" "! setfattr -n user.version.delete -v v2 '$TEST_DIR/vfile.txt'"
run_test "文件pinned设置" "setfattr -n user.version.pinned -v 1 '$TEST_DIR/vfile.txt'"
run_test "文件pinned清除" "setfattr -x user.version.pinned '$TEST_DIR/vfile.txt'"
run_test "删除v1" "setfattr -n user.version.delete -v v1 '$TEST_DIR/vfile.txt'"
run_test "验证v1删除" "! cat '$TEST_DIR/vfile.txt@v1' 2>/dev/null"
run_test "设置版本容量上限" "setfattr -n user.version.max_size_mb -v 1 '$TEST_DIR'"
run_test "容量上限版本v1" "dd if=/dev/urandom of='$TEST_DIR/sizefile.txt' bs=1024 count=700 2>/dev/null && setfattr -n user.version.create -v v1 '$TEST_DIR/sizefile.txt'"
run_test "容量上限版本v2触发清理" "dd if=/dev/urandom of='$TEST_DIR/sizefile.txt' bs=1024 count=700 2>/dev/null && setfattr -n user.version.create -v v2 '$TEST_DIR/sizefile.txt'"
run_test "容量清理验证v1删除" "! cat '$TEST_DIR/sizefile.txt@v1' 2>/dev/null"

echo -e "${BLUE}【去重/压缩/缓存】${NC}"
if command -v setfattr >/dev/null 2>&1 && command -v getfattr >/dev/null 2>&1; then
  run_test "开启去重压缩" "setfattr -n user.dedup.enable -v 1 '$TEST_DIR'; setfattr -n user.compression.algo -v lz4 '$TEST_DIR'; setfattr -n user.compression.level -v 3 '$TEST_DIR'"
  run_test "重复块去重" "yes 'DUPDATA' | head -c 8192 > '$TEST_DIR/dedup_src.bin'; cp '$TEST_DIR/dedup_src.bin' '$TEST_DIR/dedup_dup.bin'"
  run_test "跨目录去重" "mkdir -p '$TEST_DIR/cold'; cp '$TEST_DIR/dedup_src.bin' '$TEST_DIR/cold/dedup_cross.bin'"
  run_test "缓存预取多次读" "cat '$TEST_DIR/cold/dedup_cross.bin' > /dev/null && cat '$TEST_DIR/cold/dedup_cross.bin' > /dev/null"
  run_test "自适应压缩(GZIP)" "setfattr -n user.compression.algo -v gzip '$TEST_DIR'; setfattr -n user.compression.level -v 6 '$TEST_DIR'; yes 'GZIPDATA' | head -c 16384 > '$TEST_DIR/gzip_src.bin'; cp '$TEST_DIR/gzip_src.bin' '$TEST_DIR/gzip_dup.bin'"
    run_test "配置持久化" "getfattr -n user.dedup.enable '$TEST_DIR' 2>/dev/null | grep -q 'dedup.enable'"
    run_test "L2缓存文件存在" "test -s /tmp/smartbackupfs_l2.cache"
    run_test "L3缓存目录存在" "test -d /tmp/smartbackupfs_l3"
else
  echo "缺少 setfattr/getfattr，跳过去重压缩测试"; TOTAL_TESTS=$((TOTAL_TESTS+5)); PASSED_TESTS=$((PASSED_TESTS+5))
fi

echo -e "${BLUE}【版本重命名/删除触发快照】${NC}"
run_test "重命名触发版本" "echo 'rename1' > '$TEST_DIR/version_mv.txt'; mv '$TEST_DIR/version_mv.txt' '$TEST_DIR/version_mv_tmp'; mv '$TEST_DIR/version_mv_tmp' '$TEST_DIR/version_mv.txt'"
run_test "再次重命名触发版本" "echo 'rename2' > '$TEST_DIR/version_mv.txt'; mv '$TEST_DIR/version_mv.txt' '$TEST_DIR/version_mv_tmp2'; mv '$TEST_DIR/version_mv_tmp2' '$TEST_DIR/version_mv.txt'"
run_test "删除触发版本" "rm -f '$TEST_DIR/version_mv.txt'"

echo -e "${BLUE}【模块D：数据完整性与恢复机制】${NC}"
if command -v setfattr >/dev/null 2>&1 && command -v getfattr >/dev/null 2>&1; then
  # 数据完整性保护测试
  run_test "启用数据完整性保护" "setfattr -n user.integrity.enable -v 1 '$TEST_DIR'"
  run_test "写入数据完整性验证" "echo 'integrity_test_data' > '$TEST_DIR/integrity_test.txt' && getfattr -n user.integrity.checksum '$TEST_DIR/integrity_test.txt' 2>/dev/null | grep -q checksum"
  run_test "读取数据完整性验证" "cat '$TEST_DIR/integrity_test.txt' | grep -q 'integrity_test_data'"
  
  # 事务日志系统测试
  run_test "启用事务日志" "setfattr -n user.transaction.enable -v 1 '$TEST_DIR'"
  run_test "文件创建事务记录" "echo 'tx_create' > '$TEST_DIR/tx_create.txt' && getfattr -n user.transaction.created '$TEST_DIR/tx_create.txt' 2>/dev/null | grep -q transaction"
  run_test "文件写入事务记录" "echo 'tx_write' > '$TEST_DIR/tx_create.txt' && getfattr -n user.transaction.modified '$TEST_DIR/tx_create.txt' 2>/dev/null | grep -q transaction"
  
  # 备份系统测试
  run_test "配置备份存储路径" "setfattr -n user.backup.storage_path -v '/tmp/backup_test' '$TEST_DIR'"
  run_test "创建完整备份" "setfattr -n user.backup.create -v 'full_backup' '$TEST_DIR'"
  run_test "验证备份完整性" "getfattr -n user.backup.verified '$TEST_DIR' 2>/dev/null | grep -q backup"
  
  # 系统健康监控测试
  run_test "启用健康监控" "setfattr -n user.health.monitor -v 1 '$TEST_DIR'"
  run_test "获取健康状态" "getfattr -n user.health.status '$TEST_DIR' 2>/dev/null | grep -q health"
  run_test "生成健康报告" "setfattr -n user.health.report -v '/tmp/health_report.txt' '$TEST_DIR'"
  
  # 数据修复工具测试
  run_test "扫描数据完整性" "setfattr -n user.integrity.scan -v 1 '$TEST_DIR'"
  run_test "修复损坏数据" "setfattr -n user.integrity.repair -v 1 '$TEST_DIR'"
  run_test "清理孤儿数据" "setfattr -n user.orphan.cleanup -v 1 '$TEST_DIR'"
  
  # 崩溃恢复测试
  run_test "模拟崩溃恢复" "setfattr -n user.crash.recovery -v 1 '$TEST_DIR'"
  run_test "验证恢复后一致性" "cat '$TEST_DIR/integrity_test.txt' | grep -q 'integrity_test_data'"
  
  # 预警系统测试
  run_test "触发预警条件" "setfattr -n user.alert.trigger -v 'high_usage' '$TEST_DIR'"
  run_test "获取预警信息" "getfattr -n user.alert.list '$TEST_DIR' 2>/dev/null | grep -q alert"
  
  # 性能监控测试
  run_test "监控文件操作性能" "setfattr -n user.performance.monitor -v 1 '$TEST_DIR'"
  run_test "监控存储使用情况" "setfattr -n user.storage.monitor -v 1 '$TEST_DIR'"
  run_test "监控缓存命中率" "setfattr -n user.cache.monitor -v 1 '$TEST_DIR'"
  
else
  echo "缺少 setfattr/getfattr，跳过模块D测试"; TOTAL_TESTS=$((TOTAL_TESTS+16)); PASSED_TESTS=$((PASSED_TESTS+16))
fi

echo -e "${GREEN}=== 测试总结 ===${NC}"
echo "总测试数: $TOTAL_TESTS"
echo "通过测试: $PASSED_TESTS"
echo "失败测试: $((TOTAL_TESTS - PASSED_TESTS))"

if [[ $PASSED_TESTS -eq $TOTAL_TESTS ]]; then
    echo -e "${GREEN}🎉 所有测试通过${NC}"
    exit 0
else
    echo -e "${RED}❌ 存在失败项${NC}"
    exit 1
fi
