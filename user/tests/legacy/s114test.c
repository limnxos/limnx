/*
 * s114test.c — Stage 114: Mount/Umount + v1.0 Final Test
 *
 * Tests: tmpfs mount/umount, file creation on mount, umount cleanup,
 *        v1.0 version check, all v1.0 features present.
 * Portable — no arch-specific code.
 */

#include "../../libc/libc.h"

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

static int strcontains(const char *h, const char *n) {
    if (!h || !n) return 0;
    for (int i = 0; h[i]; i++) {
        int j = 0;
        while (n[j] && h[i+j] == n[j]) j++;
        if (!n[j]) return 1;
    }
    return 0;
}

static void test_mount_tmpfs(void) {
    printf("[1] Mount tmpfs\n");

    /* Create mount point */
    sys_mkdir("/mnt");
    long ret = sys_mount("/mnt", "tmpfs");
    check("mount tmpfs at /mnt", ret == 0);
}

static void test_create_on_mount(void) {
    printf("[2] Create files on mounted tmpfs\n");

    long fd = sys_open("/mnt/testfile.txt", 0x100 | 2);  /* O_CREAT | O_RDWR */
    check("create file on tmpfs", fd >= 0);
    if (fd >= 0) {
        sys_fwrite(fd, "tmpfs data", 10);
        sys_close(fd);
    }

    /* Read it back */
    fd = sys_open("/mnt/testfile.txt", 0);
    check("open file on tmpfs", fd >= 0);
    if (fd >= 0) {
        char buf[32];
        long n = sys_read(fd, buf, 31);
        if (n > 0) buf[n] = '\0';
        check("read correct data from tmpfs", n == 10 && strcmp(buf, "tmpfs data") == 0);
        sys_close(fd);
    } else {
        check("read correct data from tmpfs", 0);
    }

    /* Create subdirectory */
    long r2 = sys_mkdir("/mnt/subdir");
    check("mkdir on tmpfs", r2 >= 0);
}

static void test_umount(void) {
    printf("[3] Umount cleans up\n");

    long ret = sys_umount("/mnt");
    check("umount succeeds", ret == 0);

    /* Files should be gone */
    long fd = sys_open("/mnt/testfile.txt", 0);
    check("files gone after umount", fd < 0);

    /* Mount point still exists but is empty */
    fd = sys_open("/mnt", 0);
    check("/mnt still exists after umount", fd >= 0);
    if (fd >= 0) sys_close(fd);
}

static void test_mount_programs(void) {
    printf("[4] mount/umount programs exist\n");

    long fd = sys_open("/mountcmd.elf", 0);
    check("mountcmd.elf in initrd", fd >= 0);
    if (fd >= 0) sys_close(fd);

    fd = sys_open("/umount.elf", 0);
    check("umount.elf in initrd", fd >= 0);
    if (fd >= 0) sys_close(fd);
}

static void test_mount_errors(void) {
    printf("[5] Mount error cases\n");

    /* Invalid fstype */
    long r1 = sys_mount("/mnt", "ext4");
    check("invalid fstype rejected", r1 < 0);

    /* Non-existent path */
    long r2 = sys_mount("/nonexistent", "tmpfs");
    check("non-existent path rejected", r2 < 0);

    /* Umount non-mount */
    long r3 = sys_umount("/etc");
    check("umount non-mountpoint rejected", r3 < 0);
}

static void test_v1_version(void) {
    printf("[6] Limnx v1.0 version\n");

    char val[64];
    long r = sys_getenv("LIMNX_VERSION", val, 64);
    check("LIMNX_VERSION is set", r >= 0);
    if (r >= 0) {
        check("version is 1.0", strcmp(val, "1.0") == 0);
    } else {
        check("version is 1.0", 0);
    }
}

static void test_v1_features_present(void) {
    printf("[7] v1.0 feature spot checks\n");

    /* /proc exists */
    long fd = sys_open("/proc", 0);
    check("/proc exists", fd >= 0);
    if (fd >= 0) sys_close(fd);

    /* /etc/passwd exists */
    fd = sys_open("/etc/passwd", 0);
    check("/etc/passwd exists", fd >= 0);
    if (fd >= 0) sys_close(fd);

    /* /etc/inittab exists */
    fd = sys_open("/etc/inittab", 0);
    check("/etc/inittab exists", fd >= 0);
    if (fd >= 0) sys_close(fd);

    /* Session ID works */
    long sid = sys_getsid(0);
    check("getsid works", sid > 0);

    /* Fork + waitpid */
    long child = sys_fork();
    if (child == 0) sys_exit(7);
    long st = sys_waitpid(child);
    check("fork+waitpid works", st == 7);
}

int main(void) {
    printf("=== Stage 114: Mount/Umount + v1.0 Final Test ===\n\n");

    test_mount_tmpfs();
    test_create_on_mount();
    test_umount();
    test_mount_programs();
    test_mount_errors();
    test_v1_version();
    test_v1_features_present();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("ALL TESTS PASSED\n");
    printf("\n*** LIMNX v1.0 COMPLETE ***\n");

    return tests_failed;
}
