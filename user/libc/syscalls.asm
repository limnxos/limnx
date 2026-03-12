; Limnx — C-callable syscall wrappers
; System V AMD64 ABI: args in rdi, rsi, rdx, rcx, r8, r9
; SYSCALL convention:  args in rdi, rsi, rdx, r10, r8 (rcx clobbered by syscall)
; Syscall number in rax, return value in rax.

section .text
bits 64

; --- sys_write(buf, len) ---
global sys_write
sys_write:
    mov rax, 0          ; SYS_WRITE
    syscall
    ret

; --- sys_yield() ---
global sys_yield
sys_yield:
    mov rax, 1          ; SYS_YIELD
    syscall
    ret

; --- sys_exit(status) ---
global sys_exit
sys_exit:
    mov rax, 2          ; SYS_EXIT
    syscall
    cli
    hlt
    jmp $

; --- sys_open(path, flags) ---
global sys_open
sys_open:
    mov rax, 3          ; SYS_OPEN
    syscall
    ret

; --- sys_read(fd, buf, len) ---
global sys_read
sys_read:
    mov rax, 4          ; SYS_READ
    syscall
    ret

; --- sys_close(fd) ---
global sys_close
sys_close:
    mov rax, 5          ; SYS_CLOSE
    syscall
    ret

; --- sys_stat(path, stat_buf) ---
global sys_stat
sys_stat:
    mov rax, 6          ; SYS_STAT
    syscall
    ret

; --- sys_exec(path) ---
global sys_exec
sys_exec:
    mov rax, 7          ; SYS_EXEC
    syscall
    ret

; --- sys_socket() ---
global sys_socket
sys_socket:
    mov rax, 8          ; SYS_SOCKET
    syscall
    ret

; --- sys_bind(sockfd, port) ---
global sys_bind
sys_bind:
    mov rax, 9          ; SYS_BIND
    syscall
    ret

; --- sys_sendto(sockfd, buf, len, dst_ip, dst_port) ---
; 5 args: rdi, rsi, rdx, rcx→r10, r8
global sys_sendto
sys_sendto:
    mov r10, rcx        ; move 4th arg from rcx to r10
    mov rax, 10         ; SYS_SENDTO
    syscall
    ret

; --- sys_recvfrom(sockfd, buf, len, src_ip_ptr, src_port_ptr) ---
; 5 args: rdi, rsi, rdx, rcx→r10, r8
global sys_recvfrom
sys_recvfrom:
    mov r10, rcx        ; move 4th arg from rcx to r10
    mov rax, 11         ; SYS_RECVFROM
    syscall
    ret

; --- sys_fwrite(fd, buf, len) ---
global sys_fwrite
sys_fwrite:
    mov rax, 12         ; SYS_FWRITE
    syscall
    ret

; --- sys_create(path) ---
global sys_create
sys_create:
    mov rax, 13         ; SYS_CREATE
    syscall
    ret

; --- sys_unlink(path) ---
global sys_unlink
sys_unlink:
    mov rax, 14         ; SYS_UNLINK
    syscall
    ret

; --- sys_mmap(num_pages) ---
global sys_mmap
sys_mmap:
    mov rax, 15         ; SYS_MMAP
    syscall
    ret

; --- sys_munmap(virt_addr) ---
global sys_munmap
sys_munmap:
    mov rax, 16         ; SYS_MUNMAP
    syscall
    ret

; --- sys_getchar() ---
global sys_getchar
sys_getchar:
    mov rax, 17         ; SYS_GETCHAR
    syscall
    ret

; --- sys_waitpid(pid) ---
; Note: zero RSI (flags) for backward compatibility
global sys_waitpid
sys_waitpid:
    xor esi, esi        ; flags = 0 (blocking wait)
    mov rax, 18         ; SYS_WAITPID
    syscall
    ret

; --- sys_waitpid_flags(pid, flags) ---
global sys_waitpid_flags
sys_waitpid_flags:
    mov rax, 18         ; SYS_WAITPID
    syscall
    ret

; --- sys_pipe(rfd_ptr, wfd_ptr) ---
global sys_pipe
sys_pipe:
    mov rax, 19         ; SYS_PIPE
    syscall
    ret

