/*
 * s109test.c — Stage 109: Coreutils Test
 *
 * Tests coreutils programs exist in initrd and can be exec'd.
 * Also tests underlying syscalls that coreutils depend on.
 * Portable — no arch-specific code.
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

static void test_echo(void) {
    printf("[1] echo: fork+exec echo.elf\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"echo.elf", "hello", "world", (void *)0};
        sys_execve("/echo.elf", argv);
        sys_exit(127);
    }
    long status = sys_waitpid(pid);
    check("echo.elf executes", status == 0);
}

static void test_mkdir_and_ls(void) {
    printf("[2] mkdir + ls: create dir and list it\n");

    /* Create test directory */
    long ret = sys_mkdir("/testdir109");
    check("mkdir /testdir109", ret >= 0);

    /* Create a file inside */
    long fd = sys_open("/testdir109/file1.txt", 0x100 | 2);  /* O_CREAT | O_RDWR */
    if (fd >= 0) {
        sys_fwrite(fd, "data", 4);
        sys_close(fd);
    }

    /* readdir should find file1.txt */
    char dirent[272];
    ret = sys_readdir("/testdir109", 0, dirent);
    check("ls /testdir109 finds entry", ret == 0);
    check("entry is file1.txt", strcmp(dirent, "file1.txt") == 0);

    /* Fork+exec ls.elf */
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"ls.elf", "/testdir109", (void *)0};
        sys_execve("/ls.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("ls.elf executes", st == 0);
}

static void test_cat(void) {
    printf("[3] cat: read file content\n");

    /* Create test file */
    long fd = sys_open("/cattest109.txt", 0x100 | 2);
    if (fd >= 0) {
        sys_fwrite(fd, "hello cat", 9);
        sys_close(fd);
    }

    /* Fork+exec cat.elf */
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"cat.elf", "/cattest109.txt", (void *)0};
        sys_execve("/cat.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("cat.elf executes", st == 0);
}

static void test_cp(void) {
    printf("[4] cp: copy file\n");

    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"cp.elf", "/cattest109.txt", "/cptest109.txt", (void *)0};
        sys_execve("/cp.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("cp.elf executes", st == 0);

    /* Verify copy exists and has correct content */
    long fd = sys_open("/cptest109.txt", 0);
    check("cp destination exists", fd >= 0);
    if (fd >= 0) {
        char buf[32];
        long n = sys_read(fd, buf, 31);
        if (n > 0) buf[n] = '\0';
        check("cp content correct", n == 9 && strcmp(buf, "hello cat") == 0);
        sys_close(fd);
    } else {
        check("cp content correct", 0);
    }
}

static void test_mv(void) {
    printf("[5] mv: rename file\n");

    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"mv.elf", "/cptest109.txt", "/mvtest109.txt", (void *)0};
        sys_execve("/mv.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("mv.elf executes", st == 0);

    /* Old name should be gone, new name should exist */
    long fd_old = sys_open("/cptest109.txt", 0);
    check("mv source gone", fd_old < 0);
    long fd_new = sys_open("/mvtest109.txt", 0);
    check("mv destination exists", fd_new >= 0);
    if (fd_new >= 0) sys_close(fd_new);
}

static void test_rm(void) {
    printf("[6] rm: remove file\n");

    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"rm.elf", "/mvtest109.txt", (void *)0};
        sys_execve("/rm.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("rm.elf executes", st == 0);

    long fd = sys_open("/mvtest109.txt", 0);
    check("rm file gone", fd < 0);
}

static void test_wc(void) {
    printf("[7] wc: count lines/words/chars\n");

    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"wc.elf", "/cattest109.txt", (void *)0};
        sys_execve("/wc.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("wc.elf executes", st == 0);
}

static void test_ps(void) {
    printf("[8] ps: process listing\n");

    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"ps.elf", (void *)0};
        sys_execve("/ps.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("ps.elf executes", st == 0);
}

static void test_kill(void) {
    printf("[9] kill: send signal\n");

    /* Fork a child that sleeps, then kill it */
    long child = sys_fork();
    if (child == 0) {
        /* Sleep indefinitely */
        for (;;) sys_yield();
    }

    /* Send SIGKILL (9) to child */
    long ret = sys_kill(child, 9);
    check("kill sends signal", ret == 0);

    long st = sys_waitpid(child);
    check("killed child reaped", st != -1);
}

static void test_programs_in_initrd(void) {
    printf("[10] All coreutils in initrd\n");

    const char *progs[] = {
        "/echo.elf", "/ls.elf", "/cat.elf", "/cp.elf", "/mv.elf",
        "/rm.elf", "/mkdircmd.elf", "/ps.elf", "/killcmd.elf", "/wc.elf",
        (void *)0
    };
    int all_found = 1;
    for (int i = 0; progs[i]; i++) {
        long fd = sys_open(progs[i], 0);
        if (fd < 0) {
            printf("    missing: %s\n", progs[i]);
            all_found = 0;
        } else {
            sys_close(fd);
        }
    }
    check("all 10 coreutils in initrd", all_found);
}

int main(void) {
    printf("=== Stage 109: Coreutils Test ===\n\n");

    test_programs_in_initrd();
    test_echo();
    test_mkdir_and_ls();
    test_cat();
    test_cp();
    test_mv();
    test_rm();
    test_wc();
    test_ps();
    test_kill();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("ALL TESTS PASSED\n");

    return tests_failed;
}
