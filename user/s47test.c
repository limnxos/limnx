/*
 * s47test.c — Stage 47 Tests: Process Table Locking + Model Serving Infrastructure
 *
 * Phase 1: Process table locking (concurrent fork stress on SMP)
 * Phase 2: Model serving infrastructure (registration, health, routing, failover)
 */

#include "libc/libc.h"

static int test_num = 0;
static int pass_count = 0;

static void check(int ok, const char *desc) {
    test_num++;
    if (ok) {
        printf("  [%d] PASS: %s\n", test_num, desc);
        pass_count++;
    } else {
        printf("  [%d] FAIL: %s\n", test_num, desc);
    }
}

/* ============================================================
 * Phase 1: Process Table Locking — Concurrent Fork Stress
 * ============================================================ */

static void test_concurrent_fork(void) {
    /* Fork 8 children rapidly. Each child exits immediately.
     * On SMP without proc_table locking, this could corrupt the table. */
    long pids[8];
    int fork_ok = 1;

    for (int i = 0; i < 8; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: yield a few times to exercise SMP scheduling, then exit */
            for (int j = 0; j < 3; j++)
                sys_yield();
            sys_exit(0);
        }
        if (pid < 0) {
            fork_ok = 0;
            pids[i] = 0;
        } else {
            pids[i] = pid;
        }
    }

    /* Test 1: All 8 forks succeeded */
    check(fork_ok, "8 rapid forks succeeded under SMP");

    /* Reap all children */
    int reap_ok = 1;
    for (int i = 0; i < 8; i++) {
        if (pids[i] > 0) {
            long r = sys_waitpid(pids[i]);
            if (r < 0) reap_ok = 0;
        }
    }

    /* Test 2: All children reaped */
    check(reap_ok, "all 8 children reaped successfully");
}

static void test_fork_and_lookup(void) {
    /* Fork a child, have parent look up the child by PID before it exits */
    long child_pid = sys_fork();
    if (child_pid == 0) {
        /* Child: sleep a bit then exit */
        for (int i = 0; i < 10; i++)
            sys_yield();
        sys_exit(42);
    }

    /* Test 3: Child PID is valid (positive) */
    check(child_pid > 0, "fork returns valid child PID");

    /* Test 4: Waitpid returns child's exit status */
    long r = sys_waitpid(child_pid);
    check(r == 42, "waitpid returns child exit status (42)");
}

static void test_nested_fork(void) {
    /* Parent forks child, child forks grandchild. Tests PID allocation under lock. */
    long child_pid = sys_fork();
    if (child_pid == 0) {
        /* Child: fork grandchild */
        long gc_pid = sys_fork();
        if (gc_pid == 0) {
            /* Grandchild */
            sys_exit(0);
        }
        if (gc_pid > 0)
            sys_waitpid(gc_pid);
        sys_exit(gc_pid > 0 ? 0 : 1);
    }

    /* Test 5: Nested fork+waitpid works (child exits 0 on success) */
    long r = sys_waitpid(child_pid);
    check(r == 0, "nested fork (parent→child→grandchild) works");
}

/* ============================================================
 * Phase 2: Model Serving Infrastructure
 * ============================================================ */

/* Simple inline inference daemon — runs in forked child.
 * Registers as service, reports health, serves one request, exits. */
static void mini_inferd(const char *name, const char *sock_path) {
    long sock_fd = sys_unix_socket();
    if (sock_fd < 0) sys_exit(1);

    if (sys_unix_bind(sock_fd, sock_path) < 0) { sys_close(sock_fd); sys_exit(1); }
    if (sys_unix_listen(sock_fd) < 0) { sys_close(sock_fd); sys_exit(1); }
    if (sys_infer_register(name, sock_path) < 0) { sys_close(sock_fd); sys_exit(1); }

    /* Report healthy with load=0 */
    sys_infer_health(0);

    /* Serve one request */
    long client_fd = sys_unix_accept(sock_fd);
    if (client_fd >= 0) {
        char req[128];
        long n = sys_read(client_fd, req, sizeof(req) - 1);
        if (n > 0) {
            req[n] = '\0';
            const char *resp = "inference-ok";
            int rlen = 0;
            while (resp[rlen]) rlen++;
            sys_fwrite(client_fd, resp, rlen);
        }
        sys_close(client_fd);
    }

    /* Update health to show we handled a request */
    sys_infer_health(1);

    /* Wait for more potential requests (up to 50 yields) */
    for (int i = 0; i < 50; i++)
        sys_yield();

    sys_close(sock_fd);
    sys_exit(0);
}