; --- sys_getpid() ---
global sys_getpid
sys_getpid:
    mov rax, 20         ; SYS_GETPID
    syscall
    ret

; --- sys_fmmap(fd) ---
global sys_fmmap
sys_fmmap:
    mov rax, 21         ; SYS_FMMAP
    syscall
    ret

; --- sys_readdir(dir_path, index, dirent_ptr) ---
global sys_readdir
sys_readdir:
    mov rax, 22         ; SYS_READDIR
    syscall
    ret

; --- sys_mkdir(path) ---
global sys_mkdir
sys_mkdir:
    mov rax, 23         ; SYS_MKDIR
    syscall
    ret

; --- sys_seek(fd, offset, whence) ---
global sys_seek
sys_seek:
    mov rax, 24         ; SYS_SEEK
    syscall
    ret

; --- sys_truncate(path, new_size) ---
global sys_truncate
sys_truncate:
    mov rax, 25         ; SYS_TRUNCATE
    syscall
    ret

; --- sys_chdir(path) ---
global sys_chdir
sys_chdir:
    mov rax, 26         ; SYS_CHDIR
    syscall
    ret

; --- sys_getcwd(buf, size) ---
global sys_getcwd
sys_getcwd:
    mov rax, 27         ; SYS_GETCWD
    syscall
    ret

; --- sys_fstat(fd, stat_buf) ---
global sys_fstat
sys_fstat:
    mov rax, 28         ; SYS_FSTAT
    syscall
    ret

; --- sys_rename(old_path, new_path) ---
global sys_rename
sys_rename:
    mov rax, 29         ; SYS_RENAME
    syscall
    ret

; --- sys_dup(fd) ---
global sys_dup
sys_dup:
    mov rax, 30         ; SYS_DUP
    syscall
    ret

; --- sys_dup2(oldfd, newfd) ---
global sys_dup2
sys_dup2:
    mov rax, 31         ; SYS_DUP2
    syscall
    ret

; --- sys_kill(pid, signal) ---
global sys_kill
sys_kill:
    mov rax, 32         ; SYS_KILL
    syscall
    ret

; --- sys_fcntl(fd, cmd, arg) ---
global sys_fcntl
sys_fcntl:
    mov rax, 33         ; SYS_FCNTL
    syscall
    ret

; --- sys_setpgid(pid, pgid) ---
global sys_setpgid
sys_setpgid:
    mov rax, 34         ; SYS_SETPGID
    syscall
    ret

; --- sys_getpgid(pid) ---
global sys_getpgid
sys_getpgid:
    mov rax, 35         ; SYS_GETPGID
    syscall
    ret

; --- sys_chmod(path, mode) ---
global sys_chmod
sys_chmod:
    mov rax, 36         ; SYS_CHMOD
    syscall
    ret

; --- sys_shmget(key, num_pages) ---
global sys_shmget
sys_shmget:
    mov rax, 37         ; SYS_SHMGET
    syscall
    ret

; --- sys_shmat(shmid) ---
global sys_shmat
sys_shmat:
    mov rax, 38         ; SYS_SHMAT
    syscall
    ret

; --- sys_shmdt(virt_addr) ---
global sys_shmdt
sys_shmdt:
    mov rax, 39         ; SYS_SHMDT
    syscall
    ret

; --- sys_fork() ---
global sys_fork
sys_fork:
    mov rax, 40         ; SYS_FORK
    syscall
    ret

; --- sys_sigaction(signum, handler) ---
global sys_sigaction
sys_sigaction:
    mov rax, 41         ; SYS_SIGACTION
    syscall
    ret

; --- sys_sigreturn() ---
global sys_sigreturn
sys_sigreturn:
    mov rax, 42         ; SYS_SIGRETURN
    syscall
    ret

; --- sys_openpty(master_fd, slave_fd) ---
global sys_openpty
sys_openpty:
    mov rax, 43         ; SYS_OPENPTY
    syscall
    ret

; --- sys_tcp_socket() ---
global sys_tcp_socket
sys_tcp_socket:
    mov rax, 44         ; SYS_TCP_SOCKET
    syscall
    ret

; --- sys_tcp_connect(conn, ip, port) ---
global sys_tcp_connect
sys_tcp_connect:
    mov rax, 45         ; SYS_TCP_CONNECT
    syscall
    ret

