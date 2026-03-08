/*
 * s50test.c — Stage 50 Tests: File-Backed mmap
 *
 * Tests lazy file-backed memory mapping where pages are loaded
 * from disk on first access via page faults.
 */

#include "libc/libc.h"

static int test_num = 0;
static int pass_count = 0;

static void check(int ok, const char *desc) {
    test_num++;
    if (ok) {
        printf("  [%d] PASS: %s\n", test_num, desc);
        pass_count++;
    } else {
        printf("  [%d] FAIL: %s\n", test_num, desc);
    }
}

static void test_mmap_file_basic(void) {
    /* mmap a known initrd file and read its contents via memory access */
    long fd = sys_open("/hello.elf", O_RDONLY);
    if (fd < 0) {
        check(0, "mmap_file basic: could not open /hello.elf");
        return;
    }

    /* Get file size */
    uint8_t st[16];
    long sr = sys_stat("/hello.elf", st);
    if (sr < 0) {
        check(0, "mmap_file basic: could not stat /hello.elf");
        sys_close(fd);
        return;
    }

    uint64_t fsize = *(uint64_t *)st;
    long num_pages = (fsize + 4095) / 4096;
    long addr = sys_mmap_file(fd, 0, num_pages);

    /* Test 1: mmap_file returns valid address */
    check(addr > 0, "mmap_file returns valid address");

    /* Test 2: first bytes are ELF magic (0x7f 'E' 'L' 'F') */
    if (addr > 0) {
        unsigned char *p = (unsigned char *)addr;
        check(p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F',
              "mmap_file content has ELF magic header");
    } else {
        check(0, "mmap_file content has ELF magic header");
    }

    sys_close(fd);
    if (addr > 0)
        sys_munmap(addr);
}

static void test_mmap_file_matches_read(void) {
    /* Compare mmap content with sys_read content */
    const char *path = "/hello.elf";
    long fd1 = sys_open(path, O_RDONLY);
    long fd2 = sys_open(path, O_RDONLY);
    if (fd1 < 0 || fd2 < 0) {
        check(0, "mmap vs read: could not open file");
        if (fd1 >= 0) sys_close(fd1);
        if (fd2 >= 0) sys_close(fd2);
        return;
    }

    uint8_t st[16];
    sys_stat(path, st);
    uint64_t fsize = *(uint64_t *)st;
    long num_pages = (fsize + 4095) / 4096;

    /* mmap via fd1 */
    long addr = sys_mmap_file(fd1, 0, num_pages);
    if (addr <= 0) {
        check(0, "mmap vs read: mmap failed");
        sys_close(fd1);
        sys_close(fd2);
        return;
    }

    /* Read first 128 bytes via fd2 */
    char read_buf[128];
    long n = sys_read(fd2, read_buf, 128);

    /* Test 3: mmap content matches read content */
    int match = 1;
    if (n > 0) {
        unsigned char *p = (unsigned char *)addr;
        for (long i = 0; i < n; i++) {
            if (p[i] != (unsigned char)read_buf[i]) {
                match = 0;
                break;
            }
        }
    } else {
        match = 0;
    }
    check(match, "mmap content matches sys_read content");

    sys_close(fd1);
    sys_close(fd2);
    sys_munmap(addr);
}

static void test_mmap_file_offset(void) {
    /* mmap with non-zero offset into file */
    const char *path = "/hello.elf";
    long fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        check(0, "mmap offset: could not open file");
        return;
    }

    /* mmap starting at offset 4096 (second page of file) */
    uint8_t st[16];
    sys_stat(path, st);
    uint64_t fsize = *(uint64_t *)st;
    if (fsize <= 4096) {
        /* File too small for this test, skip gracefully */
        check(1, "mmap with offset reads correct data (skipped: file too small)");
        sys_close(fd);
        return;
    }

    long addr = sys_mmap_file(fd, 4096, 1);

    /* Read the same offset via sys_read for comparison */
    long fd2 = sys_open(path, O_RDONLY);
    sys_seek(fd2, 4096, 0);  /* SEEK_SET */
    char read_buf[64];
    long n = sys_read(fd2, read_buf, 64);
    sys_close(fd2);

    /* Test 4: offset mmap matches read at same offset */
    int match = 1;
    if (addr > 0 && n > 0) {
        unsigned char *p = (unsigned char *)addr;
        for (long i = 0; i < n && i < 64; i++) {
            if (p[i] != (unsigned char)read_buf[i]) {
                match = 0;
                break;
            }
        }
    } else {
        match = 0;
    }
    check(match, "mmap with offset reads correct data");

    sys_close(fd);
    if (addr > 0) sys_munmap(addr);
}

