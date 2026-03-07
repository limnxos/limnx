#include "ipc/agent_reg.h"
#include "proc/process.h"

static agent_entry_t agent_table[MAX_AGENTS];

static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

int agent_register_ns(const char *name, uint64_t pid, uint32_t ns_id) {
    /* Check if name already exists in this namespace — replace */
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!agent_table[i].used) continue;
        if (agent_table[i].ns_id != ns_id) continue;
        if (name_eq(agent_table[i].name, name)) {
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
            agent_table[i].ns_id = ns_id;
            agent_table[i].used = 1;
            return 0;
        }
    }

    return -12;  /* ENOMEM */
}

int agent_lookup_ns(const char *name, uint64_t *pid_out, uint32_t ns_id) {
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!agent_table[i].used) continue;
        if (agent_table[i].ns_id != ns_id) continue;
        if (!name_eq(agent_table[i].name, name)) continue;

        /* Validate pid is still alive */
        process_t *proc = process_lookup(agent_table[i].pid);
        if (!proc || (proc->main_thread && proc->main_thread->state == THREAD_DEAD)) {
            agent_table[i].used = 0;
            continue;
        }

        if (pid_out)
            *pid_out = agent_table[i].pid;
        return 0;
    }
    return -1;
}

/* Legacy wrappers for global namespace */
int agent_register(const char *name, uint64_t pid) {
    return agent_register_ns(name, pid, 0);
}

int agent_lookup(const char *name, uint64_t *pid_out) {
    return agent_lookup_ns(name, pid_out, 0);
}

void agent_unregister_pid(uint64_t pid) {
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (agent_table[i].used && agent_table[i].pid == pid)
            agent_table[i].used = 0;
    }
}

void agent_unregister_ns(uint32_t ns_id) {
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (agent_table[i].used && agent_table[i].ns_id == ns_id)
            agent_table[i].used = 0;
    }
}
