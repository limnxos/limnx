/*
 * ARM64 Linux syscall numbers (generic/asm-generic table)
 * Matches include/uapi/asm-generic/unistd.h
 * Used by ARM64, RISC-V, LoongArch, and other new Linux architectures
 */
#ifndef LIMNX_ARM64_SYSCALL_NR_H
#define LIMNX_ARM64_SYSCALL_NR_H

#define __NR_io_setup            0
#define __NR_io_destroy          1
#define __NR_io_submit           2
#define __NR_io_cancel           3
#define __NR_io_getevents        4
#define __NR_setxattr            5
#define __NR_dup3               24
#define __NR_fcntl              25
#define __NR_ioctl              29
#define __NR_faccessat          48
#define __NR_chdir              49
#define __NR_openat             56
#define __NR_close              57
#define __NR_pipe2              59
#define __NR_getdents64         61
#define __NR_lseek              62
#define __NR_read               63
#define __NR_write              64
#define __NR_writev             66
#define __NR_pselect6           72
#define __NR_ppoll              73
#define __NR_readlinkat         78
#define __NR_fstatat            79   /* newfstatat */
#define __NR_fstat              80
#define __NR_truncate           45
#define __NR_ftruncate          46
#define __NR_fallocate          47
#define __NR_utimensat          88
#define __NR_exit               93
#define __NR_exit_group         94
#define __NR_set_tid_address    96
#define __NR_futex              98
#define __NR_nanosleep         101
#define __NR_clock_gettime     113
#define __NR_sched_yield       124
#define __NR_kill              129
#define __NR_rt_sigaction      134
#define __NR_rt_sigprocmask    135
#define __NR_rt_sigreturn      139
#define __NR_setuid            146
#define __NR_setgid            144
#define __NR_getuid            174
#define __NR_getgid            176
#define __NR_geteuid           175
#define __NR_getegid           177
#define __NR_setpgid           154
#define __NR_getpgid           155
#define __NR_getppid           173
#define __NR_getpid            172
#define __NR_setsid            157
#define __NR_getsid            156
#define __NR_getgroups         158
#define __NR_setgroups         159
#define __NR_socket            198
#define __NR_bind              200
#define __NR_listen            201
#define __NR_accept            202
#define __NR_connect           203
#define __NR_sendto            206
#define __NR_recvfrom          207
#define __NR_shutdown          210
#define __NR_brk               214
#define __NR_munmap            215
#define __NR_mmap              222
#define __NR_mprotect          226
#define __NR_wait4             260
#define __NR_execve            221
#define __NR_getrlimit          163   /* via prlimit64=261 on generic, but keep for compat */
#define __NR_setrlimit          164
#define __NR_umask             166
#define __NR_getcwd            17
#define __NR_chown             -1    /* use fchownat=54 */
#define __NR_fchown            55
#define __NR_chmod             -1    /* use fchmodat=53 */
#define __NR_fchmodat          53
#define __NR_fchownat          54
#define __NR_mkdirat           34
#define __NR_unlinkat          35
#define __NR_symlinkat         36
#define __NR_renameat2         276
#define __NR_mount             40
#define __NR_umount2           39
#define __NR_fork              -1    /* use clone=220 */
#define __NR_clone             220
#define __NR_prlimit64         261
#define __NR_epoll_create1     20
#define __NR_epoll_ctl         21
#define __NR_epoll_pwait       22
#define __NR_eventfd2          19
#define __NR_seccomp           277
#define __NR_getrandom         278
#define __NR_io_uring_setup    425
#define __NR_io_uring_enter    426
#define __NR_clone3            435

/* Generic table does NOT have these classic syscalls.
 * Limnx provides them anyway for compatibility: */
#define __NR_open              -1    /* use openat */
#define __NR_stat              -1    /* use fstatat */
#define __NR_access            -1    /* use faccessat */
#define __NR_pipe              -1    /* use pipe2 */
#define __NR_dup               -1    /* use dup3 */
#define __NR_dup2              -1    /* use dup3 */
#define __NR_select            -1    /* use pselect6 */
#define __NR_mkdir             -1    /* use mkdirat */
#define __NR_rmdir             -1    /* use unlinkat */
#define __NR_creat             -1    /* use openat */
#define __NR_unlink            -1    /* use unlinkat */
#define __NR_symlink           -1    /* use symlinkat */
#define __NR_readlink          -1    /* use readlinkat */
#define __NR_rename            -1    /* use renameat2 */
#define __NR_arch_prctl        -1    /* x86_64 only */
#define __NR_getdents          -1    /* use getdents64 */

#endif
