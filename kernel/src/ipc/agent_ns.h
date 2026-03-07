#ifndef LIMNX_AGENT_NS_H
#define LIMNX_AGENT_NS_H

#include <stdint.h>

#define MAX_NAMESPACES 8
#define NS_NAME_MAX    32

typedef struct agent_namespace {
    char     name[NS_NAME_MAX];
    uint64_t owner_pid;
    uint8_t  used;
} agent_namespace_t;

/* Create a namespace. Returns ns_id (1-based) or -errno. */
int  agent_ns_create(const char *name, uint64_t owner_pid);

/* Join a namespace. Only owner or root can join. Returns 0 or -errno. */
int  agent_ns_join(uint32_t ns_id, uint64_t caller_pid, uint16_t caller_uid);

/* Cleanup: free namespaces owned by pid. */
void agent_ns_cleanup_pid(uint64_t pid);

/* Check if ns_id is valid. */
int  agent_ns_valid(uint32_t ns_id);

#endif
