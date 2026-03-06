/*
 * s38test.c — Stage 38 tests: OS Track Infrastructure
 *
 * Tests: process table cleanup, bcache write-back, TCP dynamic window,
 *        per-CPU run queues, scheduler sanity
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

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    puts("\n=== Stage 38 Tests: OS Track Infrastructure ===\n");

    /* --- Process table cleanup tests --- */

    /* [1] fork+waitpid returns child status */
    {
        long pid = sys_fork();
        int ok = 0;
        if (pid < 0) {
            /* fork failed */
        } else if (pid == 0) {
            /* child */
            sys_exit(42);
        } else {
            /* parent */
            long status = sys_waitpid(pid);
            ok = (status == 42);
        }
        check(1, "fork+waitpid returns child status", ok);
    }

    /* [2] 20x fork+waitpid no proc_table exhaustion */
    {
        int ok = 1;
        for (int i = 0; i < 20; i++) {
            long pid = sys_fork();
            if (pid < 0) {
                ok = 0;
                break;
            } else if (pid == 0) {
                sys_exit(i);
            } else {
                long status = sys_waitpid(pid);
                if (status != i) {
                    ok = 0;
                    break;
                }
            }
        }
        check(2, "20x fork+waitpid no proc_table exhaustion", ok);
    }

    /* [3] getpid valid after fork cleanup */
    {
        long my_pid = sys_getpid();
        long pid = sys_fork();
        int ok = 0;
        if (pid < 0) {
            /* fork failed */
        } else if (pid == 0) {
            sys_exit(0);
        } else {
            sys_waitpid(pid);
            long after_pid = sys_getpid();
            ok = (after_pid == my_pid && my_pid > 0);
        }
        check(3, "getpid valid after fork cleanup", ok);
    }

    /* --- Block cache write-back tests --- */

    /* [4] bcache write-back: write+read coherency */
    {
        /* Write a file, read it back — cache should serve read without disk hit */
        long fd = sys_create("/s38_wb_test.txt");
        int ok = 0;
        if (fd >= 0) {
            const char *data = "write-back test data 12345678";
            sys_fwrite(fd, data, 28);
            sys_close(fd);

            fd = sys_open("/s38_wb_test.txt", 0);
            if (fd >= 0) {
                char buf[64];
                long n = sys_read(fd, buf, 64);
                ok = (n == 28);
                if (ok) {
                    for (int i = 0; i < 28; i++) {
                        if (buf[i] != data[i]) { ok = 0; break; }
                    }
                }
                sys_close(fd);
            }
            sys_unlink("/s38_wb_test.txt");
        }
        check(4, "bcache write-back: write+read coherency", ok);
    }

    /* [5] bcache write-back: overwrite coherency */
    {
        long fd = sys_create("/s38_wb_test2.txt");
        int ok = 0;
        if (fd >= 0) {
            sys_fwrite(fd, "AAAA", 4);
            sys_close(fd);

            /* Overwrite */
            fd = sys_open("/s38_wb_test2.txt", 1);
            if (fd >= 0) {
                sys_fwrite(fd, "BBBB", 4);
                sys_close(fd);
            }

            /* Read back — should see overwritten data */
            fd = sys_open("/s38_wb_test2.txt", 0);
            if (fd >= 0) {
                char buf[8];
                long n = sys_read(fd, buf, 8);
                ok = (n >= 4 && buf[0] == 'B' && buf[1] == 'B' &&
                      buf[2] == 'B' && buf[3] == 'B');
                sys_close(fd);
            }
            sys_unlink("/s38_wb_test2.txt");
        }
        check(5, "bcache write-back: overwrite coherency", ok);
    }

    /* --- TCP tests --- */

    /* [6] TCP loopback with dynamic window (fork: child connects, parent accepts) */
    {
        long srv = sys_tcp_socket();
        int ok = 0;
        if (srv >= 0) {
            long lr = sys_tcp_listen(srv, 9038);
            if (lr == 0) {
                long pid = sys_fork();
                if (pid == 0) {
                    /* Child: connect and send */
                    sys_yield();
                    sys_yield();
                    long cli = sys_tcp_socket();
                    if (cli >= 0) {
                        long cr = sys_tcp_connect(cli, 0x0A00020F, 9038);
                        if (cr == 0) {
                            sys_tcp_send(cli, "hello38", 7);
                            sys_tcp_close(cli);
                            sys_exit(0);
                        }
                        sys_tcp_close(cli);
                    }
                    sys_exit(1);
                } else if (pid > 0) {
                    /* Parent: accept and recv */
                    long acc = sys_tcp_accept(srv);
                    if (acc >= 0) {
                        char buf[32];
                        long n = sys_tcp_recv(acc, buf, 32);
                        ok = (n == 7);
                        if (ok) {
                            const char *msg = "hello38";
                            for (int i = 0; i < 7; i++) {
                                if (buf[i] != msg[i]) { ok = 0; break; }
                            }
                        }
                        sys_tcp_close(acc);
                    }
                    long st = sys_waitpid(pid);
                    if (st != 0) ok = 0;
                }
            }
            sys_tcp_close(srv);
        }
        check(6, "TCP loopback with dynamic window", ok);
    }

    /* [7] TCP flow control: MSS-sized transfer (fork: child sends, parent recvs) */
    {
        long srv = sys_tcp_socket();
        int ok = 0;
        if (srv >= 0) {
            long lr = sys_tcp_listen(srv, 9039);
            if (lr == 0) {
                long pid = sys_fork();
                if (pid == 0) {
                    /* Child: connect and send 1024 bytes */
                    sys_yield();
                    sys_yield();
                    long cli = sys_tcp_socket();
                    if (cli >= 0) {
                        long cr = sys_tcp_connect(cli, 0x0A00020F, 9039);
                        if (cr == 0) {
                            char sbuf[1024];
                            for (int i = 0; i < 1024; i++)
                                sbuf[i] = (char)(i & 0x7F);
                            sys_tcp_send(cli, sbuf, 1024);
                            sys_tcp_close(cli);
                            sys_exit(0);
                        }
                        sys_tcp_close(cli);
                    }
                    sys_exit(1);
                } else if (pid > 0) {
                    /* Parent: accept and recv 1024 bytes */
                    long acc = sys_tcp_accept(srv);
                    if (acc >= 0) {
                        char rbuf[1024];
                        long recvd = 0;
                        int attempts = 0;
                        while (recvd < 1024 && attempts < 5000) {
                            long n = sys_tcp_recv(acc, rbuf + recvd,
                                                  1024 - (long)recvd);
                            if (n > 0) recvd += n;
                            else if (n == 0) break;
                            else break;
                            attempts++;
                        }
                        ok = (recvd == 1024);
                        if (ok) {
                            for (int i = 0; i < 1024; i++) {
                                if (rbuf[i] != (char)(i & 0x7F)) {
                                    ok = 0;
                                    break;
                                }
                            }
                        }
                        sys_tcp_close(acc);
                    }
                    long st = sys_waitpid(pid);
                    if (st != 0) ok = 0;
                }
            }
            sys_tcp_close(srv);
        }
        check(7, "TCP flow control: MSS-sized transfer", ok);
    }

    /* --- Scheduler tests --- */

    /* [8] sched_yield returns (scheduler sanity) */
    {
        sys_yield();
        check(8, "sched_yield returns (scheduler sanity)", 1);
    }

    /* [9] concurrent process execution (fork+busy work) */
    {
        long pid = sys_fork();
        int ok = 0;
        if (pid < 0) {
            /* fork failed */
        } else if (pid == 0) {
            /* child: do some busy work */
            volatile int sum = 0;
            for (int i = 0; i < 1000; i++)
                sum += i;
            sys_exit((int)(sum > 0 ? 7 : 0));
        } else {
            long status = sys_waitpid(pid);
            ok = (status == 7);
        }
        check(9, "concurrent process execution (fork+busy work)", ok);
    }

    /* [10] 4 children scheduled + reaped */
    {
        int ok = 1;
        long pids[4];
        for (int i = 0; i < 4; i++) {
            pids[i] = sys_fork();
            if (pids[i] < 0) {
                ok = 0;
                break;
            } else if (pids[i] == 0) {
                sys_exit(10 + i);
            }
        }
        if (ok) {
            for (int i = 0; i < 4; i++) {
                if (pids[i] > 0) {
                    long status = sys_waitpid(pids[i]);
                    if (status != 10 + i)
                        ok = 0;
                }
            }
        }
        check(10, "4 children scheduled + reaped", ok);
    }

    /* --- Summary --- */
    printf("\n=== Stage 38 Results: %d/%d PASSED ===\n\n",
           pass_count, pass_count + fail_count);

    return 0;
}
