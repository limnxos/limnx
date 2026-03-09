/*
 * s63test.c — Stage 63 tests: TCP close fixes + proper signal handling
 *
 * Tests: TCP close correctness, sigprocmask, signal queue, SA_RESTART
 */

#include "libc/libc.h"

static int pass_count = 0;
static int fail_count = 0;

static void check(int num, const char *name, int cond) {
    if (cond) {
        printf("  [%d]  PASS: %s\n", num, name);
        pass_count++;
    } else {
        printf("  [%d]  FAIL: %s\n", num, name);
        fail_count++;
    }
}

/* --- Signal handler helpers --- */

static volatile int sig_received = 0;
static volatile int sig_count = 0;

static void sig_handler(int sig) {
    (void)sig;
    sig_received = 1;
    sig_count++;
    sys_sigreturn();
}

static void sigchld_handler(int sig) {
    (void)sig;
    sig_count++;
    sys_sigreturn();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("\n=== Stage 63 Tests: TCP Close + Signal Handling ===\n\n");

    /* --- TCP close tests --- */

    /* [1] TCP loopback connect+send+close (FIN ordering) */
    {
        long srv = sys_tcp_socket();
        int ok = 0;
        if (srv >= 0) {
            long lr = sys_tcp_listen(srv, 9063);
            if (lr == 0) {
                long pid = sys_fork();
                if (pid == 0) {
                    sys_yield();
                    sys_yield();
                    long cli = sys_tcp_socket();
                    if (cli >= 0) {
                        long cr = sys_tcp_connect(cli, 0x0A00020F, 9063);
                        if (cr == 0) {
                            sys_tcp_send(cli, "tcp63", 5);
                            sys_tcp_close(cli);
                            sys_exit(0);
                        }
                        sys_tcp_close(cli);
                    }
                    sys_exit(1);
                } else if (pid > 0) {
                    long acc = sys_tcp_accept(srv);
                    if (acc >= 0) {
                        char buf[32];
                        long n = sys_tcp_recv(acc, buf, 32);
                        ok = (n == 5);
                        if (ok) {
                            for (int i = 0; i < 5; i++)
                                if (buf[i] != "tcp63"[i]) { ok = 0; break; }
                        }
                        sys_tcp_close(acc);
                    }
                    long st = sys_waitpid(pid);
                    if (st != 0) ok = 0;
                }
            }
            sys_tcp_close(srv);
        }
        check(1, "TCP loopback connect+send+close (FIN ordering)", ok);
    }

    /* [2] TCP bidirectional send+close */
    {
        long srv = sys_tcp_socket();
        int ok = 0;
        if (srv >= 0) {
            long lr = sys_tcp_listen(srv, 9064);
            if (lr == 0) {
                long pid = sys_fork();
                if (pid == 0) {
                    sys_yield();
                    sys_yield();
                    long cli = sys_tcp_socket();
                    if (cli >= 0) {
                        long cr = sys_tcp_connect(cli, 0x0A00020F, 9064);
                        if (cr == 0) {
                            sys_tcp_send(cli, "ping", 4);
                            char rbuf[32];
                            long n = sys_tcp_recv(cli, rbuf, 32);
                            if (n == 4 && rbuf[0] == 'p' && rbuf[1] == 'o' &&
                                rbuf[2] == 'n' && rbuf[3] == 'g') {
                                sys_tcp_close(cli);
                                sys_exit(0);
                            }
                            sys_tcp_close(cli);
                        } else {
                            sys_tcp_close(cli);
                        }
                    }
                    sys_exit(1);
                } else if (pid > 0) {
                    long acc = sys_tcp_accept(srv);
                    if (acc >= 0) {
                        char buf[32];
                        long n = sys_tcp_recv(acc, buf, 32);
                        if (n == 4) {
                            sys_tcp_send(acc, "pong", 4);
                        }
                        sys_tcp_close(acc);
                    }
                    long st = sys_waitpid(pid);
                    ok = (st == 0);
                }
            }
            sys_tcp_close(srv);
        }
        check(2, "TCP bidirectional send+close", ok);
    }

    /* [3] TCP 4KB transfer with proper close */
    {
        long srv = sys_tcp_socket();
        int ok = 0;
        if (srv >= 0) {
            long lr = sys_tcp_listen(srv, 9065);
            if (lr == 0) {
                long pid = sys_fork();
                if (pid == 0) {
                    sys_yield();
                    sys_yield();
                    long cli = sys_tcp_socket();
                    if (cli >= 0) {
                        long cr = sys_tcp_connect(cli, 0x0A00020F, 9065);
                        if (cr == 0) {
                            /* Send 4 chunks of 1024 bytes */
                            char sbuf[1024];
                            for (int chunk = 0; chunk < 4; chunk++) {
                                for (int i = 0; i < 1024; i++)
                                    sbuf[i] = (char)((chunk * 1024 + i) & 0x7F);
                                sys_tcp_send(cli, sbuf, 1024);
                            }
                            sys_tcp_close(cli);
                            sys_exit(0);
                        }
                        sys_tcp_close(cli);
                    }
                    sys_exit(1);
                } else if (pid > 0) {
                    long acc = sys_tcp_accept(srv);
                    if (acc >= 0) {
                        long total = 0;
                        char rbuf[1024];
                        int attempts = 0;
                        int data_ok = 1;
                        while (total < 4096 && attempts < 10000) {
                            long n = sys_tcp_recv(acc, rbuf,
                                                   1024 < (4096 - total) ? 1024 : (4096 - total));
                            if (n > 0) {
                                for (long i = 0; i < n; i++) {
                                    if (rbuf[i] != (char)((total + i) & 0x7F))
                                        data_ok = 0;
                                }
                                total += n;
                            } else break;
                            attempts++;
                        }
                        ok = (total == 4096 && data_ok);
                        sys_tcp_close(acc);
                    }
                    long st = sys_waitpid(pid);
                    if (st != 0) ok = 0;
                }
            }
            sys_tcp_close(srv);
        }
        check(3, "TCP 4KB transfer with proper close", ok);
    }

    /* --- Signal handling tests --- */

    /* [4] sigprocmask: block and unblock SIGINT */
    {
        sig_received = 0;
        sys_sigaction(SIGINT, sig_handler);

        /* Block SIGINT */
        unsigned int old_mask = 0;
        long r = sys_sigprocmask(SIG_BLOCK, (1U << SIGINT), &old_mask);
        check(4, "sigprocmask SIG_BLOCK returns 0", r == 0);
    }

    /* [5] sigprocmask: old mask returned correctly */
    {
        unsigned int old_mask = 0xDEAD;
        sys_sigprocmask(SIG_BLOCK, 0, &old_mask);
        /* old_mask should have SIGINT bit set from test 4 */
        check(5, "sigprocmask returns old mask with SIGINT blocked",
              (old_mask & (1U << SIGINT)) != 0);
    }

    /* [6] blocked signal stays pending */
    {
        sig_received = 0;
        /* SIGINT is still blocked from test 4 */
        sys_kill(sys_getpid(), SIGINT);
        /* Signal should be pending but not delivered */
        check(6, "blocked signal stays pending (not delivered)", sig_received == 0);
    }

    /* [7] unblocking delivers pending signal */
    {
        /* Unblock SIGINT — should deliver the pending one */
        sys_sigprocmask(SIG_UNBLOCK, (1U << SIGINT), 0);
        /* After unblock, the next syscall return should deliver it */
        sys_yield();  /* trigger signal delivery */
        check(7, "unblocking delivers pending signal", sig_received == 1);
        sys_sigaction(SIGINT, SIG_DFL);
    }

    /* [8] SIG_SETMASK replaces mask */
    {
        unsigned int old = 0;
        sys_sigprocmask(SIG_SETMASK, (1U << SIGTERM) | (1U << SIGINT), &old);
        unsigned int cur = 0;
        sys_sigprocmask(SIG_BLOCK, 0, &cur);
        check(8, "SIG_SETMASK sets exact mask",
              (cur & ((1U << SIGTERM) | (1U << SIGINT))) ==
              ((1U << SIGTERM) | (1U << SIGINT)));
        /* Clean up */
        sys_sigprocmask(SIG_SETMASK, 0, 0);
    }

    /* [9] SIGKILL cannot be blocked */
    {
        unsigned int old = 0;
        sys_sigprocmask(SIG_SETMASK, (1U << SIGKILL), &old);
        unsigned int cur = 0;
        sys_sigprocmask(SIG_BLOCK, 0, &cur);
        check(9, "SIGKILL cannot be blocked", (cur & (1U << SIGKILL)) == 0);
        sys_sigprocmask(SIG_SETMASK, 0, 0);
    }

    /* [10] SIGSTOP cannot be blocked */
    {
        sys_sigprocmask(SIG_SETMASK, (1U << SIGSTOP), 0);
        unsigned int cur = 0;
        sys_sigprocmask(SIG_BLOCK, 0, &cur);
        check(10, "SIGSTOP cannot be blocked", (cur & (1U << SIGSTOP)) == 0);
        sys_sigprocmask(SIG_SETMASK, 0, 0);
    }

    /* [11] signal queue: blocked signals queue up and deliver on unblock */
    {
        sig_count = 0;
        sys_sigaction(SIGINT, sig_handler);

        /* Block SIGINT, then send 3 signals */
        sys_sigprocmask(SIG_BLOCK, (1U << SIGINT), 0);
        sys_kill(sys_getpid(), SIGINT);
        sys_kill(sys_getpid(), SIGINT);
        sys_kill(sys_getpid(), SIGINT);

        /* Unblock — all 3 should deliver */
        sys_sigprocmask(SIG_UNBLOCK, (1U << SIGINT), 0);
        sys_yield();  /* let signals deliver */
        sys_yield();
        sys_yield();

        check(11, "signal queue: 3 blocked signals all delivered", sig_count >= 3);
        sys_sigaction(SIGINT, SIG_DFL);
    }

    /* [12] sys_sigaction3 with SA_RESTART flag */
    {
        sig_received = 0;
        long r = sys_sigaction3(SIGINT, sig_handler, SA_RESTART);
        check(12, "sys_sigaction3 with SA_RESTART returns 0", r == 0);
        sys_sigaction(SIGINT, SIG_DFL);
    }

    /* [13] sigprocmask with NULL old_mask */
    {
        long r = sys_sigprocmask(SIG_SETMASK, (1U << SIGINT), 0);
        check(13, "sigprocmask with NULL old_mask returns 0", r == 0);

        unsigned int cur = 0;
        sys_sigprocmask(SIG_BLOCK, 0, &cur);
        check(14, "mask correctly set without old_mask ptr",
              (cur & (1U << SIGINT)) != 0);
        sys_sigprocmask(SIG_SETMASK, 0, 0);
    }

    /* [15] signal mask inherited by fork */
    {
        sys_sigprocmask(SIG_SETMASK, (1U << SIGTERM), 0);
        long pid = sys_fork();
        if (pid == 0) {
            unsigned int child_mask = 0;
            sys_sigprocmask(SIG_BLOCK, 0, &child_mask);
            /* Exit with 0 if mask inherited, 1 if not */
            sys_exit((child_mask & (1U << SIGTERM)) ? 0 : 1);
        }
        long st = sys_waitpid(pid);
        check(15, "signal mask inherited by fork", st == 0);
        sys_sigprocmask(SIG_SETMASK, 0, 0);
    }

    /* --- Summary --- */
    printf("\n=== Stage 63 Results: %d/%d PASSED ===\n\n",
           pass_count, pass_count + fail_count);

    return 0;
}