; --- sys_tcp_listen(conn, port) ---
global sys_tcp_listen
sys_tcp_listen:
    mov rax, 46         ; SYS_TCP_LISTEN
    syscall
    ret

; --- sys_tcp_accept(listen_conn) ---
global sys_tcp_accept
sys_tcp_accept:
    mov rax, 47         ; SYS_TCP_ACCEPT
    syscall
    ret

; --- sys_tcp_send(conn, buf, len) ---
global sys_tcp_send
sys_tcp_send:
    mov rax, 48         ; SYS_TCP_SEND
    syscall
    ret

; --- sys_tcp_recv(conn, buf, len) ---
global sys_tcp_recv
sys_tcp_recv:
    mov rax, 49         ; SYS_TCP_RECV
    syscall
    ret

; --- sys_tcp_close(conn) ---
global sys_tcp_close
sys_tcp_close:
    mov rax, 50         ; SYS_TCP_CLOSE
    syscall
    ret

; --- sys_ioctl(fd, cmd, arg) ---
global sys_ioctl
sys_ioctl:
    mov rax, 51         ; SYS_IOCTL
    syscall
    ret

; --- sys_clock_gettime(clockid, timespec_ptr) ---
global sys_clock_gettime
sys_clock_gettime:
    mov rax, 52         ; SYS_CLOCK_GETTIME
    syscall
    ret

; --- sys_nanosleep(timespec_ptr) ---
global sys_nanosleep
sys_nanosleep:
    mov rax, 53         ; SYS_NANOSLEEP
    syscall
    ret

; --- sys_getenv(key, val_buf, val_buf_size) ---
global sys_getenv
sys_getenv:
    mov rax, 54         ; SYS_GETENV
    syscall
    ret

; --- sys_setenv(key, value) ---
global sys_setenv
sys_setenv:
    mov rax, 55         ; SYS_SETENV
    syscall
    ret

; --- sys_poll(fds, nfds, timeout_ms) ---
global sys_poll
sys_poll:
    mov rax, 56         ; SYS_POLL
    syscall
    ret

; --- sys_getuid() ---
global sys_getuid
sys_getuid:
    mov rax, 57         ; SYS_GETUID
    syscall
    ret

; --- sys_setuid(uid) ---
global sys_setuid
sys_setuid:
    mov rax, 58         ; SYS_SETUID
    syscall
    ret

; --- sys_getgid() ---
global sys_getgid
sys_getgid:
    mov rax, 59         ; SYS_GETGID
    syscall
    ret

; --- sys_setgid(gid) ---
global sys_setgid
sys_setgid:
    mov rax, 60         ; SYS_SETGID
    syscall
    ret

; --- sys_getcap() ---
global sys_getcap
sys_getcap:
    mov rax, 61         ; SYS_GETCAP
    syscall
    ret

; --- sys_setcap(pid, caps) ---
global sys_setcap
sys_setcap:
    mov rax, 62         ; SYS_SETCAP
    syscall
    ret

; --- sys_getrlimit(resource, rlimit_ptr) ---
global sys_getrlimit
sys_getrlimit:
    mov rax, 63         ; SYS_GETRLIMIT
    syscall
    ret

; --- sys_setrlimit(resource, rlimit_ptr) ---
global sys_setrlimit
sys_setrlimit:
    mov rax, 64         ; SYS_SETRLIMIT
    syscall
    ret

; --- sys_seccomp(mask, strict) ---
global sys_seccomp
sys_seccomp:
    mov rax, 65         ; SYS_SECCOMP
    syscall
    ret

; --- sys_setaudit(pid, flags) ---
global sys_setaudit
sys_setaudit:
    mov rax, 66         ; SYS_SETAUDIT
    syscall
    ret

; --- sys_unix_socket() ---
global sys_unix_socket
sys_unix_socket:
    mov rax, 67         ; SYS_UNIX_SOCKET
    syscall
    ret

; --- sys_unix_bind(fd, path) ---
global sys_unix_bind
sys_unix_bind:
    mov rax, 68         ; SYS_UNIX_BIND
    syscall
    ret

; --- sys_unix_listen(fd) ---
global sys_unix_listen
sys_unix_listen:
    mov rax, 69         ; SYS_UNIX_LISTEN
    syscall
    ret

