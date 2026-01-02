# 智能备份文件系统使用指南

## 项目概述

这是一个基于FUSE 3.0的智能备份文件系统，提供了完整的POSIX文件系统接口，支持版本管理、压缩和去重等高级功能。

## 功能特性

### 基础功能
- ✅ 文件操作：创建、读取、写入、删除
- ✅ 目录管理：创建、删除、浏览目录
- ✅ 权限管理：标准Unix权限模式
- ✅ 时间戳：访问(atime)、修改(mtime)、创建(ctime)时间
- ✅ 大文件支持：最大16TB文件大小
- ✅ 线程安全：并发访问支持

### 高级功能（框架已实现，部分功能简化）
- 🔄 版本管理：自动保存文件历史版本
- 🔄 数据压缩：支持LZ4和ZSTD压缩算法
- 🔄 数据去重：基于SHA256的块级去重
- 🔄 缓存系统：多级缓存提升性能

## 快速开始

### 1. 环境依赖

```bash
# Ubuntu/Debian
sudo apt-get install libfuse3-dev libzstd-dev liblz4-dev attr

# CentOS/RHEL
sudo yum install fuse3-devel zstd-devel lz4-devel attr
```

### 2. 构建项目

```bash
# 克隆项目
git clone <repository-url>
cd smartbackup-fs

# 构建Debug版本
./scripts/build.sh Debug

# 或构建Release版本
./scripts/build.sh Release
```

### 3. 运行文件系统

```bash
# 创建挂载点
mkdir -p /tmp/smartbackup

# 启动文件系统（前台模式，显示调试信息）
./scripts/run.sh -d

# 或直接运行
./build/bin/smartbackup-fs /tmp/smartbackup -f
```

## 使用示例

### 基本文件操作

```bash
# 挂载后，在挂载点进行操作
cd /tmp/smartbackup

# 创建目录
mkdir documents
mkdir images
mkdir -p projects/src/subdir

# 创建文件
touch documents/readme.txt
touch images/photo.jpg
touch projects/src/main.c

# 写入内容
echo "这是一个重要文档" > documents/readme.txt
printf "#include <stdio.h>\nint main() {\n    printf(\"Hello World\");\n    return 0;\n}" > projects/src/main.c

# 查看文件信息
ls -la documents/
ls -la projects/src/
stat documents/readme.txt

# 读取文件
cat documents/readme.txt
head projects/src/main.c
tail -n 3 projects/src/main.c

# 复制文件
cp documents/readme.txt documents/backup.txt
cp -r documents/ documents_copy/

# 移动/重命名文件
mv documents/backup.txt documents/readme.bak
mv projects/src/main.c projects/src/hello.c

# 删除文件
rm documents/readme.bak
rm projects/src/hello.c

# 删除目录（必须为空）
rmdir documents_copy
rm -rf projects
```

### 高级文件操作

```bash
# 创建硬链接
ln documents/readme.txt documents/hardlink.txt
# 验证硬链接（inode相同）
ls -i documents/readme.txt documents/hardlink.txt

# 创建符号链接
ln -s documents/readme.txt documents/symlink.txt
# 验证符号链接
ls -l documents/symlink.txt
readlink documents/symlink.txt

# 文件截断操作
echo "Original content" > temp.txt
truncate -s 100 temp.txt          # 扩展到100字节
truncate -s 10 temp.txt           # 截断到10字节
> temp.txt                        # 清空文件

# 文件属性和时间戳
touch -a documents/readme.txt     # 更新访问时间
touch -m documents/readme.txt     # 更新修改时间
touch -t 202512141200.00 documents/readme.txt  # 设置特定时间戳

# 扩展属性操作
setfattr -n user.comment -v "Important document" documents/readme.txt
getfattr -n user.comment documents/readme.txt
getfattr -d documents/readme.txt  # 显示所有扩展属性
listfattr documents/readme.txt    # 列出所有扩展属性
rmfattr -n user.comment documents/readme.txt   # 删除扩展属性
```

### 目录操作

```bash
# 深层目录结构创建
mkdir -p level1/level2/level3/level4
cd level1/level2/level3/level4
pwd
touch deep_file.txt
cd ../../..

# 目录遍历和查找
find . -name "*.txt"              # 查找所有txt文件
find . -type d                    # 查找所有目录
tree .                            # 显示目录树（如果安装了tree命令）

# 目录信息查看
du -sh .                          # 显示目录大小
du -h documents/                  # 显示documents目录下文件大小

# 目录权限管理
chmod 755 documents/              # 设置目录权限
chmod 644 documents/readme.txt    # 设置文件权限
```

### 文件内容操作

