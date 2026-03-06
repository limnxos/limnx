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

/* --- Test 1: 5MB file write+read (exercises double indirect) --- */
static void test_5mb_write_read(void) {
    const char *name = "5MB file write+read";

    sys_unlink("/s27big.dat");
    long fd = sys_open("/s27big.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    /* Write 5MB in 4KB chunks */
    char wbuf[4096];
    uint32_t total_chunks = (5 * 1024 * 1024) / 4096;  /* 1280 chunks */

    for (uint32_t c = 0; c < total_chunks; c++) {
        uint8_t pattern = (uint8_t)(c & 0xFF);
        for (int i = 0; i < 4096; i++)
            wbuf[i] = (char)pattern;
        long w = sys_fwrite(fd, wbuf, 4096);
        if (w != 4096) {
            printf("  write failed at chunk %u (wrote %ld)\n", c, w);
            fail(name, "write failed");
            sys_close(fd);
            sys_unlink("/s27big.dat");
            return;
        }
    }
    sys_close(fd);

    /* Read back and verify */
    fd = sys_open("/s27big.dat", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); sys_unlink("/s27big.dat"); return; }

    int ok = 1;
    for (uint32_t c = 0; c < total_chunks; c++) {
        char rbuf[4096];
        long r = sys_read(fd, rbuf, 4096);
        if (r != 4096) { ok = 0; break; }
        uint8_t pattern = (uint8_t)(c & 0xFF);
        for (int i = 0; i < 4096; i++) {
            if (rbuf[i] != (char)pattern) { ok = 0; break; }
        }
        if (!ok) break;
    }
    sys_close(fd);

    if (ok) pass(name);
    else fail(name, "data mismatch");

    sys_unlink("/s27big.dat");
}

/* --- Test 2: direct->indirect boundary --- */
static void test_boundary_direct_indirect(void) {
    const char *name = "boundary: direct->indirect";

    sys_unlink("/s27bnd1.dat");
    long fd = sys_open("/s27bnd1.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    /* Write exactly 40KB = 10 blocks (all direct) */
    char buf[4096];
    for (int i = 0; i < 4096; i++) buf[i] = 'D';
    for (int b = 0; b < 10; b++) {
        long w = sys_fwrite(fd, buf, 4096);
        if (w != 4096) { fail(name, "write 40KB failed"); sys_close(fd); sys_unlink("/s27bnd1.dat"); return; }
    }

    /* Write 1 more block (41st KB, first indirect) */
    for (int i = 0; i < 4096; i++) buf[i] = 'I';
    long w = sys_fwrite(fd, buf, 4096);
    if (w != 4096) { fail(name, "write 11th block failed"); sys_close(fd); sys_unlink("/s27bnd1.dat"); return; }

    sys_close(fd);

    /* Verify: read block 9 (last direct) and block 10 (first indirect) */
    fd = sys_open("/s27bnd1.dat", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); sys_unlink("/s27bnd1.dat"); return; }

    /* Seek to block 9 */
    sys_seek(fd, 9 * 4096, SEEK_SET);
    char rbuf[4096];
    long r = sys_read(fd, rbuf, 4096);
    if (r != 4096 || rbuf[0] != 'D') {
        fail(name, "block 9 (direct) mismatch");
        sys_close(fd);
        sys_unlink("/s27bnd1.dat");
        return;
    }

    /* Block 10 (first indirect) */
    r = sys_read(fd, rbuf, 4096);
    if (r != 4096 || rbuf[0] != 'I') {
        fail(name, "block 10 (indirect) mismatch");
        sys_close(fd);
        sys_unlink("/s27bnd1.dat");
        return;
    }

    sys_close(fd);
    pass(name);
    sys_unlink("/s27bnd1.dat");
}

/* --- Test 3: indirect->double indirect boundary --- */
static void test_boundary_indirect_double(void) {
    const char *name = "boundary: indirect->double indirect";

    sys_unlink("/s27bnd2.dat");
    long fd = sys_open("/s27bnd2.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    /* Write 1034 blocks = 10 direct + 1024 indirect = ~4.04MB */
    char buf[4096];
    for (int i = 0; i < 4096; i++) buf[i] = 'X';
    for (int b = 0; b < 1034; b++) {
        long w = sys_fwrite(fd, buf, 4096);
        if (w != 4096) {
            printf("  write failed at block %d\n", b);
            fail(name, "write 1034 blocks failed");
            sys_close(fd);
            sys_unlink("/s27bnd2.dat");
            return;
        }
    }

    /* Write 1 more block — first double indirect */
    for (int i = 0; i < 4096; i++) buf[i] = 'Y';
    long w = sys_fwrite(fd, buf, 4096);
    if (w != 4096) { fail(name, "write 1035th block failed"); sys_close(fd); sys_unlink("/s27bnd2.dat"); return; }

    sys_close(fd);

    /* Read back last indirect block (1033) and first double indirect block (1034) */
    fd = sys_open("/s27bnd2.dat", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); sys_unlink("/s27bnd2.dat"); return; }

    sys_seek(fd, (long)1033 * 4096, SEEK_SET);
    char rbuf[4096];
    long r = sys_read(fd, rbuf, 4096);
    if (r != 4096 || rbuf[0] != 'X') {
        fail(name, "last indirect block mismatch");
        sys_close(fd);
        sys_unlink("/s27bnd2.dat");
        return;
    }

    r = sys_read(fd, rbuf, 4096);
    if (r != 4096 || rbuf[0] != 'Y') {
        fail(name, "first double indirect block mismatch");
        sys_close(fd);
        sys_unlink("/s27bnd2.dat");
        return;
    }

    sys_close(fd);
    pass(name);
    sys_unlink("/s27bnd2.dat");
}

