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
sudo apt-get install libfuse3-dev libzstd-dev liblz4-dev

# CentOS/RHEL
sudo yum install fuse3-devel zstd-devel lz4-devel
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
mkdir -p /tmp/myfs

# 启动文件系统（前台模式，显示调试信息）
./scripts/run.sh -d

# 或直接运行
./build/bin/smartbackup-fs /tmp/myfs -f
```

## 使用示例

### 基本文件操作

```bash
# 挂载后，在挂载点进行操作
cd /tmp/myfs

# 创建目录
mkdir documents
mkdir images

# 创建文件
touch documents/readme.txt
touch images/photo.jpg

# 写入内容
echo "这是一个重要文档" > documents/readme.txt

# 查看文件信息
ls -la documents/
stat documents/readme.txt

# 复制文件
cp documents/readme.txt documents/backup.txt

# 删除文件
rm documents/backup.txt
```

### 权限管理

```bash
# 修改文件权限
chmod 644 documents/readme.txt

# 修改所有者（需要root权限）
sudo chown user:group documents/readme.txt
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

## 最近修复 (Module A)

- 2025-12-14: 修复 `lookup_path()` 路径遍历与锁管理问题，原实现存在在遍历子目录时错误释放/未释放读写锁的竞态，导致只能在根目录下创建文件。现在使用 `strtok_r` 并在切换到子目录前先获取子目录的读锁，再释放父目录锁，正确支持在子目录中创建/查找文件。