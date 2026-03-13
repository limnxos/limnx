/*
 * s96test.c — Stage 96: POSIX permission system test
 *
 * Tests: euid/egid, setuid/setgid POSIX semantics, umask,
 *        chown/fchown, supplementary groups, file creation ownership.
 */

#include "../libc/libc.h"

static int tests_passed = 0;
static int tests_failed = 0;

static void check(const char *name, int cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        tests_passed++;
    } else {
        printf("  FAIL: %s\n", name);
        tests_failed++;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== Stage 96: POSIX Permission System Test ===\n\n");

    long ret;

    /* --- Test 1: geteuid/getegid --- */
    printf("[1] Effective UID/GID\n");
    long euid = sys_geteuid();
    long egid = sys_getegid();
    long uid = sys_getuid();
    long gid = sys_getgid();
    check("geteuid returns value", euid >= 0);
    check("getegid returns value", egid >= 0);
    check("euid matches uid (no setuid)", euid == uid);
    check("egid matches gid (no setgid)", egid == gid);

    /* --- Test 2: umask --- */
    printf("\n[2] Umask\n");
    long old_mask = sys_umask(0077);
    check("umask returns old mask", old_mask >= 0);
    long cur_mask = sys_umask(0022);
    check("umask(0077) was set", cur_mask == 0077);
    long restored = sys_umask(0022);
    check("umask(0022) restored", restored == 0022);

    /* --- Test 3: file creation ownership (as root, before priv drop) --- */
    printf("\n[3] File creation ownership\n");
    ret = sys_create("/s96_owner_test");
    if (ret >= 0) {
        struct {
            uint64_t size;
            uint8_t type;
            uint8_t pad1;
            uint16_t mode;
            uint16_t uid;
            uint16_t gid;
        } st;
        ret = sys_stat("/s96_owner_test", &st);
        if (ret == 0) {
            check("created file uid matches euid(0)", st.uid == 0);
            check("created file gid matches egid(0)", st.gid == 0);
        }
        sys_unlink("/s96_owner_test");
    } else {
        printf("  SKIP: could not create test file\n");
    }

    /* --- Test 4: umask effect on file creation --- */
    printf("\n[4] Umask effect on creation\n");
    sys_umask(0077);
    ret = sys_create("/s96_umask_test");
    if (ret >= 0) {
        struct {
            uint64_t size;
            uint8_t type;
            uint8_t pad1;
            uint16_t mode;
            uint16_t uid;
            uint16_t gid;
        } st;
        ret = sys_stat("/s96_umask_test", &st);
        if (ret == 0) {
            uint16_t perm = st.mode & 0x1FF;
            check("umask(0077): file perms = 0600", perm == 0600);
        }
        sys_unlink("/s96_umask_test");
    }
    sys_umask(0022);

    /* --- Test 5: chown as root (has CAP_CHOWN) --- */
    printf("\n[5] chown/fchown as root\n");
    ret = sys_create("/s96_chown_test");
    if (ret >= 0) {
        ret = sys_chown("/s96_chown_test", 500, 500);
        check("root chown succeeds", ret == 0);

        /* Verify ownership changed */
        struct {
            uint64_t size;
            uint8_t type;
            uint8_t pad1;
            uint16_t mode;
            uint16_t uid;
            uint16_t gid;
        } st;
        ret = sys_stat("/s96_chown_test", &st);
        if (ret == 0) {
            check("chown: uid changed to 500", st.uid == 500);
            check("chown: gid changed to 500", st.gid == 500);
        }

        /* Test fchown */
        long fd = sys_open("/s96_chown_test", 0);
        if (fd >= 0) {
            ret = sys_fchown(fd, 600, 600);
            check("root fchown succeeds", ret == 0);
            sys_close(fd);
        }

        sys_unlink("/s96_chown_test");
    } else {
        printf("  SKIP: could not create test file\n");
    }

    /* --- Test 6: supplementary groups (as root) --- */
    printf("\n[6] Supplementary groups\n");
    long ngroups = sys_getgroups(0, NULL);
    check("getgroups(0) returns count >= 0", ngroups >= 0);

    uint16_t new_groups[2] = { 100, 200 };
    ret = sys_setgroups(2, new_groups);
    check("root setgroups succeeds", ret == 0);

    ngroups = sys_getgroups(0, NULL);
    check("getgroups returns 2 after setgroups", ngroups == 2);

    uint16_t read_groups[4] = {0};
    ret = sys_getgroups(4, read_groups);
    if (ret >= 2) {
        check("group[0] = 100", read_groups[0] == 100);
        check("group[1] = 200", read_groups[1] == 200);
    }

    /* --- Test 7: setuid POSIX semantics (privilege drop) --- */
    printf("\n[7] setuid/setgid privilege drop\n");
    uid = sys_getuid();
    if (uid == 0) {
        ret = sys_setuid(1000);
        check("root setuid(1000) succeeds", ret == 0);

        euid = sys_geteuid();
        uid = sys_getuid();
        check("after setuid(1000): euid=1000", euid == 1000);
        check("after setuid(1000): uid=1000", uid == 1000);

        long caps = sys_getcap();
        check("caps dropped after setuid to non-root", caps == 0);

        ret = sys_setuid(0);
        check("unprivileged setuid(0) fails", ret < 0);

        ret = sys_setuid(1000);
        check("setuid(saved_uid) succeeds", ret == 0);
    } else {
        printf("  SKIP: not running as root\n");
    }

    /* --- Test 8: setgid semantics (as non-root) --- */
    printf("\n[8] setgid semantics (non-root)\n");
    gid = sys_getgid();
    ret = sys_setgid(gid);
    check("setgid(own gid) succeeds", ret == 0);
    ret = sys_setgid(9999);
    check("setgid(other gid) fails as non-root", ret < 0);

    /* --- Test 9: chown fails without CAP_CHOWN --- */
    printf("\n[9] chown without CAP_CHOWN (non-root)\n");
    ret = sys_chown("/s96test.elf", 500, 500);
    check("chown fails without CAP_CHOWN", ret < 0);

    /* setgroups also fails without root */
    ret = sys_setgroups(1, new_groups);
    check("setgroups fails as non-root", ret < 0);

    /* --- Summary --- */
    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    if (tests_failed == 0)
        printf("STAGE96 PASS\n");
    else
        printf("STAGE96 FAIL\n");

    return tests_failed;
}
