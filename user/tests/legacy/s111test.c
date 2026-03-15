/*
 * s111test.c — Stage 111: Coreutils Batch 2 + Shell bg
 *
 * Tests: head, tail, grep, chmod, chown, env programs.
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

/* Create a test file with numbered lines */
static void create_test_file(void) {
    long fd = sys_open("/s111_lines.txt", 0x100 | 2);  /* O_CREAT | O_RDWR */
    if (fd >= 0) {
        const char *lines =
            "line1\n"
            "line2\n"
            "line3\n"
            "line4 hello\n"
            "line5 HELLO\n"
            "line6\n"
            "line7 world\n"
            "line8\n"
            "line9\n"
            "line10\n";
        int len = 0;
        while (lines[len]) len++;
        sys_fwrite(fd, lines, len);
        sys_close(fd);
    }
}

static void test_programs_exist(void) {
    printf("[1] All batch 2 coreutils in initrd\n");
    const char *progs[] = {
        "/head.elf", "/tail.elf", "/grep.elf",
        "/chmodcmd.elf", "/chowncmd.elf", "/env.elf",
        (void *)0
    };
    int all = 1;
    for (int i = 0; progs[i]; i++) {
        long fd = sys_open(progs[i], 0);
        if (fd < 0) { printf("    missing: %s\n", progs[i]); all = 0; }
        else sys_close(fd);
    }
    check("all 6 batch 2 coreutils in initrd", all);
}

static void test_head(void) {
    printf("[2] head: first 3 lines\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"head.elf", "-n", "3", "/s111_lines.txt", (void *)0};
        sys_execve("/head.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("head.elf executes", st == 0);
}

static void test_tail(void) {
    printf("[3] tail: last 3 lines\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"tail.elf", "-n", "3", "/s111_lines.txt", (void *)0};
        sys_execve("/tail.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("tail.elf executes", st == 0);
}

static void test_grep(void) {
    printf("[4] grep: find 'hello' in file\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"grep.elf", "hello", "/s111_lines.txt", (void *)0};
        sys_execve("/grep.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("grep finds 'hello' (exit 0)", st == 0);
}

static void test_grep_nocase(void) {
    printf("[5] grep -i: case-insensitive\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"grep.elf", "-i", "hello", "/s111_lines.txt", (void *)0};
        sys_execve("/grep.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("grep -i finds 'hello' case-insensitive", st == 0);
}

static void test_grep_nomatch(void) {
    printf("[6] grep: no match returns 1\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"grep.elf", "xyznonexistent", "/s111_lines.txt", (void *)0};
        sys_execve("/grep.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("grep returns 1 on no match", st == 1);
}

static void test_chmod(void) {
    printf("[7] chmod: change file mode\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"chmodcmd.elf", "755", "/s111_lines.txt", (void *)0};
        sys_execve("/chmodcmd.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("chmod.elf executes", st == 0);
}

static void test_chown(void) {
    printf("[8] chown: change file owner\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"chowncmd.elf", "0:0", "/s111_lines.txt", (void *)0};
        sys_execve("/chowncmd.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("chown.elf executes", st == 0);
}

static void test_env(void) {
    printf("[9] env: list environment\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"env.elf", (void *)0};
        sys_execve("/env.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("env.elf executes", st == 0);
}

static void test_env_lookup(void) {
    printf("[10] env: lookup specific variable\n");
    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"env.elf", "LIMNX_VERSION", (void *)0};
        sys_execve("/env.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("env LIMNX_VERSION lookup", st == 0);
}

int main(void) {
    printf("=== Stage 111: Coreutils Batch 2 Test ===\n\n");

    create_test_file();

    test_programs_exist();
    test_head();
    test_tail();
    test_grep();
    test_grep_nocase();
    test_grep_nomatch();
    test_chmod();
    test_chown();
    test_env();
    test_env_lookup();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("ALL TESTS PASSED\n");

    return tests_failed;
}
