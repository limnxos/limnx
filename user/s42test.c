#include "libc/libc.h"

static int pass_count = 0;
static int fail_count = 0;
static int test_num = 0;

static void PASS(const char *msg) {
    test_num++;
    pass_count++;
    printf("  [%d]  PASS: %s\n", test_num, msg);
}
static void FAIL(const char *msg) {
    test_num++;
    fail_count++;
    printf("  [%d]  FAIL: %s\n", test_num, msg);
}
static void CHECK(int cond, const char *msg) {
    if (cond) PASS(msg); else FAIL(msg);
}

/* --- SIGCHLD test support --- */
static volatile int sigchld_received = 0;
static volatile int sigchld_signum = 0;

static void sigchld_handler(int sig) {
    sigchld_received = 1;
    sigchld_signum = sig;
    sys_sigreturn();
}

int main(void) {
    printf("=== Stage 42: Agent Lifecycle Management ===\n");

    /* --- SIGCHLD Tests --- */

    /* 1. SIGCHLD delivered on child exit */
    {
        sigchld_received = 0;
        sigchld_signum = 0;
        sys_sigaction(SIGCHLD, sigchld_handler);

        long pid = sys_fork();
        if (pid == 0) {
            sys_exit(42);
        }
        sys_waitpid(pid);
        /* After waitpid returns, signal should have been delivered */
        /* Give a few yields for signal delivery */
        for (int i = 0; i < 20 && !sigchld_received; i++)
            sys_yield();
        CHECK(sigchld_received, "SIGCHLD delivered on child exit");
    }

    /* 2. SIGCHLD handler receives correct signal number */
    {
        CHECK(sigchld_signum == SIGCHLD, "SIGCHLD handler receives signal 20");
    }

    /* 3. SIGCHLD not delivered if no handler (SIG_DFL) */
    {
        sigchld_received = 0;
        sys_sigaction(SIGCHLD, (void (*)(int))0); /* SIG_DFL */

        long pid = sys_fork();
        if (pid == 0) {
            sys_exit(0);
        }
        sys_waitpid(pid);
        for (int i = 0; i < 20; i++)
            sys_yield();
        CHECK(!sigchld_received, "SIGCHLD not delivered with SIG_DFL");
    }

    /* 4. SIGCHLD with multiple children */
    {
        sigchld_received = 0;
        sys_sigaction(SIGCHLD, sigchld_handler);

        long p1 = sys_fork();
        if (p1 == 0) sys_exit(1);
        long p2 = sys_fork();
        if (p2 == 0) sys_exit(2);

        sys_waitpid(p1);
        sys_waitpid(p2);
        for (int i = 0; i < 20 && !sigchld_received; i++)
            sys_yield();
        CHECK(sigchld_received, "SIGCHLD with multiple children");
    }

    /* --- Agent Message Protocol Tests --- */

    /* 5. agent_msg_send/recv over unix socket */
    {
        /* Create a unix socket pair via bind+connect */
        long sfd = sys_unix_socket();
        sys_unix_bind(sfd, "/tmp/msg_test");
        sys_unix_listen(sfd);

        long pid = sys_fork();
        if (pid == 0) {
            sys_close(sfd);
            long cfd = sys_unix_connect("/tmp/msg_test");
            if (cfd < 0) sys_exit(1);

            /* Send a message */
            agent_msg_t msg;
            msg.type = AMSG_REQUEST;
            msg.len = 5;
            msg.payload[0] = 'h'; msg.payload[1] = 'e';
            msg.payload[2] = 'l'; msg.payload[3] = 'l';
            msg.payload[4] = 'o';
            agent_msg_send((int)cfd, &msg);
            sys_close(cfd);
            sys_exit(0);
        }

        long afd = sys_unix_accept(sfd);
        agent_msg_t rmsg;
        int r = agent_msg_recv((int)afd, &rmsg);
        int ok = (r == 0 && rmsg.type == AMSG_REQUEST && rmsg.len == 5);
        if (ok) {
            ok = (rmsg.payload[0] == 'h' && rmsg.payload[1] == 'e' &&
                  rmsg.payload[2] == 'l' && rmsg.payload[3] == 'l' &&
                  rmsg.payload[4] == 'o');
        }
        sys_close(afd);
        sys_close(sfd);
        sys_waitpid(pid);
        CHECK(ok, "agent_msg_send/recv over unix socket");
    }

    /* 6. agent_msg preserves type and payload */
    {
        long sfd = sys_unix_socket();
        sys_unix_bind(sfd, "/tmp/msg_test2");
        sys_unix_listen(sfd);

        long pid = sys_fork();
        if (pid == 0) {
            sys_close(sfd);
            long cfd = sys_unix_connect("/tmp/msg_test2");
            if (cfd < 0) sys_exit(1);

            agent_msg_t msg;
            msg.type = AMSG_RESPONSE;
            msg.len = 3;
            msg.payload[0] = 'A'; msg.payload[1] = 'B'; msg.payload[2] = 'C';
            agent_msg_send((int)cfd, &msg);

            /* Send heartbeat with no payload */
            agent_msg_t hb;
            hb.type = AMSG_HEARTBEAT;
            hb.len = 0;
            agent_msg_send((int)cfd, &hb);

            sys_close(cfd);
            sys_exit(0);
        }

        long afd = sys_unix_accept(sfd);

        agent_msg_t m1, m2;
        int r1 = agent_msg_recv((int)afd, &m1);
        int r2 = agent_msg_recv((int)afd, &m2);

        int ok = (r1 == 0 && m1.type == AMSG_RESPONSE && m1.len == 3);
        ok = ok && (m1.payload[0] == 'A' && m1.payload[1] == 'B' && m1.payload[2] == 'C');
        ok = ok && (r2 == 0 && m2.type == AMSG_HEARTBEAT && m2.len == 0);

        sys_close(afd);
        sys_close(sfd);
        sys_waitpid(pid);
        CHECK(ok, "agent_msg preserves type and payload");
    }

    /* --- Supervisor Pattern Tests --- */

    /* 7. Supervisor detects child exit via SIGCHLD + waitpid(WNOHANG) */
    {
        sigchld_received = 0;
        sys_sigaction(SIGCHLD, sigchld_handler);

        long pid = sys_fork();
        if (pid == 0) {
            /* Simulate an agent that does work then exits */
            for (int i = 0; i < 10; i++)
                sys_yield();
            sys_exit(0);
        }

        /* Supervisor loop: wait for SIGCHLD */
        int detected = 0;
        for (int i = 0; i < 200; i++) {
            if (sigchld_received) {
                long st = sys_waitpid_flags(pid, 1 /* WNOHANG */);
                if (st >= 0) {
                    detected = 1;
                    break;
                }
            }
            sys_yield();
        }
        if (!detected) sys_waitpid(pid); /* cleanup */
        CHECK(detected, "supervisor detects child exit via SIGCHLD");
    }

    /* 8. Supervisor auto-restarts crashed agent */
    {
        sigchld_received = 0;
        sys_sigaction(SIGCHLD, sigchld_handler);

        int restarts = 0;
        int max_restarts = 2;
        long pid = sys_fork();
        if (pid == 0) sys_exit(1); /* simulate crash */

        for (int attempt = 0; attempt < 10; attempt++) {
            /* Wait for child */
            long st = sys_waitpid(pid);
            sigchld_received = 0;

            if (st != 0 && restarts < max_restarts) {
                /* Child crashed — restart */
                restarts++;
                pid = sys_fork();
                if (pid == 0) {
                    if (restarts < max_restarts)
                        sys_exit(1); /* crash again */
                    else
                        sys_exit(0); /* finally succeed */
                }
            } else {
                break;
            }
        }
        CHECK(restarts == max_restarts, "supervisor auto-restarts crashed agent");
    }

    /* 9. Supervisor creates namespace for agents */
    {
        long ns = sys_ns_create("supervisor_ns");
        int created = (ns > 0);
        long ret = sys_ns_join(ns);
        int joined = (ret == 0);

        /* Register supervisor in its namespace */
        sys_agent_register("supervisor");
        long pid_out = 0;
        sys_agent_lookup("supervisor", &pid_out);
        int registered = (pid_out == (long)sys_getpid());

        sys_ns_join(0); /* back to global */
        CHECK(created && joined && registered, "supervisor creates namespace");
    }

    /* 10. Supervisor grants token to agent */
    {
        long child_pid = sys_fork();
        if (child_pid == 0) {
            /* Child: drop FS_READ, try to open, fail, then exit */
            long caps = sys_getcap();
            sys_setcap(0, caps & ~0x80);
            /* The parent should have created a token for us */
            /* Try to read a file */
            sys_create("/agent_data.txt");
            /* Can't create without CAP_FS_WRITE... just try open */
            long fd = sys_open("/agent_data.txt", 0);
            sys_exit(fd >= 0 ? 0 : 1);
        }

        /* Parent: create file and token before child runs */
        sys_create("/agent_data.txt");
        long fd = sys_open("/agent_data.txt", 1);
        if (fd >= 0) {
            sys_fwrite(fd, "data", 4);
            sys_close(fd);
        }
        long tok = sys_token_create(0x80, child_pid, "/agent_data.txt");
        int tok_ok = (tok > 0);

        long st = sys_waitpid(child_pid);
        CHECK(tok_ok && st == 0, "supervisor grants token to agent");
        if (tok > 0) sys_token_revoke(tok);
        sys_unlink("/agent_data.txt");
    }

    /* 11. Agent IPC: request-response pattern */
    {
        long sfd = sys_unix_socket();
        sys_unix_bind(sfd, "/tmp/agent_ipc");
        sys_unix_listen(sfd);

        long pid = sys_fork();
        if (pid == 0) {
            sys_close(sfd);
            long cfd = sys_unix_connect("/tmp/agent_ipc");
            if (cfd < 0) sys_exit(1);

            /* Send request */
            agent_msg_t req;
            req.type = AMSG_REQUEST;
            req.len = 4;
            req.payload[0] = 'p'; req.payload[1] = 'i';
            req.payload[2] = 'n'; req.payload[3] = 'g';
            agent_msg_send((int)cfd, &req);

            /* Read response */
            agent_msg_t resp;
            int r = agent_msg_recv((int)cfd, &resp);
            sys_close(cfd);
            if (r == 0 && resp.type == AMSG_RESPONSE && resp.len == 4 &&
                resp.payload[0] == 'p' && resp.payload[1] == 'o' &&
                resp.payload[2] == 'n' && resp.payload[3] == 'g')
                sys_exit(0);
            sys_exit(1);
        }

        long afd = sys_unix_accept(sfd);
        agent_msg_t req;
        agent_msg_recv((int)afd, &req);

        /* Echo back as response */
        agent_msg_t resp;
        resp.type = AMSG_RESPONSE;
        resp.len = 4;
        resp.payload[0] = 'p'; resp.payload[1] = 'o';
        resp.payload[2] = 'n'; resp.payload[3] = 'g';
        agent_msg_send((int)afd, &resp);

        sys_close(afd);
        sys_close(sfd);
        long st = sys_waitpid(pid);
        CHECK(st == 0, "agent IPC request-response pattern");
    }

    /* 12. Full lifecycle: spawn, monitor, communicate, restart */
    {
        sys_sigaction(SIGCHLD, sigchld_handler);
        sigchld_received = 0;

        /* Spawn a "worker" agent */
        long pid = sys_fork();
        if (pid == 0) {
            /* Worker: register, do work, exit */
            sys_agent_register("worker");
            for (int i = 0; i < 5; i++)
                sys_yield();
            sys_exit(0);
        }

        /* Wait for worker to complete */
        long st = sys_waitpid(pid);
        int worker_ok = (st == 0);

        /* Verify agent entry was cleaned up */
        long pid_out = 0;
        int lookup = sys_agent_lookup("worker", &pid_out);
        int cleaned = (lookup != 0); /* should not be found */

        CHECK(worker_ok && cleaned, "full lifecycle: spawn, register, exit, cleanup");
    }

    printf("=== Stage 42 Results: %d/%d PASSED ===\n",
           pass_count, pass_count + fail_count);
    return fail_count > 0 ? 1 : 0;
}
