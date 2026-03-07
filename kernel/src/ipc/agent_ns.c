#include "ipc/agent_ns.h"
#include "ipc/agent_reg.h"
#include "serial.h"

static agent_namespace_t namespaces[MAX_NAMESPACES];

int agent_ns_create(const char *name, uint64_t owner_pid) {
    /* Find free slot (slot 0 is reserved for global namespace) */
    for (int i = 1; i < MAX_NAMESPACES; i++) {
        if (!namespaces[i].used) {
            namespaces[i].used = 1;
            namespaces[i].owner_pid = owner_pid;
            int j = 0;
            if (name) {
                for (; j < NS_NAME_MAX - 1 && name[j]; j++)
                    namespaces[i].name[j] = name[j];
            }
            namespaces[i].name[j] = '\0';
            return i; /* ns_id = index */
        }
    }
    return -12; /* -ENOMEM */
}

int agent_ns_join(uint32_t ns_id, uint64_t caller_pid, uint16_t caller_uid) {
    if (ns_id == 0) return 0; /* global namespace always joinable */
    if (ns_id >= MAX_NAMESPACES || !namespaces[ns_id].used)
        return -2; /* -ENOENT */

    /* Only owner or root can join */
    if (caller_uid != 0 && namespaces[ns_id].owner_pid != caller_pid)
        return -1; /* -EPERM */

    return 0;
}

void agent_ns_cleanup_pid(uint64_t pid) {
    for (int i = 1; i < MAX_NAMESPACES; i++) {
        if (namespaces[i].used && namespaces[i].owner_pid == pid) {
            /* Unregister all agents in this namespace */
            agent_unregister_ns((uint32_t)i);
            namespaces[i].used = 0;
        }
    }
}

int agent_ns_valid(uint32_t ns_id) {
    if (ns_id == 0) return 1; /* global always valid */
    if (ns_id >= MAX_NAMESPACES) return 0;
    return namespaces[ns_id].used;
}

int agent_ns_set_quota(uint32_t ns_id, uint32_t resource, uint32_t limit,
                        uint64_t caller_pid, uint16_t caller_uid) {
    if (ns_id == 0 || ns_id >= MAX_NAMESPACES || !namespaces[ns_id].used)
        return -2; /* -ENOENT */
    if (caller_uid != 0 && namespaces[ns_id].owner_pid != caller_pid)
        return -1; /* -EPERM */

    if (resource == NS_QUOTA_PROCS)
        namespaces[ns_id].max_procs = limit;
    else if (resource == NS_QUOTA_MEM_PAGES)
        namespaces[ns_id].max_mem_pages = limit;
    else
        return -22; /* -EINVAL */
    return 0;
}

int agent_ns_quota_check(uint32_t ns_id, uint32_t resource, uint32_t count) {
    if (ns_id == 0 || ns_id >= MAX_NAMESPACES || !namespaces[ns_id].used)
        return 1; /* global/invalid ns = no quota */

    if (resource == NS_QUOTA_PROCS) {
        if (namespaces[ns_id].max_procs == 0) return 1; /* unlimited */
        return (namespaces[ns_id].cur_procs + count <= namespaces[ns_id].max_procs);
    } else if (resource == NS_QUOTA_MEM_PAGES) {
        if (namespaces[ns_id].max_mem_pages == 0) return 1;
        return (namespaces[ns_id].cur_mem_pages + count <= namespaces[ns_id].max_mem_pages);
    }
    return 1;
}

void agent_ns_quota_adjust(uint32_t ns_id, uint32_t resource, int32_t delta) {
    if (ns_id == 0 || ns_id >= MAX_NAMESPACES || !namespaces[ns_id].used)
        return;

    if (resource == NS_QUOTA_PROCS) {
        int32_t val = (int32_t)namespaces[ns_id].cur_procs + delta;
        namespaces[ns_id].cur_procs = (val > 0) ? (uint32_t)val : 0;
    } else if (resource == NS_QUOTA_MEM_PAGES) {
        int32_t val = (int32_t)namespaces[ns_id].cur_mem_pages + delta;
        namespaces[ns_id].cur_mem_pages = (val > 0) ? (uint32_t)val : 0;
    }
}