; --- sys_unix_accept(fd) ---
global sys_unix_accept
sys_unix_accept:
    mov rax, 70         ; SYS_UNIX_ACCEPT
    syscall
    ret

; --- sys_unix_connect(path) ---
global sys_unix_connect
sys_unix_connect:
    mov rax, 71         ; SYS_UNIX_CONNECT
    syscall
    ret

; --- sys_agent_register(name) ---
global sys_agent_register
sys_agent_register:
    mov rax, 72         ; SYS_AGENT_REGISTER
    syscall
    ret

; --- sys_agent_lookup(name, pid_out) ---
global sys_agent_lookup
sys_agent_lookup:
    mov rax, 73         ; SYS_AGENT_LOOKUP
    syscall
    ret

; --- sys_eventfd(flags) ---
global sys_eventfd
sys_eventfd:
    mov rax, 74         ; SYS_EVENTFD
    syscall
    ret

; --- sys_epoll_create(flags) ---
global sys_epoll_create
sys_epoll_create:
    mov rax, 75         ; SYS_EPOLL_CREATE
    syscall
    ret

; --- sys_epoll_ctl(epfd, op, fd, event) ---
; 4 args: rdi, rsi, rdx, rcx→r10
global sys_epoll_ctl
sys_epoll_ctl:
    mov r10, rcx        ; move 4th arg from rcx to r10
    mov rax, 76         ; SYS_EPOLL_CTL
    syscall
    ret

; --- sys_epoll_wait(epfd, events, max_events, timeout_ms) ---
; 4 args: rdi, rsi, rdx, rcx→r10
global sys_epoll_wait
sys_epoll_wait:
    mov r10, rcx        ; move 4th arg from rcx to r10
    mov rax, 77         ; SYS_EPOLL_WAIT
    syscall
    ret

; --- sys_swap_stat(stat_ptr) ---
global sys_swap_stat
sys_swap_stat:
    mov rax, 78         ; SYS_SWAP_STAT
    syscall
    ret

; --- sys_infer_register(name, sock_path) ---
global sys_infer_register
sys_infer_register:
    mov rax, 79         ; SYS_INFER_REGISTER
    syscall
    ret

; --- sys_infer_request(name, req_buf, req_len, resp_buf, resp_len) ---
; 5 args: rdi, rsi, rdx, rcx→r10, r8
global sys_infer_request
sys_infer_request:
    mov r10, rcx        ; move 4th arg from rcx to r10
    mov rax, 80         ; SYS_INFER_REQUEST
    syscall
    ret

; --- sys_uring_setup(entries, params) ---
global sys_uring_setup
sys_uring_setup:
    mov rax, 81         ; SYS_URING_SETUP
    syscall
    ret

; --- sys_uring_enter(uring_fd, sqe_ptr, count, cqe_ptr) ---
; 4 args: rdi, rsi, rdx, rcx→r10
global sys_uring_enter
sys_uring_enter:
    mov r10, rcx        ; move 4th arg from rcx to r10
    mov rax, 82         ; SYS_URING_ENTER
    syscall
    ret

; --- sys_mmap2(num_pages, flags) ---
global sys_mmap2
sys_mmap2:
    mov rax, 83         ; SYS_MMAP2
    syscall
    ret

; --- sys_token_create(perms, target_pid, resource) ---
global sys_token_create
sys_token_create:
    mov rax, 84         ; SYS_TOKEN_CREATE
    mov r10, rcx
    syscall
    ret

; --- sys_token_revoke(token_id) ---
global sys_token_revoke
sys_token_revoke:
    mov rax, 85         ; SYS_TOKEN_REVOKE
    syscall
    ret

; --- sys_token_list(buf, max_count) ---
global sys_token_list
sys_token_list:
    mov rax, 86         ; SYS_TOKEN_LIST
    syscall
    ret

; --- sys_ns_create(name) ---
global sys_ns_create
sys_ns_create:
    mov rax, 87         ; SYS_NS_CREATE
    syscall
    ret

; --- sys_ns_join(ns_id) ---
global sys_ns_join
sys_ns_join:
    mov rax, 88         ; SYS_NS_JOIN
    syscall
    ret

; --- sys_procinfo(index, info_ptr) ---
global sys_procinfo
sys_procinfo:
    mov rax, 89         ; SYS_PROCINFO
    syscall
    ret

