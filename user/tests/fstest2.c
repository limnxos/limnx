#include "libc/libc.h"

static int pass_count = 0;
static int fail_count = 0;

static void pass(const char *name) {
    printf("  [PASS] %s\n", name);
    pass_count++;
}

static void fail(const char *name) {
    printf("  [FAIL] %s\n", name);
    fail_count++;
}

/* Helper: simple stat wrapper */
static int stat_size(const char *path, uint64_t *out_size, uint8_t *out_type) {
    uint8_t st[16];
    if (sys_stat(path, st) != 0) return -1;
    if (out_size) *out_size = *(uint64_t *)st;
    if (out_type) *out_type = st[8];
    return 0;
}

/* Test 1: O_CREAT */
static void test_o_creat(void) {
    const char *name = "O_CREAT";
    sys_unlink("/creat_test.txt");
    long fd = sys_open("/creat_test.txt", O_WRONLY | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_close(fd);
    uint64_t sz = 0;
    if (stat_size("/creat_test.txt", &sz, NULL) != 0) { fail(name); return; }
    sys_unlink("/creat_test.txt");
    pass(name);
}

/* Test 2: O_TRUNC */
static void test_o_trunc(void) {
    const char *name = "O_TRUNC";
    long fd = sys_open("/trunc_test.txt", O_WRONLY | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "hello", 5);
    sys_close(fd);

    /* Verify data written */
    uint64_t sz = 0;
    stat_size("/trunc_test.txt", &sz, NULL);
    if (sz != 5) { fail(name); sys_unlink("/trunc_test.txt"); return; }

    /* Reopen with O_TRUNC */
    fd = sys_open("/trunc_test.txt", O_WRONLY | O_TRUNC);
    if (fd < 0) { fail(name); sys_unlink("/trunc_test.txt"); return; }
    sys_close(fd);

    stat_size("/trunc_test.txt", &sz, NULL);
    if (sz != 0) { fail(name); sys_unlink("/trunc_test.txt"); return; }
    sys_unlink("/trunc_test.txt");
    pass(name);
}

/* Test 3: O_APPEND */
static void test_o_append(void) {
    const char *name = "O_APPEND";
    long fd = sys_open("/append_test.txt", O_WRONLY | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "ABC", 3);
    sys_close(fd);

    fd = sys_open("/append_test.txt", O_WRONLY | O_APPEND);
    if (fd < 0) { fail(name); sys_unlink("/append_test.txt"); return; }
    sys_fwrite(fd, "DEF", 3);
    sys_close(fd);

    /* Verify total size is 6 and content is "ABCDEF" */
    fd = sys_open("/append_test.txt", O_RDONLY);
    if (fd < 0) { fail(name); sys_unlink("/append_test.txt"); return; }
    char buf[16];
    long n = sys_read(fd, buf, 16);
    sys_close(fd);
    if (n != 6 || buf[0] != 'A' || buf[3] != 'D') {
        fail(name); sys_unlink("/append_test.txt"); return;
    }
    sys_unlink("/append_test.txt");
    pass(name);
}

/* Test 4: O_RDONLY guard — fwrite should fail */
static void test_rdonly_guard(void) {
    const char *name = "O_RDONLY guard";
    /* Use an existing file */
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name); return; }
    long w = sys_fwrite(fd, "X", 1);
    sys_close(fd);
    if (w >= 0) { fail(name); return; }
    pass(name);
}

/* Test 5: O_WRONLY guard — read should fail */
static void test_wronly_guard(void) {
    const char *name = "O_WRONLY guard";
    long fd = sys_open("/wronly_test.txt", O_WRONLY | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "data", 4);
    sys_close(fd);

    fd = sys_open("/wronly_test.txt", O_WRONLY);
    if (fd < 0) { fail(name); sys_unlink("/wronly_test.txt"); return; }
    char buf[8];
    long r = sys_read(fd, buf, 4);
    sys_close(fd);
    sys_unlink("/wronly_test.txt");
    if (r >= 0) { fail(name); return; }
    pass(name);
}

