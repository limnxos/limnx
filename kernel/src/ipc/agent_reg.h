#ifndef LIMNX_AGENT_REG_H
#define LIMNX_AGENT_REG_H

#include <stdint.h>

#define MAX_AGENTS      16
#define AGENT_NAME_MAX  32

typedef struct agent_entry {
    char     name[AGENT_NAME_MAX];
    uint64_t pid;
    uint8_t  used;
} agent_entry_t;

/* Register name → pid mapping. Replaces if name exists. Returns 0 or -errno */
int  agent_register(const char *name, uint64_t pid);

/* Lookup pid by name. Returns 0 and writes *pid_out, or -1 if not found */
int  agent_lookup(const char *name, uint64_t *pid_out);

/* Unregister all entries for a dying process */
void agent_unregister_pid(uint64_t pid);

#endif
