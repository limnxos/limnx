#include "ipc/agent_reg.h"
#include "proc/process.h"

static agent_entry_t agent_table[MAX_AGENTS];

int agent_register(const char *name, uint64_t pid) {
    /* Check if name already exists — replace */
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!agent_table[i].used) continue;
        const char *a = agent_table[i].name;
        const char *b = name;
        int match = 1;
        while (*a && *b) {
            if (*a != *b) { match = 0; break; }
            a++; b++;
        }
        if (match && *a == *b) {
            agent_table[i].pid = pid;
            return 0;
        }
    }

    /* Find free slot */
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!agent_table[i].used) {
            int j = 0;
            while (name[j] && j < AGENT_NAME_MAX - 1) {
                agent_table[i].name[j] = name[j];
                j++;
            }
            agent_table[i].name[j] = '\0';
            agent_table[i].pid = pid;
            agent_table[i].used = 1;
            return 0;
        }
    }

    return -12;  /* ENOMEM */
}

int agent_lookup(const char *name, uint64_t *pid_out) {
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!agent_table[i].used) continue;

        const char *a = agent_table[i].name;
        const char *b = name;
        int match = 1;
        while (*a && *b) {
            if (*a != *b) { match = 0; break; }
            a++; b++;
        }
        if (!match || *a != *b) continue;

        /* Validate pid is still alive */
        process_t *proc = process_lookup(agent_table[i].pid);
        if (!proc || (proc->main_thread && proc->main_thread->state == THREAD_DEAD)) {
            /* Auto-cleanup dead entry */
            agent_table[i].used = 0;
            continue;
        }

        if (pid_out)
            *pid_out = agent_table[i].pid;
        return 0;
    }
    return -1;
}

void agent_unregister_pid(uint64_t pid) {
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (agent_table[i].used && agent_table[i].pid == pid)
            agent_table[i].used = 0;
    }
}