/* Test 6: seek SEEK_SET */
static void test_seek_set(void) {
    const char *name = "seek SEEK_SET";
    long fd = sys_open("/seek_test.txt", O_RDWR | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "ABCDEF", 6);

    long off = sys_seek(fd, 0, SEEK_SET);
    if (off != 0) { fail(name); sys_close(fd); sys_unlink("/seek_test.txt"); return; }

    char buf[8];
    long n = sys_read(fd, buf, 6);
    sys_close(fd);
    sys_unlink("/seek_test.txt");
    if (n != 6 || buf[0] != 'A' || buf[5] != 'F') { fail(name); return; }
    pass(name);
}

/* Test 7: seek SEEK_CUR */
static void test_seek_cur(void) {
    const char *name = "seek SEEK_CUR";
    long fd = sys_open("/seekcur_test.txt", O_RDWR | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "ABCDEF", 6);
    sys_seek(fd, 0, SEEK_SET);
    sys_seek(fd, 3, SEEK_CUR);

    char buf[8];
    long n = sys_read(fd, buf, 3);
    sys_close(fd);
    sys_unlink("/seekcur_test.txt");
    if (n != 3 || buf[0] != 'D') { fail(name); return; }
    pass(name);
}

/* Test 8: seek SEEK_END */
static void test_seek_end(void) {
    const char *name = "seek SEEK_END";
    long fd = sys_open("/seekend_test.txt", O_RDWR | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "ABCDEF", 6);

    long off = sys_seek(fd, 0, SEEK_END);
    sys_close(fd);
    sys_unlink("/seekend_test.txt");
    if (off != 6) { fail(name); return; }
    pass(name);
}

/* Test 9: truncate grow */
static void test_truncate_grow(void) {
    const char *name = "truncate grow";
    long fd = sys_open("/tgrow_test.txt", O_WRONLY | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "AB", 2);
    sys_close(fd);

    if (sys_truncate("/tgrow_test.txt", 10) != 0) {
        fail(name); sys_unlink("/tgrow_test.txt"); return;
    }

    uint64_t sz = 0;
    stat_size("/tgrow_test.txt", &sz, NULL);
    if (sz != 10) { fail(name); sys_unlink("/tgrow_test.txt"); return; }

    /* Check zero-fill: read from offset 2 should be zeros */
    fd = sys_open("/tgrow_test.txt", O_RDONLY);
    if (fd < 0) { fail(name); sys_unlink("/tgrow_test.txt"); return; }
    sys_seek(fd, 2, SEEK_SET);
    char buf[8];
    sys_read(fd, buf, 8);
    sys_close(fd);
    sys_unlink("/tgrow_test.txt");
    if (buf[0] != 0 || buf[7] != 0) { fail(name); return; }
    pass(name);
}

/* Test 10: truncate shrink */
static void test_truncate_shrink(void) {
    const char *name = "truncate shrink";
    long fd = sys_open("/tshrink_test.txt", O_WRONLY | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "ABCDEFGH", 8);
    sys_close(fd);

    if (sys_truncate("/tshrink_test.txt", 3) != 0) {
        fail(name); sys_unlink("/tshrink_test.txt"); return;
    }

    uint64_t sz = 0;
    stat_size("/tshrink_test.txt", &sz, NULL);
    sys_unlink("/tshrink_test.txt");
    if (sz != 3) { fail(name); return; }
    pass(name);
}

/* Test 11: . resolution */
static void test_dot_resolution(void) {
    const char *name = ". resolution";
    long fd = sys_open("/./hello.txt", O_RDONLY);
    if (fd < 0) { fail(name); return; }
    char buf[8];
    long n = sys_read(fd, buf, 5);
    sys_close(fd);
    if (n <= 0) { fail(name); return; }
    pass(name);
}

