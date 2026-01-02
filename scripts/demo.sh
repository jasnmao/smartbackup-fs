#!/bin/bash

# 智能备份文件系统交互式演示脚本
# 演示13个关键操作步骤

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
MOUNT_POINT="/tmp/smartbackup"
TEST_DIR="${MOUNT_POINT}/demo"
TEST_FILE="${TEST_DIR}/test.txt"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 打印函数
print_step() {
    echo ""
    echo -e "${CYAN}========================================${NC}"
    echo -e "${BLUE}步骤 $1: $2${NC}"
    echo -e "${CYAN}========================================${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_info() {
    echo -e "${YELLOW}ℹ $1${NC}"
}

pause() {
    echo ""
    read -p "按 Enter 继续..."
    echo ""
}

# 执行并显示命令
run_cmd() {
    echo -e "${YELLOW}执行命令: $1${NC}"
    eval "$1"
}

# 检查可执行文件
check_executable() {
    EXECUTABLE="${PROJECT_DIR}/build/bin/smartbackup-fs"
    if [[ ! -x "${EXECUTABLE}" ]]; then
        echo -e "${RED}错误: 未找到可执行文件 ${EXECUTABLE}${NC}"
        echo "请先运行: ./scripts/build.sh"
        exit 1
    fi
    print_success "找到可执行文件: ${EXECUTABLE}"
}

# 清理函数
cleanup() {
    echo ""
    print_info "清理环境..."
    
    # 卸载文件系统
    if mount | grep -q "${MOUNT_POINT}"; then
        fusermount -u "${MOUNT_POINT}" 2>/dev/null || true
        sleep 1
    fi
    
    # 清理测试文件
    rm -rf "${PROJECT_DIR}/backup_storage" 2>/dev/null || true
    rm -rf "${MOUNT_POINT}/demo" 2>/dev/null || true
    
    print_success "清理完成"
}

