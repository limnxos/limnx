/*
 * orchestrator.c — Agent orchestration demo for Limnx
 *
 * Demonstrates the full AI-native kernel orchestration stack:
 *   1. Namespace — isolated agent group
 *   2. Supervisor — manages worker lifecycle (auto-restart)
 *   3. Task graph — DAG workflow with dependencies
 *   4. Pub/sub — broadcast task assignments + collect results
 *   5. Capability tokens — scoped permissions for workers
 *   6. Inference service — workers call inferd for AI tasks
 *
 * Usage: orchestrate
 */

#include "../libc/libc.h"

#define NUM_WORKERS  3
#define NUM_TASKS    3
#define CAP_INFER      (1 << 8)
#define CAP_XNS_INFER  (1 << 11)

/* Wait for a task to complete (polling) */
static int wait_task(long task_id, int max_polls) {
    for (int i = 0; i < max_polls; i++) {
        long ret = sys_task_wait(task_id);
        if (ret == 0) return 0;  /* done */
        sys_yield();
    }
    return -1;  /* timeout */
}

int main(void) {
    printf("\n");
    printf("=============================================\n");
    printf("  Limnx Agent Orchestration Demo\n");
    printf("=============================================\n");
    printf("  Kernel primitives: namespace, supervisor,\n");
    printf("  task graph, pub/sub, capability tokens\n");
    printf("=============================================\n\n");

    long my_pid = sys_getpid();

    /* Step 1: Create an isolated namespace */
    printf("[orch] Step 1: Creating namespace...\n");
    long ns_id = sys_ns_create("demo_ns");
    if (ns_id < 0) {
        printf("[orch] ns_create failed (err=%ld), using global ns\n", ns_id);
        ns_id = 0;
    } else {
        printf("[orch] Namespace created: id=%ld\n", ns_id);
        sys_ns_setquota(ns_id, 0 /* NS_QUOTA_PROCS */, 8);
    }

    /* Step 2: Start inference daemon */
    printf("[orch] Step 2: Starting inference daemon...\n");
    long inferd_pid = sys_fork();
    if (inferd_pid == 0) {
        /* Child: exec inferd with test model */
        const char *argv[] = {"inferd", "/test.gguf", "default",
                               "/tmp/inferd.sock", "0", (void *)0};
        sys_execve("/inferd.elf", argv);
        sys_exit(99);
    }
    if (inferd_pid > 0) {
        printf("[orch] inferd started (pid=%ld)\n", inferd_pid);
        /* Wait for inferd to load model and register */
        for (int i = 0; i < 200; i++) sys_yield();
        /* Verify it registered */
        char probe[16];
        long pret = sys_infer_request("default", "?", 1, probe, sizeof(probe));
        if (pret > 0)
            printf("[orch] Inference service ready\n");
        else
            printf("[orch] WARN: inferd not responding yet\n");
    } else {
        printf("[orch] WARN: fork inferd failed\n");
    }

    /* Step 3: Create pub/sub topics */
    printf("[orch] Step 3: Creating pub/sub topics...\n");
    /* Flush inference cache so workers get fresh responses */
    sys_infer_cache_ctrl(2 /* FLUSH */, (void *)0);
    long task_topic = sys_topic_create("tasks", ns_id);
    long result_topic = sys_topic_create("results", ns_id);
    if (task_topic < 0 || result_topic < 0) {
        printf("[orch] FAIL: topic_create (task=%ld result=%ld)\n",
               task_topic, result_topic);
        return 1;
    }
    printf("[orch] Topics: tasks=%ld, results=%ld\n", task_topic, result_topic);

    /* Subscribe to results topic to collect worker output */
    sys_topic_subscribe(result_topic);

    /* Step 4: Create supervisor */
    printf("[orch] Step 4: Creating supervisor...\n");
    long super_id = sys_super_create("demo_super");
    if (super_id < 0) {
        printf("[orch] FAIL: super_create (err=%ld)\n", super_id);
        return 1;
    }
    printf("[orch] Supervisor created: id=%ld\n", super_id);

    /* Set restart policy: one-for-one (restart only crashed worker) */
    sys_super_set_policy(super_id, 0 /* SUPER_ONE_FOR_ONE */);

    /* Step 5: Add workers to supervisor */
    printf("[orch] Step 5: Adding %d workers...\n", NUM_WORKERS);

    /* Build argv strings for workers: agent_worker <id> <task_topic> <result_topic> */
    for (int i = 0; i < NUM_WORKERS; i++) {
        /* Workers get the namespace and basic caps */
        long ret = sys_super_add(super_id, "/agent_worker.elf", ns_id,
                                 CAP_INFER | CAP_XNS_INFER);
        if (ret < 0) {
            printf("[orch] FAIL: super_add worker %d (err=%ld)\n", i, ret);
        } else {
            printf("[orch] Worker %d added (child idx=%ld)\n", i, ret);
        }
    }

    /* Step 6: Create task graph */
    printf("[orch] Step 6: Creating task graph...\n");

    long task_a = sys_task_create("analyze_data", ns_id);
    long task_b = sys_task_create("transform", ns_id);
    long task_c = sys_task_create("summarize", ns_id);

    if (task_a < 0 || task_b < 0 || task_c < 0) {
        printf("[orch] FAIL: task_create (a=%ld b=%ld c=%ld)\n",
               task_a, task_b, task_c);
        return 1;
    }

    /* Dependencies: B depends on A, C depends on B */
    sys_task_depend(task_b, task_a);
    sys_task_depend(task_c, task_b);

    printf("[orch] Tasks: A=%ld → B=%ld → C=%ld (dependency chain)\n",
           task_a, task_b, task_c);

    /* Step 7: Start supervisor (launches all workers) */
    printf("[orch] Step 7: Starting supervisor...\n");
    long launched = sys_super_start(super_id);
    printf("[orch] Launched %ld workers\n", launched);

    /* Give workers time to register and subscribe */
    for (int i = 0; i < 100; i++) sys_yield();

    /* Step 8: Execute task graph */
    printf("\n[orch] === Executing Task Graph ===\n\n");

    /* Task A: start immediately, publish assignment */
    printf("[orch] Starting task A (analyze)...\n");
    sys_task_start(task_a);
    {
        char msg[64];
        int len = 0;
        const char *s = "TASK:";
        while (*s) msg[len++] = *s++;
        /* append task_a id */
        char tmp[8];
        int tlen = 0;
        long v = task_a;
        if (v == 0) { tmp[tlen++] = '0'; }
        else { while (v > 0) { tmp[tlen++] = '0' + (int)(v % 10); v /= 10; } }
        for (int j = tlen - 1; j >= 0; j--) msg[len++] = tmp[j];
        msg[len++] = ':';
        s = "hello world";
        while (*s) msg[len++] = *s++;
        msg[len] = '\0';
        sys_topic_publish(task_topic, msg, (unsigned long)len);
    }

    /* Wait for task A */
    if (wait_task(task_a, 2000) == 0) {
        printf("[orch] Task A completed\n");
    } else {
        printf("[orch] Task A timed out\n");
    }

    /* Task B: depends on A, start after A completes */
    printf("[orch] Starting task B (transform)...\n");
    sys_task_start(task_b);
    {
        char msg[64];
        int len = 0;
        const char *s = "TASK:";
        while (*s) msg[len++] = *s++;
        char tmp[8]; int tlen = 0;
        long v = task_b;
        if (v == 0) { tmp[tlen++] = '0'; }
        else { while (v > 0) { tmp[tlen++] = '0' + (int)(v % 10); v /= 10; } }
        for (int j = tlen - 1; j >= 0; j--) msg[len++] = tmp[j];
        msg[len++] = ':';
        s = "the cat sat on";
        while (*s) msg[len++] = *s++;
        msg[len] = '\0';
        sys_topic_publish(task_topic, msg, (unsigned long)len);
    }

    if (wait_task(task_b, 2000) == 0) {
        printf("[orch] Task B completed\n");
    } else {
        printf("[orch] Task B timed out\n");
    }

    /* Task C: depends on B */
    printf("[orch] Starting task C (summarize)...\n");
    sys_task_start(task_c);
    {
        char msg[64];
        int len = 0;
        const char *s = "TASK:";
        while (*s) msg[len++] = *s++;
        char tmp[8]; int tlen = 0;
        long v = task_c;
        if (v == 0) { tmp[tlen++] = '0'; }
        else { while (v > 0) { tmp[tlen++] = '0' + (int)(v % 10); v /= 10; } }
        for (int j = tlen - 1; j >= 0; j--) msg[len++] = tmp[j];
        msg[len++] = ':';
        s = "limnx ai native";
        while (*s) msg[len++] = *s++;
        msg[len] = '\0';
        sys_topic_publish(task_topic, msg, (unsigned long)len);
    }

    if (wait_task(task_c, 2000) == 0) {
        printf("[orch] Task C completed\n");
    } else {
        printf("[orch] Task C timed out\n");
    }

    /* Step 9: Collect results from pub/sub */
    printf("\n[orch] === Results ===\n\n");

    int results_collected = 0;
    for (int attempt = 0; attempt < 50; attempt++) {
        char rbuf[384];
        unsigned long pub_pid = 0;
        long n = sys_topic_recv(result_topic, rbuf, sizeof(rbuf) - 1, &pub_pid);
        if (n > 0) {
            rbuf[n] = '\0';
            printf("[orch] Result from pid %lu: %s\n", pub_pid, rbuf);
            results_collected++;
        } else {
            sys_yield();
        }
        if (results_collected >= NUM_TASKS) break;
    }

    printf("\n[orch] Collected %d/%d results\n", results_collected, NUM_TASKS);

    /* Step 10: Stop supervisor and clean up */
    printf("[orch] Stopping supervisor...\n");
    sys_super_stop(super_id);

    /* Kill inferd */
    if (inferd_pid > 0)
        sys_kill(inferd_pid, 9 /* SIGKILL */);

    /* Summary */
    printf("\n=============================================\n");
    printf("  Demo Complete\n");
    printf("  Namespace: %ld\n", ns_id);
    printf("  Supervisor: %ld (%d workers)\n", super_id, NUM_WORKERS);
    printf("  Tasks: %d (A→B→C dependency chain)\n", NUM_TASKS);
    printf("  Results: %d collected via pub/sub\n", results_collected);
    printf("=============================================\n");

    return 0;
}