/* --- Test 4: file stat after large write --- */
static void test_stat_large_file(void) {
    const char *name = "stat after 5MB write";

    sys_unlink("/s27stat.dat");
    long fd = sys_open("/s27stat.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    /* Write 5MB */
    char buf[4096];
    for (int i = 0; i < 4096; i++) buf[i] = 'S';
    uint32_t target = 5 * 1024 * 1024;
    uint32_t written = 0;
    while (written < target) {
        long w = sys_fwrite(fd, buf, 4096);
        if (w != 4096) { fail(name, "write failed"); sys_close(fd); sys_unlink("/s27stat.dat"); return; }
        written += 4096;
    }
    sys_close(fd);

    /* Stat the file */
    struct {
        uint64_t size;
        uint8_t  type;
        uint8_t  pad[7];
    } st;

    if (sys_stat("/s27stat.dat", &st) != 0) {
        fail(name, "stat failed");
        sys_unlink("/s27stat.dat");
        return;
    }

    if (st.size != target) {
        printf("  expected %u, got %lu\n", target, st.size);
        fail(name, "size mismatch");
    } else {
        pass(name);
    }

    sys_unlink("/s27stat.dat");
}

/* --- Test 5: large file truncate --- */
static void test_truncate_large(void) {
    const char *name = "large file truncate";

    sys_unlink("/s27trunc.dat");
    long fd = sys_open("/s27trunc.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    /* Write 5MB */
    char buf[4096];
    for (int i = 0; i < 4096; i++) buf[i] = 'T';
    for (uint32_t b = 0; b < 1280; b++) {
        long w = sys_fwrite(fd, buf, 4096);
        if (w != 4096) { fail(name, "write failed"); sys_close(fd); sys_unlink("/s27trunc.dat"); return; }
    }
    sys_close(fd);

    /* Truncate to 1KB */
    if (sys_truncate("/s27trunc.dat", 1024) != 0) {
        fail(name, "truncate failed");
        sys_unlink("/s27trunc.dat");
        return;
    }

    /* Verify size */
    struct {
        uint64_t size;
        uint8_t  type;
        uint8_t  pad[7];
    } st;
    if (sys_stat("/s27trunc.dat", &st) != 0 || st.size != 1024) {
        fail(name, "size after truncate wrong");
        sys_unlink("/s27trunc.dat");
        return;
    }

    /* Verify data integrity of remaining 1KB */
    fd = sys_open("/s27trunc.dat", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); sys_unlink("/s27trunc.dat"); return; }

    char rbuf[1024];
    long r = sys_read(fd, rbuf, 1024);
    sys_close(fd);

    if (r != 1024) { fail(name, "read after truncate failed"); sys_unlink("/s27trunc.dat"); return; }

    int ok = 1;
    for (int i = 0; i < 1024; i++) {
        if (rbuf[i] != 'T') { ok = 0; break; }
    }

    if (ok) pass(name);
    else fail(name, "data mismatch after truncate");

    sys_unlink("/s27trunc.dat");
}

/* --- Test 6: large file delete + space reclaim --- */
static void test_delete_reclaim(void) {
    const char *name = "delete + space reclaim";

    sys_unlink("/s27del.dat");
    long fd = sys_open("/s27del.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    /* Write 5MB */
    char buf[4096];
    for (int i = 0; i < 4096; i++) buf[i] = 'R';
    for (uint32_t b = 0; b < 1280; b++) {
        long w = sys_fwrite(fd, buf, 4096);
        if (w != 4096) { fail(name, "write failed"); sys_close(fd); sys_unlink("/s27del.dat"); return; }
    }
    sys_close(fd);

    /* Delete it */
    sys_unlink("/s27del.dat");

    /* Verify space was reclaimed: create another 5MB file */
    sys_unlink("/s27del2.dat");
    fd = sys_open("/s27del2.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "second create failed"); return; }

    int reclaimed = 1;
    for (uint32_t b = 0; b < 1280; b++) {
        long w = sys_fwrite(fd, buf, 4096);
        if (w != 4096) { reclaimed = 0; break; }
    }
    sys_close(fd);

    if (reclaimed) pass(name);
    else fail(name, "space not reclaimed after delete");

    sys_unlink("/s27del2.dat");
}

int main(void) {
    printf("\ns27test: running %d tests\n", 6);

    test_5mb_write_read();
    test_boundary_direct_indirect();
    test_boundary_indirect_double();
    test_stat_large_file();
    test_truncate_large();
    test_delete_reclaim();

    if (tests_passed == tests_total)
        printf("s27test: ALL PASSED (%d/%d)\n", tests_passed, tests_total);
    else
        printf("s27test: %d/%d passed\n", tests_passed, tests_total);

    return 0;
}
