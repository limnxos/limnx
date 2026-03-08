#include "ipc/cap_token.h"
#include "serial.h"

static cap_token_t tokens[MAX_TOKENS];
static uint32_t next_token_id = 1;

/* Simple prefix match: does path start with prefix? */
static int prefix_match(const char *prefix, const char *path) {
    if (!prefix[0]) return 1; /* empty prefix matches everything */
    for (int i = 0; prefix[i]; i++) {
        if (path[i] != prefix[i]) return 0;
    }
    return 1;
}

int cap_token_create(uint64_t owner_pid, uint32_t owner_caps,
                     uint32_t perms, uint64_t target_pid, const char *resource) {
    /* Can't grant capabilities the owner doesn't have */
    if (perms & ~owner_caps)
        return -1; /* -EPERM */

    /* Find free slot */
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!tokens[i].used) {
            tokens[i].used = 1;
            tokens[i].id = next_token_id++;
            tokens[i].owner_pid = owner_pid;
            tokens[i].target_pid = target_pid;
            tokens[i].perms = perms;

            /* Copy resource path */
            int j = 0;
            if (resource) {
                for (; j < TOKEN_PATH_MAX - 1 && resource[j]; j++)
                    tokens[i].resource[j] = resource[j];
            }
            tokens[i].resource[j] = '\0';

            return (int)tokens[i].id;
        }
    }

    return -12; /* -ENOMEM */
}

int cap_token_revoke(uint32_t token_id, uint64_t caller_pid) {
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (tokens[i].used && tokens[i].id == token_id) {
            if (tokens[i].owner_pid != caller_pid)
                return -1; /* -EPERM */
            tokens[i].used = 0;
            return 0;
        }
    }
    return -2; /* -ENOENT */
}

int cap_token_check(uint64_t pid, uint32_t needed_cap, const char *resource) {
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!tokens[i].used) continue;

        /* Target must match: 0 = bearer (any), else must match pid */
        if (tokens[i].target_pid != 0 && tokens[i].target_pid != pid)
            continue;

        /* Must have the needed capability */
        if (!(tokens[i].perms & needed_cap))
            continue;

        /* Resource must match (prefix) */
        if (resource && tokens[i].resource[0]) {
            if (!prefix_match(tokens[i].resource, resource))
                continue;
        }

        return 1; /* granted */
    }
    return 0;
}

int cap_token_list(uint64_t owner_pid, token_info_t *buf, int max_count) {
    int count = 0;
    for (int i = 0; i < MAX_TOKENS && count < max_count; i++) {
        if (tokens[i].used && tokens[i].owner_pid == owner_pid) {
            buf[count].id = tokens[i].id;
            buf[count].target_pid = tokens[i].target_pid;
            buf[count].perms = tokens[i].perms;
            for (int j = 0; j < TOKEN_PATH_MAX; j++)
                buf[count].resource[j] = tokens[i].resource[j];
            count++;
        }
    }
    return count;
}

int cap_token_delegate(uint32_t parent_id, uint64_t caller_pid,
                        uint64_t target_pid, uint32_t perms, const char *resource) {
    /* Find parent token */
    cap_token_t *parent = (void *)0;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (tokens[i].used && tokens[i].id == parent_id) {
            parent = &tokens[i];
            break;
        }
    }
    if (!parent) return -2; /* -ENOENT */

    /* Caller must be target of parent (or parent is bearer) */
    if (parent->target_pid != 0 && parent->target_pid != caller_pid)
        return -1; /* -EPERM */

    /* 0 means inherit all parent perms */
    if (perms == 0)
        perms = parent->perms;

    /* Sub-token perms must be subset of parent perms */
    if (perms & ~parent->perms)
        return -1;

    /* Sub-token resource must be equal or more specific (longer prefix) */
    if (resource && resource[0] && parent->resource[0]) {
        if (!prefix_match(parent->resource, resource))
            return -1; /* sub-resource doesn't start with parent resource */
    }

    /* Create the sub-token owned by caller */
    return cap_token_create(caller_pid, perms, perms, target_pid,
                             resource && resource[0] ? resource : parent->resource);
}

void cap_token_cleanup_pid(uint64_t pid) {
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (tokens[i].used) {
            /* Remove tokens owned by this pid */
            if (tokens[i].owner_pid == pid)
                tokens[i].used = 0;
            /* Also invalidate tokens targeting this pid */
            else if (tokens[i].target_pid == pid)
                tokens[i].used = 0;
        }
    }
}
