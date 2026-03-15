/*
 * Limnx syscall numbers — SINGLE SOURCE OF TRUTH
 *
 * Linux-compatible numbers from arch-specific headers.
 * Limnx-specific numbers (512+) shared across all architectures.
 *
 * Used by: kernel (syscall dispatch), user libc (syscall wrappers),
 *          NASM programs (via generated include), musl integration.
 */
#ifndef LIMNX_SYSCALL_NR_H
#define LIMNX_SYSCALL_NR_H

/* ---- Arch-specific Linux numbers ---- */
#if defined(__x86_64__)
#include "arch/x86_64/syscall_nr.h"
#elif defined(__aarch64__)
#include "arch/arm64/syscall_nr.h"
#endif

/* ---- Portable SYS_* names mapping to arch __NR_* ---- */

/* Available on both architectures */
#define SYS_READ            __NR_read
#define SYS_WRITE           __NR_write
#define SYS_CLOSE           __NR_close
#define SYS_FSTAT           __NR_fstat
#define SYS_LSEEK           __NR_lseek
#define SYS_MMAP            __NR_mmap
#define SYS_MPROTECT        __NR_mprotect
#define SYS_MUNMAP          __NR_munmap
#define SYS_BRK             __NR_brk
#define SYS_RT_SIGACTION    __NR_rt_sigaction
#define SYS_RT_SIGPROCMASK  __NR_rt_sigprocmask
#define SYS_RT_SIGRETURN    __NR_rt_sigreturn
#define SYS_IOCTL           __NR_ioctl
#define SYS_WRITEV          __NR_writev
#define SYS_PIPE2           __NR_pipe2
#define SYS_SCHED_YIELD     __NR_sched_yield
#define SYS_NANOSLEEP       __NR_nanosleep
#define SYS_GETPID          __NR_getpid
#define SYS_SOCKET          __NR_socket
#define SYS_CONNECT         __NR_connect
#define SYS_ACCEPT          __NR_accept
#define SYS_SENDTO          __NR_sendto
#define SYS_RECVFROM        __NR_recvfrom
#define SYS_SHUTDOWN        __NR_shutdown
#define SYS_BIND            __NR_bind
#define SYS_LISTEN          __NR_listen
#define SYS_EXECVE          __NR_execve
#define SYS_EXIT            __NR_exit
#define SYS_EXIT_GROUP      __NR_exit_group
#define SYS_SET_TID_ADDRESS __NR_set_tid_address
#define SYS_GETPPID         __NR_getppid
#define SYS_WAIT4           __NR_wait4
#define SYS_KILL            __NR_kill
#define SYS_FCNTL           __NR_fcntl
#define SYS_TRUNCATE        __NR_truncate
#define SYS_GETCWD          __NR_getcwd
#define SYS_CHDIR           __NR_chdir
#define SYS_FCHOWN          __NR_fchown
#define SYS_UMASK           __NR_umask
#define SYS_GETRLIMIT       __NR_getrlimit
#define SYS_SETRLIMIT       __NR_setrlimit
#define SYS_GETUID          __NR_getuid
#define SYS_GETGID          __NR_getgid
#define SYS_SETUID          __NR_setuid
#define SYS_SETGID          __NR_setgid
#define SYS_GETEUID         __NR_geteuid
#define SYS_GETEGID         __NR_getegid
#define SYS_SETPGID         __NR_setpgid
#define SYS_GETPPID         __NR_getppid
#define SYS_SETSID          __NR_setsid
#define SYS_GETSID          __NR_getsid
#define SYS_GETGROUPS       __NR_getgroups
#define SYS_SETGROUPS       __NR_setgroups
#define SYS_MOUNT           __NR_mount
#define SYS_UMOUNT2         __NR_umount2
#define SYS_FUTEX           __NR_futex
#define SYS_CLOCK_GETTIME   __NR_clock_gettime
#define SYS_EPOLL_CTL       __NR_epoll_ctl
#define SYS_EPOLL_CREATE1   __NR_epoll_create1
#define SYS_EVENTFD2        __NR_eventfd2
#define SYS_SECCOMP         __NR_seccomp
#define SYS_GETDENTS64      __NR_getdents64
#define SYS_IO_URING_SETUP  __NR_io_uring_setup
#define SYS_IO_URING_ENTER  __NR_io_uring_enter
/* *at() variants — both archs */
#define SYS_OPENAT          __NR_openat
#define SYS_MKDIRAT         __NR_mkdirat
#define SYS_FSTATAT         __NR_fstatat
#define SYS_UNLINKAT        __NR_unlinkat
#define SYS_SYMLINKAT       __NR_symlinkat
#define SYS_READLINKAT      __NR_readlinkat
#define SYS_FCHMODAT        __NR_fchmodat
#define SYS_FCHOWNAT        __NR_fchownat
#define SYS_FACCESSAT       __NR_faccessat

