cmake_minimum_required(VERSION 3.10)
project(smartbackup-fs VERSION 1.0.0 DESCRIPTION "智能备份与版本管理文件系统")

# 设置C标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 设置编译选项
set(CMAKE_BUILD_TYPE Release)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 输出目录（使用相对路径）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY lib)

# 查找依赖包
find_package(PkgConfig REQUIRED)

# 查找FUSE3
pkg_check_modules(FUSE3 REQUIRED fuse3)
include_directories(${FUSE3_INCLUDE_DIRS})

# 查找其他依赖
find_package(OpenSSL REQUIRED)
find_package(ZLIB)
find_library(ZSTD_LIB zstd REQUIRED)
find_library(LZ4_LIB lz4 REQUIRED)
find_package(Threads REQUIRED)

# 定义项目源文件（使用相对路径）
set(SMARTBACKUPFS_SOURCES
    src/module_a/smartbackupfs_basic.c
    src/module_a/metadata_manager.c
    src/module_a/posix_operations.c
    src/module_b/version_manager.c
    src/module_c/dedup.c
    src/module_c/block_splitter.c
    src/module_c/dedup_core.c
    src/module_c/adaptive_compress.c
    src/module_c/storage_monitor_basic.c
    src/module_c/cache.c
    src/module_c/system_monitor.c
    src/module_c/storage.c
    src/module_c/storage_prediction.c
    src/module_c/module_d_adapter.c
    src/module_c/monitor.c
    src/module_c/compression.c
    src/module_d/module_d.c
    src/module_d/module_d_integration.c
)

set(SMARTBACKUPFS_HEADERS
    include/smartbackupfs.h
    include/metadata.h
    include/version_manager.h
    include/module_c/block_splitter.h
    include/module_c/dedup_core.h
    include/module_c/adaptive_compress.h
    include/module_c/storage_monitor_basic.h
    include/module_c/cache.h
    include/module_c/system_monitor.h
    include/module_c/storage_prediction.h
    include/module_c/module_d_adapter.h
    include/module_c.h
    include/module_c/compression.h
    include/module_d.h
    include/module_d_integration.h
)

# 可执行文件
add_executable(smartbackup-fs ${SMARTBACKUPFS_SOURCES})

# 包含目录（使用相对路径）
target_include_directories(smartbackup-fs PRIVATE
    include
    ${FUSE3_INCLUDE_DIRS}
)

# 链接库
target_link_libraries(smartbackup-fs PRIVATE
    ${FUSE3_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${ZSTD_LIB}
    ${LZ4_LIB}
    Threads::Threads
    m # 数学库
    dl # 动态加载库
)

if (ZLIB_FOUND)
    target_link_libraries(smartbackup-fs PRIVATE ZLIB::ZLIB)
    target_compile_definitions(smartbackup-fs PRIVATE HAVE_ZLIB=1)
else()
    message(WARNING "Zlib not found; GZIP compression disabled")
    target_compile_definitions(smartbackup-fs PRIVATE HAVE_ZLIB=0)
endif()

# 设置编译选项
target_compile_options(smartbackup-fs PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-unused-parameter
    -D_FILE_OFFSET_BITS=64
    -D_GNU_SOURCE
)

# 设置FUSE版本
target_compile_definitions(smartbackup-fs PRIVATE
    FUSE_USE_VERSION=31
)

# 安装规则
install(TARGETS smartbackup-fs
    RUNTIME DESTINATION bin
)

install(FILES ${SMARTBACKUPFS_HEADERS}
    DESTINATION include/smartbackupfs
)

# 测试目标
enable_testing()

add_test(NAME test_compilation
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target smartbackup-fs
)

# 添加子目录（后续模块）
# add_subdirectory(src/module_b)
# add_subdirectory(src/module_c)
# add_subdirectory(src/module_d)

# 添加配置（使用相对路径）
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/configs/config.yaml.in
    ${CMAKE_CURRENT_BINARY_DIR}/config.yaml
    @ONLY
)