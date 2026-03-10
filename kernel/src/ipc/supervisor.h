#ifndef LIMNX_SUPERVISOR_H
#define LIMNX_SUPERVISOR_H

#include <stdint.h>

#define MAX_SUPERVISORS   8
#define MAX_SUPER_CHILDREN 8
#define SUPER_NAME_MAX    32

/* Restart policies */
#define SUPER_ONE_FOR_ONE 0   /* restart only the crashed child */
#define SUPER_ONE_FOR_ALL 1   /* restart all children on any crash */

typedef struct super_child {
    uint64_t pid;
    char     elf_path[64];
    int64_t  ns_id;
    uint64_t caps;
    uint8_t  used;
} super_child_t;

typedef struct supervisor {
    uint64_t       owner_pid;
    uint32_t       id;
    uint8_t        used;
    uint8_t        policy;
    uint8_t        child_count;
    char           name[SUPER_NAME_MAX];
    super_child_t  children[MAX_SUPER_CHILDREN];
} supervisor_t;

void supervisor_init(void);
int  supervisor_create(uint64_t owner_pid, const char *name);
int  supervisor_add_child(uint32_t super_id, const char *elf_path,
                          int64_t ns_id, uint64_t caps);
int  supervisor_set_policy(uint32_t super_id, uint8_t policy);
int  supervisor_start(uint32_t super_id);
void supervisor_on_exit(uint64_t pid, int exit_status);

#endif
