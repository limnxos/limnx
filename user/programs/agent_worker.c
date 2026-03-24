/*
 * agent_worker.c — Worker agent for orchestration demo
 *
 * Launched by orchestrator via supervisor. Registers with agent registry,
 * subscribes to "tasks" topic, processes assigned work by calling inference,
 * publishes results to "results" topic.
 *
 * Usage: agent_worker <worker_id> <task_topic_id> <result_topic_id>
 */

#include "../libc/libc.h"

#define MAX_WORK 8

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("[worker] Usage: agent_worker <id> <task_topic> <result_topic>\n");
        return 1;
    }

    int worker_id = atoi(argv[1]);
    long task_topic = atoi(argv[2]);
    long result_topic = atoi(argv[3]);

    /* Register with agent registry */
    char name[32];
    printf("[worker %d] pid=%ld starting\n", worker_id, sys_getpid());

    int nlen = 0;
    const char *pfx = "worker_";
    while (*pfx) name[nlen++] = *pfx++;
    name[nlen++] = '0' + (char)worker_id;
    name[nlen] = '\0';
    sys_agent_register(name);

    /* Subscribe to result topic for publishing */
    sys_topic_subscribe(result_topic);

    /* Process tasks */
    int tasks_done = 0;
    for (int attempt = 0; attempt < 100; attempt++) {
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

        /* Parse task: "TASK:<task_id>:<prompt>" */
        long task_id = -1;
        char *prompt = task_buf;
        if (task_buf[0] == 'T' && task_buf[1] == 'A' && task_buf[2] == 'S' &&
            task_buf[3] == 'K' && task_buf[4] == ':') {
            /* Parse task_id */
            task_id = 0;
            int i = 5;
            while (task_buf[i] >= '0' && task_buf[i] <= '9') {
                task_id = task_id * 10 + (task_buf[i] - '0');
                i++;
            }
            if (task_buf[i] == ':') i++;
            prompt = &task_buf[i];
        }

        /* Call inference service */
        char resp[256];
        long rlen = sys_infer_request("default", prompt, strlen(prompt),
                                      resp, sizeof(resp) - 1);

        char result[384];
        int rpos = 0;

        /* Format: "RESULT:<worker_id>:<task_id>:<response>" */
        const char *rpfx = "RESULT:";
        while (*rpfx) result[rpos++] = *rpfx++;
        result[rpos++] = '0' + (char)worker_id;
        result[rpos++] = ':';

        /* task_id digits */
        if (task_id >= 0) {
            char tmp[16];
            int tlen = 0;
            long v = task_id;
            if (v == 0) { tmp[tlen++] = '0'; }
            else { while (v > 0) { tmp[tlen++] = '0' + (int)(v % 10); v /= 10; } }
            for (int j = tlen - 1; j >= 0; j--) result[rpos++] = tmp[j];
        } else {
            result[rpos++] = '-';
            result[rpos++] = '1';
        }
        result[rpos++] = ':';

        /* Append inference response or fallback */
        if (rlen > 0) {
            resp[rlen] = '\0';
            int copylen = (int)rlen;
            if (rpos + copylen > 380) copylen = 380 - rpos;
            for (int j = 0; j < copylen; j++) result[rpos++] = resp[j];
        } else {
            const char *fb = "(no inference)";
            while (*fb && rpos < 380) result[rpos++] = *fb++;
        }
        result[rpos] = '\0';

        /* Publish result */
        sys_topic_publish(result_topic, result, (unsigned long)rpos);
        printf("[worker %d] Published result for task %ld\n", worker_id, task_id);

        /* Mark task complete in the task graph */
        if (task_id >= 0) {
            sys_task_complete(task_id, 0);
        }

        tasks_done++;

        /* Check for DONE signal */
        if (strcmp(prompt, "DONE") == 0) break;
    }

    printf("[worker %d] Exiting after %d tasks\n", worker_id, tasks_done);
    return 0;
}
