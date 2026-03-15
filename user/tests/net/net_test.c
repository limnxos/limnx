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

int main(void) {
    lt_suite("net");
    test_udp_socket();
    test_tcp_socket();
    return lt_done();
}
