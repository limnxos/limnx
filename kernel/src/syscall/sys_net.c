#include "syscall/syscall_internal.h"
#include "net/net.h"
#include "net/tcp.h"
#include "sched/thread.h"

int64_t sys_socket(uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return net_socket();
}

int64_t sys_bind(uint64_t sockfd, uint64_t port,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    /* Check CAP_NET_BIND for privileged ports */
    if (port < 1024) {
        thread_t *t = thread_get_current();
        if (t && t->process && !(t->process->capabilities & CAP_NET_BIND))
            return -EACCES;
    }
    return net_bind((int)sockfd, (uint16_t)port);
}

int64_t sys_sendto(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                           uint64_t dst_ip, uint64_t dst_port) {
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -1;
    return net_sendto((int)sockfd, (const void *)buf_ptr, (uint32_t)len,
                      (uint32_t)dst_ip, (uint16_t)dst_port);
}

int64_t sys_recvfrom(uint64_t sockfd, uint64_t buf_ptr, uint64_t len,
                             uint64_t src_ip_ptr, uint64_t src_port_ptr) {
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -1;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int ret = net_recvfrom((int)sockfd, (void *)buf_ptr, (uint32_t)len,
                           &src_ip, &src_port);
    if (ret >= 0) {
        if (src_ip_ptr && validate_user_ptr(src_ip_ptr, 4) == 0)
            *(uint32_t *)src_ip_ptr = src_ip;
        if (src_port_ptr && validate_user_ptr(src_port_ptr, 2) == 0)
            *(uint16_t *)src_port_ptr = src_port;
    }
    return ret;
}

int64_t sys_tcp_socket(uint64_t a1, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    int64_t idx = tcp_socket();
    if (idx >= 0 && idx < 8) {
        thread_t *t = thread_get_current();
        if (t && t->process)
            t->process->tcp_conns[idx] = 1;
    }
    return idx;
}

int64_t sys_tcp_connect(uint64_t conn_idx, uint64_t ip, uint64_t port,
                                 uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    return tcp_connect((int)conn_idx, (uint32_t)ip, (uint16_t)port);
}

int64_t sys_tcp_listen(uint64_t conn_idx, uint64_t port,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    return tcp_listen((int)conn_idx, (uint16_t)port);
}

int64_t sys_tcp_accept(uint64_t listen_conn, uint64_t a2,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    int64_t idx = tcp_accept((int)listen_conn);
    if (idx >= 0 && idx < 8) {
        thread_t *t = thread_get_current();
        if (t && t->process)
            t->process->tcp_conns[idx] = 1;
    }
    return idx;
}

int64_t sys_tcp_send(uint64_t conn_idx, uint64_t buf_ptr, uint64_t len,
                              uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -1;
    return tcp_send((int)conn_idx, (const uint8_t *)buf_ptr, (uint32_t)len);
}

int64_t sys_tcp_recv(uint64_t conn_idx, uint64_t buf_ptr, uint64_t len,
                              uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (len > 0 && validate_user_ptr(buf_ptr, len) != 0)
        return -1;
    return tcp_recv((int)conn_idx, (uint8_t *)buf_ptr, (uint32_t)len);
}

int64_t sys_tcp_close(uint64_t conn_idx, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    int64_t r = tcp_close((int)conn_idx);
    if (r == 0 && conn_idx < 8) {
        thread_t *t = thread_get_current();
        if (t && t->process)
            t->process->tcp_conns[conn_idx] = 0;
    }
    return r;
}

int64_t sys_tcp_setopt(uint64_t conn_idx, uint64_t opt, uint64_t value,
                               uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    /* opt 1 = O_NONBLOCK */
    if (opt == 1)
        return tcp_set_nonblock((int)conn_idx, (int)value);
    return -EINVAL;
}

int64_t sys_tcp_to_fd(uint64_t conn_idx, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (conn_idx >= MAX_TCP_CONNS) return -EINVAL;
    thread_t *t = thread_get_current();
    process_t *proc = t ? t->process : NULL;
    if (!proc) return -1;
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (fd_is_free(&proc->fd_table[fd])) {
            fd_entry_t *e = &proc->fd_table[fd];
            e->tcp_conn_idx = (int16_t)conn_idx;
            e->open_flags = O_RDWR;
            e->fd_flags = 0;
            return fd;
        }
    }
    return -EMFILE;
}
