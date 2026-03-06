#include "libc/libc.h"

static int tests_passed = 0;
static int tests_total = 0;

static void check(int ok, const char *name) {
    tests_total++;
    if (ok) {
        tests_passed++;
        printf("  [PASS] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
    }
}

/* ====== Loopback Tests ====== */

/* Our IP: 10.0.2.15 = 0x0A00020F */
#define OUR_IP 0x0A00020FU

static void test_udp_loopback(void) {
    long s1 = sys_socket();
    long s2 = sys_socket();
    if (s1 < 0 || s2 < 0) {
        check(0, "1: UDP loopback (socket alloc failed)");
        return;
    }

    sys_bind(s1, 7001);
    sys_bind(s2, 7002);

    /* Send from s2 to s1's port on our own IP */
    const char *msg = "loopback!";
    long r = sys_sendto(s2, msg, 9, OUR_IP, 7001);
    check(r >= 0, "1: UDP loopback send");

    /* Receive on s1 */
    char buf[64];
    unsigned int src_ip = 0;
    unsigned short src_port = 0;
    long n = sys_recvfrom(s1, buf, 64, &src_ip, &src_port);

    int ok = (n == 9);
    if (ok) {
        for (int i = 0; i < 9; i++) {
            if (buf[i] != msg[i]) { ok = 0; break; }
        }
    }
    check(ok, "2: UDP loopback recv data matches");

    sys_close(s1);
    sys_close(s2);
}

static void test_tcp_loopback(void) {
    /* Create listener */
    long listen_conn = sys_tcp_socket();
    if (listen_conn < 0) {
        check(0, "3: TCP loopback (socket failed)");
        check(0, "4: TCP loopback data transfer");
        check(0, "5: TCP loopback close");
        return;
    }

    long r = sys_tcp_listen(listen_conn, 7777);
    if (r < 0) {
        check(0, "3: TCP loopback (listen failed)");
        check(0, "4: TCP loopback data transfer");
        check(0, "5: TCP loopback close");
        sys_tcp_close(listen_conn);
        return;
    }

    /* Fork: child connects, parent accepts */
    long pid = sys_fork();

    if (pid == 0) {
        /* Child: connect to listener */
        long cli_conn = sys_tcp_socket();
        if (cli_conn < 0) {
            sys_exit(1);
        }

        /* Small delay to let parent get into accept */
        sys_yield();
        sys_yield();

        long cr = sys_tcp_connect(cli_conn, OUR_IP, 7777);
        if (cr < 0) {
            sys_tcp_close(cli_conn);
            sys_exit(2);
        }

        /* Send data */
        const char *msg = "hello-tcp-loopback";
        sys_tcp_send(cli_conn, msg, 18);

        /* Receive response */
        char buf[64];
        long n = sys_tcp_recv(cli_conn, buf, 64);

        int ok = (n == 5);
        if (ok) {
            /* Check "reply" */
            ok = (buf[0] == 'r' && buf[1] == 'e' && buf[2] == 'p' &&
                  buf[3] == 'l' && buf[4] == 'y');
        }

        sys_tcp_close(cli_conn);
        sys_exit(ok ? 0 : 3);
    }

    /* Parent: accept connection */
    long srv_conn = sys_tcp_accept(listen_conn);
    check(srv_conn >= 0, "3: TCP loopback connect+accept");

    if (srv_conn >= 0) {
        /* Receive data from client */
        char buf[64];
        long n = sys_tcp_recv(srv_conn, buf, 64);

        int ok = (n == 18);
        if (ok) {
            /* Verify "hello-tcp-loopback" */
            const char *expected = "hello-tcp-loopback";
            for (int i = 0; i < 18; i++) {
                if (buf[i] != expected[i]) { ok = 0; break; }
            }
        }
        check(ok, "4: TCP loopback data transfer");

        /* Send reply */
        sys_tcp_send(srv_conn, "reply", 5);

        /* Small delay for client to receive */
        sys_yield();
        sys_yield();
        sys_yield();

        sys_tcp_close(srv_conn);
    } else {
        check(0, "4: TCP loopback data transfer");
    }

    /* Wait for child */
    long status = sys_waitpid(pid);
    check(status == 0, "5: TCP loopback child success");

    sys_tcp_close(listen_conn);
}

/* ====== IOCTL Tests ====== */

static void test_ioctl_tcgets(void) {
    long mfd = -1, sfd = -1;
    long r = sys_openpty(&mfd, &sfd);
    if (r < 0) {
        check(0, "6: IOCTL TCGETS (openpty failed)");
        return;
    }

    termios_t t;
    memset(&t, 0, sizeof(t));
    r = sys_ioctl(sfd, TCGETS, (long)&t);

    /* Default: ECHO | ICANON */
    int ok = (r == 0 && (t.c_lflag & TERMIOS_ECHO) && (t.c_lflag & TERMIOS_ICANON));
    check(ok, "6: IOCTL TCGETS returns default flags");

    sys_close(mfd);
    sys_close(sfd);
}

static void test_ioctl_tcsets(void) {
    long mfd = -1, sfd = -1;
    sys_openpty(&mfd, &sfd);

    /* Disable echo */
    termios_t t;
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_cflag = 0;
    t.c_lflag = TERMIOS_ICANON;  /* no ECHO */
    long r = sys_ioctl(sfd, TCSETS, (long)&t);
    check(r == 0, "7: IOCTL TCSETS succeeds");

    /* Verify echo is off: write to master, check no echo in s2m */
    const char *msg = "x\n";
    sys_fwrite(mfd, msg, 2);

    char buf[64];
    long n = sys_read(mfd, buf, 64);  /* master read (s2m) */
    /* With echo off, only the slave's writes appear in s2m.
     * Since nobody wrote to slave, should get 0 bytes. */
    check(n == 0, "8: TCSETS echo disabled (no echo in master read)");

    sys_close(mfd);
    sys_close(sfd);
}

static void test_ioctl_winsize(void) {
    long mfd = -1, sfd = -1;
    sys_openpty(&mfd, &sfd);

    winsize_t ws;
    memset(&ws, 0, sizeof(ws));
    long r = sys_ioctl(sfd, TIOCGWINSZ, (long)&ws);
    check(r == 0 && ws.ws_row == 25 && ws.ws_col == 80,
          "9: TIOCGWINSZ returns default 25x80");

    /* Set new size */
    ws.ws_row = 50;
    ws.ws_col = 132;
    r = sys_ioctl(sfd, TIOCSWINSZ, (long)&ws);
    check(r == 0, "10: TIOCSWINSZ succeeds");

    /* Read back */
    winsize_t ws2;
    memset(&ws2, 0, sizeof(ws2));
    sys_ioctl(sfd, TIOCGWINSZ, (long)&ws2);
    check(ws2.ws_row == 50 && ws2.ws_col == 132,
          "11: TIOCGWINSZ returns updated size");

    sys_close(mfd);
    sys_close(sfd);
}

static void test_ioctl_invalid(void) {
    /* ioctl on a non-PTY fd should fail */
    long fd = sys_open("/hello.elf", 0);
    long r = -1;
    if (fd >= 0) {
        r = sys_ioctl(fd, TCGETS, 0);
        sys_close(fd);
    }
    check(r == -1, "12: IOCTL on non-PTY fd returns -1");
}

/* ====== Console PTY Tests ====== */

static void test_loopback_ip(void) {
    /* Test loopback with 127.0.0.1 */
    long s1 = sys_socket();
    long s2 = sys_socket();
    if (s1 < 0 || s2 < 0) {
        check(0, "13: 127.0.0.1 loopback (socket failed)");
        return;
    }

    sys_bind(s1, 7003);
    sys_bind(s2, 7004);

    /* Send to 127.0.0.1 */
    const char *msg = "lo127";
    sys_sendto(s2, msg, 5, 0x7F000001U, 7003);

    char buf[64];
    unsigned int src_ip = 0;
    unsigned short src_port = 0;
    long n = sys_recvfrom(s1, buf, 64, &src_ip, &src_port);

    int ok = (n == 5);
    if (ok) {
        for (int i = 0; i < 5; i++) {
            if (buf[i] != msg[i]) { ok = 0; break; }
        }
    }
    check(ok, "13: 127.0.0.1 loopback works");

    sys_close(s1);
    sys_close(s2);
}

/* ====== Main ====== */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\ns33test: Stage 33 tests (Loopback, IOCTL, Console PTY)\n");
    printf("------------------------------------------------------\n");

    /* Loopback tests */
    printf("\n--- Loopback tests ---\n");
    test_udp_loopback();
    test_tcp_loopback();
    test_loopback_ip();

    /* IOCTL tests */
    printf("\n--- IOCTL tests ---\n");
    test_ioctl_tcgets();
    test_ioctl_tcsets();
    test_ioctl_winsize();
    test_ioctl_invalid();

    printf("\n------------------------------------------------------\n");
    printf("s33test: %d/%d passed", tests_passed, tests_total);
    if (tests_passed == tests_total)
        printf(" -- ALL PASSED");
    printf("\n\n");

    return 0;
}
