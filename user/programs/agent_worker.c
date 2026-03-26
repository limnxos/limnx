/*
 * agent_worker.c — Worker agent for orchestration demo
 *
 * Launched by orchestrator via supervisor. Discovers task/result topics
 * by well-known IDs (0=tasks, 1=results). Registers with agent registry,
 * processes tasks by calling inference, publishes results via pub/sub.
 */

#include "../libc/libc.h"

int main(void) {
    long my_pid = sys_getpid();

    /* Worker ID derived from PID (simple, unique per worker) */
    int worker_id = (int)(my_pid % 100);

    printf("[worker %d] pid=%ld starting\n", worker_id, my_pid);

    /* Register with agent registry */
    char name[32];
    int nlen = 0;
    const char *pfx = "worker_";
    while (*pfx) name[nlen++] = *pfx++;
    name[nlen++] = '0' + (char)(worker_id % 10);
    name[nlen] = '\0';
    sys_agent_register(name);

    /* Subscribe to results topic (ID 1 — created by orchestrator) */
    long result_topic = 1;
    sys_topic_subscribe(result_topic);

    /* Subscribe to tasks topic (ID 0 — created by orchestrator) */
    long task_topic = 0;
    sys_topic_subscribe(task_topic);

    /* Apply seccomp sandbox — only allow essential syscalls.
     * Blocks: fork, exec, open, unlink, kill, socket, mmap (except brk).
     * Allows: read(0), write(1), close(3), fstat(5), mmap(9), munmap(11),
     *         brk(12), sigaction(13), sigreturn(15), ioctl(16), writev(20),
     *         yield(24), nanosleep(35), getpid(39), exit(60), exit_group(231).
     * All Limnx syscalls (512+) pass through automatically. */
    {
        unsigned long mask_lo = 0;
        unsigned long mask_hi = 0;
        /* Bits 0-63 */
        mask_lo |= (1UL << 0);   /* read */
        mask_lo |= (1UL << 1);   /* write */
        mask_lo |= (1UL << 3);   /* close */
        mask_lo |= (1UL << 5);   /* fstat */
        mask_lo |= (1UL << 9);   /* mmap */
        mask_lo |= (1UL << 11);  /* munmap */
        mask_lo |= (1UL << 12);  /* brk */
        mask_lo |= (1UL << 13);  /* rt_sigaction */
        mask_lo |= (1UL << 15);  /* rt_sigreturn */
        mask_lo |= (1UL << 16);  /* ioctl */
        mask_lo |= (1UL << 20);  /* writev */
        mask_lo |= (1UL << 24);  /* sched_yield */
        mask_lo |= (1UL << 35);  /* nanosleep */
        mask_lo |= (1UL << 39);  /* getpid */
        mask_lo |= (1UL << 60);  /* exit */
        /* Bits 64-127 */
        mask_hi |= (1UL << (231 - 64));  /* exit_group */
        sys_seccomp(mask_lo, 1 /* strict */, mask_hi);
        printf("[worker %d] seccomp sandbox active\n", worker_id);
    }

    /* Process tasks */
    int tasks_done = 0;
    for (int attempt = 0; attempt < 200; attempt++) {
        /* Try to receive a task assignment from task topic */
        char task_buf[256];
        unsigned long sender_pid = 0;
        long n = sys_topic_recv(task_topic, task_buf, sizeof(task_buf) - 1,
                                &sender_pid);

        if (n <= 0) {
            /* No task available — yield and retry */
            sys_yield();
            continue;
        }

        task_buf[n] = '\0';
        printf("[worker %d] Got task: %s\n", worker_id, task_buf);

        /* Check for DONE signal */
        if (n >= 4 && task_buf[0] == 'D' && task_buf[1] == 'O' &&
            task_buf[2] == 'N' && task_buf[3] == 'E') break;

        /* Parse task: "TASK:<task_id>:<prompt>" */
        long task_id = -1;
        char *prompt = task_buf;
        if (n >= 5 && task_buf[0] == 'T' && task_buf[4] == ':') {
            task_id = 0;
            int i = 5;
            while (task_buf[i] >= '0' && task_buf[i] <= '9') {
                task_id = task_id * 10 + (task_buf[i] - '0');
                i++;
            }
            if (task_buf[i] == ':') i++;
            prompt = &task_buf[i];
        }

        /* Check if another worker already completed this task */
        if (task_id >= 0) {
            task_status_t ts;
            if (sys_task_status(task_id, &ts) == 0 && ts.status >= TASK_DONE) {
                continue;  /* already done by another worker */
            }
        }

        /* Call inference service */
        char resp[256];
        long rlen = sys_infer_request("default", prompt, strlen(prompt),
                                      resp, sizeof(resp) - 1);

        /* Build result: "RESULT:<worker_id>:<task_id>:<response>" */
        char result[384];
        int rpos = 0;
        const char *rpfx = "RESULT:";
        while (*rpfx) result[rpos++] = *rpfx++;
        result[rpos++] = '0' + (char)(worker_id % 10);
        result[rpos++] = ':';

        /* Append task_id */
        if (task_id >= 0) {
            char tmp[16]; int tlen = 0;
            long v = task_id;
            if (v == 0) { tmp[tlen++] = '0'; }
            else { while (v > 0) { tmp[tlen++] = '0' + (int)(v % 10); v /= 10; } }
            for (int j = tlen - 1; j >= 0; j--) result[rpos++] = tmp[j];
        } else {
            result[rpos++] = '?';
        }
        result[rpos++] = ':';

        /* Append response */
        if (rlen > 0) {
            resp[rlen] = '\0';
            int copylen = (int)rlen;
            if (rpos + copylen > 380) copylen = 380 - rpos;
            for (int j = 0; j < copylen; j++) result[rpos++] = resp[j];
        } else {
            const char *fb = "(processed locally)";
            while (*fb && rpos < 380) result[rpos++] = *fb++;
        }
        result[rpos] = '\0';

        /* Publish result */
        sys_topic_publish(result_topic, result, (unsigned long)rpos);
        printf("[worker %d] Completed task %ld\n", worker_id, task_id);

        /* Mark task complete in the task graph */
        if (task_id >= 0)
            sys_task_complete(task_id, 0);

        tasks_done++;
    }

    printf("[worker %d] Exiting after %d tasks\n", worker_id, tasks_done);
    return 0;
}
