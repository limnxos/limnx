#ifndef LIMNX_AGENT_REG_H
#define LIMNX_AGENT_REG_H

#include <stdint.h>

#define MAX_AGENTS      16
#define AGENT_NAME_MAX  32

typedef struct agent_entry {
    char     name[AGENT_NAME_MAX];
    uint64_t pid;
    uint32_t ns_id;     /* namespace this agent belongs to (0 = global) */
    uint8_t  used;
} agent_entry_t;

/* Register name → pid mapping (namespace-scoped). Replaces if name exists in same ns. */
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

#endif