static void test_service_registration(void) {
    /* Fork child as inference daemon */
    long daemon_pid = sys_fork();
    if (daemon_pid == 0) {
        mini_inferd("test-svc", "/tmp/test_infer.sock");
        sys_exit(0); /* unreachable */
    }

    /* Give daemon time to register */
    for (int i = 0; i < 20; i++)
        sys_yield();

    /* Test 6: Service registered (infer_route finds it) */
    long route_pid = sys_infer_route("test-svc");
    check(route_pid == daemon_pid, "inference service registered and routable");

    /* Test 7: Send request via sys_infer_request */
    char resp[128];
    long r = sys_infer_request("test-svc", "hello", 5, resp, sizeof(resp) - 1);
    if (r > 0) resp[r] = '\0';
    check(r > 0 && strncmp(resp, "inference-ok", 12) == 0,
          "inference request returns correct response");

    /* Cleanup */
    sys_waitpid(daemon_pid);
}

static void test_load_balancing(void) {
    /* Spawn two daemons with the same service name but different loads */
    long d1_pid = sys_fork();
    if (d1_pid == 0) {
        long sock_fd = sys_unix_socket();
        if (sock_fd < 0) sys_exit(1);
        if (sys_unix_bind(sock_fd, "/tmp/lb1.sock") < 0) sys_exit(1);
        if (sys_unix_listen(sock_fd) < 0) sys_exit(1);
        if (sys_infer_register("lb-svc", "/tmp/lb1.sock") < 0) sys_exit(1);
        sys_infer_health(10);  /* high load */
        for (int i = 0; i < 100; i++) sys_yield();
        sys_close(sock_fd);
        sys_exit(0);
    }

    long d2_pid = sys_fork();
    if (d2_pid == 0) {
        long sock_fd = sys_unix_socket();
        if (sock_fd < 0) sys_exit(1);
        if (sys_unix_bind(sock_fd, "/tmp/lb2.sock") < 0) sys_exit(1);
        if (sys_unix_listen(sock_fd) < 0) sys_exit(1);
        if (sys_infer_register("lb-svc", "/tmp/lb2.sock") < 0) sys_exit(1);
        sys_infer_health(1);   /* low load */
        for (int i = 0; i < 100; i++) sys_yield();
        sys_close(sock_fd);
        sys_exit(0);
    }

    /* Give daemons time to register and report health */
    for (int i = 0; i < 20; i++)
        sys_yield();

    /* Test 8: Route picks lowest-load daemon (d2_pid with load=1) */
    long routed = sys_infer_route("lb-svc");
    check(routed == d2_pid, "load balancer routes to lowest-load instance");

    /* Cleanup */
    sys_kill(d1_pid, 9);
    sys_kill(d2_pid, 9);
    sys_waitpid(d1_pid);
    sys_waitpid(d2_pid);
}

static void test_failover(void) {
    /* Spawn two daemons, kill one, verify route goes to survivor */
    long d1_pid = sys_fork();
    if (d1_pid == 0) {
        long sock_fd = sys_unix_socket();
        if (sock_fd < 0) sys_exit(1);
        if (sys_unix_bind(sock_fd, "/tmp/fo1.sock") < 0) sys_exit(1);
        if (sys_unix_listen(sock_fd) < 0) sys_exit(1);
        if (sys_infer_register("fo-svc", "/tmp/fo1.sock") < 0) sys_exit(1);
        sys_infer_health(0);
        for (int i = 0; i < 200; i++) sys_yield();
        sys_close(sock_fd);
        sys_exit(0);
    }

    long d2_pid = sys_fork();
    if (d2_pid == 0) {
        long sock_fd = sys_unix_socket();
        if (sock_fd < 0) sys_exit(1);
        if (sys_unix_bind(sock_fd, "/tmp/fo2.sock") < 0) sys_exit(1);
        if (sys_unix_listen(sock_fd) < 0) sys_exit(1);
        if (sys_infer_register("fo-svc", "/tmp/fo2.sock") < 0) sys_exit(1);
        sys_infer_health(0);
        for (int i = 0; i < 200; i++) sys_yield();
        sys_close(sock_fd);
        sys_exit(0);
    }

    for (int i = 0; i < 20; i++)
        sys_yield();

    /* Both should be healthy. Kill d1, its entry gets unregistered on exit. */
    sys_kill(d1_pid, 9);
    sys_waitpid(d1_pid);

    /* Give cleanup time */
    for (int i = 0; i < 10; i++)
        sys_yield();

    /* Test 9: Route now goes to surviving daemon d2 */
    long routed = sys_infer_route("fo-svc");
    check(routed == d2_pid, "failover routes to surviving daemon after kill");

    /* Cleanup */
    sys_kill(d2_pid, 9);
    sys_waitpid(d2_pid);
}

