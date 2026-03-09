/* s30test.c — Stage 30 tests: Block Cache DLL, File Permissions, Signals,
 *             Process Groups, Shared Memory, Disk Persistence */
#include "libc/libc.h"

static int passed = 0;
static int failed = 0;

static void check(int ok, const char *name) {
    if (ok) {
        printf("  [PASS] %s\n", name);
        passed++;
    } else {
        printf("  [FAIL] %s\n", name);
        failed++;
    }
}



/* Helper: convert long to decimal string */
static void ltoa(long val, char *buf) {
    if (val < 0) { *buf++ = '-'; val = -val; }
    char tmp[20];
    int i = 0;
    if (val == 0) tmp[i++] = '0';
    while (val > 0) { tmp[i++] = '0' + (int)(val % 10); val /= 10; }
    for (int j = i - 1; j >= 0; j--) *buf++ = tmp[j];
    *buf = '\0';
}

/* ===== Shared memory child mode ===== */
static void shm_child_main(int argc, char **argv) {
    /* argv: s30test.elf --shm-child <shmid> */
    if (argc < 3) sys_exit(1);
    long shmid = atol(argv[2]);
    long addr = sys_shmat(shmid);
    if (addr == 0) sys_exit(2);

    volatile uint64_t *ptr = (volatile uint64_t *)addr;
    /* Check magic written by parent */
    if (ptr[0] != 0xDEADBEEFCAFE0001ULL)
        sys_exit(3);
    /* Write response */
    ptr[1] = 0xBEEFCAFEDEAD0002ULL;
    sys_shmdt(addr);
    sys_exit(0);
}

/* ===== SIGSTOP helper child mode ===== */
static void stop_child_main(int argc, char **argv) {
    /* argv: s30test.elf --stop-child <wfd> */
    if (argc < 3) sys_exit(1);
    long wfd = atol(argv[2]);
    /* Write a marker to indicate we're running */
    sys_fwrite(wfd, "A", 1);
    /* Busy loop — parent will SIGSTOP us, then SIGCONT */
    for (volatile int i = 0; i < 500; i++)
        sys_yield();
    /* After resume, write another marker */
    sys_fwrite(wfd, "B", 1);
    sys_close(wfd);
    sys_exit(0);
}

/* ===== Group kill child mode ===== */
static void group_child_main(void) {
    /* Just loop until killed */
    for (;;) sys_yield();
}

/* ===== SIGINT child mode ===== */
static void sigint_child_main(void) {
    /* Loop doing syscalls so SIGINT can be delivered */
    for (;;) sys_yield();
}

