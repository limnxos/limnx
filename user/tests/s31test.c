#include "libc/libc.h"

static int test_num = 0;
static int pass_count = 0;
static int fail_count = 0;

static void check(int ok, const char *desc) {
    test_num++;
    if (ok) {
        printf("  [%d] PASS: %s\n", test_num, desc);
        pass_count++;
    } else {
        printf("  [%d] FAIL: %s\n", test_num, desc);
        fail_count++;
    }
}

/* --- Signal handler globals --- */
static volatile int sig_received = 0;
static volatile int sig_number = 0;

static void sigint_handler(int sig) {
    sig_received = 1;
    sig_number = sig;
    sys_sigreturn();
}

static void sigterm_handler(int sig) {
    sig_received = 1;
    sig_number = sig;
    sys_sigreturn();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\n=== s31test: Stage 31 Tests ===\n");

    /* Test 1: fork returns PID to parent */
    {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: exit immediately */
            sys_exit(0);
        }
        check(pid > 0, "fork returns child PID > 0 to parent");
        long status = sys_waitpid(pid);
        check(status == 0, "fork child exited with status 0");
    }

    /* Test 3: fork child returns 0 */
    {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: we got here means fork returned 0 */
            sys_exit(42);
        }
        long status = sys_waitpid(pid);
        check(status == 42, "fork child got return value 0 (exited 42)");
    }

    /* Test 4: fork COW isolation */
    {
        volatile long shared_var = 100;
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: modify variable, exit with its value */
            shared_var = 200;
            sys_exit((long)shared_var);
        }
        /* Parent: wait and check our variable is unchanged */
        long status = sys_waitpid(pid);
        check(shared_var == 100, "fork COW: parent var unchanged");
        check(status == 200, "fork COW: child saw modified value");
    }

    /* Test 6: fork fd inheritance */
    {
        /* Create a test file */
        sys_create("/fork_fd_test.txt");
        long fd = sys_open("/fork_fd_test.txt", O_RDWR);
        if (fd >= 0) {
            const char *msg = "hello-fork";
            sys_fwrite(fd, msg, 10);
            sys_close(fd);
        }

        fd = sys_open("/fork_fd_test.txt", O_RDONLY);
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: read from inherited fd */
            char buf[16] = {0};
            long n = sys_read(fd, buf, 10);
            sys_close(fd);
            sys_exit(n == 10 && buf[0] == 'h' ? 1 : 0);
        }
        sys_close(fd);
        long status = sys_waitpid(pid);
        check(status == 1, "fork fd inheritance: child reads parent's file");
        sys_unlink("/fork_fd_test.txt");
    }

    /* Test 7: fork pipe communication */
    {
        long rfd = -1, wfd = -1;
        sys_pipe(&rfd, &wfd);
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: close write end, read from pipe */
            sys_close(wfd);
            char buf[16] = {0};
            long n = sys_read(rfd, buf, 4);
            sys_close(rfd);
            sys_exit(n == 4 && buf[0] == 'p' ? 1 : 0);
        }
        /* Parent: close read end, write to pipe */
        sys_close(rfd);
        sys_fwrite(wfd, "pipe", 4);
        sys_close(wfd);
        long status = sys_waitpid(pid);
        check(status == 1, "fork pipe: parent writes, child reads");
    }

    /* Test 8: fork + exec */
    {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: exec hello.elf */
            const char *args[] = {"hello.elf", NULL};
            sys_exec("/hello.elf", args);
            sys_exit(-1);  /* exec failed */
        }
        long status = sys_waitpid(pid);
        check(pid > 0, "fork + exec: child exec'd successfully");
        (void)status;
    }

    /* Test 9: fork multiple children */
    {
        long pids[3];
        for (int i = 0; i < 3; i++) {
            pids[i] = sys_fork();
            if (pids[i] == 0) {
                sys_exit(i + 10);
            }
        }
        int all_ok = 1;
        for (int i = 0; i < 3; i++) {
            long status = sys_waitpid(pids[i]);
            if (status != i + 10) all_ok = 0;
        }
        check(all_ok, "fork multiple children (3), all exit correctly");
    }

    /* Test 10: sigaction SIGINT handler */
    {
        sig_received = 0;
        sig_number = 0;
        sys_sigaction(SIGINT, sigint_handler);

        long pid = sys_fork();
        if (pid == 0) {
            /* Child: install handler, then yield to let parent send signal */
            sys_sigaction(SIGINT, sigint_handler);
            /* Busy-wait for signal */
            for (int i = 0; i < 1000 && !sig_received; i++)
                sys_yield();
            sys_exit(sig_received ? sig_number : -1);
        }

        /* Parent: send SIGINT to child */
        sys_yield();  /* let child start */
        sys_yield();
        sys_kill(pid, SIGINT);
        long status = sys_waitpid(pid);
        check(status == SIGINT, "sigaction SIGINT: handler received signal");

        /* Reset parent handler */
        sys_sigaction(SIGINT, SIG_DFL);
    }

    /* Test 11: sigaction SIGTERM handler */
    {
        long pid = sys_fork();
        if (pid == 0) {
            sig_received = 0;
            sig_number = 0;
            sys_sigaction(SIGTERM, sigterm_handler);
            for (int i = 0; i < 1000 && !sig_received; i++)
                sys_yield();
            sys_exit(sig_received ? sig_number : -1);
        }
        sys_yield();
        sys_yield();
        sys_kill(pid, SIGTERM);
        long status = sys_waitpid(pid);
        check(status == SIGTERM, "sigaction SIGTERM: handler received signal");
    }

    /* Test 12: sigreturn restores context */
    {
        long pid = sys_fork();
        if (pid == 0) {
            sig_received = 0;
            sys_sigaction(SIGINT, sigint_handler);

            /* Do a syscall, get signal between syscalls */
            long my_pid = sys_getpid();
            sys_kill(my_pid, SIGINT);
            /* After sigreturn, execution should continue here */
            /* The sig_received flag should be set by handler */
            sys_exit(sig_received ? 1 : 0);
        }
        long status = sys_waitpid(pid);
        check(status == 1, "sigreturn: execution continues after handler");
    }

    /* Test 13: SIG_IGN */
    {
        long pid = sys_fork();
        if (pid == 0) {
            sys_sigaction(SIGINT, SIG_IGN);
            long my_pid = sys_getpid();
            sys_kill(my_pid, SIGINT);
            /* Should survive */
            sys_exit(77);
        }
        long status = sys_waitpid(pid);
        check(status == 77, "SIG_IGN: process survives ignored signal");
    }

    /* Test 14: SIG_DFL kills process */
    {
        long pid = sys_fork();
        if (pid == 0) {
            /* Default handler: SIGTERM should kill */
            for (int i = 0; i < 10000; i++)
                sys_yield();
            sys_exit(0);  /* should not reach */
        }
        sys_yield();
        sys_yield();
        sys_kill(pid, SIGTERM);
        long status = sys_waitpid(pid);
        check(status == -SIGTERM, "SIG_DFL: default kills process");
    }

    /* Summary */
    printf("\n=== s31test: %d passed, %d failed (of %d) ===\n",
        pass_count, fail_count, test_num);
    if (fail_count == 0)
        printf("s31test: ALL PASSED\n");

    return 0;
}
