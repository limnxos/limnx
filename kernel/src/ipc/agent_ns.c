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
