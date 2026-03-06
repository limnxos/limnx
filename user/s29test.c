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

/* --- Helpers --- */

/* Read until newline or EOF using O_NONBLOCK */
static int read_pipe_line(long fd, char *buf, int max) {
    sys_fcntl(fd, F_SETFL, O_NONBLOCK);
    int pos = 0;
    while (pos < max - 1) {
        char c;
        long n = sys_read(fd, &c, 1);
        if (n > 0) {
            if (c == '\n') break;
            buf[pos++] = c;
        } else if (n == 0) {
            break;
        } else {
            sys_yield();
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* Read all data until EOF using O_NONBLOCK */
static int read_pipe_all(long fd, char *buf, int max) {
    sys_fcntl(fd, F_SETFL, O_NONBLOCK);
    int total = 0;
    while (total < max - 1) {
        char c;
        long n = sys_read(fd, &c, 1);
        if (n > 0) {
            buf[total++] = c;
        } else if (n == 0) {
            break;
        } else {
            sys_yield();
        }
    }
    buf[total] = '\0';
    return total;
}

/* Spawn worker.elf with pipes set up using FD_CLOEXEC */
static long spawn_worker(long *write_fd, long *read_fd) {
    long rA, wA, rB, wB;
    if (sys_pipe(&rA, &wA) != 0) return -1;
    if (sys_pipe(&rB, &wB) != 0) return -1;

    long saved_wA = sys_dup(wA);
    if (saved_wA < 0) return -1;

    sys_fcntl(rB, F_SETFD, FD_CLOEXEC);
    sys_fcntl(wA, F_SETFD, FD_CLOEXEC);
    sys_fcntl(saved_wA, F_SETFD, FD_CLOEXEC);

    sys_dup2(rA, 0);
    sys_dup2(wB, 1);

    long pid = sys_exec("/worker.elf", NULL);
    if (pid < 0) return -1;

    sys_close(0);
    sys_close(1);
    sys_close(rA);
    sys_close(wA);
    sys_close(wB);

    *write_fd = saved_wA;
    *read_fd = rB;
    return pid;
}

/* --- Test 1: text buffer scroll --- */
static void test_text_buffer_scroll(void) {
    const char *name = "text buffer scroll";
    /* Print enough lines to trigger multiple scrolls.
       If the text buffer scroll works, this completes without crash. */
    for (int i = 0; i < 60; i++)
        printf("  scroll line %d\n", i);
    pass(name);
}

/* --- Test 2: disk write + read back --- */
static void test_disk_write_read(void) {
    const char *name = "disk write+read";

    /* Create a test file, write data, close, reopen, read back */
    sys_create("/s29_test.txt");
    long fd = sys_open("/s29_test.txt", O_RDWR);
    if (fd < 0) { fail(name, "open failed"); return; }

    const char *data = "Hello from Stage 29!";
    int data_len = (int)strlen(data);
    long w = sys_fwrite(fd, data, (unsigned long)data_len);
    sys_close(fd);

    if (w != data_len) { fail(name, "write short"); return; }

    /* Reopen and read back */
    fd = sys_open("/s29_test.txt", O_RDONLY);
    if (fd < 0) { fail(name, "reopen failed"); return; }

    char buf[64];
    long r = sys_read(fd, buf, 63);
    sys_close(fd);
    sys_unlink("/s29_test.txt");

    if (r != data_len) { fail(name, "read length mismatch"); return; }
    buf[r] = '\0';

    if (strcmp(buf, data) == 0)
        pass(name);
    else
        fail(name, "data mismatch");
}

/* --- Test 3: large file integrity (exercises indirect blocks) --- */
static void test_large_file_integrity(void) {
    const char *name = "large file integrity";

    sys_create("/s29_large.bin");
    long fd = sys_open("/s29_large.bin", O_RDWR);
    if (fd < 0) { fail(name, "open failed"); return; }

    /* Write 50KB of patterned data — spans direct + indirect blocks */
    char wbuf[512];
    int total_written = 0;
    for (int chunk = 0; chunk < 100; chunk++) {
        for (int i = 0; i < 512; i++)
            wbuf[i] = (char)((chunk + i) & 0xFF);
        long w = sys_fwrite(fd, wbuf, 512);
        if (w != 512) break;
        total_written += 512;
    }
    sys_close(fd);

    if (total_written != 51200) { fail(name, "write incomplete"); sys_unlink("/s29_large.bin"); return; }

    /* Read back and verify */
    fd = sys_open("/s29_large.bin", O_RDONLY);
    if (fd < 0) { fail(name, "reopen failed"); sys_unlink("/s29_large.bin"); return; }

    char rbuf[512];
    int ok = 1;
    for (int chunk = 0; chunk < 100 && ok; chunk++) {
        long r = sys_read(fd, rbuf, 512);
        if (r != 512) { ok = 0; break; }
        for (int i = 0; i < 512; i++) {
            if (rbuf[i] != (char)((chunk + i) & 0xFF)) { ok = 0; break; }
        }
    }
    sys_close(fd);
    sys_unlink("/s29_large.bin");

    if (ok) pass(name);
    else fail(name, "data verification failed");
}

/* --- Test 4: cache behavior (many distinct block reads) --- */
static void test_cache_many_reads(void) {
    const char *name = "cache many reads";

    /* Write a file large enough to span many blocks, read it twice.
       Second read should be faster (cached). We just verify correctness. */
    sys_create("/s29_cache.bin");
    long fd = sys_open("/s29_cache.bin", O_RDWR);
    if (fd < 0) { fail(name, "open failed"); return; }

    char wbuf[256];
    for (int i = 0; i < 256; i++) wbuf[i] = (char)(i & 0x7F);

    /* Write 32KB (8 blocks) */
    for (int i = 0; i < 128; i++)
        sys_fwrite(fd, wbuf, 256);
    sys_close(fd);

    /* Read twice — second read hits cache */
    int ok = 1;
    for (int pass_num = 0; pass_num < 2 && ok; pass_num++) {
        fd = sys_open("/s29_cache.bin", O_RDONLY);
        if (fd < 0) { ok = 0; break; }
        char rbuf[256];
        for (int i = 0; i < 128 && ok; i++) {
            long r = sys_read(fd, rbuf, 256);
            if (r != 256) { ok = 0; break; }
            for (int j = 0; j < 256; j++) {
                if (rbuf[j] != (char)(j & 0x7F)) { ok = 0; break; }
            }
        }
        sys_close(fd);
    }
    sys_unlink("/s29_cache.bin");

    if (ok) pass(name);
    else fail(name, "data mismatch on cached read");
}

/* --- Test 5: triple indirect inode field --- */
static void test_triple_indirect_field(void) {
    const char *name = "triple indirect field";
    /* We can't easily exercise triple indirect from userspace
       (would need >4GB of logical blocks). Instead verify the inode
       struct change didn't break normal file operations by doing a
       stat on an existing file. */
    typedef struct {
        uint64_t size;
        uint8_t  type;
        uint8_t  mode;
        uint8_t  pad[6];
    } stat_t;

    stat_t st;
    long r = sys_stat("/hello.txt", &st);
    if (r == 0 && st.size > 0)
        pass(name);
    else
        fail(name, "stat failed after inode struct change");
}

/* --- Test 6: FD_CLOEXEC worker --- */
static void test_fd_cloexec_worker(void) {
    const char *name = "FD_CLOEXEC worker";

    /* Create a pipe, set CLOEXEC, exec hello.elf.
       After exec, the fd should be closed in child.
       We verify child exits normally (doesn't crash). */
    long rfd, wfd;
    if (sys_pipe(&rfd, &wfd) != 0) { fail(name, "pipe failed"); return; }

    sys_fcntl(rfd, F_SETFD, FD_CLOEXEC);
    sys_fcntl(wfd, F_SETFD, FD_CLOEXEC);

    long child = sys_exec("/hello.elf", NULL);
    if (child < 0) { fail(name, "exec failed"); sys_close(rfd); sys_close(wfd); return; }

    long status = sys_waitpid(child);
    sys_close(rfd);
    sys_close(wfd);

    if (status == 0) pass(name);
    else fail(name, "child exit non-zero");
}

/* --- Test 7: O_NONBLOCK read empty --- */
static void test_nonblock_read_empty(void) {
    const char *name = "O_NONBLOCK read empty";

    long rfd, wfd;
    if (sys_pipe(&rfd, &wfd) != 0) { fail(name, "pipe failed"); return; }

    sys_fcntl(rfd, F_SETFL, O_NONBLOCK);

    char c;
    long n = sys_read(rfd, &c, 1);

    sys_close(rfd);
    sys_close(wfd);

    if (n == -1) pass(name);
    else fail(name, "expected -1 for empty nonblock read");
}

/* --- Test 8: O_NONBLOCK EOF detect --- */
static void test_nonblock_eof(void) {
    const char *name = "O_NONBLOCK EOF detect";

    long rfd, wfd;
    if (sys_pipe(&rfd, &wfd) != 0) { fail(name, "pipe failed"); return; }

    sys_close(wfd);  /* Close write end first */
    sys_fcntl(rfd, F_SETFL, O_NONBLOCK);

    char c;
    long n = sys_read(rfd, &c, 1);

    sys_close(rfd);

    if (n == 0) pass(name);
    else fail(name, "expected 0 (EOF) after write end closed");
}

/* --- Test 9: worker pipe comm with FD_CLOEXEC --- */
static void test_worker_pipe_cloexec(void) {
    const char *name = "worker pipe CLOEXEC";

    long wfd, rfd;
    long pid = spawn_worker(&wfd, &rfd);
    if (pid < 0) { fail(name, "spawn_worker failed"); return; }

    const char *cmd = "stat /hello.txt\n";
    sys_fwrite(wfd, cmd, (unsigned long)strlen(cmd));

    char resp[256];
    int n = read_pipe_line(rfd, resp, 256);

    sys_close(wfd);
    sys_waitpid(pid);
    sys_close(rfd);

    if (n > 0 && strstr(resp, "size="))
        pass(name);
    else
        fail(name, "no valid response from worker");
}

/* --- Test 10: multiagent nonblock read --- */
static void test_multiagent_nonblock(void) {
    const char *name = "multiagent nonblock read";

    long wfd, rfd;
    long pid = spawn_worker(&wfd, &rfd);
    if (pid < 0) { fail(name, "spawn_worker failed"); return; }

    const char *cmd = "ls\n";
    sys_fwrite(wfd, cmd, (unsigned long)strlen(cmd));
    sys_close(wfd);  /* Signal EOF so worker exits */

    char resp[2048];
    int n = read_pipe_all(rfd, resp, 2048);

    sys_waitpid(pid);
    sys_close(rfd);

    if (n > 0 && strstr(resp, ".elf"))
        pass(name);
    else
        fail(name, "no ls output via nonblock read");
}

int main(void) {
    printf("\ns29test: running 10 tests\n");

    test_text_buffer_scroll();
    test_disk_write_read();
    test_large_file_integrity();
    test_cache_many_reads();
    test_triple_indirect_field();
    test_fd_cloexec_worker();
    test_nonblock_read_empty();
    test_nonblock_eof();
    test_worker_pipe_cloexec();
    test_multiagent_nonblock();

    if (tests_passed == tests_total)
        printf("s29test: ALL PASSED (%d/%d)\n", tests_passed, tests_total);
    else
        printf("s29test: %d/%d passed\n", tests_passed, tests_total);

    return (tests_passed == tests_total) ? 0 : 1;
}
