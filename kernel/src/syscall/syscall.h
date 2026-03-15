#ifndef LIMNX_SYSCALL_H
#define LIMNX_SYSCALL_H

#include <stdint.h>
#include "errno.h"

/*
 * Syscall numbers — Linux x86_64 compatible
 *
 * Core POSIX syscalls use Linux numbers (0-300) for musl/busybox compatibility.
 * Limnx-specific syscalls use 512+ range.
 */

/* === Linux-compatible syscall numbers === */

/* File I/O */
#define SYS_READ            0
#define SYS_WRITE           1   /* write(fd, buf, len) — POSIX */
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_STAT            4
#define SYS_FSTAT           5
#define SYS_LSEEK           6   /* was SYS_SEEK */
#define SYS_POLL            7
#define SYS_MMAP            9   /* mmap(addr, len, prot, flags, fd, offset) */
#define SYS_MPROTECT       10
#define SYS_MUNMAP         11
#define SYS_BRK            12   /* stub — returns current break */
#define SYS_IOCTL          16
#define SYS_ACCESS         21   /* stub — check file accessibility */
#define SYS_PIPE           22
#define SYS_SELECT         23
#define SYS_SCHED_YIELD    24
#define SYS_DUP            32
#define SYS_DUP2           33
#define SYS_NANOSLEEP      35
#define SYS_GETPID         39
#define SYS_SOCKET         41   /* socket(domain, type, protocol) → fd */
#define SYS_CONNECT        42
#define SYS_ACCEPT         43
#define SYS_SENDTO         44
#define SYS_RECVFROM       45
#define SYS_SHUTDOWN       48
#define SYS_BIND           49
#define SYS_LISTEN         50
#define SYS_FORK           57
#define SYS_EXECVE         59
#define SYS_EXIT           60
#define SYS_WAIT4          61
#define SYS_KILL           62
#define SYS_FCNTL          72
#define SYS_TRUNCATE       76
#define SYS_GETDENTS       78   /* stub — directory entries */
#define SYS_GETCWD         79
#define SYS_CHDIR          80
#define SYS_RENAME         82
#define SYS_MKDIR          83
#define SYS_RMDIR          84
#define SYS_CREAT          85
#define SYS_UNLINK         87
#define SYS_SYMLINK        88
#define SYS_READLINK       89
#define SYS_CHMOD          90
#define SYS_CHOWN          92
#define SYS_FCHOWN         93
#define SYS_UMASK          95
#define SYS_GETRLIMIT      97
#define SYS_GETUID        102
#define SYS_GETGID        104
#define SYS_SETUID        105
#define SYS_SETGID        106
#define SYS_GETEUID       107
#define SYS_GETEGID       108
#define SYS_SETPGID       109
#define SYS_GETPGRP       111
#define SYS_SETSID        112
#define SYS_GETGROUPS     115
#define SYS_SETGROUPS     116
#define SYS_GETSID        124
#define SYS_RT_SIGACTION  13    /* Linux rt_sigaction */
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN  15
#define SYS_ARCH_PRCTL    158
#define SYS_SETRLIMIT     160
#define SYS_MOUNT         165
#define SYS_UMOUNT2       166
#define SYS_CLOCK_GETTIME 228
#define SYS_EPOLL_WAIT    232
#define SYS_EPOLL_CTL     233
#define SYS_OPENPTY       236   /* not standard Linux but close to posix_openpt area */
#define SYS_EVENTFD2      284
#define SYS_EPOLL_CREATE1 291
#define SYS_PIPE2         293
#define SYS_SECCOMP       317
#define SYS_IO_URING_SETUP 425
#define SYS_IO_URING_ENTER 426

/* === Limnx-specific syscalls (512+) === */

/* Legacy/convenience (kept for backward compat, may be removed later) */
#define SYS_PUTS           512  /* write to stdout: puts(buf, len) — old SYS_WRITE */
#define SYS_EXEC_OLD       513  /* fork+exec combo — old SYS_EXEC */
#define SYS_GETCHAR        514
#define SYS_READDIR        515  /* our custom readdir(path, index, dirent) */
#define SYS_FMMAP          516  /* mmap a file by fd */
#define SYS_MMAP2          517  /* mmap with flags (demand paging) */
#define SYS_MMAP_FILE      518
#define SYS_MMAP_GUARD     519

/* Shared memory (SysV-style) */
#define SYS_SHMGET         520
#define SYS_SHMAT          521
#define SYS_SHMDT          522

/* Environment (kernel-managed per-process env) */
#define SYS_GETENV         523
#define SYS_SETENV         524
#define SYS_ENVIRON        525

/* Agent infrastructure */
#define SYS_AGENT_REGISTER 530
#define SYS_AGENT_LOOKUP   531
#define SYS_AGENT_SEND     532
#define SYS_AGENT_RECV     533

/* Capability tokens */
#define SYS_TOKEN_CREATE   534
#define SYS_TOKEN_REVOKE   535
#define SYS_TOKEN_LIST     536
#define SYS_TOKEN_DELEGATE 537

/* Agent namespaces */
#define SYS_NS_CREATE      538
#define SYS_NS_JOIN        539
#define SYS_NS_SETQUOTA    540

