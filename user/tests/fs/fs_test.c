/*
 * fs_test.c — Filesystem tests
 * Tests: open, read, write, mkdir, readdir, symlinks, /proc, mount
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

static void test_open_read(void) {
    long fd = sys_open("/etc/inittab", 0);
    lt_ok(fd >= 0, "open /etc/inittab");
    if (fd >= 0) {
        char buf[256];
        long n = sys_read(fd, buf, 255);
        lt_ok(n > 0, "read returns data");
        sys_close(fd);
    } else {
        lt_ok(0, "read returns data");
    }
}

static void test_create_write_read(void) {
    long fd = sys_open("/fs_test_file.txt", 0x100 | 2);
    lt_ok(fd >= 0, "create file");
    if (fd >= 0) {
        sys_fwrite(fd, "testdata", 8);
        sys_close(fd);
    }
    fd = sys_open("/fs_test_file.txt", 0);
    lt_ok(fd >= 0, "reopen file");
    if (fd >= 0) {
        char buf[32];
        long n = sys_read(fd, buf, 31);
        if (n > 0) buf[n] = '\0';
        lt_ok(n == 8 && strcmp(buf, "testdata") == 0, "read back correct data");
        sys_close(fd);
    } else {
        lt_ok(0, "read back correct data");
    }
}

static void test_mkdir_readdir(void) {
    long ret = sys_mkdir("/fs_test_dir");
    lt_ok(ret >= 0, "mkdir");
    long fd = sys_open("/fs_test_dir/file1.txt", 0x100 | 2);
    if (fd >= 0) { sys_fwrite(fd, "x", 1); sys_close(fd); }
    char dirent[272];
    ret = sys_readdir("/fs_test_dir", 0, dirent);
    lt_ok(ret == 0, "readdir finds entry");
}

static void test_symlink(void) {
    sys_open("/fs_sym_target.txt", 0x100 | 2);
    long ret = sys_symlink("/fs_sym_target.txt", "/fs_sym_link");
    lt_ok(ret == 0, "symlink creation");
    char buf[256];
    long n = sys_readlink("/fs_sym_link", buf, 256);
    lt_ok(n > 0, "readlink returns target");
}

static void test_proc(void) {
    long fd = sys_open("/proc", 0);
    lt_ok(fd >= 0, "/proc exists");
    if (fd >= 0) sys_close(fd);

    long pid = sys_getpid();
    char path[64];
    int p = 0;
    const char *pfx = "/proc/";
    while (*pfx) path[p++] = *pfx++;
    long tmp = pid;
    char rev[24]; int rp = 0;
    while (tmp > 0) { rev[rp++] = '0' + (tmp % 10); tmp /= 10; }
    while (rp > 0) path[p++] = rev[--rp];
    const char *sfx = "/status";
    while (*sfx) path[p++] = *sfx++;
    path[p] = '\0';

    fd = sys_open(path, 0);
    lt_ok(fd >= 0, "/proc/<pid>/status opens");
    if (fd >= 0) sys_close(fd);
}

static void test_mount_umount(void) {
    sys_mkdir("/fs_mnt");
    long ret = sys_mount("/fs_mnt", "tmpfs");
    lt_ok(ret == 0, "mount tmpfs");
    long fd = sys_open("/fs_mnt/mfile.txt", 0x100 | 2);
    lt_ok(fd >= 0, "create file on tmpfs");
    if (fd >= 0) sys_close(fd);
    ret = sys_umount("/fs_mnt");
    lt_ok(ret == 0, "umount");
    fd = sys_open("/fs_mnt/mfile.txt", 0);
    lt_ok(fd < 0, "file gone after umount");
}

static void test_seek_truncate(void) {
    long fd = sys_open("/fs_seek_test.txt", 0x100 | 2);
    if (fd < 0) { lt_ok(0, "seek test setup"); return; }
    sys_fwrite(fd, "abcdefghij", 10);
    sys_seek(fd, 5, 0);  /* SEEK_SET to offset 5 */
    char buf[16];
    long n = sys_read(fd, buf, 10);
    lt_ok(n == 5, "seek+read returns remaining bytes");
    sys_close(fd);

    long ret = sys_truncate("/fs_seek_test.txt", 3);
    lt_ok(ret == 0, "truncate to 3 bytes");
    fd = sys_open("/fs_seek_test.txt", 0);
    if (fd >= 0) {
        n = sys_read(fd, buf, 10);
        lt_ok(n == 3, "truncated file has 3 bytes");
        sys_close(fd);
    } else {
        lt_ok(0, "truncated file has 3 bytes");
    }
}

static void test_rename(void) {
    sys_open("/fs_rename_src.txt", 0x100 | 2);
    long ret = sys_rename("/fs_rename_src.txt", "/fs_rename_dst.txt");
    lt_ok(ret == 0, "rename succeeds");
    long fd = sys_open("/fs_rename_src.txt", 0);
    lt_ok(fd < 0, "old name gone after rename");
    fd = sys_open("/fs_rename_dst.txt", 0);
    lt_ok(fd >= 0, "new name exists after rename");
    if (fd >= 0) sys_close(fd);
}

static void test_cwd(void) {
    sys_mkdir("/fs_cwd_dir");
    sys_chdir("/fs_cwd_dir");
    char cwd[256];
    sys_getcwd(cwd, 256);
    lt_ok(strcmp(cwd, "/fs_cwd_dir") == 0, "chdir+getcwd");
    sys_chdir("/");
    sys_getcwd(cwd, 256);
    lt_ok(strcmp(cwd, "/") == 0, "chdir back to /");
}

static void test_stat(void) {
    char st[16];  /* vfs_stat_t: size(8) + type(1) + pad(1) + mode(2) + uid(2) + gid(2) */
    long ret = sys_stat("/etc", st);
    lt_ok(ret == 0, "stat /etc");
    lt_ok(st[8] == 1, "stat reports directory type");
}

int main(void) {
    lt_suite("fs");
    test_open_read();
    test_create_write_read();
    test_mkdir_readdir();
    test_symlink();
    test_proc();
    test_mount_umount();
    test_seek_truncate();
    test_rename();
    test_cwd();
    test_stat();
    return lt_done();
}
