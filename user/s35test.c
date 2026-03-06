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

/* Stat structure matching kernel vfs_stat_t (16 bytes) */
typedef struct {
    uint64_t size;
    uint8_t  type;
    uint8_t  pad1;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
} stat_t;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n=== Stage 35 Tests: Agent Security & Isolation ===\n\n");

    /* --- Test 1: Open 32+ fds (larger fd table) --- */
    {
        int count = 0;
        long fds[40];
        for (int i = 0; i < 40; i++) {
            fds[i] = sys_open("/hello.elf", O_RDONLY);
            if (fds[i] >= 0) count++;
            else break;
        }
        check(count >= 32, "1. Open 32+ fds simultaneously");
        for (int i = 0; i < count; i++)
            sys_close(fds[i]);
    }

    /* --- Test 2: getuid returns 0 (root) --- */
    {
        long uid = sys_getuid();
        check(uid == 0, "2. getuid returns 0 (root)");
    }

    /* --- Test 3: getgid returns 0 (root) --- */
    {
        long gid = sys_getgid();
        check(gid == 0, "3. getgid returns 0 (root)");
    }

    /* --- Test 4: setuid changes uid in forked child --- */
    {
        long child = sys_fork();
        if (child == 0) {
            /* Child */
            long rc = sys_setuid(1000);
            long uid = sys_getuid();
            if (rc == 0 && uid == 1000)
                sys_exit(42);
            else
                sys_exit(1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "4. setuid changes uid in forked child");
    }

    /* --- Test 5: Non-root setuid fails with -EPERM --- */
    {
        long child = sys_fork();
        if (child == 0) {
            sys_setuid(1000);  /* become non-root */
            /* Remove CAP_SETUID */
            long caps = sys_getcap();
            sys_setcap(0, caps & ~CAP_SETUID);
            long rc = sys_setuid(2000);
            if (rc == -EPERM)
                sys_exit(42);
            else
                sys_exit(1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "5. Non-root setuid fails with -EPERM");
    }

    /* --- Test 6: uid inherited on fork --- */
    {
        long child = sys_fork();
        if (child == 0) {
            sys_setuid(500);
            long grandchild = sys_fork();
            if (grandchild == 0) {
                long uid = sys_getuid();
                sys_exit(uid == 500 ? 42 : 1);
            }
            long status = sys_waitpid(grandchild);
            sys_exit(status == 42 ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "6. uid inherited on fork");
    }

    /* --- Test 7: getcap returns CAP_ALL for root --- */
    {
        long caps = sys_getcap();
        check(caps == CAP_ALL, "7. getcap returns CAP_ALL for root");
    }

    /* --- Test 8: setcap restricts child capabilities --- */
    {
        long child = sys_fork();
        if (child == 0) {
            long caps = sys_getcap();
            /* Remove CAP_KILL */
            sys_setcap(0, caps & ~CAP_KILL);
            long new_caps = sys_getcap();
            sys_exit((new_caps & CAP_KILL) == 0 ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "8. setcap restricts capabilities");
    }

    /* --- Test 9: Kill without CAP_KILL denied for different uid --- */
    {
        /* Create a target process */
        long target = sys_fork();
        if (target == 0) {
            /* Target: sleep for a while */
            timespec_t ts = { .tv_sec = 5, .tv_nsec = 0 };
            sys_nanosleep(&ts);
            sys_exit(0);
        }
        /* Create killer with different uid and no CAP_KILL */
        long killer = sys_fork();
        if (killer == 0) {
            sys_setuid(999);
            sys_setcap(0, CAP_ALL & ~CAP_KILL);
            long rc = sys_kill(target, SIGKILL);
            sys_exit(rc == -EPERM ? 42 : 1);
        }
        long kstatus = sys_waitpid(killer);
        check(kstatus == 42, "9. Kill without CAP_KILL denied");
        /* Clean up target */
        sys_kill(target, SIGKILL);
        sys_waitpid(target);
    }

    /* --- Test 10: Exec without CAP_EXEC denied --- */
    {
        long child = sys_fork();
        if (child == 0) {
            sys_setcap(0, CAP_ALL & ~CAP_EXEC);
            long rc = sys_exec("/hello.elf", NULL);
            /* exec should fail and return here */
            sys_exit(rc == -EACCES ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "10. Exec without CAP_EXEC denied");
    }

    /* --- Test 11: Create without CAP_FS_WRITE denied --- */
    {
        long child = sys_fork();
        if (child == 0) {
            sys_setcap(0, CAP_ALL & ~CAP_FS_WRITE);
            long rc = sys_create("/test_s35_denied.txt");
            sys_exit(rc == -EACCES ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "11. Create without CAP_FS_WRITE denied");
    }

    /* --- Test 12: fd count resource limit enforced --- */
    {
        long child = sys_fork();
        if (child == 0) {
            /* Close all inherited fds */
            for (int i = 0; i < 64; i++) sys_close(i);

            rlimit_t rl = { .current = 0, .max = 2 };
            sys_setrlimit(RLIMIT_NFDS, &rl);

            /* 0 fds open, limit=2. Open 2 should succeed, 3rd should fail */
            long fd1 = sys_open("/hello.elf", O_RDONLY);
            long fd2 = sys_open("/hello.elf", O_RDONLY);
            long fd3 = sys_open("/hello.elf", O_RDONLY);  /* should fail: 2 fds reached */
            if (fd1 >= 0 && fd2 >= 0 && fd3 == -EMFILE)
                sys_exit(42);
            else
                sys_exit(1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "12. fd count resource limit enforced");
    }

    /* --- Test 13: Memory resource limit enforced --- */
    {
        long child = sys_fork();
        if (child == 0) {
            rlimit_t rl = { .current = 0, .max = 2 };
            sys_setrlimit(RLIMIT_MEM, &rl);
            long addr1 = sys_mmap(1);
            long addr2 = sys_mmap(1);
            long addr3 = sys_mmap(1);  /* should fail */
            if (addr1 > 0 && addr2 > 0 && addr3 == -ENOMEM)
                sys_exit(42);
            else
                sys_exit(1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "13. Memory resource limit enforced");
    }

    /* --- Test 14: CPU time limit kills process --- */
    {
        long child = sys_fork();
        if (child == 0) {
            rlimit_t rl = { .current = 0, .max = 5 };  /* 5 ticks ~= 50ms */
            sys_setrlimit(RLIMIT_CPU, &rl);
            /* Spin until killed */
            volatile long x = 0;
            for (long i = 0; i < 100000000; i++) x += i;
            sys_exit((int)x);  /* should not reach here */
        }
        long status = sys_waitpid(child);
        check(status == -SIGKILL, "14. CPU time limit kills process");
    }

    /* --- Test 15: Seccomp denied syscall returns -EACCES --- */
    {
        long child = sys_fork();
        if (child == 0) {
            /* Allow only: write(0), exit(2), fork(40), waitpid(18), getpid(20), getuid(57) */
            uint64_t mask = (1ULL << 0) | (1ULL << 2) | (1ULL << 40) |
                            (1ULL << 18) | (1ULL << 20) | (1ULL << 57);
            /* Also allow seccomp(65) itself — but it's > 63, so it's auto-allowed */
            sys_seccomp(mask, 0);
            /* Try getgid(59) — should be denied */
            long rc = sys_getgid();
            sys_exit(rc == -EACCES ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "15. Seccomp denied syscall returns -EACCES");
    }

    /* --- Test 16: Seccomp allowed syscall works --- */
    {
        long child = sys_fork();
        if (child == 0) {
            uint64_t mask = (1ULL << 0) | (1ULL << 2) | (1ULL << 57);
            sys_seccomp(mask, 0);
            long uid = sys_getuid();  /* syscall 57, should be allowed */
            sys_exit(uid == 0 ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "16. Seccomp allowed syscall works");
    }

    /* --- Test 17: Seccomp cannot broaden mask --- */
    {
        long child = sys_fork();
        if (child == 0) {
            uint64_t mask1 = (1ULL << 0) | (1ULL << 2) | (1ULL << 57);
            sys_seccomp(mask1, 0);
            /* Try to add getgid(59) */
            uint64_t mask2 = mask1 | (1ULL << 59);
            sys_seccomp(mask2, 0);
            /* getgid should still be denied (AND of masks) */
            long rc = sys_getgid();
            sys_exit(rc == -EACCES ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "17. Seccomp cannot broaden mask");
    }

    /* --- Test 18: Seccomp inherited on fork --- */
    {
        long child = sys_fork();
        if (child == 0) {
            uint64_t mask = (1ULL << 0) | (1ULL << 2) | (1ULL << 40) |
                            (1ULL << 18) | (1ULL << 57);
            sys_seccomp(mask, 0);
            long grandchild = sys_fork();
            if (grandchild == 0) {
                /* getgid(59) should be denied in grandchild too */
                long rc = sys_getgid();
                sys_exit(rc == -EACCES ? 42 : 1);
            }
            long gstatus = sys_waitpid(grandchild);
            sys_exit(gstatus == 42 ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "18. Seccomp inherited on fork");
    }

    /* --- Test 19: setaudit succeeds for root --- */
    {
        long rc = sys_setaudit(0, AUDIT_SYSCALL);
        check(rc == 0, "19. setaudit succeeds for root");
        /* Disable audit to avoid spamming */
        sys_setaudit(0, 0);
    }

    /* --- Test 20: setaudit denied for non-root without CAP_SYS_ADMIN --- */
    {
        long child = sys_fork();
        if (child == 0) {
            sys_setuid(1000);
            sys_setcap(0, CAP_ALL & ~CAP_SYS_ADMIN);
            long rc = sys_setaudit(0, AUDIT_SYSCALL);
            sys_exit(rc == -EPERM ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "20. setaudit denied without CAP_SYS_ADMIN");
    }

    /* --- Test 21: File perm: owner-only file denies other uid --- */
    {
        /* Create a file as root with mode 0700 (owner only) */
        long fd = sys_create("/perm_test.txt");
        if (fd >= 0) {
            sys_fwrite(fd, "secret", 6);
            sys_close(fd);
            sys_chmod("/perm_test.txt", 0700);

            long child = sys_fork();
            if (child == 0) {
                sys_setuid(1000);
                long rc = sys_open("/perm_test.txt", O_RDONLY);
                sys_exit(rc == -EACCES ? 42 : 1);
            }
            long status = sys_waitpid(child);
            check(status == 42, "21. Owner-only file denies other uid");
            sys_unlink("/perm_test.txt");
        } else {
            check(0, "21. Owner-only file denies other uid (create failed)");
        }
    }

    /* --- Test 22: chmod denied for non-owner --- */
    {
        long fd = sys_create("/chmod_test.txt");
        if (fd >= 0) {
            sys_close(fd);
            long child = sys_fork();
            if (child == 0) {
                sys_setuid(1000);
                long rc = sys_chmod("/chmod_test.txt", 0777);
                sys_exit(rc == -EPERM ? 42 : 1);
            }
            long status = sys_waitpid(child);
            check(status == 42, "22. chmod denied for non-owner");
            sys_unlink("/chmod_test.txt");
        } else {
            check(0, "22. chmod denied for non-owner (create failed)");
        }
    }

    /* --- Test 23: setgid changes gid --- */
    {
        long child = sys_fork();
        if (child == 0) {
            sys_setgid(500);
            long gid = sys_getgid();
            sys_exit(gid == 500 ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "23. setgid changes gid");
    }

    /* --- Test 24: Open without CAP_FS_READ denied --- */
    {
        long child = sys_fork();
        if (child == 0) {
            sys_setcap(0, CAP_ALL & ~CAP_FS_READ);
            long rc = sys_open("/hello.elf", O_RDONLY);
            sys_exit(rc == -EACCES ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "24. Open without CAP_FS_READ denied");
    }

    /* --- Test 25: setrlimit denied for non-root without CAP_SYS_ADMIN --- */
    {
        long child = sys_fork();
        if (child == 0) {
            sys_setuid(1000);
            sys_setcap(0, CAP_ALL & ~CAP_SYS_ADMIN);
            rlimit_t rl = { .current = 0, .max = 100 };
            long rc = sys_setrlimit(RLIMIT_MEM, &rl);
            sys_exit(rc == -EPERM ? 42 : 1);
        }
        long status = sys_waitpid(child);
        check(status == 42, "25. setrlimit denied without CAP_SYS_ADMIN");
    }

    printf("\n=== Stage 35 Results: %d/%d passed ===\n",
           passed, passed + failed);
    if (failed == 0)
        printf("ALL PASSED\n");
    else
        printf("%d FAILED\n", failed);

    return (failed == 0) ? 0 : 1;
}
