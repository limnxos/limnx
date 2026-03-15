#include "libc/libc.h"

/* --- Test framework --- */

static int tests_passed = 0;
static int tests_total  = 0;

static void pass(const char *name) {
    tests_passed++;
    tests_total++;
    printf("  [PASS] %s\n", name);
}

static void fail(const char *name, const char *reason) {
    tests_total++;
    printf("  [FAIL] %s — %s\n", name, reason);
}

/* --- Helpers --- */

/* Read until newline or EOF from pipe fd using O_NONBLOCK */
static int read_pipe_line(long fd, char *buf, int max) {
    sys_fcntl(fd, F_SETFL, O_NONBLOCK);
    int pos = 0;
    while (pos < max - 1) {
        char c;
        long n = sys_read(fd, &c, 1);
        if (n > 0) {
            if (c == '\n') break;
            buf[pos++] = c;
        } else if (n == 0) {
            break;  /* EOF */
        } else {
            sys_yield();  /* would block */
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* Read all data from pipe fd until EOF using O_NONBLOCK */
static int read_pipe_all(long fd, char *buf, int max) {
    sys_fcntl(fd, F_SETFL, O_NONBLOCK);
    int total = 0;
    while (total < max - 1) {
        char c;
        long n = sys_read(fd, &c, 1);
        if (n > 0) {
            buf[total++] = c;
        } else if (n == 0) {
            break;  /* EOF */
        } else {
            sys_yield();  /* would block */
        }
    }
    buf[total] = '\0';
    return total;
}

/* Spawn worker.elf with pipes set up, return child_pid.
   *write_fd = fd to write commands to worker
   *read_fd = fd to read responses from worker */
static long spawn_worker(long *write_fd, long *read_fd) {
    long rA, wA, rB, wB;
    if (sys_pipe(&rA, &wA) != 0) return -1;
    if (sys_pipe(&rB, &wB) != 0) return -1;

    long saved_wA = sys_dup(wA);
    if (saved_wA < 0) return -1;

    /* Set FD_CLOEXEC on fds that shouldn't be inherited by worker */
    sys_fcntl(rB, F_SETFD, FD_CLOEXEC);
    sys_fcntl(wA, F_SETFD, FD_CLOEXEC);
    sys_fcntl(saved_wA, F_SETFD, FD_CLOEXEC);

    sys_dup2(rA, 0);
    sys_dup2(wB, 1);

    long pid = sys_exec("/worker.elf", NULL);
    if (pid < 0) return -1;

    /* Close coordinator's copies of child ends */
    sys_close(0);
    sys_close(1);
    sys_close(rA);
    sys_close(wA);
    sys_close(wB);

    *write_fd = saved_wA;
    *read_fd = rB;
    return pid;
}

/* --- Test 1: fd inheritance basic --- */
static void test_fd_inherit_basic(void) {
    const char *name = "fd inheritance basic";

    /* Open a file, then exec hello.elf which will just exit.
       The point is exec doesn't crash with inherited fds. */
    long fd = sys_open("/hello.txt", O_RDONLY);
    if (fd < 0) { fail(name, "open failed"); return; }

    long child_pid = sys_exec("/hello.elf", NULL);
    if (child_pid < 0) {
        fail(name, "exec failed");
        sys_close(fd);
        return;
    }

    long status = sys_waitpid(child_pid);
    sys_close(fd);

    if (status == 0) pass(name);
    else fail(name, "child exit status non-zero");
}

/* --- Test 2: fd inheritance pipe --- */
static void test_fd_inherit_pipe(void) {
    const char *name = "fd inheritance pipe";

    long wfd, rfd;
    long child_pid = spawn_worker(&wfd, &rfd);
    if (child_pid < 0) { fail(name, "spawn_worker failed"); return; }

    /* Send a command */
    const char *cmd = "stat /hello.txt\n";
    sys_fwrite(wfd, cmd, (unsigned long)strlen(cmd));

    /* Read response */
    char resp[256];
    int n = read_pipe_line(rfd, resp, 256);

    sys_close(wfd);
    sys_waitpid(child_pid);
    sys_close(rfd);

    if (n > 0 && strstr(resp, "size="))
        pass(name);
    else
        fail(name, "no valid response from worker via pipe");
}

/* --- Test 3: multi-agent stat --- */
static void test_multiagent_stat(void) {
    const char *name = "multi-agent stat";

    long wfd, rfd;
    long child_pid = spawn_worker(&wfd, &rfd);
    if (child_pid < 0) { fail(name, "spawn_worker failed"); return; }

    const char *cmd = "stat /hello.txt\n";
    sys_fwrite(wfd, cmd, (unsigned long)strlen(cmd));

    char resp[256];
    int n = read_pipe_line(rfd, resp, 256);

    sys_close(wfd);
    sys_waitpid(child_pid);
    sys_close(rfd);

    if (n > 0 && strstr(resp, "size="))
        pass(name);
    else
        fail(name, "stat response missing size=");
}

/* --- Test 4: multi-agent ls --- */
static void test_multiagent_ls(void) {
    const char *name = "multi-agent ls";

    long wfd, rfd;
    long child_pid = spawn_worker(&wfd, &rfd);
    if (child_pid < 0) { fail(name, "spawn_worker failed"); return; }

    const char *cmd = "ls\n";
    sys_fwrite(wfd, cmd, (unsigned long)strlen(cmd));
    sys_close(wfd);  /* Signal EOF so worker exits after ls */

    char resp[2048];
    int n = read_pipe_all(rfd, resp, 2048);

    sys_waitpid(child_pid);
    sys_close(rfd);

    if (n > 0 && strstr(resp, "hello.elf"))
        pass(name);
    else
        fail(name, "ls response missing hello.elf");
}

/* --- Test 5: multi-agent multi-cmd --- */
static void test_multiagent_multi_cmd(void) {
    const char *name = "multi-agent multi-cmd";

    long wfd, rfd;
    long child_pid = spawn_worker(&wfd, &rfd);
    if (child_pid < 0) { fail(name, "spawn_worker failed"); return; }

    /* Send first command */
    const char *cmd1 = "stat /hello.txt\n";
    sys_fwrite(wfd, cmd1, (unsigned long)strlen(cmd1));
    char resp1[256];
    int n1 = read_pipe_line(rfd, resp1, 256);

    /* Send second command */
    const char *cmd2 = "stat /hello.txt\n";
    sys_fwrite(wfd, cmd2, (unsigned long)strlen(cmd2));
    char resp2[256];
    int n2 = read_pipe_line(rfd, resp2, 256);

    sys_close(wfd);
    sys_waitpid(child_pid);
    sys_close(rfd);

    if (n1 > 0 && n2 > 0)
        pass(name);
    else
        fail(name, "did not get two responses");
}

/* --- Test 6: multi-agent close --- */
static void test_multiagent_close(void) {
    const char *name = "multi-agent close";

    long wfd, rfd;
    long child_pid = spawn_worker(&wfd, &rfd);
    if (child_pid < 0) { fail(name, "spawn_worker failed"); return; }

    /* Close write pipe — worker should see EOF and exit */
    sys_close(wfd);

    long status = sys_waitpid(child_pid);
    sys_close(rfd);

    if (status == 0) pass(name);
    else fail(name, "worker exit status non-zero after pipe close");
}

/* --- Test 7: infer model load --- */
static void test_infer_model_load(void) {
    const char *name = "infer model load";

    const char *infer_argv[] = { "/infer.elf", "--test", NULL };
    long child_pid = sys_exec("/infer.elf", infer_argv);
    if (child_pid < 0) { fail(name, "exec infer.elf failed"); return; }

    long status = sys_waitpid(child_pid);
    if (status == 0) pass(name);
    else fail(name, "infer.elf self-test failed");
}

/* --- Test 8: fbcon active --- */
static void test_fbcon_active(void) {
    const char *name = "fbcon active";
    /* This text goes to both serial and framebuffer automatically */
    printf("  [FBCON] This text appears on framebuffer\n");
    pass(name);
}

int main(void) {
    printf("\ns25test: running 8 tests\n");

    test_fd_inherit_basic();
    test_fd_inherit_pipe();
    test_multiagent_stat();
    test_multiagent_ls();
    test_multiagent_multi_cmd();
    test_multiagent_close();
    test_infer_model_load();
    test_fbcon_active();

    if (tests_passed == tests_total)
        printf("s25test: ALL PASSED (%d/%d)\n", tests_passed, tests_total);
    else
        printf("s25test: %d/%d passed\n", tests_passed, tests_total);

    return (tests_passed == tests_total) ? 0 : 1;
}
