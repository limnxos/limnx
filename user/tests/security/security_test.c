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

int main(void) {
    lt_suite("security");
    test_uid_gid();
    test_passwd();
    test_umask();
    return lt_done();
}
