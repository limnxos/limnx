/*
 * security_test.c — Permission and security tests
 * Tests: uid/gid, capabilities, umask
 * Portable — no arch-specific code.
 */
#include "../limntest.h"
#include "limnx/syscall_nr.h"

static void test_uid_gid(void) {
    long uid = sys_getuid();
    long gid = sys_getgid();
    lt_ok(uid >= 0, "getuid returns value");
    lt_ok(gid >= 0, "getgid returns value");
    long euid = sys_geteuid();
    long egid = sys_getegid();
    lt_ok(euid >= 0, "geteuid returns value");
    lt_ok(egid >= 0, "getegid returns value");
}

static void test_passwd(void) {
    long fd = sys_open("/etc/passwd", 0);
    lt_ok(fd >= 0, "/etc/passwd exists");
    if (fd >= 0) {
        char buf[256];
        long n = sys_read(fd, buf, 255);
        if (n > 0) buf[n] = '\0';
        lt_ok(lt_strcontains(buf, "root"), "passwd contains root");
        sys_close(fd);
    } else {
        lt_ok(0, "passwd contains root");
    }
}

static void test_umask(void) {
    long old = sys_umask(0077);
    lt_ok(old >= 0, "umask returns old value");
    sys_umask(old);  /* restore */
}

static void test_chmod_chown(void) {
    sys_open("/sec_test_file.txt", O_CREAT | O_RDWR);
    long ret = sys_chmod("/sec_test_file.txt", 0755);
    lt_ok(ret == 0, "chmod succeeds");
    ret = sys_chown("/sec_test_file.txt", 0, 0);
    lt_ok(ret == 0, "chown succeeds");
}

static void test_capabilities(void) {
    long caps = sys_getcap();
    lt_ok(caps != 0, "process has capabilities");
}

static void test_setuid_setgid(void) {
    /* We're running as root — verify we can set uid/gid */
    long ret = sys_setuid(0);
    lt_ok(ret == 0, "setuid(0) succeeds as root");
    ret = sys_setgid(0);
    lt_ok(ret == 0, "setgid(0) succeeds as root");
}

static void test_seccomp(void) {
    /* Fork a child, apply seccomp, try a blocked syscall */
    long pid = sys_fork();
    if (pid == 0) {
        /* Child: apply seccomp with arch-aware numbers */
        unsigned long mask_lo = 0;
        unsigned long mask_hi = 0;
        #define SALLOW(nr) do { \
            if ((nr) < 64) mask_lo |= (1UL << (nr)); \
            else if ((nr) < 128) mask_hi |= (1UL << ((nr) - 64)); \
        } while(0)
        SALLOW(SYS_READ);
        SALLOW(SYS_WRITE);
        SALLOW(SYS_SCHED_YIELD);
        SALLOW(SYS_GETPID);
        SALLOW(SYS_EXIT);
        SALLOW(SYS_EXIT_GROUP);
        SALLOW(SYS_SECCOMP);
        #undef SALLOW
        sys_seccomp(mask_lo, 0 /* not strict — return EACCES */, mask_hi);

        /* getpid should work (allowed) */
        long my_pid = sys_getpid();
        if (my_pid > 0) {
            /* Try fork — should be blocked (-EACCES) */
            long ret = sys_fork();
            if (ret == -13 /* EACCES */) {
                sys_exit(42);  /* success signal */
            }
            sys_exit(99);
        }
        sys_exit(99);
    }

    lt_ok(pid > 0, "seccomp test fork");
    if (pid > 0) {
        long status = sys_waitpid(pid);
        lt_ok(status == 42, "seccomp blocks fork (child exit=42)");
    }
}

static void test_seccomp_strict(void) {
    /* Strict mode: blocked syscall → SIGKILL.
     * Use arch-aware syscall numbers for the allowlist. */
    long pid = sys_fork();
    if (pid == 0) {
        unsigned long mask_lo = 0;
        unsigned long mask_hi = 0;
        #define SALLOW(nr) do { \
            if ((nr) < 64) mask_lo |= (1UL << (nr)); \
            else if ((nr) < 128) mask_hi |= (1UL << ((nr) - 64)); \
        } while(0)
        SALLOW(SYS_WRITE);
        SALLOW(SYS_EXIT);
        SALLOW(SYS_EXIT_GROUP);
        #undef SALLOW
        sys_seccomp(mask_lo, 1 /* strict */, mask_hi);

        /* Try getpid — not in allowlist, strict → SIGKILL */
        sys_getpid();
        /* Should never reach here — write a marker to prove */
        sys_write("ALIVE", 5);
        sys_exit(99);
    }

    lt_ok(pid > 0, "seccomp strict fork");
    if (pid > 0) {
        long status = sys_waitpid(pid);
        /* Child killed → status should NOT be 99 (normal exit) */
        lt_ok(status != 99 && status != 0, "seccomp strict kills on blocked syscall");
    }
}

int main(void) {
    lt_suite("security");
    test_uid_gid();
    test_passwd();
    test_umask();
    test_chmod_chown();
    test_capabilities();
    test_setuid_setgid();
    test_seccomp();
    test_seccomp_strict();
    return lt_done();
}