int main(int argc, char **argv) {
    /* Check for child modes */
    if (argc >= 2) {
        if (strcmp(argv[1], "--shm-child") == 0) {
            shm_child_main(argc, argv);
            return 0;
        }
        if (strcmp(argv[1], "--stop-child") == 0) {
            stop_child_main(argc, argv);
            return 0;
        }
        if (strcmp(argv[1], "--group-child") == 0) {
            group_child_main();
            return 0;
        }
        if (strcmp(argv[1], "--sigint-child") == 0) {
            sigint_child_main();
            return 0;
        }
    }

    printf("\n=== Stage 30 Tests ===\n");

    /* --- Test 1: bcache DLL correctness --- */
    {
        const char *path = "/s30_cache_test";
        long fd = sys_open(path, O_CREAT | O_RDWR);
        if (fd < 0) fd = sys_create(path);
        char wbuf[128];
        for (int i = 0; i < 128; i++) wbuf[i] = (char)(i ^ 0x5A);
        sys_fwrite(fd, wbuf, 128);
        sys_close(fd);

        fd = sys_open(path, O_RDONLY);
        char rbuf[128];
        memset(rbuf, 0, 128);
        sys_read(fd, rbuf, 128);
        sys_close(fd);

        int ok = 1;
        for (int i = 0; i < 128; i++) {
            if (rbuf[i] != (char)(i ^ 0x5A)) { ok = 0; break; }
        }
        check(ok, "bcache DLL correctness");
        sys_unlink(path);
    }

    /* --- Test 2: bcache DLL eviction --- */
    {
        /* Write multiple files to force cache evictions (>256 blocks) */
        int ok = 1;
        for (int f = 0; f < 4; f++) {
            char path[32];
            path[0] = '/'; path[1] = 's'; path[2] = '3'; path[3] = '0';
            path[4] = '_'; path[5] = 'e'; path[6] = 'v'; path[7] = 'i';
            path[8] = 'c'; path[9] = 't'; path[10] = '0' + (char)f;
            path[11] = '\0';
            long fd = sys_open(path, O_CREAT | O_RDWR);
            if (fd < 0) fd = sys_create(path);
            /* Write 256KB = 64 blocks per file = 256 blocks total */
            char buf[512];
            for (int i = 0; i < 512; i++) buf[i] = (char)(f * 64 + (i & 63));
            for (int b = 0; b < 512; b++)
                sys_fwrite(fd, buf, 512);
            sys_close(fd);
        }
        /* Read back first file and verify */
        long fd = sys_open("/s30_evict0", O_RDONLY);
        if (fd >= 0) {
            char buf[512];
            long n = sys_read(fd, buf, 512);
            if (n >= 512) {
                for (int i = 0; i < 512; i++) {
                    if (buf[i] != (char)(0 * 64 + (i & 63))) { ok = 0; break; }
                }
            } else {
                ok = 0;
            }
            sys_close(fd);
        } else {
            ok = 0;
        }
        check(ok, "bcache DLL eviction");
        for (int f = 0; f < 4; f++) {
            char path[32];
            path[0] = '/'; path[1] = 's'; path[2] = '3'; path[3] = '0';
            path[4] = '_'; path[5] = 'e'; path[6] = 'v'; path[7] = 'i';
            path[8] = 'c'; path[9] = 't'; path[10] = '0' + (char)f;
            path[11] = '\0';
            sys_unlink(path);
        }
    }

    /* --- Test 3: chmod read-only --- */
    {
        const char *path = "/s30_chmod_test";
        long fd = sys_open(path, O_CREAT | O_RDWR);
        if (fd < 0) fd = sys_create(path);
        sys_fwrite(fd, "hello", 5);
        sys_close(fd);

        /* chmod to read-only */
        long rc = sys_chmod(path, 0x04);  /* VFS_PERM_READ only */
        check(rc == 0, "chmod read-only");
        /* Try to open for write — should fail */
        fd = sys_open(path, O_WRONLY);
        int write_blocked = (fd < 0);
        if (fd >= 0) sys_close(fd);
        if (!write_blocked) {
            /* Even if open succeeded, try write via fwrite */
            fd = sys_open(path, O_RDWR);
            if (fd < 0) write_blocked = 1;
            else sys_close(fd);
        }
        /* At minimum, chmod should have set mode to read-only */
        check(write_blocked, "chmod blocks write");
    }

    /* --- Test 4: chmod restore write --- */
    {
        const char *path = "/s30_chmod_test";
        long rc = sys_chmod(path, 0x06);  /* read + write */
        check(rc == 0, "chmod restore write");
        long fd = sys_open(path, O_RDWR);
        int ok = (fd >= 0);
        if (fd >= 0) {
            long w = sys_fwrite(fd, "world", 5);
            ok = ok && (w == 5);
            sys_close(fd);
        }
        check(ok, "write after chmod restore");
        sys_unlink(path);
    }

    /* --- Test 5: SIGINT delivery --- */
    {
        const char *args[] = { "/s30test.elf", "--sigint-child", NULL };
        long child_pid = sys_exec("/s30test.elf", args);
        int ok = (child_pid > 0);
        if (ok) {
            /* Let child start */
            for (int i = 0; i < 50; i++) sys_yield();
            /* Send SIGINT */
            sys_kill(child_pid, SIGINT);
            long status = sys_waitpid(child_pid);
            ok = (status == -SIGINT);
        }
        check(ok, "SIGINT delivery");
    }

    /* --- Test 6: SIGSTOP + SIGCONT --- */
    {
        long rfd = -1, wfd = -1;
        sys_pipe(&rfd, &wfd);

        /* Pass write fd number to child as argument */
        char wfd_str[16];
        ltoa(wfd, wfd_str);
        const char *args[] = { "/s30test.elf", "--stop-child", wfd_str, NULL };

        /* Set read end to non-blocking */
        sys_fcntl(rfd, F_SETFL, O_NONBLOCK);
        /* Set cloexec on read fd so child doesn't inherit it */
        sys_fcntl(rfd, F_SETFD, FD_CLOEXEC);

        long child_pid = sys_exec("/s30test.elf", args);
        int ok = (child_pid > 0);
        if (ok) {
            /* Wait for child's first marker 'A' */
            char buf[4];
            int got_a = 0;
            for (int i = 0; i < 500 && !got_a; i++) {
                if (sys_read(rfd, buf, 1) == 1 && buf[0] == 'A')
                    got_a = 1;
                else
                    sys_yield();
            }
            ok = ok && got_a;

            /* SIGSTOP the child */
            sys_kill(child_pid, SIGSTOP);
            for (int i = 0; i < 100; i++) sys_yield();

            /* While stopped, child should NOT write 'B' */
            int got_b_while_stopped = 0;
            for (int i = 0; i < 50; i++) {
                if (sys_read(rfd, buf, 1) == 1 && buf[0] == 'B')
                    got_b_while_stopped = 1;
                sys_yield();
            }
            ok = ok && !got_b_while_stopped;

            /* SIGCONT the child */
            sys_kill(child_pid, SIGCONT);

            /* Wait for 'B' */
            int got_b = 0;
            for (int i = 0; i < 1000 && !got_b; i++) {
                if (sys_read(rfd, buf, 1) == 1 && buf[0] == 'B')
                    got_b = 1;
                else
                    sys_yield();
            }
            ok = ok && got_b;

            sys_waitpid(child_pid);
        }
        sys_close(rfd);
        sys_close(wfd);
        check(ok, "SIGSTOP + SIGCONT");
    }

    /* --- Test 7: process group default --- */
    {
        long my_pgid = sys_getpgid(0);
        const char *args[] = { "/s30test.elf", "--sigint-child", NULL };
        long child_pid = sys_exec("/s30test.elf", args);
        int ok = (child_pid > 0);
        if (ok) {
            long child_pgid = sys_getpgid(child_pid);
            ok = (child_pgid == my_pgid);
            sys_kill(child_pid, SIGKILL);
            sys_waitpid(child_pid);
        }
        check(ok, "process group default (inherit pgid)");
    }

    /* --- Test 8: setpgid + getpgid --- */
    {
        const char *args[] = { "/s30test.elf", "--sigint-child", NULL };
        long child_pid = sys_exec("/s30test.elf", args);
        int ok = (child_pid > 0);
        if (ok) {
            /* Put child in its own group */
            long rc = sys_setpgid(child_pid, child_pid);
            ok = ok && (rc == 0);
            long pgid = sys_getpgid(child_pid);
            ok = ok && (pgid == child_pid);
            sys_kill(child_pid, SIGKILL);
            sys_waitpid(child_pid);
        }
        check(ok, "setpgid + getpgid");
    }

    /* --- Test 9: group kill --- */
    {
        const char *args[] = { "/s30test.elf", "--group-child", NULL };
        long child1 = sys_exec("/s30test.elf", args);
        long child2 = sys_exec("/s30test.elf", args);
        int ok = (child1 > 0 && child2 > 0);
        if (ok) {
            /* Put both children in same group (child1's pid as pgid) */
            sys_setpgid(child1, child1);
            sys_setpgid(child2, child1);

            /* Verify both have same pgid */
            ok = ok && (sys_getpgid(child1) == child1);
            ok = ok && (sys_getpgid(child2) == child1);

            for (int i = 0; i < 20; i++) sys_yield();

            /* Kill the group with negative pid */
            long rc = sys_kill(-child1, SIGKILL);
            ok = ok && (rc == 0);

            long s1 = sys_waitpid(child1);
            long s2 = sys_waitpid(child2);
            ok = ok && (s1 == -SIGKILL) && (s2 == -SIGKILL);
        }
        check(ok, "group kill (negative pid)");
    }

    /* --- Test 10: shmget + shmat --- */
    {
        long shmid = sys_shmget(42, 1);  /* 1 page, key=42 */
        int ok = (shmid >= 0);
        if (ok) {
            long addr = sys_shmat(shmid);
            ok = ok && (addr != 0);
            if (addr != 0) {
                volatile uint64_t *ptr = (volatile uint64_t *)addr;
                ptr[0] = 0x1234567890ABCDEFULL;
                ok = ok && (ptr[0] == 0x1234567890ABCDEFULL);
                sys_shmdt(addr);
            }
        }
        check(ok, "shmget + shmat");
    }

    /* --- Test 11: shared memory IPC (cross-exec) --- */
    {
        long shmid = sys_shmget(99, 1);  /* key=99, 1 page */
        int ok = (shmid >= 0);
        if (ok) {
            long addr = sys_shmat(shmid);
            ok = ok && (addr != 0);
            if (addr != 0) {
                volatile uint64_t *ptr = (volatile uint64_t *)addr;
                ptr[0] = 0xDEADBEEFCAFE0001ULL;
                ptr[1] = 0;

                /* Exec child with shmid as argument */
                char shmid_str[16];
                ltoa(shmid, shmid_str);
                const char *args[] = { "/s30test.elf", "--shm-child", shmid_str, NULL };
                long child_pid = sys_exec("/s30test.elf", args);
                ok = ok && (child_pid > 0);
                if (child_pid > 0) {
                    long status = sys_waitpid(child_pid);
                    ok = ok && (status == 0);
                    /* Check child's response */
                    ok = ok && (ptr[1] == 0xBEEFCAFEDEAD0002ULL);
                }
                sys_shmdt(addr);
            }
        }
        check(ok, "shared memory IPC (cross-exec)");
    }

    /* --- Test 12: disk persistence --- */
    {
        const char *path = "/s30_persist_test";
        long fd = sys_open(path, O_CREAT | O_RDWR);
        if (fd < 0) fd = sys_create(path);
        const char *data = "Stage30-Persistence-OK";
        long wlen = (long)strlen(data);
        sys_fwrite(fd, data, (unsigned long)wlen);
        sys_close(fd);

        /* Re-open and verify */
        fd = sys_open(path, O_RDONLY);
        char rbuf[64];
        memset(rbuf, 0, 64);
        long n = sys_read(fd, rbuf, 63);
        sys_close(fd);

        int ok = (n == wlen) && (strcmp(rbuf, data) == 0);
        check(ok, "disk persistence");
        sys_unlink(path);
    }

    /* --- Summary --- */
    printf("\ns30test: %d passed, %d failed\n", passed, failed);
    if (failed == 0)
        printf("s30test: ALL PASSED\n");
    else
        printf("s30test: SOME TESTS FAILED\n");

    return failed;
}
