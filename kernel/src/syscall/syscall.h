#ifndef LIMNX_SYSCALL_H
#define LIMNX_SYSCALL_H

#include <stdint.h>

/* Syscall numbers */
#define SYS_WRITE  0
#define SYS_YIELD  1
#define SYS_EXIT   2
#define SYS_OPEN   3
#define SYS_READ   4
#define SYS_CLOSE  5
#define SYS_STAT   6
#define SYS_EXEC     7
#define SYS_SOCKET   8
#define SYS_BIND     9
#define SYS_SENDTO   10
#define SYS_RECVFROM 11
#define SYS_FWRITE   12
#define SYS_CREATE   13
#define SYS_UNLINK   14
#define SYS_MMAP     15
#define SYS_MUNMAP   16
#define SYS_GETCHAR  17
#define SYS_WAITPID  18
#define SYS_PIPE     19
#define SYS_GETPID   20
#define SYS_FMMAP    21
#define SYS_READDIR  22
#define SYS_MKDIR    23
#define SYS_SEEK     24
#define SYS_TRUNCATE 25
#define SYS_CHDIR    26
#define SYS_GETCWD   27
#define SYS_FSTAT    28
#define SYS_RENAME   29
#define SYS_DUP      30
#define SYS_DUP2     31
#define SYS_KILL     32
#define SYS_FCNTL    33
#define SYS_SETPGID  34
#define SYS_GETPGID  35
#define SYS_CHMOD    36
#define SYS_SHMGET   37
#define SYS_SHMAT    38
#define SYS_SHMDT    39
#define SYS_FORK     40
#define SYS_SIGACTION 41
#define SYS_SIGRETURN  42
#define SYS_OPENPTY    43
#define SYS_TCP_SOCKET 44
#define SYS_TCP_CONNECT 45
#define SYS_TCP_LISTEN 46
#define SYS_TCP_ACCEPT 47
#define SYS_TCP_SEND   48
#define SYS_TCP_RECV   49
#define SYS_TCP_CLOSE  50
#define SYS_IOCTL          51
#define SYS_CLOCK_GETTIME  52
#define SYS_NANOSLEEP      53
#define SYS_GETENV         54
#define SYS_SETENV         55
#define SYS_POLL           56
#define SYS_GETUID         57
#define SYS_SETUID         58
#define SYS_GETGID         59
#define SYS_SETGID         60
#define SYS_GETCAP         61
#define SYS_SETCAP         62
#define SYS_GETRLIMIT      63
#define SYS_SETRLIMIT      64
#define SYS_SECCOMP        65
#define SYS_SETAUDIT       66
#define SYS_UNIX_SOCKET    67
#define SYS_UNIX_BIND      68
#define SYS_UNIX_LISTEN    69
#define SYS_UNIX_ACCEPT    70
#define SYS_UNIX_CONNECT   71
#define SYS_AGENT_REGISTER 72
#define SYS_AGENT_LOOKUP   73
#define SYS_EVENTFD        74
#define SYS_EPOLL_CREATE   75
#define SYS_EPOLL_CTL      76
#define SYS_EPOLL_WAIT     77
#define SYS_SWAP_STAT      78
#define SYS_INFER_REGISTER 79
#define SYS_INFER_REQUEST  80
#define SYS_URING_SETUP    81
#define SYS_URING_ENTER    82
#define SYS_MMAP2          83
#define SYS_TOKEN_CREATE   84
#define SYS_TOKEN_REVOKE   85
#define SYS_TOKEN_LIST     86
#define SYS_NS_CREATE      87
#define SYS_NS_JOIN        88
#define SYS_PROCINFO       89
#define SYS_FSSTAT         90
#define SYS_TASK_CREATE    91
#define SYS_TASK_DEPEND    92
#define SYS_TASK_START     93
#define SYS_TASK_COMPLETE  94
#define SYS_TASK_STATUS    95
#define SYS_TASK_WAIT      96
#define SYS_TOKEN_DELEGATE 97
#define SYS_NS_SETQUOTA    98
#define SYS_INFER_HEALTH   99
#define SYS_INFER_ROUTE   100
#define SYS_AGENT_SEND    101
#define SYS_AGENT_RECV    102
#define SYS_FUTEX_WAIT    103
#define SYS_FUTEX_WAKE    104
#define SYS_MMAP_FILE     105
#define SYS_MPROTECT      106
#define SYS_MMAP_GUARD    107
#define SYS_SIGPROCMASK   108
#define SYS_ARCH_PRCTL    109
#define SYS_SELECT        110
#define SYS_SUPER_CREATE  111
#define SYS_SUPER_ADD     112
#define SYS_SUPER_SET_POLICY 113
#define SYS_PIPE2         114
#define SYS_SUPER_START   115
#define SYS_TCP_SETOPT    116
#define SYS_TCP_TO_FD     117
#define SYS_INFER_SET_POLICY 118
#define SYS_INFER_QUEUE_STAT 119
#define SYS_NR            120

/* Capability bits */
#define CAP_NET_BIND  (1 << 0)
#define CAP_NET_RAW   (1 << 1)
#define CAP_KILL      (1 << 2)
#define CAP_SETUID    (1 << 3)
#define CAP_SYS_ADMIN (1 << 4)
#define CAP_EXEC      (1 << 5)
#define CAP_FS_WRITE  (1 << 6)
#define CAP_FS_READ   (1 << 7)
#define CAP_ALL       0xFF

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

/* errno codes (returned as negative values from new syscalls) */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EINVAL  22
#define EMFILE  24
#define ENOSYS  38
#define EAGAIN  11
#define EADDRINUSE  98
#define ENOTCONN   107
#define ECONNREFUSED 111
#define EEXIST    17
#define EBADF      9
#define ENOBUFS  105

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

/* Maximum user-space address (canonical lower-half boundary) */
#define USER_ADDR_MAX 0x0000800000000000ULL

void syscall_init(void);

/* Called from syscall_entry.asm */
int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5);

/* Set the kernel stack for SYSCALL entry (called on context switch) */
void syscall_set_kernel_stack(uint64_t rsp);

#endif
