/*
 * Linux-compatible struct stat — matches musl/glibc expectations.
 * Used by sys_stat, sys_fstat, sys_fstatat return values.
 *
 * Layout matches Linux x86_64 struct stat (144 bytes).
 * ARM64 uses the same layout (asm-generic/stat.h).
 */
#ifndef LIMNX_STAT_H
#define LIMNX_STAT_H

#include <stdint.h>

/* File type bits for st_mode (matches Linux) */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFSOCK 0140000

#if defined(__x86_64__)
/* x86_64 struct stat — matches Linux x86_64 layout (144 bytes) */
struct linux_stat {
    uint64_t  st_dev;       /*   0: device ID */
    uint64_t  st_ino;       /*   8: inode number */
    uint64_t  st_nlink;     /*  16: number of hard links */
    uint32_t  st_mode;      /*  24: file type + permissions */
    uint32_t  st_uid;       /*  28: owner UID */
    uint32_t  st_gid;       /*  32: owner GID */
    uint32_t  __pad0;       /*  36: padding */
    uint64_t  st_rdev;      /*  40: device ID (if special file) */
    int64_t   st_size;      /*  48: total size in bytes */
    int64_t   st_blksize;   /*  56: block size for I/O */
    int64_t   st_blocks;    /*  64: number of 512B blocks allocated */
    int64_t   st_atime_sec; /*  72: access time (seconds) */
    int64_t   st_atime_nsec;/*  80: access time (nanoseconds) */
    int64_t   st_mtime_sec; /*  88: modification time */
    int64_t   st_mtime_nsec;/*  96: */
    int64_t   st_ctime_sec; /* 104: status change time */
    int64_t   st_ctime_nsec;/* 112: */
    int64_t   _reserved[3]; /* 120-143: reserved */
};                          /* total: 144 bytes */
#elif defined(__aarch64__)
/* ARM64 struct stat — matches asm-generic/stat.h (128 bytes) */
struct linux_stat {
    uint64_t  st_dev;       /*   0: device ID */
    uint64_t  st_ino;       /*   8: inode number */
    uint32_t  st_mode;      /*  16: file type + permissions */
    uint32_t  st_nlink;     /*  20: number of hard links */
    uint32_t  st_uid;       /*  24: owner UID */
    uint32_t  st_gid;       /*  28: owner GID */
    uint64_t  st_rdev;      /*  32: device ID (if special file) */
    uint64_t  __pad1;       /*  40: padding */
    int64_t   st_size;      /*  48: total size in bytes */
    int32_t   st_blksize;   /*  56: block size for I/O */
    int32_t   __pad2;       /*  60: padding */
    int64_t   st_blocks;    /*  64: number of 512B blocks allocated */
    int64_t   st_atime_sec; /*  72: access time (seconds) */
    int64_t   st_atime_nsec;/*  80: access time (nanoseconds) */
    int64_t   st_mtime_sec; /*  88: modification time */
    int64_t   st_mtime_nsec;/*  96: */
    int64_t   st_ctime_sec; /* 104: status change time */
    int64_t   st_ctime_nsec;/* 112: */
    uint32_t  _reserved[2]; /* 120-127: reserved */
};                          /* total: 128 bytes */
#endif

#endif