/* Inference service */
#define SYS_INFER_REGISTER 541
#define SYS_INFER_REQUEST  542
#define SYS_INFER_HEALTH   543
#define SYS_INFER_ROUTE    544
#define SYS_INFER_SET_POLICY 545
#define SYS_INFER_QUEUE_STAT 546
#define SYS_INFER_CACHE_CTRL 547
#define SYS_INFER_SUBMIT   548
#define SYS_INFER_POLL     549
#define SYS_INFER_RESULT   550
#define SYS_INFER_SWAP     551

/* Task graph (DAG workflows) */
#define SYS_TASK_CREATE    552
#define SYS_TASK_DEPEND    553
#define SYS_TASK_START     554
#define SYS_TASK_COMPLETE  555
#define SYS_TASK_STATUS    556
#define SYS_TASK_WAIT      557

/* Supervisor trees */
#define SYS_SUPER_CREATE   558
#define SYS_SUPER_ADD      559
#define SYS_SUPER_SET_POLICY 560
#define SYS_SUPER_START    561
#define SYS_SUPER_LIST     562
#define SYS_SUPER_STOP     563

/* Pub/sub messaging */
#define SYS_TOPIC_CREATE   564
#define SYS_TOPIC_SUB      565
#define SYS_TOPIC_PUB      566
#define SYS_TOPIC_RECV     567

/* Unix domain sockets */
#define SYS_UNIX_SOCKET    568
#define SYS_UNIX_BIND      569
#define SYS_UNIX_LISTEN    570
#define SYS_UNIX_ACCEPT    571
#define SYS_UNIX_CONNECT   572

/* TCP legacy (kept until socket API fully unified) */
#define SYS_TCP_SOCKET     573
#define SYS_TCP_CONNECT    574
#define SYS_TCP_LISTEN     575
#define SYS_TCP_ACCEPT     576
#define SYS_TCP_SEND       577
#define SYS_TCP_RECV       578
#define SYS_TCP_CLOSE      579
#define SYS_TCP_SETOPT     580
#define SYS_TCP_TO_FD      581

/* Misc */
#define SYS_PROCINFO       582
#define SYS_FSSTAT         583
#define SYS_SWAP_STAT      584
#define SYS_SETAUDIT       585
#define SYS_GETCAP         586
#define SYS_SETCAP         587
#define SYS_FUTEX_WAIT     588
#define SYS_FUTEX_WAKE     589
#define SYS_MKFIFO         590
#define SYS_TCSETPGRP      591
#define SYS_TCGETPGRP      592
#define SYS_GETPGID        593

/* Maximum syscall number + 1 */
#define SYS_NR             594

/* Capability bits */
#define CAP_NET_BIND  (1 << 0)
#define CAP_NET_RAW   (1 << 1)
#define CAP_KILL      (1 << 2)
#define CAP_SETUID    (1 << 3)
#define CAP_SYS_ADMIN (1 << 4)
#define CAP_EXEC      (1 << 5)
#define CAP_FS_WRITE  (1 << 6)
#define CAP_FS_READ   (1 << 7)
#define CAP_INFER     (1 << 8)
#define CAP_XNS_TASK  (1 << 9)
#define CAP_XNS_PUBSUB (1 << 10)
#define CAP_XNS_INFER  (1 << 11)
#define CAP_CHOWN      (1 << 12)
#define CAP_ALL       0x1FFF

/* Resource limit IDs */
#define RLIMIT_MEM   0
#define RLIMIT_CPU   1
#define RLIMIT_NFDS  2

/* Resource limit structure */
typedef struct rlimit {
    uint64_t current;
    uint64_t max;
} rlimit_t;

/* Audit flags */
#define AUDIT_SYSCALL  (1 << 0)
#define AUDIT_SECURITY (1 << 1)
#define AUDIT_EXEC     (1 << 2)
#define AUDIT_FILE     (1 << 3)

/* waitpid flags */
#define WNOHANG  1

/* Clock IDs */
#define CLOCK_MONOTONIC 1

/* Time structures */
typedef struct timespec {
    int64_t  tv_sec;
    int64_t  tv_nsec;
} timespec_t;

/* Poll structures */
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010

typedef struct pollfd {
    int32_t  fd;
    int16_t  events;
    int16_t  revents;
} pollfd_t;

/* epoll constants */
#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_MOD  2
#define EPOLL_CTL_DEL  3
#define EPOLLIN   0x001
#define EPOLLOUT  0x004
#define EPOLLERR  0x008
#define EPOLLHUP  0x010

/* io_uring opcodes */
#define IORING_OP_NOP       0
#define IORING_OP_READ      1
#define IORING_OP_WRITE     2
#define IORING_OP_POLL_ADD  3

/* mmap2 flags */
#define MMAP_DEMAND  1

/* mprotect protection flags */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/* mmap flags (Linux-compatible) */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* Maximum user-space address (canonical lower-half boundary) */
#define USER_ADDR_MAX 0x0000800000000000ULL

void syscall_init(void);

/* Called from syscall_entry.asm */
int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5);

/* Set the kernel stack for SYSCALL entry (called on context switch) */
void syscall_set_kernel_stack(uint64_t rsp);

#endif