; --- sys_fsstat(stat_ptr) ---
global sys_fsstat
sys_fsstat:
    mov rax, 90         ; SYS_FSSTAT
    syscall
    ret

; --- sys_task_create(name, ns_id) ---
global sys_task_create
sys_task_create:
    mov rax, 91         ; SYS_TASK_CREATE
    syscall
    ret

; --- sys_task_depend(task_id, dep_id) ---
global sys_task_depend
sys_task_depend:
    mov rax, 92         ; SYS_TASK_DEPEND
    syscall
    ret

; --- sys_task_start(task_id) ---
global sys_task_start
sys_task_start:
    mov rax, 93         ; SYS_TASK_START
    syscall
    ret

; --- sys_task_complete(task_id, result) ---
global sys_task_complete
sys_task_complete:
    mov rax, 94         ; SYS_TASK_COMPLETE
    syscall
    ret

; --- sys_task_status(task_id, buf_ptr) ---
global sys_task_status
sys_task_status:
    mov rax, 95         ; SYS_TASK_STATUS
    syscall
    ret

; --- sys_task_wait(task_id) ---
global sys_task_wait
sys_task_wait:
    mov rax, 96         ; SYS_TASK_WAIT
    syscall
    ret

; --- sys_token_delegate(parent_id, target_pid, perms, resource) ---
global sys_token_delegate
sys_token_delegate:
    mov r10, rcx        ; 4th arg: rcx → r10 (syscall clobbers rcx)
    mov rax, 97         ; SYS_TOKEN_DELEGATE
    syscall
    ret

; --- sys_ns_setquota(ns_id, resource, limit) ---
global sys_ns_setquota
sys_ns_setquota:
    mov rax, 98         ; SYS_NS_SETQUOTA
    syscall
    ret

; --- sys_infer_health(load) ---
global sys_infer_health
sys_infer_health:
    mov rax, 99         ; SYS_INFER_HEALTH
    syscall
    ret

; --- sys_infer_route(name) ---
global sys_infer_route
sys_infer_route:
    mov rax, 100        ; SYS_INFER_ROUTE
    syscall
    ret

; --- sys_agent_send(name, msg_buf, msg_len, token_id) ---
global sys_agent_send
sys_agent_send:
    mov r10, rcx        ; 4th arg: rcx → r10
    mov rax, 101        ; SYS_AGENT_SEND
    syscall
    ret

; --- sys_agent_recv(msg_buf, msg_len, sender_pid_ptr, token_id_ptr) ---
global sys_agent_recv
sys_agent_recv:
    mov r10, rcx        ; 4th arg: rcx → r10
    mov rax, 102        ; SYS_AGENT_RECV
    syscall
    ret

; --- sys_futex_wait(addr, expected) ---
global sys_futex_wait
sys_futex_wait:
    mov rax, 103        ; SYS_FUTEX_WAIT
    syscall
    ret

; --- sys_futex_wake(addr, max_wake) ---
global sys_futex_wake
sys_futex_wake:
    mov rax, 104        ; SYS_FUTEX_WAKE
    syscall
    ret

; --- sys_mmap_file(fd, offset, num_pages) ---
global sys_mmap_file
sys_mmap_file:
    mov rax, 105        ; SYS_MMAP_FILE
    syscall
    ret

; --- sys_mprotect(virt_addr, num_pages, prot) ---
global sys_mprotect
sys_mprotect:
    mov rax, 106        ; SYS_MPROTECT
    syscall
    ret

; --- sys_mmap_guard(num_pages) ---
global sys_mmap_guard
sys_mmap_guard:
    mov rax, 107        ; SYS_MMAP_GUARD
    syscall
    ret

; --- sys_sigaction3(signum, handler, flags) ---
global sys_sigaction3
sys_sigaction3:
    mov rax, 41         ; SYS_SIGACTION (same syscall, 3rd arg = flags)
    syscall
    ret

; --- sys_sigprocmask(how, new_mask, old_mask_ptr) ---
global sys_sigprocmask
sys_sigprocmask:
    mov rax, 108        ; SYS_SIGPROCMASK
    syscall
    ret

