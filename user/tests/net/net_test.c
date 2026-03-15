/*
 * net_test.c — Network tests
 * Tests: UDP socket, bind, TCP basics
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

static void test_udp_socket(void) {
    long fd = sys_socket();
    lt_ok(fd >= 0, "UDP socket create");
    if (fd >= 0) {
        long ret = sys_bind(fd, 9999);
        lt_ok(ret == 0, "UDP bind port 9999");
    } else {
        lt_ok(0, "UDP bind");
    }
}

static void test_tcp_socket(void) {
    long fd = sys_tcp_socket();
    lt_ok(fd >= 0, "TCP socket create");
}

static void test_tcp_listen_accept(void) {
    long listen_fd = sys_tcp_socket();
    lt_ok(listen_fd >= 0, "TCP listen socket");
    if (listen_fd >= 0) {
        long ret = sys_tcp_listen(listen_fd, 8080);
        lt_ok(ret == 0, "TCP listen on port 8080");
        /* Can't test accept without a client connecting —
         * just verify the listen setup works */
        sys_tcp_close(listen_fd);
    } else {
        lt_ok(0, "TCP listen on port 8080");
    }
}

static void test_clock_gettime(void) {
    long ts[2] = {0, 0};
    long ret = sys_clock_gettime(1, ts);  /* CLOCK_MONOTONIC */
    lt_ok(ret == 0, "clock_gettime succeeds");
    lt_ok(ts[0] >= 0, "time is non-negative");
}

static void test_nanosleep(void) {
    long ts[2] = {0, 10000000};  /* 10ms */
    long ret = sys_nanosleep(ts);
    lt_ok(ret == 0, "nanosleep returns 0");
}

int main(void) {
    lt_suite("net");
    test_udp_socket();
    test_tcp_socket();
    test_tcp_listen_accept();
    test_clock_gettime();
    test_nanosleep();
    return lt_done();
}