static void test_mmap_file_past_eof(void) {
    /* Create a small file (< 1 page), mmap 1 page, verify zero-fill past EOF */
    sys_create("/tmp/mmaptest.txt");
    long fd = sys_open("/tmp/mmaptest.txt", O_WRONLY);
    if (fd >= 0) {
        sys_fwrite(fd, "hello-mmap", 10);
        sys_close(fd);
    }

    fd = sys_open("/tmp/mmaptest.txt", O_RDONLY);
    if (fd < 0) {
        check(0, "mmap past EOF: could not open file");
        return;
    }

    long addr = sys_mmap_file(fd, 0, 1);

    /* Test 5: first bytes match file content */
    int content_ok = 0;
    int zeros_ok = 0;
    if (addr > 0) {
        unsigned char *p = (unsigned char *)addr;
        content_ok = (p[0] == 'h' && p[1] == 'e' && p[2] == 'l' &&
                      p[3] == 'l' && p[4] == 'o');
        /* Bytes past file size should be zero */
        zeros_ok = 1;
        for (int i = 10; i < 64; i++) {
            if (p[i] != 0) { zeros_ok = 0; break; }
        }
    }
    check(content_ok, "mmap reads file content correctly");

    /* Test 6: bytes past EOF are zero-filled */
    check(zeros_ok, "mmap zero-fills past EOF");

    sys_close(fd);
    if (addr > 0) sys_munmap(addr);
    sys_unlink("/tmp/mmaptest.txt");
}

static void test_mmap_file_disk(void) {
    /* Create a disk file, write data, mmap it */
    sys_mkdir("/tmp");
    sys_create("/tmp/diskmap.txt");
    long fd = sys_open("/tmp/diskmap.txt", O_WRONLY);
    if (fd >= 0) {
        const char *data = "disk-backed-mmap-test-data-1234567890";
        sys_fwrite(fd, data, 37);
        sys_close(fd);
    }

    fd = sys_open("/tmp/diskmap.txt", O_RDONLY);
    if (fd < 0) {
        check(0, "disk mmap: could not open file");
        return;
    }

    long addr = sys_mmap_file(fd, 0, 1);

    /* Test 7: disk-backed file content via mmap */
    int ok = 0;
    if (addr > 0) {
        unsigned char *p = (unsigned char *)addr;
        ok = (p[0] == 'd' && p[1] == 'i' && p[2] == 's' && p[3] == 'k');
    }
    check(ok, "disk-backed file readable via mmap");

    sys_close(fd);
    if (addr > 0) sys_munmap(addr);
    sys_unlink("/tmp/diskmap.txt");
}

static void test_mmap_file_fd_close(void) {
    /* mmap a file, close the fd, verify data is still accessible */
    long fd = sys_open("/hello.elf", O_RDONLY);
    if (fd < 0) {
        check(0, "mmap after fd close: could not open file");
        return;
    }

    long addr = sys_mmap_file(fd, 0, 1);
    sys_close(fd);  /* close fd before accessing mmap */

    /* Test 8: data still accessible after fd close */
    int ok = 0;
    if (addr > 0) {
        unsigned char *p = (unsigned char *)addr;
        /* Access triggers page fault — fd is closed but vfs_node is still valid */
        ok = (p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F');
    }
    check(ok, "mmap data accessible after fd close");

    if (addr > 0) sys_munmap(addr);
}

int main(void) {
    printf("=== Stage 50 Tests: File-Backed mmap ===\n\n");

    test_mmap_file_basic();
    test_mmap_file_matches_read();
    test_mmap_file_offset();
    test_mmap_file_past_eof();
    test_mmap_file_disk();
    test_mmap_file_fd_close();

    printf("\n=== Stage 50 Results: %d/%d passed ===\n", pass_count, test_num);
    return 0;
}
