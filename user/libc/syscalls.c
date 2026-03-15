/*
 * Limnx libc — portable C syscall wrappers
 *
 * Replaces the NASM syscalls.asm with arch-independent C using
 * inline assembly from arch/<ARCH>/syscall_arch.h.
 */

#if defined(__x86_64__)
#include "arch/x86_64/syscall_arch.h"
#elif defined(__aarch64__)
#include "arch/arm64/syscall_arch.h"
#endif

/* Syscall numbers (must match kernel syscall table and user/syscall.inc) */
#define SYS_WRITE           0
#define SYS_YIELD           1
#define SYS_EXIT            2
#define SYS_OPEN            3
#define SYS_READ            4
#define SYS_CLOSE           5
#define SYS_STAT            6
#define SYS_EXEC            7
#define SYS_SOCKET          8
#define SYS_BIND            9
#define SYS_SENDTO         10
#define SYS_RECVFROM       11
#define SYS_FWRITE         12
#define SYS_CREATE         13
#define SYS_UNLINK         14
#define SYS_MMAP           15
#define SYS_MUNMAP         16
#define SYS_GETCHAR        17
#define SYS_WAITPID        18
#define SYS_PIPE           19
#define SYS_GETPID         20
#define SYS_FMMAP          21
#define SYS_READDIR        22
#define SYS_MKDIR          23
#define SYS_SEEK           24
#define SYS_TRUNCATE       25
#define SYS_CHDIR          26
#define SYS_GETCWD         27
#define SYS_FSTAT          28
#define SYS_RENAME         29
#define SYS_DUP            30
#define SYS_DUP2           31
#define SYS_KILL           32
#define SYS_FCNTL          33
#define SYS_SETPGID        34
#define SYS_GETPGID        35
#define SYS_CHMOD          36
#define SYS_SHMGET         37
#define SYS_SHMAT          38
#define SYS_SHMDT          39
#define SYS_FORK           40
#define SYS_SIGACTION      41
#define SYS_SIGRETURN      42
#define SYS_OPENPTY        43
#define SYS_TCP_SOCKET     44
#define SYS_TCP_CONNECT    45
#define SYS_TCP_LISTEN     46
#define SYS_TCP_ACCEPT     47
#define SYS_TCP_SEND       48
#define SYS_TCP_RECV       49
#define SYS_TCP_CLOSE      50
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
#define SYS_NS_SETQUOTA   98
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
#define SYS_INFER_CACHE_CTRL 120
#define SYS_INFER_SUBMIT  121
#define SYS_INFER_POLL    122
#define SYS_INFER_RESULT  123
#define SYS_EXECVE        124
#define SYS_TOPIC_CREATE  125
#define SYS_TOPIC_SUB     126
#define SYS_TOPIC_PUB     127
#define SYS_TOPIC_RECV    128
#define SYS_INFER_SWAP    129
#define SYS_ENVIRON       130
#define SYS_SUPER_LIST    131
#define SYS_SUPER_STOP    132
#define SYS_CHOWN         133
#define SYS_FCHOWN        134
#define SYS_UMASK         135
#define SYS_GETEUID       136
#define SYS_GETEGID       137
#define SYS_GETGROUPS     138
#define SYS_SETGROUPS     139
#define SYS_SYMLINK       140
#define SYS_READLINK      141
#define SYS_SETSID        142
#define SYS_GETSID        143
#define SYS_TCSETPGRP     144
#define SYS_TCGETPGRP     145
#define SYS_MKFIFO        146

/* --- Syscall wrappers --- */

long sys_write(const void *buf, unsigned long len) {
    return __syscall2(SYS_WRITE, (long)buf, (long)len);
}

long sys_yield(void) {
    return __syscall0(SYS_YIELD);
}

