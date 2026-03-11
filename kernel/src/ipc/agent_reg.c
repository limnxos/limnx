#define pr_fmt(fmt) "[agent] " fmt
#include "klog.h"

#include "ipc/agent_reg.h"
#include "ipc/cap_token.h"
#include "proc/process.h"
#include "sync/spinlock.h"
#include "errno.h"
#include "kutil.h"

static agent_entry_t agent_table[MAX_AGENTS];
/*
 * Lock ordering: agent_lock is at level 4 (subsystem).
 * Must NOT hold sched_lock, pmm_lock, or kheap_lock when acquiring.
 * Does NOT call kmalloc or pmm_alloc while held.
 * May call process_lookup while held (lightweight, no locks taken).
 */
static spinlock_t agent_lock = SPINLOCK_INIT;

int agent_register_ns(const char *name, uint64_t pid, uint32_t ns_id) {
    uint64_t flags;
    spin_lock_irqsave(&agent_lock, &flags);

    /* Check if name already exists in this namespace — replace */
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!agent_table[i].used) continue;
        if (agent_table[i].ns_id != ns_id) continue;
        if (str_eq(agent_table[i].name, name)) {
            agent_table[i].pid = pid;
            /* Reset mailbox on re-registration */
            agent_table[i].mbox.head = 0;
            agent_table[i].mbox.tail = 0;
            agent_table[i].mbox.count = 0;
            spin_unlock_irqrestore(&agent_lock, flags);
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
            agent_table[i].mbox.head = 0;
            agent_table[i].mbox.tail = 0;
            agent_table[i].mbox.count = 0;
            spin_unlock_irqrestore(&agent_lock, flags);
            return 0;
        }
    }

    spin_unlock_irqrestore(&agent_lock, flags);
    pr_err("agent table full\n");
    return -ENOMEM;
}

int agent_lookup_ns(const char *name, uint64_t *pid_out, uint32_t ns_id) {
    uint64_t flags;
    spin_lock_irqsave(&agent_lock, &flags);

    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!agent_table[i].used) continue;
        if (agent_table[i].ns_id != ns_id) continue;
        if (!str_eq(agent_table[i].name, name)) continue;

        /* Validate pid is still alive */
        process_t *proc = process_lookup(agent_table[i].pid);
        if (!proc || (proc->main_thread && proc->main_thread->state == THREAD_DEAD)) {
            agent_table[i].used = 0;
            continue;
        }

        if (pid_out)
            *pid_out = agent_table[i].pid;
        spin_unlock_irqrestore(&agent_lock, flags);
        return 0;
    }

    spin_unlock_irqrestore(&agent_lock, flags);
    return -ESRCH;
}

/* Legacy wrappers for global namespace */
int agent_register(const char *name, uint64_t pid) {
    return agent_register_ns(name, pid, 0);
}

int agent_lookup(const char *name, uint64_t *pid_out) {
    return agent_lookup_ns(name, pid_out, 0);
}

void agent_unregister_pid(uint64_t pid) {
    uint64_t flags;
    spin_lock_irqsave(&agent_lock, &flags);
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (agent_table[i].used && agent_table[i].pid == pid)
            agent_table[i].used = 0;
    }
    spin_unlock_irqrestore(&agent_lock, flags);
}

void agent_unregister_ns(uint32_t ns_id) {
    uint64_t flags;
    spin_lock_irqsave(&agent_lock, &flags);
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (agent_table[i].used && agent_table[i].ns_id == ns_id)
            agent_table[i].used = 0;
    }
    spin_unlock_irqrestore(&agent_lock, flags);
}

int agent_send(const char *name, uint32_t ns_id,
               uint64_t sender_pid, uint32_t sender_caps,
               const uint8_t *data, uint32_t len,
               uint32_t token_id) {
    (void)sender_caps;  /* reserved for future capability checks */
    if (len > AGENT_MSG_MAX) len = AGENT_MSG_MAX;

    uint64_t flags;
    spin_lock_irqsave(&agent_lock, &flags);

    /* Find target agent */
    int target_idx = -1;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (!agent_table[i].used) continue;
        /* Allow send within same namespace or to global namespace */
        if (agent_table[i].ns_id != ns_id && agent_table[i].ns_id != 0 && ns_id != 0)
            continue;
        if (str_eq(agent_table[i].name, name)) {
            target_idx = i;
            break;
        }
    }

    if (target_idx < 0) {
        spin_unlock_irqrestore(&agent_lock, flags);
        return -ENOENT;
    }

    agent_mbox_t *mbox = &agent_table[target_idx].mbox;
    if (mbox->count >= AGENT_MBOX_SLOTS) {
        spin_unlock_irqrestore(&agent_lock, flags);
        return -ENOBUFS;
    }

    /* Handle token delegation */
    uint32_t delegated_token = 0;
    if (token_id != 0) {
        uint64_t target_pid = agent_table[target_idx].pid;
        /* Delegate sub-token to recipient (same perms, same resource) */
        spin_unlock_irqrestore(&agent_lock, flags);
        int new_tok = cap_token_delegate(token_id, sender_pid,
                                          target_pid, 0, "");
        spin_lock_irqsave(&agent_lock, &flags);
        if (new_tok > 0)
            delegated_token = (uint32_t)new_tok;
        /* Re-validate target still exists after releasing lock */
        if (!agent_table[target_idx].used) {
            spin_unlock_irqrestore(&agent_lock, flags);
            return -ENOENT;
        }
        mbox = &agent_table[target_idx].mbox;
    }

    /* Enqueue message */
    agent_mbox_msg_t *slot = &mbox->msgs[mbox->tail];
    slot->sender_pid = sender_pid;
    slot->token_id = delegated_token;
    slot->len = len;
    for (uint32_t i = 0; i < len; i++)
        slot->data[i] = data[i];

    mbox->tail = (mbox->tail + 1) % AGENT_MBOX_SLOTS;
    mbox->count++;

    spin_unlock_irqrestore(&agent_lock, flags);
    return 0;
}

int agent_recv(uint64_t pid,
               uint64_t *sender_pid_out, uint32_t *token_id_out,
               uint8_t *data, uint32_t max_len) {
    uint64_t flags;
    spin_lock_irqsave(&agent_lock, &flags);

    /* Find agent entry for this pid */
    int idx = -1;
    for (int i = 0; i < MAX_AGENTS; i++) {
        if (agent_table[i].used && agent_table[i].pid == pid) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        spin_unlock_irqrestore(&agent_lock, flags);
        return -ESRCH;
    }

    agent_mbox_t *mbox = &agent_table[idx].mbox;
    if (mbox->count == 0) {
        spin_unlock_irqrestore(&agent_lock, flags);
        return -EAGAIN;
    }

    /* Dequeue message */
    agent_mbox_msg_t *slot = &mbox->msgs[mbox->head];
    if (sender_pid_out) *sender_pid_out = slot->sender_pid;
    if (token_id_out) *token_id_out = slot->token_id;

    uint32_t copy_len = slot->len;
    if (copy_len > max_len) copy_len = max_len;
    for (uint32_t i = 0; i < copy_len; i++)
        data[i] = slot->data[i];

    mbox->head = (mbox->head + 1) % AGENT_MBOX_SLOTS;
    mbox->count--;

    spin_unlock_irqrestore(&agent_lock, flags);
    return (int)copy_len;
}
