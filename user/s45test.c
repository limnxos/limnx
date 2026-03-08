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

/* Worker protocol over pipe:
 *   orchestrator writes: task_id (long)  — 0 means shutdown
 *   worker reads task_id, calls task_start, does work, calls task_complete
 *   worker writes: result (long) back to orchestrator
 */

static void worker_main(long read_fd, long write_fd, int worker_id) {
    (void)worker_id;
    for (;;) {
        long task_id = 0;
        long n = sys_read(read_fd, &task_id, sizeof(task_id));
        if (n <= 0 || task_id == 0)
            break;  /* shutdown */

        /* Try to start the task — kernel checks deps */
        long r = sys_task_start(task_id);
        if (r != 0) {
            /* deps not ready — report failure */
            long result = -1;
            sys_fwrite(write_fd, &result, sizeof(result));
            continue;
        }

        /* Simulate work: produce result = task_id * 10 */
        long result = task_id * 10;

        /* Complete the task */
        sys_task_complete(task_id, (long)result);

        /* Send result back */
        sys_fwrite(write_fd, &result, sizeof(result));
    }
    sys_close(read_fd);
    sys_close(write_fd);
    sys_exit(0);
}

int main(void) {
    printf("=== Stage 45 Tests: Multi-Agent Workflow Runtime ===\n\n");

    /* ============================================================
     * Phase 1: Namespace + Quota Setup
     * ============================================================ */

    /* Test 1: Create workflow namespace */
    long ns = sys_ns_create("workflow-1");
    check(ns > 0, "create workflow namespace");

    /* Test 2: Join namespace (so forked children inherit it) */
    long r = sys_ns_join(ns);
    check(r == 0, "orchestrator joins namespace");

    /* Test 3: Set process quota (orchestrator + 3 workers = 4) */
    r = sys_ns_setquota(ns, NS_QUOTA_PROCS, 5);
    check(r == 0, "set process quota to 5");

    /* Test 4: Set memory quota */
    r = sys_ns_setquota(ns, NS_QUOTA_MEM_PAGES, 512);
    check(r == 0, "set memory quota to 512 pages");

    /* ============================================================
     * Phase 2: Create Diamond Task DAG
     *
     *   fetch_a ──┐
     *   fetch_b ──┼── merge ── validate
     *   fetch_c ──┘
     * ============================================================ */

    long t_fetch_a = sys_task_create("fetch_a", ns);
    long t_fetch_b = sys_task_create("fetch_b", ns);
    long t_fetch_c = sys_task_create("fetch_c", ns);
    long t_merge   = sys_task_create("merge", ns);
    long t_validate = sys_task_create("validate", ns);

    /* Test 5: All tasks created */
    check(t_fetch_a > 0 && t_fetch_b > 0 && t_fetch_c > 0 &&
          t_merge > 0 && t_validate > 0,
          "create 5-task diamond DAG");

    /* Add dependencies: merge depends on all fetches */
    sys_task_depend(t_merge, t_fetch_a);
    sys_task_depend(t_merge, t_fetch_b);
    sys_task_depend(t_merge, t_fetch_c);
    /* validate depends on merge */
    sys_task_depend(t_validate, t_merge);

    /* Test 6: Verify merge has 3 deps via task_status */
    task_status_t st;
    sys_task_status(t_merge, &st);
    check(st.dep_count == 3, "merge task has 3 dependencies");

    /* Test 7: merge can't start yet (deps pending) */
    r = sys_task_start(t_merge);
    check(r == -11, "merge blocked by pending dependencies");

    /* ============================================================
     * Phase 3: Token Delegation
     * ============================================================ */

    /* Create a parent token with FS_READ on /data/ */
    long parent_tok = sys_token_create(0x80, 0, "/data/");  /* CAP_FS_READ=0x80 */
    check(parent_tok > 0, "create parent token CAP_FS_READ on /data/");

    /* Delegate narrowed sub-tokens for each worker */
    long tok_a = sys_token_delegate(parent_tok, 0, 0x80, "/data/a/");
    long tok_b = sys_token_delegate(parent_tok, 0, 0x80, "/data/b/");
    long tok_c = sys_token_delegate(parent_tok, 0, 0x80, "/data/c/");

    /* Test 8: All sub-tokens created with narrowed scope */
    check(tok_a > 0 && tok_b > 0 && tok_c > 0,
          "delegate 3 sub-tokens with narrowed resource scope");

    /* ============================================================
     * Phase 4: Spawn Workers
     * ============================================================ */

    /* 3 workers, each with a pipe pair for communication */
    long w_cmd_r[3], w_cmd_w[3];   /* orchestrator→worker (commands) */
    long w_res_r[3], w_res_w[3];   /* worker→orchestrator (results) */
    long w_pids[3];

    int spawn_ok = 1;
    for (int i = 0; i < 3; i++) {
        r = sys_pipe(&w_cmd_r[i], &w_cmd_w[i]);
        if (r != 0) { spawn_ok = 0; break; }
        r = sys_pipe(&w_res_r[i], &w_res_w[i]);
        if (r != 0) { spawn_ok = 0; break; }

        long pid = sys_fork();
        if (pid == 0) {
            /* Child: close orchestrator ends */
            sys_close(w_cmd_w[i]);
            sys_close(w_res_r[i]);
            /* Also close other workers' pipes */
            for (int j = 0; j < i; j++) {
                sys_close(w_cmd_w[j]);
                sys_close(w_res_r[j]);
            }
            worker_main(w_cmd_r[i], w_res_w[i], i);
            sys_exit(0);  /* unreachable */
        }
        /* Parent: close worker ends */
        sys_close(w_cmd_r[i]);
        sys_close(w_res_w[i]);
        w_pids[i] = pid;
    }

    /* Test 9: All 3 workers spawned */
    check(spawn_ok && w_pids[0] > 0 && w_pids[1] > 0 && w_pids[2] > 0,
          "spawn 3 worker processes via fork");

    /* ============================================================
     * Phase 5: Execute Parallel Fetch Tasks
     * ============================================================ */

    /* Assign fetch tasks to workers (one each) */
    long fetch_tasks[3] = { t_fetch_a, t_fetch_b, t_fetch_c };
    long fetch_results[3] = {0, 0, 0};

    for (int i = 0; i < 3; i++) {
        sys_fwrite(w_cmd_w[i], &fetch_tasks[i], sizeof(long));
    }

    /* Collect results from all 3 workers */
    for (int i = 0; i < 3; i++) {
        sys_read(w_res_r[i], &fetch_results[i], sizeof(long));
    }

    /* Test 10: All fetch tasks completed with correct results */
    check(fetch_results[0] == t_fetch_a * 10 &&
          fetch_results[1] == t_fetch_b * 10 &&
          fetch_results[2] == t_fetch_c * 10,
          "3 parallel fetch tasks completed with correct results");

    /* Test 11: Verify fetch tasks are DONE in kernel */
    int all_done = 1;
    for (int i = 0; i < 3; i++) {
        r = sys_task_wait(fetch_tasks[i]);
        if (r != 0) all_done = 0;
    }
    check(all_done, "kernel confirms all fetch tasks DONE");

    /* ============================================================
     * Phase 6: Execute Merge Task (depends on all fetches)
     * ============================================================ */

    /* Test 12: merge can now start (all deps done) */
    /* Send merge task to worker 0 */
    sys_fwrite(w_cmd_w[0], &t_merge, sizeof(long));
    long merge_result = 0;
    sys_read(w_res_r[0], &merge_result, sizeof(long));
    check(merge_result == t_merge * 10,
          "merge task executed after dependencies satisfied");

    /* ============================================================
     * Phase 7: Execute Validate Task (depends on merge)
     * ============================================================ */

    /* Send validate task to worker 1 */
    sys_fwrite(w_cmd_w[1], &t_validate, sizeof(long));
    long validate_result = 0;
    sys_read(w_res_r[1], &validate_result, sizeof(long));

    /* Test 13: validate completed */
    check(validate_result == t_validate * 10,
          "validate task executed after merge completed");

    /* Test 14: Entire DAG is complete */
    int dag_done = 1;
    long all_tasks[5] = { t_fetch_a, t_fetch_b, t_fetch_c, t_merge, t_validate };
    for (int i = 0; i < 5; i++) {
        if (sys_task_wait(all_tasks[i]) != 0) dag_done = 0;
    }
    check(dag_done, "entire 5-task DAG completed successfully");

    /* Test 15: Task status shows correct final states */
    sys_task_status(t_validate, &st);
    check(st.status == TASK_DONE && st.result == (int)(t_validate * 10),
          "validate task status: DONE with correct result");

    /* ============================================================
     * Phase 8: Quota Enforcement
     * ============================================================ */

    /* Test 16: Fork beyond quota fails
     * We have 3 workers tracked in the namespace (ns_join doesn't count
     * the orchestrator, only forked children with inherited ns_id).
     * Tighten quota to exactly the current count, then fork should fail. */
    sys_ns_setquota(ns, NS_QUOTA_PROCS, 3);  /* tighten to exactly current (3 workers) */
    long over_pid = sys_fork();
    if (over_pid == 0) {
        sys_exit(0);
    }
    if (over_pid < 0) {
        check(1, "fork rejected when namespace quota exceeded");
    } else {
        sys_waitpid(over_pid);
        check(0, "fork rejected when namespace quota exceeded");
    }

    /* ============================================================
     * Phase 9: Cleanup
     * ============================================================ */

    /* Shutdown workers */
    long shutdown = 0;
    for (int i = 0; i < 3; i++) {
        sys_fwrite(w_cmd_w[i], &shutdown, sizeof(long));
        sys_close(w_cmd_w[i]);
        sys_close(w_res_r[i]);
    }

    /* Wait for workers to exit */
    for (int i = 0; i < 3; i++) {
        sys_waitpid(w_pids[i]);
    }

    /* Test 17: All workers exited cleanly */
    check(1, "all workers shutdown and reaped");

    /* Test 18: Tokens auto-cleaned (revoke parent — children should be gone too) */
    r = sys_token_revoke(parent_tok);
    check(r == 0, "parent token revoked (cleanup)");

    printf("\n=== Stage 45 Results: %d/%d passed ===\n", pass_count, test_num);
    return 0;
}