; --- sys_arch_prctl(code, addr) ---
global sys_arch_prctl
sys_arch_prctl:
    mov rax, 109        ; SYS_ARCH_PRCTL
    syscall
    ret

; --- sys_select(nfds, readfds, writefds, timeout_us) ---
global sys_select
sys_select:
    mov r10, rcx        ; 4th arg: rcx → r10
    mov rax, 110        ; SYS_SELECT
    syscall
    ret

; --- sys_super_create(name) ---
global sys_super_create
sys_super_create:
    mov rax, 111        ; SYS_SUPER_CREATE
    syscall
    ret

; --- sys_super_add(super_id, elf_path, ns_id, caps) ---
global sys_super_add
sys_super_add:
    mov r10, rcx        ; 4th arg: rcx → r10
    mov rax, 112        ; SYS_SUPER_ADD
    syscall
    ret

; --- sys_super_set_policy(super_id, policy) ---
global sys_super_set_policy
sys_super_set_policy:
    mov rax, 113        ; SYS_SUPER_SET_POLICY
    syscall
    ret

; --- sys_pipe2(rfd_ptr, wfd_ptr, flags) ---
global sys_pipe2
sys_pipe2:
    mov rax, 114        ; SYS_PIPE2
    syscall
    ret

; --- sys_super_start(super_id) ---
global sys_super_start
sys_super_start:
    mov rax, 115        ; SYS_SUPER_START
    syscall
    ret

; --- sys_tcp_setopt(conn, opt, value) ---
global sys_tcp_setopt
sys_tcp_setopt:
    mov rax, 116        ; SYS_TCP_SETOPT
    syscall
    ret

; --- sys_tcp_to_fd(conn) ---
global sys_tcp_to_fd
sys_tcp_to_fd:
    mov rax, 117        ; SYS_TCP_TO_FD
    syscall
    ret

; --- sys_infer_set_policy(policy) ---
global sys_infer_set_policy
sys_infer_set_policy:
    mov rax, 118        ; SYS_INFER_SET_POLICY
    syscall
    ret

; --- sys_infer_queue_stat(stat_ptr) ---
global sys_infer_queue_stat
sys_infer_queue_stat:
    mov rax, 119        ; SYS_INFER_QUEUE_STAT
    syscall
    ret

; --- sys_infer_cache_ctrl(cmd, arg) ---
global sys_infer_cache_ctrl
sys_infer_cache_ctrl:
    mov rax, 120        ; SYS_INFER_CACHE_CTRL
    syscall
    ret

; --- sys_infer_submit(name, req_buf, req_len, eventfd_idx) ---
; 4 args: rdi, rsi, rdx, rcx→r10
global sys_infer_submit
sys_infer_submit:
    mov r10, rcx        ; 4th arg
    mov rax, 121        ; SYS_INFER_SUBMIT
    syscall
    ret

; --- sys_infer_poll(request_id) ---
global sys_infer_poll
sys_infer_poll:
    mov rax, 122        ; SYS_INFER_POLL
    syscall
    ret

; --- sys_infer_result(request_id, resp_buf, resp_len) ---
global sys_infer_result
sys_infer_result:
    mov rax, 123        ; SYS_INFER_RESULT
    syscall
    ret

; --- sys_execve(path, argv) ---
; True exec: replaces current process image. Only returns on error.
global sys_execve
sys_execve:
    mov rax, 124        ; SYS_EXECVE
    syscall
    ret

; --- sys_topic_create(name, ns_id) ---
global sys_topic_create
sys_topic_create:
    mov rax, 125        ; SYS_TOPIC_CREATE
    syscall
    ret

; --- sys_topic_subscribe(topic_id) ---
global sys_topic_subscribe
sys_topic_subscribe:
    mov rax, 126        ; SYS_TOPIC_SUB
    syscall
    ret

; --- sys_topic_publish(topic_id, buf, len) ---
global sys_topic_publish
sys_topic_publish:
    mov rax, 127        ; SYS_TOPIC_PUB
    syscall
    ret

; --- sys_topic_recv(topic_id, buf, max_len, pub_pid_ptr) ---
; 4 args: rdi, rsi, rdx, rcx→r10
global sys_topic_recv
sys_topic_recv:
    mov r10, rcx        ; 4th arg
    mov rax, 128        ; SYS_TOPIC_RECV
    syscall
    ret
