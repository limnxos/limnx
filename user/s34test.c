#include "libc/libc.h"

static int passed = 0;
static int failed = 0;

static void check(int ok, const char *name) {
    if (ok) {
        printf("  [PASS] %s\n", name);
        passed++;
    } else {
        printf("  [FAIL] %s\n", name);
        failed++;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("s34test: Stage 34 — Agent Runtime Foundation\n");

    /* --- Test 1: clock_gettime valid --- */
    {
        timespec_t ts;
        long rc = sys_clock_gettime(CLOCK_MONOTONIC, &ts);
        check(rc == 0 && ts.tv_sec >= 0 && ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000,
              "1. clock_gettime valid");
    }

    /* --- Test 2: clock_gettime monotonic --- */
    {
        timespec_t t1, t2;
        sys_clock_gettime(CLOCK_MONOTONIC, &t1);
        for (volatile int i = 0; i < 10000; i++) {}
        sys_clock_gettime(CLOCK_MONOTONIC, &t2);
        long s1 = t1.tv_sec * 1000000000 + t1.tv_nsec;
        long s2 = t2.tv_sec * 1000000000 + t2.tv_nsec;
        check(s2 >= s1, "2. clock_gettime monotonic");
    }

    /* --- Test 3: nanosleep duration --- */
    {
        timespec_t before, after;
        sys_clock_gettime(CLOCK_MONOTONIC, &before);
        timespec_t req = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        sys_nanosleep(&req);
        sys_clock_gettime(CLOCK_MONOTONIC, &after);
        long elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000 +
                          (after.tv_nsec - before.tv_nsec);
        /* At 18.2 Hz, granularity is ~55ms. Just check >= 50ms */
        check(elapsed_ns >= 50000000, "3. nanosleep duration >= 50ms");
    }

    /* --- Test 4: clock_gettime bad clockid --- */
    {
        timespec_t ts;
        long rc = sys_clock_gettime(99, &ts);
        check(rc == -EINVAL, "4. clock_gettime bad clockid");
    }

    /* --- Test 5: waitpid WNOHANG running --- */
    {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: loop for a while then exit */
            for (volatile long i = 0; i < 500000; i++) {}
            sys_exit(42);
        }
        /* Parent: WNOHANG should return 0 immediately */
        long rc = sys_waitpid_flags(pid, WNOHANG);
        check(rc == 0, "5. waitpid WNOHANG running child");
        /* Now blocking wait */
        long status = sys_waitpid(pid);
        check(status == 42, "6. waitpid blocking compat");
    }

    /* --- Test 7: getenv inherited --- */
    {
        char val[64];
        long rc = sys_getenv("LIMNX_VERSION", val, sizeof(val));
        check(rc > 0 && val[0] != '\0', "7. getenv inherited LIMNX_VERSION");
    }

    /* --- Test 8: setenv + getenv roundtrip --- */
    {
        sys_setenv("TEST_KEY", "hello_world");
        char val[64];
        long rc = sys_getenv("TEST_KEY", val, sizeof(val));
        check(rc > 0 && strcmp(val, "hello_world") == 0, "8. setenv + getenv roundtrip");
    }

    /* --- Test 9: getenv nonexistent --- */
    {
        char val[64];
        long rc = sys_getenv("NONEXISTENT_KEY", val, sizeof(val));
        check(rc == -ENOENT, "9. getenv nonexistent");
    }

    /* --- Test 10: setenv overwrite --- */
    {
        sys_setenv("TEST_KEY", "first");
        sys_setenv("TEST_KEY", "second");
        char val[64];
        long rc = sys_getenv("TEST_KEY", val, sizeof(val));
        check(rc > 0 && strcmp(val, "second") == 0, "10. setenv overwrite");
    }

    /* --- Test 11: env inherited across fork --- */
    {
        sys_setenv("FORK_TEST", "from_parent");
        long pid = sys_fork();
        if (pid == 0) {
            char val[64];
            long rc = sys_getenv("FORK_TEST", val, sizeof(val));
            sys_exit(rc > 0 && strcmp(val, "from_parent") == 0 ? 1 : 0);
        }
        long status = sys_waitpid(pid);
        check(status == 1, "11. env inherited across fork");
    }

    /* --- Test 12: ^U erases line --- */
    {
        long mfd, sfd;
        long rc = sys_openpty(&mfd, &sfd);
        if (rc == 0) {
            /* Write "abc" then ^U then "x\n" to master */
            sys_fwrite(mfd, "abc", 3);
            char ctrl_u = 0x15;
            sys_fwrite(mfd, &ctrl_u, 1);
            sys_fwrite(mfd, "x\n", 2);
            /* Read from slave */
            char buf[16];
            long n = sys_read(sfd, buf, sizeof(buf));
            check(n == 2 && buf[0] == 'x' && buf[1] == '\n', "12. ^U erases line");
            sys_close(mfd);
            sys_close(sfd);
        } else {
            check(0, "12. ^U erases line (openpty failed)");
        }
    }

    /* --- Test 13: ^D signals EOF --- */
    {
        long mfd, sfd;
        long rc = sys_openpty(&mfd, &sfd);
        if (rc == 0) {
            char ctrl_d = 0x04;
            sys_fwrite(mfd, &ctrl_d, 1);
            char buf[16];
            long n = sys_read(sfd, buf, sizeof(buf));
            check(n == 0, "13. ^D signals EOF");
            sys_close(mfd);
            sys_close(sfd);
        } else {
            check(0, "13. ^D signals EOF (openpty failed)");
        }
    }

    /* --- Test 14: ^C sends SIGINT --- */
    {
        long mfd, sfd;
        long rc = sys_openpty(&mfd, &sfd);
        if (rc == 0) {
            long pid = sys_fork();
            if (pid == 0) {
                /* Child: set own pgid to own pid, then loop */
                long me = sys_getpid();
                sys_setpgid(0, me);
                for (;;) sys_yield();
            }
            /* Parent: set fg_pgid on PTY to child's pid, then send ^C */
            /* Give child time to run setpgid */
            for (int i = 0; i < 200; i++) sys_yield();
            sys_ioctl(mfd, TIOCSPGRP, pid);
            char ctrl_c = 0x03;
            sys_fwrite(mfd, &ctrl_c, 1);
            /* Wait for child — it should be killed by SIGINT */
            long status = sys_waitpid(pid);
            check(status == -2, "14. ^C sends SIGINT");  /* -SIGINT = -2 */
            sys_close(mfd);
            sys_close(sfd);
        } else {
            check(0, "14. ^C sends SIGINT (openpty failed)");
        }
    }

    /* --- Test 15: poll pipe readable --- */
    {
        long rfd, wfd;
        sys_pipe(&rfd, &wfd);
        sys_fwrite(wfd, "data", 4);
        pollfd_t pfd = { .fd = (int)rfd, .events = POLLIN, .revents = 0 };
        long n = sys_poll(&pfd, 1, 0);
        check(n == 1 && (pfd.revents & POLLIN), "15. poll pipe readable");
        sys_close(rfd);
        sys_close(wfd);
    }

    /* --- Test 16: poll pipe empty --- */
    {
        long rfd, wfd;
        sys_pipe(&rfd, &wfd);
        pollfd_t pfd = { .fd = (int)rfd, .events = POLLIN, .revents = 0 };
        long n = sys_poll(&pfd, 1, 0);
        check(n == 0, "16. poll pipe empty");
        sys_close(rfd);
        sys_close(wfd);
    }

    /* --- Test 17: poll PTY readable --- */
    {
        long mfd, sfd;
        long rc = sys_openpty(&mfd, &sfd);
        if (rc == 0) {
            sys_fwrite(mfd, "hello\n", 6);
            pollfd_t pfd = { .fd = (int)sfd, .events = POLLIN, .revents = 0 };
            long n = sys_poll(&pfd, 1, 0);
            check(n == 1 && (pfd.revents & POLLIN), "17. poll PTY readable");
            sys_close(mfd);
            sys_close(sfd);
        } else {
            check(0, "17. poll PTY readable (openpty failed)");
        }
    }

    /* --- Test 18: poll regular file --- */
    {
        sys_create("/poll_test");
        long fd = sys_open("/poll_test", 2); /* O_RDWR */
        if (fd >= 0) {
            pollfd_t pfd = { .fd = (int)fd, .events = POLLIN | POLLOUT, .revents = 0 };
            long n = sys_poll(&pfd, 1, 0);
            check(n == 1 && (pfd.revents & POLLIN) && (pfd.revents & POLLOUT),
                  "18. poll regular file");
            sys_close(fd);
        } else {
            check(0, "18. poll regular file (open failed)");
        }
        sys_unlink("/poll_test");
    }

    /* --- Test 19: poll timeout --- */
    {
        long rfd, wfd;
        sys_pipe(&rfd, &wfd);
        timespec_t before, after;
        sys_clock_gettime(CLOCK_MONOTONIC, &before);
        pollfd_t pfd = { .fd = (int)rfd, .events = POLLIN, .revents = 0 };
        long n = sys_poll(&pfd, 1, 100); /* 100ms timeout */
        sys_clock_gettime(CLOCK_MONOTONIC, &after);
        long elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000 +
                          (after.tv_nsec - before.tv_nsec);
        check(n == 0 && elapsed_ns >= 50000000,
              "19. poll timeout");
        sys_close(rfd);
        sys_close(wfd);
    }

    /* --- Test 20: poll POLLHUP --- */
    {
        long rfd, wfd;
        sys_pipe(&rfd, &wfd);
        sys_close(wfd);  /* close write end */
        pollfd_t pfd = { .fd = (int)rfd, .events = POLLIN, .revents = 0 };
        long n = sys_poll(&pfd, 1, 0);
        check(n == 1 && (pfd.revents & POLLHUP), "20. poll POLLHUP");
        sys_close(rfd);
    }

    printf("\ns34test: %d/%d passed", passed, passed + failed);
    if (failed == 0)
        printf(" — ALL PASSED\n");
    else
        printf(" — %d FAILED\n", failed);

    return 0;
}
