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

static void test_multi_namespace(void) {
    /* Create two isolated namespaces with their own pub/sub topics */
    long ns1 = sys_ns_create("test_ns1");
    long ns2 = sys_ns_create("test_ns2");
    lt_ok(ns1 >= 0, "ns_create ns1");
    lt_ok(ns2 >= 0, "ns_create ns2");
    if (ns1 < 0 || ns2 < 0) return;

    /* Verify different IDs */
    lt_ok(ns1 != ns2, "namespaces have different IDs");

    /* Create topic with same name in each namespace */
    long t1 = sys_topic_create("events", ns1);
    long t2 = sys_topic_create("events", ns2);
    lt_ok(t1 >= 0, "topic in ns1");
    lt_ok(t2 >= 0, "topic in ns2");
    lt_ok(t1 != t2, "topics have different IDs");

    /* Subscribe to both */
    sys_topic_subscribe(t1);
    sys_topic_subscribe(t2);

    /* Publish to ns1 topic only */
    sys_topic_publish(t1, "msg_ns1", 7);

    /* Should receive from ns1 */
    char buf[32];
    unsigned long sender = 0;
    long n = sys_topic_recv(t1, buf, sizeof(buf) - 1, &sender);
    lt_ok(n == 7, "recv from ns1 topic");

    /* Should NOT receive from ns2 (nothing published there) */
    n = sys_topic_recv(t2, buf, sizeof(buf) - 1, &sender);
    lt_ok(n <= 0, "ns2 topic empty (isolated)");

    /* Publish to ns2 and verify */
    sys_topic_publish(t2, "msg_ns2", 7);
    n = sys_topic_recv(t2, buf, sizeof(buf) - 1, &sender);
    lt_ok(n == 7, "recv from ns2 topic");
}

int main(void) {
    lt_suite("ipc");
    test_pipe();
    test_fifo();
    test_eventfd();
    test_epoll();
    test_unix_socket();
    test_pipe2();
    test_multi_namespace();
    return lt_done();
}
