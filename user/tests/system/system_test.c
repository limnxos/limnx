/*
 * system_test.c — System-level integration tests
 * Tests: init, coreutils, shell, env, login
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

static void test_init(void) {
    long fd = sys_open("/etc/inittab", 0);
    lt_ok(fd >= 0, "/etc/inittab exists");
    if (fd >= 0) sys_close(fd);

    fd = sys_open("/proc/1/status", 0);
    lt_ok(fd >= 0, "init (pid 1) in /proc");
    if (fd >= 0) sys_close(fd);
}

static void test_env(void) {
    char val[64];
    long ret = sys_getenv("LIMNX_VERSION", val, 64);
    lt_ok(ret >= 0, "LIMNX_VERSION env set");
    if (ret >= 0) {
        lt_ok(val[0] != '\0', "version is non-empty");
    } else {
        lt_ok(0, "version is non-empty");
    }
}

static void test_coreutils_exist(void) {
    const char *progs[] = {
        "/echo.elf", "/ls.elf", "/cat.elf", "/cp.elf", "/mv.elf",
        "/rm.elf", "/ps.elf", "/killcmd.elf", "/grep.elf",
        "/head.elf", "/tail.elf", "/wc.elf", "/env.elf",
        (void *)0
    };
    int all = 1;
    for (int i = 0; progs[i]; i++) {
        long fd = sys_open(progs[i], 0);
        if (fd < 0) all = 0;
        else sys_close(fd);
    }
    lt_ok(all, "all coreutils in initrd");
}

static void test_echo_exec(void) {
    long child = sys_fork();
    if (child == 0) {
        const char *argv[] = {"echo.elf", "test", (void *)0};
        sys_execve("/echo.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(child);
    lt_ok(st == 0, "echo.elf executes");
}

static void test_login_whoami(void) {
    long fd = sys_open("/login.elf", 0);
    lt_ok(fd >= 0, "login.elf exists");
    if (fd >= 0) sys_close(fd);

    long child = sys_fork();
    if (child == 0) {
        const char *argv[] = {"whoami.elf", (void *)0};
        sys_execve("/whoami.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(child);
    lt_ok(st == 0, "whoami.elf executes");
}

int main(void) {
    lt_suite("system");
    test_init();
    test_env();
    test_coreutils_exist();
    test_echo_exec();
    test_login_whoami();
    return lt_done();
}