/* Test 12: .. resolution */
static void test_dotdot_resolution(void) {
    const char *name = ".. resolution";
    sys_mkdir("/dottest");
    long fd = sys_open("/dottest/../hello.txt", O_RDONLY);
    if (fd < 0) { fail(name); return; }
    char buf[8];
    long n = sys_read(fd, buf, 5);
    sys_close(fd);
    if (n <= 0) { fail(name); return; }
    pass(name);
}

/* Test 13: chdir + relative open */
static void test_chdir_relative(void) {
    const char *name = "chdir + relative open";
    /* /testdir and /testdir/file.txt may exist from fstest; create if not */
    sys_mkdir("/testdir");
    long fd = sys_open("/testdir/reltest.txt", O_WRONLY | O_CREAT);
    if (fd >= 0) {
        sys_fwrite(fd, "relative!", 9);
        sys_close(fd);
    }

    if (sys_chdir("/testdir") != 0) { fail(name); return; }

    fd = sys_open("reltest.txt", O_RDONLY);
    if (fd < 0) { fail(name); sys_chdir("/"); return; }

    char buf[16];
    long n = sys_read(fd, buf, 9);
    sys_close(fd);
    sys_chdir("/");
    sys_unlink("/testdir/reltest.txt");

    if (n != 9 || buf[0] != 'r') { fail(name); return; }
    pass(name);
}

/* Test 14: getcwd */
static void test_getcwd(void) {
    const char *name = "getcwd";
    sys_mkdir("/cwdtest");
    if (sys_chdir("/cwdtest") != 0) { fail(name); return; }

    char cwdbuf[256];
    sys_getcwd(cwdbuf, sizeof(cwdbuf));

    sys_chdir("/");

    if (strcmp(cwdbuf, "/cwdtest") != 0) { fail(name); return; }
    pass(name);
}

/* Test 15: fstat */
static void test_fstat(void) {
    const char *name = "fstat";
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name); return; }

    uint8_t st_fd[16], st_path[16];
    if (sys_fstat(fd, st_fd) != 0) { fail(name); sys_close(fd); return; }
    sys_close(fd);

    if (sys_stat("/hello.txt", st_path) != 0) { fail(name); return; }

    uint64_t sz_fd = *(uint64_t *)st_fd;
    uint64_t sz_path = *(uint64_t *)st_path;
    if (sz_fd != sz_path) { fail(name); return; }
    if (st_fd[8] != st_path[8]) { fail(name); return; }  /* type match */
    pass(name);
}

/* Test 16: rename */
static void test_rename(void) {
    const char *name = "rename";
    long fd = sys_open("/rename_src.txt", O_WRONLY | O_CREAT);
    if (fd < 0) { fail(name); return; }
    sys_fwrite(fd, "rename_data", 11);
    sys_close(fd);

    if (sys_rename("/rename_src.txt", "/rename_dst.txt") != 0) {
        fail(name); sys_unlink("/rename_src.txt"); return;
    }

    /* Old should be gone */
    uint64_t sz = 0;
    if (stat_size("/rename_src.txt", &sz, NULL) == 0) {
        fail(name); sys_unlink("/rename_dst.txt"); return;
    }

    /* New should exist with correct size */
    if (stat_size("/rename_dst.txt", &sz, NULL) != 0 || sz != 11) {
        fail(name); sys_unlink("/rename_dst.txt"); return;
    }

    sys_unlink("/rename_dst.txt");
    pass(name);
}

int main(void) {
    printf("=== fstest2: filesystem completion tests ===\n");

    test_o_creat();
    test_o_trunc();
    test_o_append();
    test_rdonly_guard();
    test_wronly_guard();
    test_seek_set();
    test_seek_cur();
    test_seek_end();
    test_truncate_grow();
    test_truncate_shrink();
    test_dot_resolution();
    test_dotdot_resolution();
    test_chdir_relative();
    test_getcwd();
    test_fstat();
    test_rename();

    printf("=== fstest2: %d passed, %d failed ===\n", pass_count, fail_count);
    if (fail_count == 0)
        printf("=== fstest2: ALL PASSED ===\n");

    return fail_count;
}
