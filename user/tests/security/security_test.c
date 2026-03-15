/*
 * security_test.c — Permission and security tests
 * Tests: uid/gid, capabilities, umask
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

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
    sys_open("/sec_test_file.txt", 0x100 | 2);
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

int main(void) {
    lt_suite("security");
    test_uid_gid();
    test_passwd();
    test_umask();
    test_chmod_chown();
    test_capabilities();
    test_setuid_setgid();
    return lt_done();
}
