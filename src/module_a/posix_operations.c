/**
 * POSIX操作模块
 */

#include "smartbackupfs.h"
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

// POSIX文件操作的基本实现
int posix_read_file(const char *path, char *buf, size_t size, off_t offset) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        return -errno;
    }
    
    int result = pread(fd, buf, size, offset);
    if (result == -1) {
        result = -errno;
    }
    
    close(fd);
    return result;
}

int posix_write_file(const char *path, const char *buf, size_t size, off_t offset) {
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd == -1) {
        return -errno;
    }
    
    int result = pwrite(fd, buf, size, offset);
    if (result == -1) {
        result = -errno;
    }
    
    close(fd);
    return result;
}