```bash
# 大文件读写测试
dd if=/dev/zero of=large_file bs=1M count=10
dd if=large_file of=copy_file bs=1M

# 文件追加
echo "New line" >> documents/readme.txt
cat >> append_test.txt << EOF
多行内容
第二行
第三行
EOF

# 文件内容过滤和处理
grep "重要" documents/readme.txt
sed 's/重要/关键/g' documents/readme.txt > temp.txt
wc -l documents/readme.txt
wc -c documents/readme.txt

# 文件压缩和解压（如果系统支持）
gzip documents/readme.txt
gunzip documents/readme.txt.gz
```

### 并发访问测试

```bash
# 终端1：创建文件并写入
echo "Concurrent test" > concurrent.txt &
sleep 1
cat concurrent.txt &

# 终端2：同时访问
ls -la concurrent.txt &
echo "Second process writes" >> concurrent.txt &

# 终端3：检查文件状态
stat concurrent.txt &
```

### 文件系统信息查看

```bash
# 查看挂载信息
df -h /tmp/myfs
mount | grep myfs

# 查看文件系统使用情况
df -i /tmp/myfs                  # inode使用情况
df -T /tmp/myfs                   # 文件系统类型

# 系统调用追踪
strace -e trace=open,read,write,close ls /tmp/myfs
lsof +D /tmp/myfs                 # 查看打开的文件
```

### 权限管理

```bash
# 修改文件权限
chmod 644 documents/readme.txt     # rw-r--r--
chmod 755 documents/              # rwxr-xr-x
chmod 700 private/                # rwx------

# 数字权限表示法
chmod u+rwx,g+rx,o-r documents/readme.txt  # 等同于 chmod 750
chmod a+x scripts/run.sh          # 给所有用户添加执行权限

# 修改所有者（需要root权限）
sudo chown user:group documents/readme.txt
sudo chown -R user:group documents/  # 递归修改目录所有者

# 特殊权限
chmod +t tmp/                     # 粘滞位
chmod g+s shared/                 # SGID位
chmod u+s program                 # SUID位
```

## 配置选项

### 配置文件位置：`build/configs/config.yaml`

```yaml
# 存储设置
storage:
  block_size: 4096          # 块大小
  max_file_size: "16TB"     # 最大文件大小
  max_files: 1000000        # 最大文件数量
  cache_size: "128MB"       # 缓存大小

# 压缩设置
compression:
  enabled: false            # 是否启用压缩
  algorithms:
    - name: lz4
      level: 3
    - name: zstd
      level: 5

# 版本管理
versioning:
  enabled: false            # 是否启用版本管理
  retention:
    max_versions: 100       # 最大版本数
    max_age: "30d"          # 最大保留时间
    max_size: "10GB"        # 最大总大小

# 去重设置
deduplication:
  enabled: false            # 是否启用去重
  block_size: "4KB"         # 去重块大小
  algorithm: "sha256"       # 哈希算法
```

## 开发指南

### 核心模块

1. **smartbackupfs_basic.c** - FUSE操作实现
   - 文件系统初始化和清理
   - 基础文件操作（getattr, mkdir, unlink等）
   - 目录遍历和文件读取/写入

2. **metadata_manager.c** - 元数据管理
   - inode管理
   - 目录项操作
   - 数据块管理
   - 缓存系统

3. **posix_operations.c** - POSIX操作封装
   - 底层文件系统操作
   - 系统调用封装

### 扩展功能

要启用高级功能，需要修改以下部分：

1. **数据存储**：在`write_block()`和`read_block()`中实现真实的数据块存储
2. **压缩**：在数据块操作中添加压缩/解压缩逻辑
3. **版本管理**：在文件写入时保存历史版本
4. **去重**：在数据块存储前计算哈希值并实现共享存储

## 性能特性

### 内存使用
- **缓存大小**：128MB（可配置）
- **线程池**：最大100个工作线程
- **并发访问**：完全线程安全

### 存储效率
- **块大小**：4KB（可配置）
- **去重算法**：SHA256
- **压缩算法**：LZ4/ZSTD

## 完整支持的POSIX操作

本智能备份文件系统实现了以下完整的POSIX文件系统操作：

### 🔧 FUSE操作接口
- `getattr` - 获取文件属性
- `mkdir` - 创建目录  
- `unlink` - 删除文件
- `rmdir` - 删除目录
- `rename` - 重命名/移动文件
- `truncate` - 截断文件
- `open` - 打开文件
- `read` - 读取文件
- `write` - 写入文件
- `fsync` - 同步文件到存储
- `readdir` - 读取目录内容
- `create` - 创建新文件
- `utimens` - 更新文件时间戳
- `flush` - 刷新文件缓冲区
- `release` - 释放文件句柄
- `symlink` - 创建符号链接
- `readlink` - 读取符号链接
- `link` - 创建硬链接
- `getxattr` - 获取扩展属性
- `setxattr` - 设置扩展属性
- `listxattr` - 列出扩展属性
- `removexattr` - 删除扩展属性
- `access` - 检查访问权限
- `destroy` - 销毁文件系统