static void test_health_reporting(void) {
    /* Fork daemon, have it report health, verify via route */
    long d_pid = sys_fork();
    if (d_pid == 0) {
        long sock_fd = sys_unix_socket();
        if (sock_fd < 0) sys_exit(1);
        if (sys_unix_bind(sock_fd, "/tmp/hr.sock") < 0) sys_exit(1);
        if (sys_unix_listen(sock_fd) < 0) sys_exit(1);
        if (sys_infer_register("hr-svc", "/tmp/hr.sock") < 0) sys_exit(1);
        /* Initially NOT reporting health — should be unhealthy */
        for (int i = 0; i < 30; i++) sys_yield();
        /* Now report health */
        sys_infer_health(5);
        for (int i = 0; i < 100; i++) sys_yield();
        sys_close(sock_fd);
        sys_exit(0);
    }

    for (int i = 0; i < 10; i++)
        sys_yield();

    /* Test 10: Before health report, route should fail (unhealthy) */
    long r1 = sys_infer_route("hr-svc");
    check(r1 < 0, "unhealthy service not routable before heartbeat");

    /* Wait for daemon to report health */
    for (int i = 0; i < 30; i++)
        sys_yield();

    /* Test 11: After health report, route succeeds */
    long r2 = sys_infer_route("hr-svc");
    check(r2 == d_pid, "service becomes routable after health heartbeat");

    /* Cleanup */
    sys_kill(d_pid, 9);
    sys_waitpid(d_pid);
}

static void test_supervisor_restart(void) {
    /* Supervisor pattern: spawn daemon, if it dies, restart it.
     * Uses SIGCHLD + WNOHANG pattern from Stage 42. */
    long first_pid = sys_fork();
    if (first_pid == 0) {
        /* First daemon: register, serve, then exit quickly */
        long sock_fd = sys_unix_socket();
        if (sock_fd < 0) sys_exit(1);
        if (sys_unix_bind(sock_fd, "/tmp/sv.sock") < 0) sys_exit(1);
        if (sys_unix_listen(sock_fd) < 0) sys_exit(1);
        if (sys_infer_register("sv-svc", "/tmp/sv.sock") < 0) sys_exit(1);
        sys_infer_health(0);
        /* Exit after brief period (simulates crash) */
        for (int i = 0; i < 10; i++) sys_yield();
        sys_close(sock_fd);
        sys_exit(0);
    }

    /* Wait for first daemon to register, then die */
    sys_waitpid(first_pid);

    /* Give cleanup time */
    for (int i = 0; i < 5; i++)
        sys_yield();

    /* Supervisor detects death, restarts */
    long restart_pid = sys_fork();
    if (restart_pid == 0) {
        long sock_fd = sys_unix_socket();
        if (sock_fd < 0) sys_exit(1);
        if (sys_unix_bind(sock_fd, "/tmp/sv2.sock") < 0) sys_exit(1);
        if (sys_unix_listen(sock_fd) < 0) sys_exit(1);
        if (sys_infer_register("sv-svc", "/tmp/sv2.sock") < 0) sys_exit(1);
        sys_infer_health(0);
        for (int i = 0; i < 100; i++) sys_yield();
        sys_close(sock_fd);
        sys_exit(0);
    }

    for (int i = 0; i < 20; i++)
        sys_yield();

    /* Test 12: Restarted daemon is routable */
    long routed = sys_infer_route("sv-svc");
    check(routed == restart_pid, "supervisor-restarted daemon is routable");

    /* Cleanup */
    sys_kill(restart_pid, 9);
    sys_waitpid(restart_pid);
}

int main(void) {
    printf("=== Stage 47 Tests: Process Table Locking + Model Serving ===\n\n");

    printf("--- Phase 1: Process Table Locking ---\n");
    test_concurrent_fork();
    test_fork_and_lookup();
    test_nested_fork();

    printf("\n--- Phase 2: Model Serving Infrastructure ---\n");
    test_service_registration();
    test_load_balancing();
    test_failover();
    test_health_reporting();
    test_supervisor_restart();

    printf("\n=== Stage 47 Results: %d/%d passed ===\n", pass_count, test_num);
    return 0;
}