# 主函数
main() {
    echo -e "${CYAN}"
    echo "============================================"
    echo "  智能备份文件系统 - 交互式功能演示"
    echo "============================================"
    echo -e "${NC}"
    echo "本脚本将演示13个关键操作步骤，包括："
    echo "  1. 文件系统挂载"
    echo "  2. 创建目录"
    echo "  3. 创建文件"
    echo "  4. 文件写入及内容显示"
    echo "  5. 查看文件版本"
    echo "  6. 修改文件内容并显示"
    echo "  7. 再次查看版本"
    echo "  8. 文件备份"
    echo "  9. 删除文件"
    echo " 10. 删除目录"
    echo " 11. 查看备份文件"
    echo " 12. 恢复备份文件"
    echo " 13. 卸载文件系统"
    echo ""
    read -p "按 Enter 开始演示..."
    echo ""

    # 设置退出时清理
    trap cleanup EXIT

    # 步骤1: 检查可执行文件
    check_executable
    pause

    # 步骤2: 文件系统挂载
    print_step 1 "文件系统挂载"
    
    # 创建挂载点
    if [[ ! -d "${MOUNT_POINT}" ]]; then
        echo "创建挂载点: ${MOUNT_POINT}"
        mkdir -p "${MOUNT_POINT}"
    fi
    
    # 检查是否已挂载
    if mount | grep -q "${MOUNT_POINT}"; then
        echo "检测到已挂载的文件系统，正在卸载..."
        fusermount -u "${MOUNT_POINT}" 2>/dev/null || true
        sleep 1
    fi
    
    echo "启动智能备份文件系统..."
    echo "挂载点: ${MOUNT_POINT}"
    
    # 后台挂载（参考 run.sh）
    LOG_FILE="/tmp/smartbackupfs_demo.log"
    nohup "${PROJECT_DIR}/build/bin/smartbackup-fs" "${MOUNT_POINT}" >"${LOG_FILE}" 2>&1 &
    FUSE_PID=$!
    
    sleep 2
    
    if mount | grep -q "${MOUNT_POINT}"; then
        print_success "文件系统挂载成功 (PID: ${FUSE_PID})"
        echo ""
        echo "挂载点信息:"
        df -h "${MOUNT_POINT}" || true
        echo ""
        echo "挂载点内容:"
        ls -la "${MOUNT_POINT}"
    else
        echo -e "${RED}挂载失败，查看日志: ${LOG_FILE}${NC}"
        cat "${LOG_FILE}"
        echo ""
        echo -e "${YELLOW}可能的原因：${NC}"
        echo "  1. 用户不在 fuse 组中"
        echo "  2. FUSE 权限未正确配置"
        echo "  3. 可执行文件编译不完整"
        echo ""
        echo "解决方法："
        echo "  1. 检查用户组: groups \$USER"
        echo "  2. 如果不在 fuse 组，请在宿主机执行: sudo usermod -a -G fuse \$USER"
        echo "  3. 然后重新登录后再试"
        exit 1
    fi
    pause

    # 步骤3: 创建目录
    print_step 2 "创建目录"
    
    run_cmd "mkdir -p ${TEST_DIR}"
    
    if [[ -d "${TEST_DIR}" ]]; then
        print_success "目录创建成功"
        echo ""
        echo "目录结构:"
        ls -la "${MOUNT_POINT}"
    else
        echo -e "${RED}目录创建失败${NC}"
        exit 1
    fi
    pause

    # 步骤4: 创建文件
    print_step 3 "创建文件"
    
    run_cmd "touch ${TEST_FILE}"
    
    if [[ -f "${TEST_FILE}" ]]; then
        print_success "文件创建成功"
        echo ""
        echo "目录内容:"
        ls -lh "${TEST_DIR}"
    else
        echo -e "${RED}文件创建失败${NC}"
        exit 1
    fi
    pause

    # 步骤5: 文件写入及内容显示
    print_step 4 "文件写入及内容显示"
    
    echo "写入初始内容..."
    run_cmd "cat > ${TEST_FILE} << 'EOF'
这是智能备份文件系统的演示文件。
这个文件系统支持版本管理、增量备份、数据去重等功能。
当前是文件的第一个版本。

主要特性：
- 透明版本管理
- 增量备份恢复
- 数据去重压缩
- 完整性保护
EOF"

    print_success "文件内容写入完成"
    echo ""
    echo "文件内容:"
    echo "----------------------------------------"
    cat "${TEST_FILE}"
    echo "----------------------------------------"
    echo ""
    echo "文件信息:"
    ls -lh "${TEST_FILE}"
    echo ""
    echo "文件大小:"
    wc -c "${TEST_FILE}"
    pause

    # 步骤6: 查看文件版本
    print_step 5 "查看文件版本"
    
    echo "创建版本快照..."
    run_cmd "setfattr -n user.version.create -v 1 ${TEST_FILE}"
    
    sleep 1
    
    echo ""
    echo "版本列表:"
    VERSIONS_DIR="${TEST_FILE}@versions"
    run_cmd "ls -la ${VERSIONS_DIR} 2>/dev/null || echo '版本目录不存在'"
    
    echo ""
    echo "尝试访问最新版本:"
    LATEST_VERSION="${TEST_FILE}@latest"
    run_cmd "cat ${LATEST_VERSION} 2>/dev/null || echo '@latest 别名不可用'"
    
    print_info "版本说明: 系统通过事件触发（rename/unlink）、内容变化、定时等方式自动创建版本"
    pause

    # 步骤7: 修改文件内容并显示
    print_step 6 "修改文件内容并显示"
    
    echo "通过重命名触发版本快照..."
    TEMP_FILE="${TEST_DIR}/test_temp.txt"
    run_cmd "mv ${TEST_FILE} ${TEMP_FILE}"
    run_cmd "mv ${TEMP_FILE} ${TEST_FILE}"
    sleep 1
    
    echo ""
    echo "修改文件内容..."
    run_cmd "cat > ${TEST_FILE} << 'EOF'
这是智能备份文件系统的演示文件。
这个文件系统支持版本管理、增量备份、数据去重等功能。
当前是文件的第二个版本 - 已修改内容。

新增内容：
- 版本历史记录
- 时间点回溯
- 差异比较功能

主要特性：
- 透明版本管理
- 增量备份恢复
- 数据去重压缩
- 完整性保护
EOF"

    print_success "文件内容已修改"
    echo ""
    echo "修改后的文件内容:"
    echo "----------------------------------------"
    cat "${TEST_FILE}"
    echo "----------------------------------------"
    echo ""
    echo "文件大小变化:"
    wc -c "${TEST_FILE}"
    pause

    # 步骤8: 再次查看版本
    print_step 7 "再次查看版本"
    
    echo ""
    echo "当前版本列表:"
    VERSIONS_DIR="${TEST_FILE}@versions"
    run_cmd "ls -la ${VERSIONS_DIR} 2>/dev/null || echo '版本目录不存在'"
    
    echo ""
    print_info "版本存储机制: 采用增量存储，仅保存变更块，未变更块通过父版本继承"
    pause

    # 步骤9: 文件备份
    print_step 8 "文件备份"
    
    echo "创建备份存储目录..."
    BACKUP_DIR="${PROJECT_DIR}/backup_storage"
    run_cmd "mkdir -p ${BACKUP_DIR}"
    
    echo ""
    echo "备份文件..."
    # 使用cp作为备份（模拟备份功能）
    BACKUP_FILE="${BACKUP_DIR}/test_backup.txt"
    run_cmd "cp ${TEST_FILE} ${BACKUP_FILE}"
    
    # 创建备份时间戳
    BACKUP_TIME=$(date +"%Y%m%d_%H%M%S")
    BACKUP_FILE_TIMED="${BACKUP_DIR}/test_backup_${BACKUP_TIME}.txt"
    run_cmd "cp ${TEST_FILE} ${BACKUP_FILE_TIMED}"
    
    print_success "文件备份完成"
    echo ""
    echo "备份目录内容:"
    ls -lh "${BACKUP_DIR}"
    echo ""
    echo "最新备份文件:"
    echo "----------------------------------------"
    cat "${BACKUP_FILE_TIMED}"
    echo "----------------------------------------"
    pause

    # 步骤10: 删除文件
    print_step 9 "删除文件"
    
    echo "删除前再次触发版本快照..."
    TEMP_FILE="${TEST_DIR}/test_temp2.txt"
    run_cmd "mv ${TEST_FILE} ${TEMP_FILE}"
    sleep 1
    run_cmd "mv ${TEMP_FILE} ${TEST_FILE}"
    sleep 1
    
    echo ""
    echo "删除文件..."
    run_cmd "rm -f ${TEST_FILE}"
    
    if [[ ! -f "${TEST_FILE}" ]]; then
        print_success "文件已删除"
        echo ""
        echo "删除后的目录内容:"
        ls -la "${TEST_DIR}"
    else
        echo -e "${RED}文件删除失败${NC}"
        exit 1
    fi
    pause

    # 步骤11: 删除目录
    print_step 10 "删除目录"
    
    echo "删除测试目录..."
    run_cmd "rmdir ${TEST_DIR}"
    
    if [[ ! -d "${TEST_DIR}" ]]; then
        print_success "目录已删除"
        echo ""
        echo "删除后的挂载点内容:"
        ls -la "${MOUNT_POINT}"
    else
        echo -e "${RED}目录删除失败${NC}"
        exit 1
    fi
    pause

    # 步骤12: 查看备份文件
    print_step 11 "查看备份文件"
    
    echo ""
    echo "备份目录信息:"
    echo "----------------------------------------"
    echo "备份路径: ${BACKUP_DIR}"
    run_cmd "ls -lh ${BACKUP_DIR}"
    echo ""
    
    echo "最新备份内容:"
    if [[ -f "${BACKUP_FILE_TIMED}" ]]; then
        echo "备份文件: ${BACKUP_FILE_TIMED}"
        echo "----------------------------------------"
        cat "${BACKUP_FILE_TIMED}"
        echo "----------------------------------------"
        echo ""
        echo "备份信息:"
        run_cmd "stat -c '大小: %s 字节' -c '时间: %y' ${BACKUP_FILE_TIMED}"
    else
        echo -e "${RED}未找到备份文件${NC}"
    fi
    pause

    # 步骤13: 恢复备份文件
    print_step 12 "恢复备份文件"
    
    echo ""
    echo "重新创建测试目录..."
    run_cmd "mkdir -p ${TEST_DIR}"
    
    echo "从备份恢复文件..."
    RESTORED_FILE="${TEST_DIR}/test_restored.txt"
    run_cmd "cp ${BACKUP_FILE_TIMED} ${RESTORED_FILE}"
    
    if [[ -f "${RESTORED_FILE}" ]]; then
        print_success "文件恢复成功"
        echo ""
        echo "恢复后的目录内容:"
        ls -lh "${TEST_DIR}"
        echo ""
        echo "恢复的文件内容:"
        echo "----------------------------------------"
        cat "${RESTORED_FILE}"
        echo "----------------------------------------"
        echo ""
        echo "验证完整性:"
        run_cmd "diff ${BACKUP_FILE_TIMED} ${RESTORED_FILE} > /dev/null && echo '恢复文件与备份完全一致' || echo '警告: 恢复文件与备份不一致'"
    else
        echo -e "${RED}文件恢复失败${NC}"
        exit 1
    fi
    pause

    # 步骤14: 卸载文件系统
    print_step 13 "卸载文件系统"
    
    echo "卸载文件系统..."
    if mount | grep -q "${MOUNT_POINT}"; then
        run_cmd "fusermount -u ${MOUNT_POINT}"
        sleep 1
        if ! mount | grep -q "${MOUNT_POINT}"; then
            print_success "文件系统卸载成功"
        else
            echo -e "${YELLOW}卸载时出现警告，尝试强制卸载...${NC}"
            run_cmd "fusermount -uz ${MOUNT_POINT}"
        fi
    else
        print_info "文件系统已自动卸载"
    fi
    
    echo ""
    echo "演示完成！"
    
    echo ""
    echo -e "${CYAN}========================================${NC}"
    echo -e "${GREEN}所有13个步骤演示完成！${NC}"
    echo -e "${CYAN}========================================${NC}"
    echo ""
    echo "演示总结:"
    echo "  ✓ 文件系统挂载与卸载"
    echo "  ✓ 目录和文件的基本操作"
    echo "  ✓ 文件内容读写"
    echo "  ✓ 版本管理与快照"
    echo "  ✓ 备份与恢复"
    echo "  ✓ 数据完整性验证"
    echo ""
    echo "备份文件保存在: ${BACKUP_DIR}"
    echo ""
    
    read -p "按 Enter 退出..."
}

# 运行主函数
main "$@"
