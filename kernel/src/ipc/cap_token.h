#ifndef LIMNX_CAP_TOKEN_H
#define LIMNX_CAP_TOKEN_H

#include <stdint.h>

#define MAX_TOKENS     32
#define TOKEN_PATH_MAX 64

#define TOKEN_MAX_DEPTH  4   /* max delegation chain depth */

typedef struct cap_token {
    uint32_t id;                        /* unique token ID (1-based) */
    uint64_t owner_pid;                 /* creator — only owner can revoke */
    uint64_t target_pid;                /* who can use it (0 = bearer/any) */
    uint32_t perms;                     /* capability bits this token grants */
    char     resource[TOKEN_PATH_MAX];  /* resource path prefix (empty = any) */
    uint32_t parent_id;                 /* 0 = root token, else parent's id */
    uint8_t  depth;                     /* delegation depth (0 = root) */
    uint8_t  used;
} cap_token_t;

/* User-visible token info for TOKEN_LIST */
typedef struct token_info {
    uint32_t id;
    uint64_t target_pid;
    uint32_t perms;
    char     resource[TOKEN_PATH_MAX];
} token_info_t;

/* Create a token. Creator must have `perms` capabilities. Returns token ID or -errno. */
int  cap_token_create(uint64_t owner_pid, uint32_t owner_caps,
                      uint32_t perms, uint64_t target_pid, const char *resource);

/* Revoke a token. Only owner can revoke. Returns 0 or -errno. */
int  cap_token_revoke(uint32_t token_id, uint64_t caller_pid);

/* Check if pid has a token granting `needed_cap` for `resource`. Returns 1 if yes. */
int  cap_token_check(uint64_t pid, uint32_t needed_cap, const char *resource);

/* List tokens owned by pid. Writes to buf, returns count. */
int  cap_token_list(uint64_t owner_pid, token_info_t *buf, int max_count);

/* Delegate: create sub-token from existing token. Caller must be target of parent.
 * Sub-token perms must be subset of parent perms. Returns new token ID or -errno. */
int  cap_token_delegate(uint32_t parent_id, uint64_t caller_pid,
                         uint64_t target_pid, uint32_t perms, const char *resource);

/* Cleanup all tokens owned by or targeting pid (called on process exit). */
void cap_token_cleanup_pid(uint64_t pid);

#endif
