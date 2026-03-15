/*
 * ipc_test.c — IPC tests
 * Tests: pipe, FIFO, eventfd, epoll, unix socket
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

static void test_pipe(void) {
    long rfd, wfd;
    long ret = sys_pipe(&rfd, &wfd);
    lt_ok(ret == 0, "pipe creation");
    sys_fwrite(wfd, "pipe!", 5);
    char buf[16];
    long n = sys_read(rfd, buf, 15);
    if (n > 0) buf[n] = '\0';
    lt_ok(n == 5 && strcmp(buf, "pipe!") == 0, "pipe data correct");
    sys_close(rfd);
    sys_close(wfd);
}

static void test_fifo(void) {
    long ret = sys_mkfifo("/ipc_test_fifo");
    lt_ok(ret == 0, "mkfifo");
    long wfd = sys_open("/ipc_test_fifo", 1);
    lt_ok(wfd >= 0, "FIFO open write");
    if (wfd >= 0) {
        sys_fwrite(wfd, "fifo", 4);
        long rfd = sys_open("/ipc_test_fifo", 0);
        lt_ok(rfd >= 0, "FIFO open read");
        if (rfd >= 0) {
            char buf[16];
            long n = sys_read(rfd, buf, 15);
            if (n > 0) buf[n] = '\0';
            lt_ok(n == 4 && strcmp(buf, "fifo") == 0, "FIFO data correct");
            sys_close(rfd);
        } else {
            lt_ok(0, "FIFO data correct");
        }
        sys_close(wfd);
    } else {
        lt_ok(0, "FIFO open read");
        lt_ok(0, "FIFO data correct");
    }
}

static void test_eventfd(void) {
    long efd = sys_eventfd(0);
    lt_ok(efd >= 0, "eventfd create");
    if (efd >= 0) sys_close(efd);
}

static void test_epoll(void) {
    long epfd = sys_epoll_create(0);
    lt_ok(epfd >= 0, "epoll_create");
    if (epfd >= 0) sys_close(epfd);
}

static void test_unix_socket(void) {
    long fd = sys_unix_socket();
    lt_ok(fd >= 0, "unix socket create");
    if (fd >= 0) {
        long ret = sys_unix_bind(fd, "/ipc_test.sock");
        lt_ok(ret == 0, "unix socket bind");
        ret = sys_unix_listen(fd);
        lt_ok(ret == 0, "unix socket listen");
        sys_close(fd);
    } else {
        lt_ok(0, "unix socket bind");
        lt_ok(0, "unix socket listen");
    }
}

static void test_pipe2(void) {
    long rfd, wfd;
    long ret = sys_pipe2(&rfd, &wfd, 0);
    lt_ok(ret == 0, "pipe2 creation");
    if (ret == 0) {
        sys_fwrite(wfd, "p2", 2);
        char buf[8];
        long n = sys_read(rfd, buf, 7);
        lt_ok(n == 2, "pipe2 data transfer");
        sys_close(rfd);
        sys_close(wfd);
    } else {
        lt_ok(0, "pipe2 data transfer");
    }
}

int main(void) {
    lt_suite("ipc");
    test_pipe();
    test_fifo();
    test_eventfd();
    test_epoll();
    test_unix_socket();
    test_pipe2();
    return lt_done();
}
