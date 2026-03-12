#include "libc/libc.h"

static int tests_passed = 0;
static int tests_failed = 0;

static void pass(const char *name) {
    printf("  [PASS] %s\n", name);
    tests_passed++;
}

static void fail(const char *name) {
    printf("  [FAIL] %s\n", name);
    tests_failed++;
}

int main(void) {
    printf("=== fstest: filesystem tests ===\n");

    /* Test 1: readdir root — verify known files exist */
    {
        dirent_t ent;
        int found_hello_txt = 0;
        int found_hello_elf = 0;
        int count = 0;
        for (unsigned long i = 0; sys_readdir("/", i, &ent) == 0; i++) {
            count++;
            if (strcmp(ent.name, "hello.txt") == 0) found_hello_txt = 1;
            if (strcmp(ent.name, "hello.elf") == 0) found_hello_elf = 1;
        }
        if (count > 0 && found_hello_txt && found_hello_elf)
            pass("readdir root");
        else {
            printf("    count=%d hello.txt=%d hello.elf=%d\n",
                   count, found_hello_txt, found_hello_elf);
            fail("readdir root");
        }
    }

    /* Test 2: mkdir /testdir (may already exist from previous boot on persistent disk) */
    {
        long rc = sys_mkdir("/testdir");
        /* If mkdir fails, check if it already exists as a directory */
        uint8_t st[16];
        if (rc >= 0 || sys_stat("/testdir", st) == 0) {
            if (sys_stat("/testdir", st) == 0) {
                uint8_t type = st[8];
                if (type == 1)  /* VFS_DIRECTORY */
                    pass("mkdir /testdir");
                else {
                    printf("    type=%u (expected 1)\n", type);
                    fail("mkdir /testdir");
                }
            } else {
                fail("mkdir /testdir (stat failed)");
            }
        } else {
            fail("mkdir /testdir (mkdir returned error)");
        }
    }

    /* Test 3: create /testdir/file.txt — write, close, reopen, read, verify */
    {
        long fd = sys_create("/testdir/file.txt");
        if (fd < 0) {
            fail("create /testdir/file.txt");
        } else {
            const char *msg = "hello from subdir";
            unsigned long mlen = strlen(msg);
            long w = sys_fwrite(fd, msg, mlen);
            sys_close(fd);

            if (w != (long)mlen) {
                fail("write /testdir/file.txt");
            } else {
                /* Reopen and read */
                fd = sys_open("/testdir/file.txt", 0);
                if (fd < 0) {
                    fail("reopen /testdir/file.txt");
                } else {
                    char buf[64];
                    long n = sys_read(fd, buf, sizeof(buf) - 1);
                    sys_close(fd);
                    if (n == (long)mlen) {
                        buf[n] = '\0';
                        if (strcmp(buf, msg) == 0)
                            pass("create+write+read /testdir/file.txt");
                        else
                            fail("content mismatch /testdir/file.txt");
                    } else {
                        printf("    read returned %ld (expected %lu)\n", n, mlen);
                        fail("read /testdir/file.txt");
                    }
                }
            }
        }
    }

    /* Test 4: readdir /testdir — verify file.txt appears */
    {
        dirent_t ent;
        int found = 0;
        for (unsigned long i = 0; sys_readdir("/testdir", i, &ent) == 0; i++) {
            if (strcmp(ent.name, "file.txt") == 0) found = 1;
        }
        if (found)
            pass("readdir /testdir");
        else
            fail("readdir /testdir");
    }

    /* Test 5: readdir / — verify testdir appears */
    {
        dirent_t ent;
        int found = 0;
        for (unsigned long i = 0; sys_readdir("/", i, &ent) == 0; i++) {
            if (strcmp(ent.name, "testdir") == 0 && ent.type == 1) found = 1;
        }
        if (found)
            pass("readdir / has testdir");
        else
            fail("readdir / has testdir");
    }

    /* Test 6: unlink /testdir/file.txt — readdir /testdir is empty */
    {
        long rc = sys_unlink("/testdir/file.txt");
        if (rc != 0) {
            fail("unlink /testdir/file.txt");
        } else {
            dirent_t ent;
            int count = 0;
            for (unsigned long i = 0; sys_readdir("/testdir", i, &ent) == 0; i++)
                count++;
            if (count == 0)
                pass("unlink /testdir/file.txt");
            else {
                printf("    %d entries remain\n", count);
                fail("unlink /testdir/file.txt (not empty)");
            }
        }
    }

    /* Test 7: legacy path /hello.txt — still opens correctly */
    {
        long fd = sys_open("/hello.txt", 0);
        if (fd >= 0) {
            char buf[32];
            long n = sys_read(fd, buf, sizeof(buf) - 1);
            sys_close(fd);
            if (n > 0)
                pass("legacy /hello.txt open+read");
            else
                fail("legacy /hello.txt read");
        } else {
            fail("legacy /hello.txt open");
        }
    }

    /* Test 8: root is a directory */
    {
        uint8_t st[16];
        if (sys_stat("/", st) == 0) {
            uint8_t type = st[8];
            if (type == 1) {
                pass("stat / (directory)");
            } else {
                fail("stat / (not directory)");
            }
        } else {
            fail("stat / failed");
        }
    }

    /* Summary */
    printf("=== fstest: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    if (tests_failed == 0)
        printf("=== fstest: ALL PASSED ===\n");

    return tests_failed;
}
