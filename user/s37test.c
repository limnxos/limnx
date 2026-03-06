/*
 * s37test.c — Stage 37 tests: Agent Scale
 *
 * Tests: epoll, demand paging, swap stat, inference service,
 *        io_uring, mmap2
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

    puts("\n=== Stage 37 Tests: Agent Scale ===\n");

    /* --- epoll tests --- */

    /* [1] epoll_create returns valid fd */
    long epfd = sys_epoll_create(0);
    check(1, "epoll_create returns valid fd", epfd >= 0);

    /* [2] epoll_ctl ADD/DEL */
    {
        /* Create a pipe to register with epoll */
        long rfd = -1, wfd = -1;
        long pret = sys_pipe(&rfd, &wfd);
        int ok = 0;
        if (pret == 0 && epfd >= 0) {
            epoll_event_t ev;
            ev.events = EPOLLIN;
            ev.data = 42;
            long add_ret = sys_epoll_ctl(epfd, EPOLL_CTL_ADD, rfd, &ev);
            long del_ret = sys_epoll_ctl(epfd, EPOLL_CTL_DEL, rfd, (void *)0);
            ok = (add_ret == 0 && del_ret == 0);
            sys_close(rfd);
            sys_close(wfd);
        }
        check(2, "epoll_ctl ADD/DEL", ok);
    }

    /* [3] epoll_wait pipe ready */
    {
        long rfd = -1, wfd = -1;
        long pret = sys_pipe(&rfd, &wfd);
        int ok = 0;
        if (pret == 0 && epfd >= 0) {
            epoll_event_t ev;
            ev.events = EPOLLIN;
            ev.data = 99;
            sys_epoll_ctl(epfd, EPOLL_CTL_ADD, rfd, &ev);

            /* Write data to make pipe readable */
            sys_fwrite(wfd, "hi", 2);

            epoll_event_t out[4];
            long ready = sys_epoll_wait(epfd, out, 4, 0);
            ok = (ready >= 1 && out[0].events == EPOLLIN && out[0].data == 99);

            sys_epoll_ctl(epfd, EPOLL_CTL_DEL, rfd, (void *)0);
            sys_close(rfd);
            sys_close(wfd);
        }
        check(3, "epoll_wait pipe ready", ok);
    }

    /* [4] epoll_wait timeout returns 0 */
    {
        int ok = 0;
        if (epfd >= 0) {
            epoll_event_t out[4];
            long ready = sys_epoll_wait(epfd, out, 4, 0);
            ok = (ready == 0);
        }
        check(4, "epoll_wait timeout returns 0", ok);
    }

    /* [5] epoll_wait unix socket ready */
    {
        /* Create a unix socket pair */
        long srv_fd = sys_unix_socket();
        int ok = 0;
        if (srv_fd >= 0 && epfd >= 0) {
            long bret = sys_unix_bind(srv_fd, "/tmp/ep_test.sock");
            long lret = sys_unix_listen(srv_fd);
            if (bret == 0 && lret == 0) {
                /* Connect a client */
                long cli_fd = sys_unix_connect("/tmp/ep_test.sock");
                if (cli_fd >= 0) {
                    /* Accept */
                    long acc_fd = sys_unix_accept(srv_fd);
                    if (acc_fd >= 0) {
                        /* Register accepted socket in epoll */
                        epoll_event_t ev;
                        ev.events = EPOLLIN;
                        ev.data = 55;
                        sys_epoll_ctl(epfd, EPOLL_CTL_ADD, acc_fd, &ev);

                        /* Send data from client */
                        sys_fwrite(cli_fd, "test", 4);
                        sys_yield();  /* let data propagate */

                        epoll_event_t out[4];
                        long ready = sys_epoll_wait(epfd, out, 4, 100);
                        ok = (ready >= 1);

                        sys_epoll_ctl(epfd, EPOLL_CTL_DEL, acc_fd, (void *)0);
                        sys_close(acc_fd);
                    }
                    sys_close(cli_fd);
                }
            }
            sys_close(srv_fd);
        }
        check(5, "epoll_wait unix socket ready", ok);
    }

    if (epfd >= 0)
        sys_close(epfd);

    /* --- Demand paging + swap tests --- */

    /* [6] mmap2 DEMAND returns valid address */
    long demand_addr = sys_mmap2(4, MMAP_DEMAND);
    check(6, "mmap2 DEMAND returns valid address", demand_addr > 0);

    /* [7] demand page write triggers fault + alloc */
    {
        int ok = 0;
        if (demand_addr > 0) {
            volatile uint32_t *p = (volatile uint32_t *)demand_addr;
            *p = 0xDEADBEEF;  /* triggers page fault → demand alloc */
            ok = (*p == 0xDEADBEEF);
        }
        check(7, "demand page write triggers fault + alloc", ok);
    }

    /* [8] demand page read after write */
    {
        int ok = 0;
        if (demand_addr > 0) {
            volatile uint32_t *p = (volatile uint32_t *)demand_addr;
            ok = (*p == 0xDEADBEEF);
        }
        check(8, "demand page read after write", ok);
    }

    /* [9] multiple demand pages */
    {
        int ok = 0;
        if (demand_addr > 0) {
            /* Write to page 0 (already touched) and page 2 (new fault) */
            volatile uint32_t *p0 = (volatile uint32_t *)demand_addr;
            volatile uint32_t *p2 = (volatile uint32_t *)(demand_addr + 2 * 4096);
            *p0 = 0x11111111;
            *p2 = 0x22222222;
            ok = (*p0 == 0x11111111 && *p2 == 0x22222222);
        }
        check(9, "multiple demand pages", ok);
    }

    /* [10] swap_stat returns usage */
    {
        uint32_t stat[2] = {0, 0};
        long ret = sys_swap_stat(stat);
        check(10, "swap_stat returns usage", ret == 0 && stat[0] == 2048);
    }

    /* --- Inference service tests --- */

    /* [11] infer_register succeeds */
    {
        long ret = sys_infer_register("test_model", "/tmp/test_infer.sock");
        check(11, "infer_register succeeds", ret == 0);
    }

    /* [12] infer_register duplicate replaces */
    {
        long ret = sys_infer_register("test_model", "/tmp/test_infer2.sock");
        check(12, "infer_register duplicate replaces", ret == 0);
    }

    /* [13] infer_request no daemon returns error */
    {
        char resp[64];
        long ret = sys_infer_request("nonexistent", "hello", 5, resp, sizeof(resp));
        check(13, "infer_request no daemon returns error", ret < 0);
    }

    /* [14] infer_request no listener returns error */
    {
        char resp[64];
        long ret = sys_infer_request("test_model", "hello", 5, resp, sizeof(resp));
        check(14, "infer_request no listener returns error", ret < 0);
    }

    /* --- io_uring tests --- */

    /* [15] uring_setup returns valid fd */
    long uring_fd = sys_uring_setup(32, (void *)0);
    check(15, "uring_setup returns valid fd", uring_fd >= 0);

    /* [16] uring_enter NOP completes */
    {
        int ok = 0;
        if (uring_fd >= 0) {
            uring_sqe_t sqe;
            sqe.opcode = IORING_OP_NOP;
            sqe.flags = 0;
            sqe.reserved = 0;
            sqe.fd = 0;
            sqe.off = 0;
            sqe.addr = 0;
            sqe.len = 0;
            sqe.user_data = 1;

            uring_cqe_t cqe;
            long ret = sys_uring_enter(uring_fd, &sqe, 1, &cqe);
            ok = (ret == 1 && cqe.user_data == 1 && cqe.res == 0);
        }
        check(16, "uring_enter NOP completes", ok);
    }

    /* [17] uring_enter READ batch */
    {
        int ok = 0;
        if (uring_fd >= 0) {
            /* Create a pipe with data */
            long rfd = -1, wfd = -1;
            if (sys_pipe(&rfd, &wfd) == 0) {
                sys_fwrite(wfd, "batch_data", 10);

                char buf[32];
                uring_sqe_t sqe;
                sqe.opcode = IORING_OP_READ;
                sqe.flags = 0;
                sqe.reserved = 0;
                sqe.fd = (int32_t)rfd;
                sqe.off = 0;
                sqe.addr = (uint64_t)buf;
                sqe.len = sizeof(buf);
                sqe.user_data = 2;

                uring_cqe_t cqe;
                long ret = sys_uring_enter(uring_fd, &sqe, 1, &cqe);
                ok = (ret == 1 && cqe.res == 10 && cqe.user_data == 2);

                sys_close(rfd);
                sys_close(wfd);
            }
        }
        check(17, "uring_enter READ batch", ok);
    }

    /* [18] uring_enter WRITE batch */
    {
        int ok = 0;
        if (uring_fd >= 0) {
            long rfd = -1, wfd = -1;
            if (sys_pipe(&rfd, &wfd) == 0) {
                const char *msg = "uring_wr";
                uring_sqe_t sqe;
                sqe.opcode = IORING_OP_WRITE;
                sqe.flags = 0;
                sqe.reserved = 0;
                sqe.fd = (int32_t)wfd;
                sqe.off = 0;
                sqe.addr = (uint64_t)msg;
                sqe.len = 8;
                sqe.user_data = 3;

                uring_cqe_t cqe;
                long ret = sys_uring_enter(uring_fd, &sqe, 1, &cqe);
                ok = (ret == 1 && cqe.res == 8 && cqe.user_data == 3);

                /* Verify data arrived */
                if (ok) {
                    char verify[16];
                    long n = sys_read(rfd, verify, 16);
                    ok = (n == 8);
                }

                sys_close(rfd);
                sys_close(wfd);
            }
        }
        check(18, "uring_enter WRITE batch", ok);
    }

    /* [19] uring_enter POLL_ADD */
    {
        int ok = 0;
        if (uring_fd >= 0) {
            long rfd = -1, wfd = -1;
            if (sys_pipe(&rfd, &wfd) == 0) {
                sys_fwrite(wfd, "poll", 4);

                uring_sqe_t sqe;
                sqe.opcode = IORING_OP_POLL_ADD;
                sqe.flags = 0;
                sqe.reserved = 0;
                sqe.fd = (int32_t)rfd;
                sqe.off = 0;
                sqe.addr = 0;
                sqe.len = EPOLLIN;
                sqe.user_data = 4;

                uring_cqe_t cqe;
                long ret = sys_uring_enter(uring_fd, &sqe, 1, &cqe);
                ok = (ret == 1 && cqe.user_data == 4 && (cqe.res & EPOLLIN));

                sys_close(rfd);
                sys_close(wfd);
            }
        }
        check(19, "uring_enter POLL_ADD", ok);
    }

    /* [20] uring close */
    {
        int ok = 0;
        if (uring_fd >= 0) {
            long ret = sys_close(uring_fd);
            ok = (ret == 0);
        }
        check(20, "uring close", ok);
    }

    /* --- Summary --- */
    printf("\n=== Stage 37 Results: %d/%d PASSED ===\n\n",
           pass_count, pass_count + fail_count);

    return 0;
}
