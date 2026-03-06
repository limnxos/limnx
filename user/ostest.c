#include "libc/libc.h"

/* --- Test framework --- */

static int tests_passed = 0;
static int tests_total  = 0;

static void pass(const char *name) {
    tests_passed++;
    tests_total++;
    printf("  [PASS] %s\n", name);
}

static void fail(const char *name, const char *reason) {
    tests_total++;
    printf("  [FAIL] %s — %s\n", name, reason);
}

/* --- Test 1: dup basic --- */
static void test_dup_basic(void) {
    const char *name = "dup basic";
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name, "open failed"); return; }

    long fd2 = sys_dup(fd);
    if (fd2 < 0) { fail(name, "dup returned -1"); sys_close(fd); return; }
    if (fd2 == fd) { fail(name, "dup returned same fd"); sys_close(fd); return; }

    /* Both fds should read the same data */
    char buf1[16], buf2[16];
    long n1 = sys_read(fd, buf1, 5);
    /* Reset dup'd fd to offset 0 — it was copied at offset 0 so no seek needed */
    long n2 = sys_read(fd2, buf2, 5);

    if (n1 != n2 || n1 <= 0) { fail(name, "read mismatch"); }
    else {
        int ok = 1;
        for (long i = 0; i < n1; i++)
            if (buf1[i] != buf2[i]) { ok = 0; break; }
        if (ok) pass(name);
        else fail(name, "data mismatch");
    }
    sys_close(fd);
    sys_close(fd2);
}

/* --- Test 2: dup independent offset --- */
static void test_dup_independent_offset(void) {
    const char *name = "dup independent offset";
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name, "open failed"); return; }

    long fd2 = sys_dup(fd);
    if (fd2 < 0) { fail(name, "dup failed"); sys_close(fd); return; }

    /* Read 5 bytes from original → offset advances to 5 */
    char buf[16];
    sys_read(fd, buf, 5);

    /* Read 5 bytes from dup'd fd → starts at offset 0 (copied when dup'd) */
    char buf2[16];
    sys_read(fd2, buf2, 5);

    /* dup'd fd should still read from offset 0, same as first 5 bytes */
    int ok = 1;
    for (int i = 0; i < 5; i++)
        if (buf[i] != buf2[i]) { ok = 0; break; }

    if (ok) pass(name);
    else fail(name, "offsets not independent");

    sys_close(fd);
    sys_close(fd2);
}

/* --- Test 3: dup close independence --- */
static void test_dup_close_independence(void) {
    const char *name = "dup close independence";
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name, "open failed"); return; }

    long fd2 = sys_dup(fd);
    if (fd2 < 0) { fail(name, "dup failed"); sys_close(fd); return; }

    /* Close original */
    sys_close(fd);

    /* dup'd fd should still work */
    char buf[16];
    long n = sys_read(fd2, buf, 5);
    if (n > 0) pass(name);
    else fail(name, "read after closing original failed");

    sys_close(fd2);
}

/* --- Test 4: dup2 basic --- */
static void test_dup2_basic(void) {
    const char *name = "dup2 basic";
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name, "open failed"); return; }

    /* Pick a target fd that's probably free */
    long target = 10;
    long ret = sys_dup2(fd, target);
    if (ret != target) { fail(name, "dup2 did not return target fd"); sys_close(fd); return; }

    /* Read from target fd */
    char buf[16];
    long n = sys_read(target, buf, 5);
    if (n > 0) pass(name);
    else fail(name, "read from dup2 target failed");

    sys_close(fd);
    sys_close(target);
}

/* --- Test 5: dup2 same fd --- */
static void test_dup2_same_fd(void) {
    const char *name = "dup2 same fd";
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name, "open failed"); return; }

    long ret = sys_dup2(fd, fd);
    if (ret == fd) pass(name);
    else fail(name, "dup2(fd, fd) did not return fd");

    sys_close(fd);
}

/* --- Test 6: dup2 auto-close --- */
static void test_dup2_auto_close(void) {
    const char *name = "dup2 auto-close";

    /* Open two different files */
    long fd1 = sys_open("/hello.txt", O_RDONLY);
    if (fd1 < 0) { fail(name, "open hello.txt failed"); return; }

    /* Create a temp file to use as the target fd */
    long fd2 = sys_create("/dup2test.tmp");
    if (fd2 < 0) { fail(name, "create tmp failed"); sys_close(fd1); return; }

    /* dup2(fd1, fd2) should close fd2 and make it a copy of fd1 */
    long ret = sys_dup2(fd1, fd2);
    if (ret != fd2) { fail(name, "dup2 did not return target"); sys_close(fd1); return; }

    /* fd2 should now read from hello.txt, not the temp file */
    char buf[16];
    long n = sys_read(fd2, buf, 5);
    if (n > 0) pass(name);
    else fail(name, "read from redirected fd failed");

    sys_close(fd1);
    sys_close(fd2);
    sys_unlink("/dup2test.tmp");
}

