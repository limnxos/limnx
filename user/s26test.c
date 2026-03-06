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

/* --- Test 1: disk write+read --- */
static void test_disk_write_read(void) {
    const char *name = "disk write+read";

    long fd = sys_open("/s26test.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "open/create failed"); return; }

    const char *data = "Hello from LimnFS!";
    long dlen = (long)strlen(data);
    long w = sys_fwrite(fd, data, dlen);
    if (w != dlen) { fail(name, "write length mismatch"); sys_close(fd); return; }

    sys_close(fd);

    /* Re-open and read back */
    fd = sys_open("/s26test.dat", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); return; }

    char rbuf[64];
    long r = sys_read(fd, rbuf, 63);
    sys_close(fd);

    if (r != dlen) { fail(name, "read length mismatch"); goto cleanup; }

    int ok = 1;
    for (long i = 0; i < dlen; i++)
        if (rbuf[i] != data[i]) { ok = 0; break; }

    if (ok) pass(name);
    else fail(name, "data mismatch");

cleanup:
    sys_unlink("/s26test.dat");
}

/* --- Test 2: large file (indirect blocks) --- */
static void test_large_file(void) {
    const char *name = "large file (indirect)";

    long fd = sys_open("/s26large.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    /* Write 50KB (exceeds 10 direct blocks = 40KB) */
    char wbuf[1024];
    for (int i = 0; i < 1024; i++)
        wbuf[i] = (char)(i & 0xFF);

    long total_written = 0;
    for (int block = 0; block < 50; block++) {
        long w = sys_fwrite(fd, wbuf, 1024);
        if (w != 1024) { fail(name, "write failed"); sys_close(fd); sys_unlink("/s26large.dat"); return; }
        total_written += w;
    }

    sys_close(fd);

    /* Read back and verify */
    fd = sys_open("/s26large.dat", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); sys_unlink("/s26large.dat"); return; }

    int ok = 1;
    for (int block = 0; block < 50; block++) {
        char rbuf[1024];
        long r = sys_read(fd, rbuf, 1024);
        if (r != 1024) { ok = 0; break; }
        for (int i = 0; i < 1024; i++) {
            if (rbuf[i] != (char)(i & 0xFF)) { ok = 0; break; }
        }
        if (!ok) break;
    }

    sys_close(fd);

    if (ok) pass(name);
    else fail(name, "data mismatch");

    sys_unlink("/s26large.dat");
}

/* --- Test 3: mkdir + file inside --- */
static void test_mkdir_file(void) {
    const char *name = "mkdir + file inside";

    if (sys_mkdir("/s26dir") != 0) { fail(name, "mkdir failed"); return; }

    long fd = sys_open("/s26dir/test.txt", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create in dir failed"); sys_unlink("/s26dir"); return; }

    const char *data = "nested file data";
    long dlen = (long)strlen(data);
    sys_fwrite(fd, data, dlen);
    sys_close(fd);

    /* Read back */
    fd = sys_open("/s26dir/test.txt", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); sys_unlink("/s26dir/test.txt"); sys_unlink("/s26dir"); return; }

    char rbuf[64];
    long r = sys_read(fd, rbuf, 63);
    sys_close(fd);

    int ok = (r == dlen);
    if (ok) {
        for (long i = 0; i < dlen; i++)
            if (rbuf[i] != data[i]) { ok = 0; break; }
    }

    if (ok) pass(name);
    else fail(name, "data mismatch");

    sys_unlink("/s26dir/test.txt");
    sys_unlink("/s26dir");
}