/* x86_64 only — classic syscalls (undefined on ARM64) */
#ifdef __NR_open
#define SYS_OPEN            __NR_open
#define SYS_STAT            __NR_stat
#define SYS_PIPE            __NR_pipe
#define SYS_SELECT          __NR_select
#define SYS_DUP             __NR_dup
#define SYS_DUP2            __NR_dup2
#define SYS_FORK            __NR_fork
#define SYS_RENAME          __NR_rename
#define SYS_MKDIR           __NR_mkdir
#define SYS_RMDIR           __NR_rmdir
#define SYS_CREAT           __NR_creat
#define SYS_UNLINK          __NR_unlink
#define SYS_SYMLINK         __NR_symlink
#define SYS_READLINK        __NR_readlink
#define SYS_CHMOD           __NR_chmod
#define SYS_CHOWN           __NR_chown
#define SYS_ARCH_PRCTL      __NR_arch_prctl
#define SYS_EPOLL_WAIT      __NR_epoll_wait
#define SYS_POLL            __NR_poll
#endif

/* ARM64 only — generic table equivalents */
#ifdef __aarch64__
#define SYS_OPEN            __NR_openat
#define SYS_STAT            __NR_fstatat
#define SYS_PIPE            __NR_pipe2
#define SYS_SELECT          __NR_pselect6
#define SYS_DUP             __NR_dup3
#define SYS_DUP2            __NR_dup3
#define SYS_FORK            __NR_clone
#define SYS_RENAME          __NR_renameat2
#define SYS_MKDIR           __NR_mkdirat
#define SYS_CREAT           __NR_openat
#define SYS_UNLINK          __NR_unlinkat
#define SYS_SYMLINK         __NR_symlinkat
#define SYS_READLINK        __NR_readlinkat
#define SYS_CHMOD           __NR_fchmodat
#define SYS_CHOWN           __NR_fchownat
#define SYS_EPOLL_WAIT      __NR_epoll_pwait
#define SYS_POLL            __NR_ppoll
#endif

/* ---- Limnx-specific (512+, same on all archs) ---- */
#define SYS_PUTS           512
#define SYS_EXEC_OLD       513
#define SYS_GETCHAR        514
#define SYS_READDIR        515
#define SYS_FMMAP          516
#define SYS_MMAP2          517
#define SYS_MMAP_FILE      518
#define SYS_MMAP_GUARD     519
#define SYS_SHMGET         520
#define SYS_SHMAT          521
#define SYS_SHMDT          522
#define SYS_GETENV         523
#define SYS_SETENV         524
#define SYS_ENVIRON        525
#define SYS_AGENT_REGISTER 530
#define SYS_AGENT_LOOKUP   531
#define SYS_AGENT_SEND     532
#define SYS_AGENT_RECV     533
#define SYS_TOKEN_CREATE   534
#define SYS_TOKEN_REVOKE   535
#define SYS_TOKEN_LIST     536
#define SYS_TOKEN_DELEGATE 537
#define SYS_NS_CREATE      538
#define SYS_NS_JOIN        539
#define SYS_NS_SETQUOTA    540
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
#define SYS_TASK_CREATE    552
#define SYS_TASK_DEPEND    553
#define SYS_TASK_START     554
#define SYS_TASK_COMPLETE  555
#define SYS_TASK_STATUS    556
#define SYS_TASK_WAIT      557
#define SYS_SUPER_CREATE   558
#define SYS_SUPER_ADD      559
#define SYS_SUPER_SET_POLICY 560
#define SYS_SUPER_START    561
#define SYS_SUPER_LIST     562
#define SYS_SUPER_STOP     563
#define SYS_TOPIC_CREATE   564
#define SYS_TOPIC_SUB      565
#define SYS_TOPIC_PUB      566
#define SYS_TOPIC_RECV     567
#define SYS_UNIX_SOCKET    568
#define SYS_UNIX_BIND      569
#define SYS_UNIX_LISTEN    570
#define SYS_UNIX_ACCEPT    571
#define SYS_UNIX_CONNECT   572
#define SYS_TCP_SOCKET     573
#define SYS_TCP_CONNECT    574
#define SYS_TCP_LISTEN     575
#define SYS_TCP_ACCEPT     576
#define SYS_TCP_SEND       577
#define SYS_TCP_RECV       578
#define SYS_TCP_CLOSE      579
#define SYS_TCP_SETOPT     580
#define SYS_TCP_TO_FD      581
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
#define SYS_OPENPTY        594

#define SYS_NR             600

#endif /* LIMNX_SYSCALL_NR_H */
