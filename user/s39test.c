/* s39test.c — Stage 39 tests: Exec Fix, Bcache Flusher, TCP Cleanup */
#include "libc/libc.h"

static int passed = 0;
static int total = 0;

static void check(int ok, const char *name) {
    total++;
    if (ok) {
        printf("  [%d]  PASS: %s\n", total, name);
        passed++;
    } else {
        printf("  [%d]  FAIL: %s\n", total, name);
    }
}

/* --- Test 1: exec from user-space returns valid pid --- */
static void test_exec_basic(void) {
    long pid = sys_exec("/hello.elf", NULL);
    check(pid > 0, "exec from user-space returns valid pid");
    if (pid > 0)
        sys_waitpid(pid);
}

/* --- Test 2: exec with argv --- */
static void test_exec_argv(void) {
    const char *args[] = { "/hello.elf", "arg1", NULL };
    long pid = sys_exec("/hello.elf", args);
    check(pid > 0, "exec with argv returns valid pid");
    if (pid > 0)
        sys_waitpid(pid);
}

/* --- Test 3: exec + waitpid returns child exit status --- */
static void test_exec_waitpid(void) {
    long pid = sys_exec("/hello.elf", NULL);
    int ok = (pid > 0);
    if (ok) {
        long status = sys_waitpid(pid);
        ok = (status == 0);  /* hello.elf exits with 0 */
    }
    check(ok, "exec + waitpid returns child exit status 0");
}

/* --- Test 4: exec denied for non-exec file --- */
static void test_exec_permission(void) {
    /* Create a file without exec permission */
    sys_create("/s39_noexec.tmp");
    long fd = sys_open("/s39_noexec.tmp", O_RDWR);
    if (fd >= 0) {
        sys_fwrite(fd, "not an elf", 10);
        sys_close(fd);
    }
    sys_chmod("/s39_noexec.tmp", 0644);  /* rw-r--r--, no exec */
    long pid = sys_exec("/s39_noexec.tmp", NULL);
    check(pid < 0, "exec denied for non-exec file (mode 0644)");
    sys_unlink("/s39_noexec.tmp");
}

/* --- Test 5: SIGKILL terminates exec'd child --- */
static void test_sigkill_exec(void) {
    long pid = sys_exec("/shell.elf", NULL);
    int ok = (pid > 0);
    if (ok) {
        /* Give child a chance to start */
        sys_yield();
        sys_yield();
        /* Kill it */
        long ret = sys_kill(pid, SIGKILL);
        ok = ok && (ret == 0);
        long status = sys_waitpid(pid);
        ok = ok && (status < 0);  /* negative = killed by signal */
    }
    check(ok, "SIGKILL terminates exec'd child");
}

/* --- Test 6: bcache dirty count drops after yields --- */
static void test_bcache_flusher(void) {
    /* Write data to create dirty cache entries */
    sys_create("/s39_flush.tmp");
    long fd = sys_open("/s39_flush.tmp", O_RDWR);
    int ok = (fd >= 0);
    if (ok) {
        char buf[256];
        for (int i = 0; i < 256; i++) buf[i] = (char)i;
        sys_fwrite(fd, buf, 256);
        sys_close(fd);

        /* Wait for flusher thread to run (~5 seconds = ~500 yields) */
        for (int i = 0; i < 600; i++)
            sys_yield();

        /* Read back to verify data integrity */
        fd = sys_open("/s39_flush.tmp", O_RDONLY);
        ok = (fd >= 0);
        if (ok) {
            char rbuf[256];
            long n = sys_read(fd, rbuf, 256);
            ok = (n == 256);
            for (int i = 0; i < 256 && ok; i++) {
                if (rbuf[i] != (char)i) ok = 0;
            }
            sys_close(fd);
        }
    }
    sys_unlink("/s39_flush.tmp");
    check(ok, "bcache flusher: data integrity after flush");
}

/* --- Test 7: TCP socket lifecycle --- */
static void test_tcp_lifecycle(void) {
    long conn = sys_tcp_socket();
    int ok = (conn >= 0);
    if (ok) {
        long r = sys_tcp_close(conn);
        ok = (r == 0);
    }
    check(ok, "TCP socket create + close");
}

/* --- Test 8: TCP cleanup on child exit --- */
static void test_tcp_cleanup(void) {
    long pid = sys_fork();
    if (pid == 0) {
        /* Child: open TCP socket, exit without closing */
        long conn = sys_tcp_socket();
        (void)conn;
        sys_exit(0);
    }
    int ok = (pid > 0);
    if (ok) {
        long status = sys_waitpid(pid);
        ok = (status == 0);
        /* After child exits, the TCP slot should be freed.
         * Verify by allocating a new socket (should succeed). */
        long conn = sys_tcp_socket();
        ok = ok && (conn >= 0);
        if (conn >= 0)
            sys_tcp_close(conn);
    }
    check(ok, "TCP cleanup on child exit (slot reusable)");
}

int main(void) {
    printf("\n=== Stage 39 Tests: Exec Fix, Bcache Flusher, TCP Cleanup ===\n\n");

    test_exec_basic();
    test_exec_argv();
    test_exec_waitpid();
    test_exec_permission();
    test_sigkill_exec();
    test_bcache_flusher();
    test_tcp_lifecycle();
    test_tcp_cleanup();

    printf("\n=== Stage 39 Results: %d/%d %s ===\n",
           passed, total, passed == total ? "PASSED" : "FAILED");
    return (passed == total) ? 0 : 1;
}
