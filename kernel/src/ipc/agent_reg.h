#ifndef LIMNX_AGENT_REG_H
#define LIMNX_AGENT_REG_H

#include <stdint.h>

#define MAX_AGENTS         16
#define AGENT_NAME_MAX     32
#define AGENT_MBOX_SLOTS   4
#define AGENT_MSG_MAX      256

/* Per-slot message in agent mailbox */
typedef struct agent_mbox_msg {
    uint64_t sender_pid;
    uint32_t token_id;      /* delegated token ID, 0 = none */
    uint32_t len;
    uint8_t  data[AGENT_MSG_MAX];
} agent_mbox_msg_t;

/* Agent mailbox — small ring buffer */
typedef struct agent_mbox {
    agent_mbox_msg_t msgs[AGENT_MBOX_SLOTS];
    uint32_t head;   /* next slot to read */
    uint32_t tail;   /* next slot to write */
    uint32_t count;  /* number of messages in queue */
} agent_mbox_t;

typedef struct agent_entry {
    char        name[AGENT_NAME_MAX];
    uint64_t    pid;
    uint32_t    ns_id;     /* namespace this agent belongs to (0 = global) */
    uint8_t     used;
    agent_mbox_t mbox;
} agent_entry_t;

/* Register name -> pid mapping (namespace-scoped). Replaces if name exists in same ns. */
int  agent_register_ns(const char *name, uint64_t pid, uint32_t ns_id);

/* Lookup pid by name (namespace-scoped). Returns 0 and writes *pid_out, or -1. */
int  agent_lookup_ns(const char *name, uint64_t *pid_out, uint32_t ns_id);

/* Legacy: register/lookup in global namespace (ns_id=0) */
int  agent_register(const char *name, uint64_t pid);
int  agent_lookup(const char *name, uint64_t *pid_out);

/* Unregister all entries for a dying process */
void agent_unregister_pid(uint64_t pid);

/* Unregister all entries in a namespace (called when namespace is destroyed) */
void agent_unregister_ns(uint32_t ns_id);

/* Send message to named agent in given namespace.
 * token_id: if non-zero, kernel delegates sub-token to recipient.
 * Returns 0 on success, -ENOENT if agent not found, -ENOSPC if mailbox full. */
int  agent_send(const char *name, uint32_t ns_id,
                uint64_t sender_pid, uint32_t sender_caps,
                const uint8_t *data, uint32_t len,
                uint32_t token_id);

/* Receive next message from own mailbox.
 * Writes sender_pid, token_id, data. Returns message len or -1 if empty. */
int  agent_recv(uint64_t pid,
                uint64_t *sender_pid_out, uint32_t *token_id_out,
                uint8_t *data, uint32_t max_len);

#endif