void __attribute__((noreturn)) sys_exit(long status) {
    __syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

long sys_open(const char *path, unsigned long flags) {
    return __syscall2(SYS_OPEN, (long)path, (long)flags);
}

long sys_read(long fd, void *buf, unsigned long len) {
    return __syscall3(SYS_READ, fd, (long)buf, (long)len);
}

long sys_close(long fd) {
    return __syscall1(SYS_CLOSE, fd);
}

long sys_stat(const char *path, void *stat_buf) {
    return __syscall2(SYS_STAT, (long)path, (long)stat_buf);
}

long sys_exec(const char *path, const char **argv) {
    return __syscall2(SYS_EXEC, (long)path, (long)argv);
}

long sys_socket(void) {
    return __syscall0(SYS_SOCKET);
}

long sys_bind(long sockfd, unsigned long port) {
    return __syscall2(SYS_BIND, sockfd, (long)port);
}

long sys_sendto(long sockfd, const void *buf, unsigned long len,
                unsigned long dst_ip, unsigned long dst_port) {
    return __syscall5(SYS_SENDTO, sockfd, (long)buf, (long)len, (long)dst_ip, (long)dst_port);
}

long sys_recvfrom(long sockfd, void *buf, unsigned long len,
                  void *src_ip_ptr, void *src_port_ptr) {
    return __syscall5(SYS_RECVFROM, sockfd, (long)buf, (long)len, (long)src_ip_ptr, (long)src_port_ptr);
}

long sys_fwrite(long fd, const void *buf, unsigned long len) {
    return __syscall3(SYS_FWRITE, fd, (long)buf, (long)len);
}

long sys_create(const char *path) {
    return __syscall1(SYS_CREATE, (long)path);
}

long sys_unlink(const char *path) {
    return __syscall1(SYS_UNLINK, (long)path);
}

long sys_mmap(unsigned long num_pages) {
    return __syscall1(SYS_MMAP, (long)num_pages);
}

long sys_munmap(unsigned long virt_addr) {
    return __syscall1(SYS_MUNMAP, (long)virt_addr);
}

long sys_getchar(void) {
    return __syscall0(SYS_GETCHAR);
}

long sys_waitpid(long pid) {
    return __syscall2(SYS_WAITPID, pid, 0);
}

long sys_waitpid_flags(long pid, long flags) {
    return __syscall2(SYS_WAITPID, pid, flags);
}

long sys_pipe(long *rfd_ptr, long *wfd_ptr) {
    return __syscall2(SYS_PIPE, (long)rfd_ptr, (long)wfd_ptr);
}

long sys_getpid(void) {
    return __syscall0(SYS_GETPID);
}

long sys_fmmap(long fd) {
    return __syscall1(SYS_FMMAP, fd);
}

long sys_readdir(const char *dir_path, unsigned long index, void *dirent_ptr) {
    return __syscall3(SYS_READDIR, (long)dir_path, (long)index, (long)dirent_ptr);
}

long sys_mkdir(const char *path) {
    return __syscall1(SYS_MKDIR, (long)path);
}

long sys_seek(long fd, long offset, int whence) {
    return __syscall3(SYS_SEEK, fd, offset, (long)whence);
}

long sys_truncate(const char *path, unsigned long new_size) {
    return __syscall2(SYS_TRUNCATE, (long)path, (long)new_size);
}

long sys_chdir(const char *path) {
    return __syscall1(SYS_CHDIR, (long)path);
}

long sys_getcwd(char *buf, unsigned long size) {
    return __syscall2(SYS_GETCWD, (long)buf, (long)size);
}

long sys_fstat(long fd, void *stat_buf) {
    return __syscall2(SYS_FSTAT, fd, (long)stat_buf);
}

long sys_rename(const char *old_path, const char *new_path) {
    return __syscall2(SYS_RENAME, (long)old_path, (long)new_path);
}

long sys_dup(long fd) {
    return __syscall1(SYS_DUP, fd);
}

long sys_dup2(long oldfd, long newfd) {
    return __syscall2(SYS_DUP2, oldfd, newfd);
}

long sys_kill(long pid, long signal) {
    return __syscall2(SYS_KILL, pid, signal);
}

long sys_fcntl(long fd, long cmd, long arg) {
    return __syscall3(SYS_FCNTL, fd, cmd, arg);
}

long sys_setpgid(long pid, long pgid) {
    return __syscall2(SYS_SETPGID, pid, pgid);
}

long sys_getpgid(long pid) {
    return __syscall1(SYS_GETPGID, pid);
}

long sys_chmod(const char *path, long mode) {
    return __syscall2(SYS_CHMOD, (long)path, mode);
}

long sys_shmget(long key, long num_pages) {
    return __syscall2(SYS_SHMGET, key, num_pages);
}

long sys_shmat(long shmid) {
    return __syscall1(SYS_SHMAT, shmid);
}

long sys_shmdt(long addr) {
    return __syscall1(SYS_SHMDT, addr);
}

long sys_fork(void) {
    return __syscall0(SYS_FORK);
}

long sys_sigaction(int signum, void (*handler)(int)) {
    return __syscall2(SYS_SIGACTION, (long)signum, (long)handler);
}

long sys_sigaction3(int signum, void (*handler)(int), int flags) {
    return __syscall3(SYS_SIGACTION, (long)signum, (long)handler, (long)flags);
}

void sys_sigreturn(void) {
    __syscall0(SYS_SIGRETURN);
}

long sys_sigprocmask(int how, unsigned int new_mask, unsigned int *old_mask) {
    return __syscall3(SYS_SIGPROCMASK, (long)how, (long)new_mask, (long)old_mask);
}

long sys_openpty(long *master_fd, long *slave_fd) {
    return __syscall2(SYS_OPENPTY, (long)master_fd, (long)slave_fd);
}

long sys_tcp_socket(void) {
    return __syscall0(SYS_TCP_SOCKET);
}

long sys_tcp_connect(long conn, unsigned int ip, int port) {
    return __syscall3(SYS_TCP_CONNECT, conn, (long)ip, (long)port);
}

long sys_tcp_listen(long conn, int port) {
    return __syscall2(SYS_TCP_LISTEN, conn, (long)port);
}

long sys_tcp_accept(long listen_conn) {
    return __syscall1(SYS_TCP_ACCEPT, listen_conn);
}

long sys_tcp_send(long conn, const void *buf, long len) {
    return __syscall3(SYS_TCP_SEND, conn, (long)buf, len);
}

long sys_tcp_recv(long conn, void *buf, long len) {
    return __syscall3(SYS_TCP_RECV, conn, (long)buf, len);
}

long sys_tcp_close(long conn) {
    return __syscall1(SYS_TCP_CLOSE, conn);
}

long sys_tcp_setopt(long conn, long opt, long value) {
    return __syscall3(SYS_TCP_SETOPT, conn, opt, value);
}

long sys_tcp_to_fd(long conn) {
    return __syscall1(SYS_TCP_TO_FD, conn);
}

long sys_ioctl(long fd, long cmd, long arg) {
    return __syscall3(SYS_IOCTL, fd, cmd, arg);
}

long sys_clock_gettime(long clockid, void *timespec_ptr) {
    return __syscall2(SYS_CLOCK_GETTIME, clockid, (long)timespec_ptr);
}

long sys_nanosleep(const void *timespec_ptr) {
    return __syscall1(SYS_NANOSLEEP, (long)timespec_ptr);
}

long sys_getenv(const char *key, char *val_buf, long val_buf_size) {
    return __syscall3(SYS_GETENV, (long)key, (long)val_buf, val_buf_size);
}

long sys_setenv(const char *key, const char *value) {
    return __syscall2(SYS_SETENV, (long)key, (long)value);
}

long sys_poll(void *fds, long nfds, long timeout_ms) {
    return __syscall3(SYS_POLL, (long)fds, nfds, timeout_ms);
}

long sys_getuid(void) {
    return __syscall0(SYS_GETUID);
}

long sys_setuid(long uid) {
    return __syscall1(SYS_SETUID, uid);
}

long sys_getgid(void) {
    return __syscall0(SYS_GETGID);
}

long sys_setgid(long gid) {
    return __syscall1(SYS_SETGID, gid);
}

long sys_getcap(void) {
    return __syscall0(SYS_GETCAP);
}

long sys_setcap(long pid, long caps) {
    return __syscall2(SYS_SETCAP, pid, caps);
}

long sys_getrlimit(long resource, void *rlimit_ptr) {
    return __syscall2(SYS_GETRLIMIT, resource, (long)rlimit_ptr);
}

long sys_setrlimit(long resource, const void *rlimit_ptr) {
    return __syscall2(SYS_SETRLIMIT, resource, (long)rlimit_ptr);
}

long sys_seccomp(unsigned long mask, long strict) {
    return __syscall2(SYS_SECCOMP, (long)mask, strict);
}

long sys_setaudit(long pid, long flags) {
    return __syscall2(SYS_SETAUDIT, pid, flags);
}

long sys_unix_socket(void) {
    return __syscall0(SYS_UNIX_SOCKET);
}

long sys_unix_bind(long fd, const char *path) {
    return __syscall2(SYS_UNIX_BIND, fd, (long)path);
}

long sys_unix_listen(long fd) {
    return __syscall1(SYS_UNIX_LISTEN, fd);
}

long sys_unix_accept(long fd) {
    return __syscall1(SYS_UNIX_ACCEPT, fd);
}

long sys_unix_connect(const char *path) {
    return __syscall1(SYS_UNIX_CONNECT, (long)path);
}

long sys_agent_register(const char *name) {
    return __syscall1(SYS_AGENT_REGISTER, (long)name);
}

long sys_agent_lookup(const char *name, long *pid_out) {
    return __syscall2(SYS_AGENT_LOOKUP, (long)name, (long)pid_out);
}

long sys_eventfd(long flags) {
    return __syscall1(SYS_EVENTFD, flags);
}

long sys_epoll_create(long flags) {
    return __syscall1(SYS_EPOLL_CREATE, flags);
}

long sys_epoll_ctl(long epfd, long op, long fd, void *event) {
    return __syscall4(SYS_EPOLL_CTL, epfd, op, fd, (long)event);
}

long sys_epoll_wait(long epfd, void *events, long max_events, long timeout_ms) {
    return __syscall4(SYS_EPOLL_WAIT, epfd, (long)events, max_events, timeout_ms);
}

long sys_swap_stat(void *stat) {
    return __syscall1(SYS_SWAP_STAT, (long)stat);
}

long sys_infer_register(const char *name, const char *sock_path) {
    return __syscall2(SYS_INFER_REGISTER, (long)name, (long)sock_path);
}

long sys_infer_request(const char *name, const void *req_buf, long req_len,
                       void *resp_buf, long resp_len) {
    return __syscall5(SYS_INFER_REQUEST, (long)name, (long)req_buf, req_len, (long)resp_buf, resp_len);
}

long sys_infer_health(long load) {
    return __syscall1(SYS_INFER_HEALTH, load);
}

long sys_infer_route(const char *name) {
    return __syscall1(SYS_INFER_ROUTE, (long)name);
}

long sys_infer_set_policy(long policy) {
    return __syscall1(SYS_INFER_SET_POLICY, policy);
}

long sys_infer_queue_stat(void *stat_ptr) {
    return __syscall1(SYS_INFER_QUEUE_STAT, (long)stat_ptr);
}

long sys_infer_cache_ctrl(long cmd, void *arg) {
    return __syscall2(SYS_INFER_CACHE_CTRL, cmd, (long)arg);
}

long sys_infer_submit(const char *name, const void *req_buf, long req_len, long eventfd_idx) {
    return __syscall4(SYS_INFER_SUBMIT, (long)name, (long)req_buf, req_len, eventfd_idx);
}

long sys_infer_poll(long request_id) {
    return __syscall1(SYS_INFER_POLL, request_id);
}

long sys_infer_result(long request_id, void *resp_buf, long resp_len) {
    return __syscall3(SYS_INFER_RESULT, request_id, (long)resp_buf, resp_len);
}

long sys_infer_swap(const char *name, const char *new_sock_path) {
    return __syscall2(SYS_INFER_SWAP, (long)name, (long)new_sock_path);
}

long sys_uring_setup(long entries, void *params) {
    return __syscall2(SYS_URING_SETUP, entries, (long)params);
}

long sys_uring_enter(long uring_fd, void *sqe_ptr, long count, void *cqe_ptr) {
    return __syscall4(SYS_URING_ENTER, uring_fd, (long)sqe_ptr, count, (long)cqe_ptr);
}

long sys_mmap2(long num_pages, long flags) {
    return __syscall2(SYS_MMAP2, num_pages, flags);
}

long sys_token_create(long perms, long target_pid, const char *resource) {
    return __syscall3(SYS_TOKEN_CREATE, perms, target_pid, (long)resource);
}

long sys_token_revoke(long token_id) {
    return __syscall1(SYS_TOKEN_REVOKE, token_id);
}

long sys_token_list(void *buf, long max_count) {
    return __syscall2(SYS_TOKEN_LIST, (long)buf, max_count);
}

long sys_token_delegate(long parent_id, long target_pid, long perms,
                        const char *resource) {
    return __syscall4(SYS_TOKEN_DELEGATE, parent_id, target_pid, perms, (long)resource);
}

long sys_ns_create(const char *name) {
    return __syscall1(SYS_NS_CREATE, (long)name);
}

long sys_ns_join(long ns_id) {
    return __syscall1(SYS_NS_JOIN, ns_id);
}

long sys_ns_setquota(long ns_id, long resource, long limit) {
    return __syscall3(SYS_NS_SETQUOTA, ns_id, resource, limit);
}

long sys_procinfo(long index, void *info) {
    return __syscall2(SYS_PROCINFO, index, (long)info);
}

long sys_fsstat(void *st) {
    return __syscall1(SYS_FSSTAT, (long)st);
}

long sys_task_create(const char *name, long ns_id) {
    return __syscall2(SYS_TASK_CREATE, (long)name, ns_id);
}

long sys_task_depend(long task_id, long dep_id) {
    return __syscall2(SYS_TASK_DEPEND, task_id, dep_id);
}

long sys_task_start(long task_id) {
    return __syscall1(SYS_TASK_START, task_id);
}

long sys_task_complete(long task_id, long result) {
    return __syscall2(SYS_TASK_COMPLETE, task_id, result);
}

long sys_task_status(long task_id, void *out) {
    return __syscall2(SYS_TASK_STATUS, task_id, (long)out);
}

long sys_task_wait(long task_id) {
    return __syscall1(SYS_TASK_WAIT, task_id);
}

long sys_agent_send(const char *name, const void *msg_buf, long msg_len, long token_id) {
    return __syscall4(SYS_AGENT_SEND, (long)name, (long)msg_buf, msg_len, token_id);
}

long sys_agent_recv(void *msg_buf, long msg_len, long *sender_pid_ptr, long *token_id_ptr) {
    return __syscall4(SYS_AGENT_RECV, (long)msg_buf, msg_len, (long)sender_pid_ptr, (long)token_id_ptr);
}

long sys_futex_wait(volatile unsigned int *addr, unsigned int expected) {
    return __syscall2(SYS_FUTEX_WAIT, (long)addr, (long)expected);
}

long sys_futex_wake(volatile unsigned int *addr, unsigned int max_wake) {
    return __syscall2(SYS_FUTEX_WAKE, (long)addr, (long)max_wake);
}

long sys_mmap_file(long fd, long offset, long num_pages) {
    return __syscall3(SYS_MMAP_FILE, fd, offset, num_pages);
}

long sys_mprotect(long virt_addr, long num_pages, long prot) {
    return __syscall3(SYS_MPROTECT, virt_addr, num_pages, prot);
}

long sys_mmap_guard(long num_pages) {
    return __syscall1(SYS_MMAP_GUARD, num_pages);
}

long sys_arch_prctl(long code, long addr) {
    return __syscall2(SYS_ARCH_PRCTL, code, addr);
}

long sys_select(long nfds, void *readfds, void *writefds, long timeout_us) {
    return __syscall4(SYS_SELECT, nfds, (long)readfds, (long)writefds, timeout_us);
}

long sys_super_create(const char *name) {
    return __syscall1(SYS_SUPER_CREATE, (long)name);
}

long sys_super_add(long super_id, const char *elf_path, long ns_id, long caps) {
    return __syscall4(SYS_SUPER_ADD, super_id, (long)elf_path, ns_id, caps);
}

long sys_super_set_policy(long super_id, long policy) {
    return __syscall2(SYS_SUPER_SET_POLICY, super_id, policy);
}

long sys_super_start(long super_id) {
    return __syscall1(SYS_SUPER_START, super_id);
}

long sys_super_list(void *buf, long max_count) {
    return __syscall2(SYS_SUPER_LIST, (long)buf, max_count);
}

long sys_super_stop(long super_id) {
    return __syscall1(SYS_SUPER_STOP, super_id);
}

long sys_pipe2(long *rfd_ptr, long *wfd_ptr, long flags) {
    return __syscall3(SYS_PIPE2, (long)rfd_ptr, (long)wfd_ptr, flags);
}

long sys_execve(const char *path, const char **argv) {
    return __syscall2(SYS_EXECVE, (long)path, (long)argv);
}

long sys_topic_create(const char *name, unsigned long ns_id) {
    return __syscall2(SYS_TOPIC_CREATE, (long)name, (long)ns_id);
}

long sys_topic_subscribe(unsigned long topic_id) {
    return __syscall1(SYS_TOPIC_SUB, (long)topic_id);
}

long sys_topic_publish(unsigned long topic_id, const void *buf, unsigned long len) {
    return __syscall3(SYS_TOPIC_PUB, (long)topic_id, (long)buf, (long)len);
}

long sys_topic_recv(unsigned long topic_id, void *buf, unsigned long max_len, unsigned long *pub_pid_ptr) {
    return __syscall4(SYS_TOPIC_RECV, (long)topic_id, (long)buf, (long)max_len, (long)pub_pid_ptr);
}

long sys_environ(void *buf, unsigned long buf_size) {
    return __syscall2(SYS_ENVIRON, (long)buf, (long)buf_size);
}

long sys_chown(const char *path, long uid, long gid) {
    return __syscall3(SYS_CHOWN, (long)path, uid, gid);
}

long sys_fchown(long fd, long uid, long gid) {
    return __syscall3(SYS_FCHOWN, fd, uid, gid);
}

long sys_umask(long mask) {
    return __syscall1(SYS_UMASK, mask);
}

long sys_geteuid(void) {
    return __syscall0(SYS_GETEUID);
}

long sys_getegid(void) {
    return __syscall0(SYS_GETEGID);
}

long sys_getgroups(long max_count, void *buf) {
    return __syscall2(SYS_GETGROUPS, (long)buf, max_count);
}

long sys_setgroups(long count, const void *buf) {
    return __syscall2(SYS_SETGROUPS, (long)buf, count);
}

long sys_symlink(const char *target, const char *path) {
    return __syscall2(SYS_SYMLINK, (long)target, (long)path);
}

long sys_readlink(const char *path, char *buf, unsigned long bufsize) {
    return __syscall3(SYS_READLINK, (long)path, (long)buf, (long)bufsize);
}

long sys_setsid(void) {
    return __syscall0(SYS_SETSID);
}

long sys_getsid(long pid) {
    return __syscall1(SYS_GETSID, pid);
}

long sys_tcsetpgrp(long fd, long pgrp) {
    return __syscall2(SYS_TCSETPGRP, fd, pgrp);
}

long sys_tcgetpgrp(long fd) {
    return __syscall1(SYS_TCGETPGRP, fd);
}

long sys_mkfifo(const char *path) {
    return __syscall1(SYS_MKFIFO, (long)path);
}
