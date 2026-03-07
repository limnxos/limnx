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

int main(void) {
    printf("=== Stage 44 Tests: Agent Orchestration Primitives ===\n\n");

    /* --- Task Graph --- */

    /* Test 1: Create a task in global namespace */
    long t1 = sys_task_create("fetch_data", 0);
    check(t1 > 0, "task_create returns positive ID");

    /* Test 2: Create second task */
    long t2 = sys_task_create("process_data", 0);
    check(t2 > 0 && t2 != t1, "second task gets different ID");

    /* Test 3: Add dependency (t2 depends on t1) */
    long r = sys_task_depend(t2, t1);
    check(r == 0, "task_depend succeeds");

    /* Test 4: Cannot start t2 before t1 is done */
    r = sys_task_start(t2);
    check(r == -11, "task_start fails with -EAGAIN when deps not done");

    /* Test 5: Can start t1 (no deps) */
    r = sys_task_start(t1);
    check(r == 0, "task_start succeeds on task with no deps");

    /* Test 6: Complete t1 */
    r = sys_task_complete(t1, 42);
    check(r == 0, "task_complete succeeds");

    /* Test 7: task_wait on completed task */
    r = sys_task_wait(t1);
    check(r == 0, "task_wait returns 0 for completed task");

    /* Test 8: Now t2 can start (deps satisfied) */
    r = sys_task_start(t2);
    check(r == 0, "task_start succeeds after deps done");

    /* Test 9: Query task status */
    task_status_t st;
    r = sys_task_status(t2, &st);
    check(r == 0 && st.status == TASK_RUNNING, "task_status shows RUNNING");

    /* Test 10: Complete t2 with failure */
    r = sys_task_complete(t2, -1);
    check(r == 0, "task_complete with negative result");

    r = sys_task_status(t2, &st);
    check(r == 0 && st.status == TASK_FAILED, "failed task shows FAILED status");

    /* Test 11: Self-dependency rejected */
    long t3 = sys_task_create("self_dep", 0);
    r = sys_task_depend(t3, t3);
    check(r == -22, "self-dependency returns -EINVAL");

    /* --- Token Delegation --- */

    /* Test 12: Create a token with limited perms, then delegate sub-token */
    long tok = sys_token_create(0x03, 0, NULL);  /* only CAP_NET_BIND | CAP_NET_RAW */
    check(tok > 0, "token_create returns positive ID");

    /* Test 13: Delegate sub-token with subset perms */
    long sub = sys_token_delegate(tok, 0, 0x01, NULL);  /* only CAP_NET_BIND */
    check(sub > 0 && sub != tok, "token_delegate creates sub-token");

    /* Test 14: Delegate with superset perms fails */
    long bad = sys_token_delegate(tok, 0, 0x0F, NULL);  /* includes perms not in parent */
    check(bad < 0, "token_delegate with excess perms fails");

    /* --- Namespace Quotas --- */

    /* Test 15: Create namespace and set quota */
    long ns = sys_ns_create("quota_test");
    check(ns > 0, "ns_create returns positive ns_id");

    r = sys_ns_setquota(ns, NS_QUOTA_PROCS, 2);
    check(r == 0, "ns_setquota succeeds");

    /* Test 16: Invalid namespace quota */
    r = sys_ns_setquota(99, NS_QUOTA_PROCS, 5);
    check(r < 0, "ns_setquota on invalid ns fails");

    /* Test 17: Task in namespace */
    long t4 = sys_task_create("ns_task", ns);
    check(t4 > 0, "task_create in namespace succeeds");

    /* Test 18: task_status shows name */
    r = sys_task_status(t4, &st);
    check(r == 0 && st.name[0] == 'n', "task name preserved in status");

    printf("\n=== Stage 44 Results: %d/%d passed ===\n", pass_count, test_num);
    return 0;
}