/* --- Test 7: kill SIGKILL --- */
static void test_kill_sigkill(void) {
    const char *name = "kill SIGKILL";

    /* Exec shell.elf — it will block on getchar, giving us time to kill it */
    long child_pid = sys_exec("/shell.elf", NULL);
    if (child_pid < 0) { fail(name, "exec failed"); return; }

    /* Give child a chance to start */
    sys_yield();
    sys_yield();

    /* Kill it */
    long ret = sys_kill(child_pid, SIGKILL);
    if (ret != 0) { fail(name, "kill returned error"); return; }

    /* Wait for it — should get negative exit status */
    long status = sys_waitpid(child_pid);
    if (status < 0) pass(name);
    else fail(name, "exit status not negative after SIGKILL");
}

/* --- Test 8: kill invalid PID --- */
static void test_kill_invalid(void) {
    const char *name = "kill invalid PID";
    long ret = sys_kill(99999, SIGKILL);
    if (ret == -1) pass(name);
    else fail(name, "expected -1 for invalid PID");
}

/* --- Test 9: permission read --- */
static void test_perm_read(void) {
    const char *name = "permission read (initrd)";

    /* Initrd files should have read + exec (0x05) — open read should work */
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd >= 0) {
        pass(name);
        sys_close(fd);
    } else {
        fail(name, "cannot open initrd file for reading");
    }
}

/* --- Test 10: permission stat mode --- */
static void test_perm_stat_mode(void) {
    const char *name = "permission stat mode";

    /* Stat structure with mode field */
    typedef struct {
        uint64_t size;
        uint8_t  type;
        uint8_t  mode;
        uint8_t  pad[6];
    } stat_t;

    stat_t st;
    long ret = sys_stat("/hello.txt", &st);
    if (ret != 0) { fail(name, "stat failed"); return; }

    /* Initrd file should have READ | EXEC = 0x05 */
    if (st.mode == 0x05) pass(name);
    else {
        printf("  [FAIL] %s — expected mode 0x05, got 0x%x\n", name, st.mode);
        tests_total++;
    }
}

/* --- Test 11: disk write + read --- */
static void test_disk_write_read(void) {
    const char *name = "disk write+read";

    /* Create file at root (LimnFS disk-backed) */
    long fd = sys_open("/ostest.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "open/create failed"); return; }

    /* Write known data */
    const char *data = "Stage24 persistence test!";
    long dlen = (long)strlen(data);
    long w = sys_fwrite(fd, data, dlen);
    if (w != dlen) { fail(name, "write length mismatch"); sys_close(fd); return; }

    sys_close(fd);

    /* Re-open and read back */
    fd = sys_open("/ostest.dat", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); return; }

    char rbuf[64];
    long r = sys_read(fd, rbuf, 63);
    sys_close(fd);

    if (r != dlen) { fail(name, "read length mismatch"); return; }

    int ok = 1;
    for (long i = 0; i < dlen; i++)
        if (rbuf[i] != data[i]) { ok = 0; break; }

    if (ok) pass(name);
    else fail(name, "data mismatch");

    /* Cleanup */
    sys_unlink("/ostest.dat");
}

/* --- Test 12: disk readdir --- */
static void test_disk_readdir(void) {
    const char *name = "disk readdir";

    /* Create a file at root (LimnFS disk-backed) */
    long fd = sys_open("/dirtest.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }
    sys_fwrite(fd, "x", 1);
    sys_close(fd);

    /* Read directory entries and look for our file */
    int found = 0;
    dirent_t ent;
    for (unsigned long i = 0; i < 128; i++) {
        if (sys_readdir("/", i, &ent) != 0) break;
        if (strcmp(ent.name, "dirtest.dat") == 0) {
            found = 1;
            break;
        }
    }

    if (found) pass(name);
    else fail(name, "file not found in readdir");

    sys_unlink("/dirtest.dat");
}

int main(void) {
    printf("\nostest: running %d tests\n", 12);

    test_dup_basic();
    test_dup_independent_offset();
    test_dup_close_independence();
    test_dup2_basic();
    test_dup2_same_fd();
    test_dup2_auto_close();
    test_kill_sigkill();
    test_kill_invalid();
    test_perm_read();
    test_perm_stat_mode();
    test_disk_write_read();
    test_disk_readdir();

    if (tests_passed == tests_total)
        printf("ostest: ALL PASSED (%d/%d)\n", tests_passed, tests_total);
    else
        printf("ostest: %d/%d passed\n", tests_passed, tests_total);

    return 0;
}