/* --- Test 4: block reuse --- */
static void test_block_reuse(void) {
    const char *name = "block reuse";

    /* Create and write a file */
    long fd = sys_open("/s26reuse.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    char data[4096];
    for (int i = 0; i < 4096; i++)
        data[i] = 'A';
    sys_fwrite(fd, data, 4096);
    sys_close(fd);

    /* Delete it */
    sys_unlink("/s26reuse.dat");

    /* Create another file — should reuse the freed blocks */
    fd = sys_open("/s26reuse2.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "second create failed"); return; }

    for (int i = 0; i < 4096; i++)
        data[i] = 'B';
    sys_fwrite(fd, data, 4096);
    sys_close(fd);

    /* Read back and verify */
    fd = sys_open("/s26reuse2.dat", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); sys_unlink("/s26reuse2.dat"); return; }

    char rbuf[4096];
    long r = sys_read(fd, rbuf, 4096);
    sys_close(fd);

    int ok = (r == 4096);
    if (ok) {
        for (int i = 0; i < 4096; i++)
            if (rbuf[i] != 'B') { ok = 0; break; }
    }

    if (ok) pass(name);
    else fail(name, "data mismatch after reuse");

    sys_unlink("/s26reuse2.dat");
}

/* --- Test 5: nested directories --- */
static void test_nested_dirs(void) {
    const char *name = "nested directories";

    if (sys_mkdir("/s26a") != 0) { fail(name, "mkdir /s26a failed"); return; }
    if (sys_mkdir("/s26a/b") != 0) { fail(name, "mkdir /s26a/b failed"); sys_unlink("/s26a"); return; }

    long fd = sys_open("/s26a/b/deep.txt", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create deep file failed"); goto cleanup5; }

    const char *data = "deep nested";
    long dlen = (long)strlen(data);
    sys_fwrite(fd, data, dlen);
    sys_close(fd);

    /* Read back */
    fd = sys_open("/s26a/b/deep.txt", O_RDONLY);
    if (fd < 0) { fail(name, "re-open failed"); goto cleanup5; }

    char rbuf[32];
    long r = sys_read(fd, rbuf, 31);
    sys_close(fd);

    if (r == dlen) {
        int ok = 1;
        for (long i = 0; i < dlen; i++)
            if (rbuf[i] != data[i]) { ok = 0; break; }
        if (ok) pass(name);
        else fail(name, "data mismatch");
    } else {
        fail(name, "read length mismatch");
    }

cleanup5:
    sys_unlink("/s26a/b/deep.txt");
    sys_unlink("/s26a/b");
    sys_unlink("/s26a");
}

/* --- Test 6: initrd on disk --- */
static void test_initrd_on_disk(void) {
    const char *name = "initrd on disk";

    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name, "open /hello.txt failed"); return; }

    char rbuf[64];
    long r = sys_read(fd, rbuf, 63);
    sys_close(fd);

    if (r > 0) pass(name);
    else fail(name, "hello.txt empty or read failed");
}

/* --- Test 7: rename disk file --- */
static void test_rename(void) {
    const char *name = "rename disk file";

    long fd = sys_open("/s26rename.dat", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) { fail(name, "create failed"); return; }

    const char *data = "rename test";
    sys_fwrite(fd, data, (long)strlen(data));
    sys_close(fd);

    /* Rename */
    if (sys_rename("/s26rename.dat", "/s26renamed.dat") != 0) {
        fail(name, "rename failed");
        sys_unlink("/s26rename.dat");
        return;
    }

    /* Old path should be gone */
    fd = sys_open("/s26rename.dat", O_RDONLY);
    if (fd >= 0) {
        fail(name, "old path still exists");
        sys_close(fd);
        sys_unlink("/s26rename.dat");
        sys_unlink("/s26renamed.dat");
        return;
    }

    /* New path should work */
    fd = sys_open("/s26renamed.dat", O_RDONLY);
    if (fd < 0) { fail(name, "new path not found"); sys_unlink("/s26renamed.dat"); return; }

    char rbuf[32];
    long r = sys_read(fd, rbuf, 31);
    sys_close(fd);

    if (r == (long)strlen(data)) pass(name);
    else fail(name, "data size mismatch after rename");

    sys_unlink("/s26renamed.dat");
}

/* --- Test 8: readdir disk --- */
static void test_readdir(void) {
    const char *name = "readdir disk";

    if (sys_mkdir("/s26dir2") != 0) { fail(name, "mkdir failed"); return; }

    /* Create 3 files */
    const char *names[] = {"aaa.txt", "bbb.txt", "ccc.txt"};
    for (int i = 0; i < 3; i++) {
        char path[64];
        strcpy(path, "/s26dir2/");
        /* Manual concat */
        int pos = 9;
        const char *n = names[i];
        while (*n) path[pos++] = *n++;
        path[pos] = '\0';

        long fd = sys_open(path, O_CREAT | O_RDWR | O_TRUNC);
        if (fd < 0) { fail(name, "create sub-file failed"); goto cleanup8; }
        sys_fwrite(fd, "x", 1);
        sys_close(fd);
    }

    /* Read directory and count matches */
    int found[3] = {0, 0, 0};
    dirent_t ent;
    for (unsigned long i = 0; i < 16; i++) {
        if (sys_readdir("/s26dir2", i, &ent) != 0) break;
        for (int j = 0; j < 3; j++) {
            if (strcmp(ent.name, names[j]) == 0)
                found[j] = 1;
        }
    }

    if (found[0] && found[1] && found[2]) pass(name);
    else fail(name, "not all files found in readdir");

cleanup8:
    for (int i = 0; i < 3; i++) {
        char path[64];
        strcpy(path, "/s26dir2/");
        int pos = 9;
        const char *n = names[i];
        while (*n) path[pos++] = *n++;
        path[pos] = '\0';
        sys_unlink(path);
    }
    sys_unlink("/s26dir2");
}

int main(void) {
    printf("\ns26test: running %d tests\n", 8);

    test_disk_write_read();
    test_large_file();
    test_mkdir_file();
    test_block_reuse();
    test_nested_dirs();
    test_initrd_on_disk();
    test_rename();
    test_readdir();

    if (tests_passed == tests_total)
        printf("s26test: ALL PASSED (%d/%d)\n", tests_passed, tests_total);
    else
        printf("s26test: %d/%d passed\n", tests_passed, tests_total);

    return 0;
}