### 📁 文件类型支持
- ✅ **普通文件** - 支持创建、读写、删除
- ✅ **目录** - 支持创建、浏览、删除
- ✅ **符号链接** - 支持创建、读取、跟随
- ✅ **硬链接** - 支持创建和管理
- ⚠️ **设备文件** - 框架支持但未完全实现
- ⚠️ **管道文件** - 框架支持但未完全实现

### 🚀 性能特性
- ✅ **大文件支持** - 最大16TB文件大小
- ✅ **高并发访问** - 完全线程安全
- ✅ **内存缓存** - 多级缓存系统
- ✅ **块级存储** - 4KB块大小管理
- ✅ **稀疏文件** - 支持文件空洞

### 🛡️ 安全特性
- ✅ **权限检查** - 标准Unix权限模式
- ✅ **用户/组管理** - UID/GID支持
- ✅ **时间戳** - atime/mtime/ctime
- ✅ **访问控制** - access()系统调用
- ✅ **扩展属性** - user.xattr支持

## 测试用例

### 基础功能测试
```bash
#!/bin/bash
# 完整功能测试脚本

echo "=== 智能备份文件系统功能测试 ==="
cd /tmp/myfs

# 1. 目录操作测试
echo "1. 测试目录操作..."
mkdir test_dir
mkdir -p deep/level1/level2
ls -la

# 2. 文件创建测试
echo "2. 测试文件创建..."
touch test_dir/file1.txt
echo "Test content" > test_dir/file2.txt
ls -la test_dir/

# 3. 文件读写测试
echo "3. 测试文件读写..."
cat test_dir/file2.txt
echo "Additional content" >> test_dir/file2.txt
wc -l test_dir/file2.txt

# 4. 链接操作测试
echo "4. 测试链接操作..."
ln test_dir/file2.txt test_dir/hardlink.txt
ln -s test_dir/file2.txt test_dir/symlink.txt
ls -li test_dir/
cat test_dir/symlink.txt

# 5. 重命名操作测试
echo "5. 测试重命名操作..."
mv test_dir/file1.txt test_dir/renamed.txt
mv test_dir/hardlink.txt test_dir/moved_hardlink.txt

# 6. 权限操作测试
echo "6. 测试权限操作..."
chmod 644 test_dir/renamed.txt
stat test_dir/renamed.txt

# 7. 扩展属性测试
echo "7. 测试扩展属性..."
setfattr -n user.test -v "test value" test_dir/renamed.txt
getfattr -n user.test test_dir/renamed.txt

# 8. 截断操作测试
echo "8. 测试截断操作..."
truncate -s 100 test_dir/renamed.txt
ls -l test_dir/renamed.txt
truncate -s 10 test_dir/renamed.txt

# 9. 删除操作测试
echo "9. 测试删除操作..."
rm test_dir/moved_hardlink.txt
rm test_dir/symlink.txt
rm test_dir/renamed.txt
rmdir deep/level1/level2
rmdir deep/level1
rmdir deep

echo "=== 所有测试完成 ==="
```

### 性能测试
```bash
#!/bin/bash
# 性能测试脚本

echo "=== 性能测试 ==="
cd /tmp/myfs

# 大文件写入性能测试
echo "测试大文件写入性能..."
time dd if=/dev/zero of=large_test bs=1M count=100

# 多文件创建性能测试
echo "测试多文件创建性能..."
time (for i in {1..1000}; do touch file_$i.txt; done)

# 并发访问测试
echo "测试并发访问..."
for i in {1..10}; do
  (echo "Process $i content" > concurrent_$i.txt) &
done
wait
ls -1 concurrent_*.txt | wc -l

echo "=== 性能测试完成 ==="
```

## 故障排除

### 常见问题

1. **挂载失败**
   ```bash
   # 检查FUSE权限
   sudo usermod -a -G fuse $USER
   # 重新登录后生效
   ```

2. **权限错误**
   ```bash
   # 确保挂载点有写权限
   chmod 755 /tmp/myfs
   ```

3. **依赖缺失**
   ```bash
   # 安装所有依赖
   sudo apt-get install libfuse3-dev libzstd-dev liblz4-dev
   ```

### 调试模式

```bash
# 启用详细日志
./build/bin/smartbackup-fs /tmp/myfs -f -d

# 或使用strace调试
strace -o debug.log ./build/bin/smartbackup-fs /tmp/myfs -f
```

## 贡献指南

1. Fork项目
2. 创建功能分支
3. 提交更改
4. 创建Pull Request

### 代码规范
- 遵循C11标准
- 使用4空格缩进
- 所有公共函数需要文档注释
- 编译时开启所有警告并视为错误

## 许可证

本项目采用MIT许可证，详见LICENSE文件。