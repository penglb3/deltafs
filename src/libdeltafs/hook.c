#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/vfs.h>

#include "deltafs/deltafs_api.h"

#define DFS_MOUNT_POINT "/dfs"
const int kDfsMagicFdPrefix = 0xffff0000;

// extern void InitPreload();
// extern void DestroyPreload();

// __attribute__((constructor)) static void setup(void) { InitPreload(); }

// __attribute__((destructor)) static void teardown(void) { DestroyPreload(); }

// Operations: minimal set to support mdtest

// - [ ] open(path, flags, [mode]) -> fd | -1
// - [ ] read(fd, buffer, count) -> (ssize_t) bytes_read | -1
// - [ ] write(fd, buffer, count) -> (ssize_t) bytes_written | -1
// - [ ] close(fd) -> 0 | -1
// - [ ] stat(path, buffer) -> 0 | -1
// - [ ] access(path, mode) -> 0 | -1
// - [ ] unlink(path) -> 0 | -1
// - [ ] mkdir(path, mode) -> 0 | -1

// NOTE THAT RMDIR AND RENAME ARE NOT SUPPORTED BY DELTAFS

static inline int is_dfs_fd(int fd) { return fd >= kDfsMagicFdPrefix; }

static inline int is_mount_path(const char *path) {
  // do not support relative path
  if (strncmp(path, DFS_MOUNT_POINT, 4) == 0) {
    return 1;
  }
  return 0;
}

static inline const char *get_dfs_path(const char *path) {
  // if the mount path is /dfs
  // input  : /dfs/text.txt
  // output : /text.txt
  // TODO: improve the robustness
  return path + strlen(DFS_MOUNT_POINT);
}

static inline void assign_fnptr(void **fnptr, void *new_fnptr) {
  *fnptr = new_fnptr;
}

#define ASSIGN_FN(fn)                                                          \
  do {                                                                         \
    if (libc_##fn == NULL) {                                                   \
      assign_fnptr((void **)&libc_##fn, dlsym(RTLD_NEXT, #fn));                \
    }                                                                          \
  } while (0)

typedef int (*open_t)(const char *pathname, int flags, ...);
open_t libc_open;

// create and open goes here
int open(const char *file, int oflag, ...) {
  ASSIGN_FN(open);
  va_list args;
  va_start(args, oflag);
  if (oflag & O_CREAT || oflag & O_TMPFILE) {
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    if (is_mount_path(file)) {
      return deltafs_open(get_dfs_path(file), oflag, mode);
    }
    return libc_open(file, oflag, mode);
  }
  va_end(args);
  if (is_mount_path(file)) {
    return deltafs_open(get_dfs_path(file), oflag, 0644);
  }
  return libc_open(file, oflag);
}

int __open_2(const char *file, int oflag) { return open(file, oflag); }

typedef int (*open64_t)(const char *pathname, int flags, ...);
open64_t libc_open64;

int open64(const char *file, int oflag, ...) {
  ASSIGN_FN(open64);
  va_list args;
  va_start(args, oflag);
  if (oflag & O_CREAT || oflag & O_TMPFILE) {
    mode_t mode = va_arg(args, mode_t);
    va_end(args);
    if (is_mount_path(file)) {
      return deltafs_open(get_dfs_path(file), oflag, mode);
    }
    return libc_open64(file, oflag, mode);
  }
  va_end(args);
  if (is_mount_path(file)) {
    return deltafs_open(get_dfs_path(file), oflag, 0644);
  }
  return libc_open64(file, oflag);
}

typedef int (*stat_t)(const char *path, struct stat *buf);
stat_t libc_stat;

int stat(const char *path, struct stat *buf) {
  ASSIGN_FN(stat);
  if (is_mount_path(path)) {
    return deltafs_stat(get_dfs_path(path), buf);
  }
  return libc_stat(path, buf);
}

typedef int (*access_t)(const char *pathname, int mode);
access_t libc_access;

int access(const char *pathname, int mode) {
  ASSIGN_FN(access);
  if (is_mount_path(pathname)) {
    return deltafs_access(get_dfs_path(pathname), mode);
  }
  return libc_access(pathname, mode);
}

typedef int (*close_t)(int fd);
close_t libc_close;

int close(int fd) {
  ASSIGN_FN(close);
  if (is_dfs_fd(fd)) {
    return deltafs_close(fd);
  }
  return libc_close(fd);
}

typedef int (*read_t)(int fd, void *buf, size_t count);
read_t libc_read;

int read(int fd, void *buf, size_t count) {
  ASSIGN_FN(read);
  if (is_dfs_fd(fd)) {
    return deltafs_read(fd, buf, count);
  }
  return libc_read(fd, buf, count);
}

typedef int (*write_t)(int fd, const void *buf, size_t count);
write_t libc_write;

int write(int fd, const void *buf, size_t count) {
  ASSIGN_FN(write);
  if (is_dfs_fd(fd)) {
    return deltafs_write(fd, buf, count);
  }
  return libc_write(fd, buf, count);
}

// https://man7.org/linux/man-pages/man2/pread.2.html
// ssize_t pread(int fd, void *buf, size_t count, off_t offset);
typedef int64_t (*pread_t)(int fd, void *buf, uint64_t count, int64_t offset);
pread_t libc_pread;

int pread(int fd, void *buf, uint64_t count, int64_t offset) {
  ASSIGN_FN(pread);
  if (is_dfs_fd(fd)) {
    return deltafs_pread(fd, buf, count, offset);
  }
  return libc_pread(fd, buf, count, offset);
}

// https://man7.org/linux/man-pages/man2/pread.2.html
// ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
typedef int64_t (*pwrite_t)(int fd, const void *buf, uint64_t count,
                            int64_t offset);
pwrite_t libc_pwrite;

int pwrite(int fd, const void *buf, uint64_t count, int64_t offset) {
  ASSIGN_FN(pwrite);
  if (is_dfs_fd(fd)) {
    return deltafs_pwrite(fd, buf, count, offset);
  }
  return libc_pwrite(fd, buf, count, offset);
}

typedef int (*mkdir_t)(const char *pathname, mode_t mode);
mkdir_t libc_mkdir;

int mkdir(const char *pathname, mode_t mode) {
  ASSIGN_FN(mkdir);
  if (is_mount_path(pathname)) {
    return deltafs_mkdir(get_dfs_path(pathname), mode);
  }
  return libc_mkdir(pathname, mode);
}

typedef int (*unlink_t)(const char *pathname);
unlink_t libc_unlink;

int unlink(const char *pathname) {
  ASSIGN_FN(unlink);
  if (is_mount_path(pathname)) {
    return deltafs_unlink(get_dfs_path(pathname));
  }
  return libc_unlink(pathname);
}
