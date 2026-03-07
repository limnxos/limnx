#ifndef LIMNX_AGENT_NS_H
#define LIMNX_AGENT_NS_H

#include <stdint.h>

#define MAX_NAMESPACES 8
#define NS_NAME_MAX    32

/* Quota resource types */
#define NS_QUOTA_PROCS     0
#define NS_QUOTA_MEM_PAGES 1

typedef struct agent_namespace {
    char     name[NS_NAME_MAX];
    uint64_t owner_pid;
    uint8_t  used;
    uint32_t max_procs;       /* 0 = unlimited */
    uint32_t max_mem_pages;   /* 0 = unlimited */
    uint32_t cur_procs;
    uint32_t cur_mem_pages;
} agent_namespace_t;

/* Create a namespace. Returns ns_id (1-based) or -errno. */
int  agent_ns_create(const char *name, uint64_t owner_pid);

/* Join a namespace. Only owner or root can join. Returns 0 or -errno. */
int  agent_ns_join(uint32_t ns_id, uint64_t caller_pid, uint16_t caller_uid);

/* Cleanup: free namespaces owned by pid. */
void agent_ns_cleanup_pid(uint64_t pid);

/* Check if ns_id is valid. */
int  agent_ns_valid(uint32_t ns_id);

/* Set quota for a namespace. Only owner or root. Returns 0 or -errno. */
int  agent_ns_set_quota(uint32_t ns_id, uint32_t resource, uint32_t limit,
                         uint64_t caller_pid, uint16_t caller_uid);

/* Check if adding `count` of resource_type is within quota. Returns 1 if ok. */
int  agent_ns_quota_check(uint32_t ns_id, uint32_t resource, uint32_t count);

/* Adjust current usage. delta can be negative. */
void agent_ns_quota_adjust(uint32_t ns_id, uint32_t resource, int32_t delta);

#endif
