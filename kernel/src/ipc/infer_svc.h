#ifndef LIMNX_INFER_SVC_H
#define LIMNX_INFER_SVC_H

#include <stdint.h>

#define MAX_INFER_SERVICES  4
#define INFER_NAME_MAX      32
#define INFER_SOCK_PATH_MAX 108

typedef struct infer_service {
    char     name[INFER_NAME_MAX];
    char     sock_path[INFER_SOCK_PATH_MAX];
    uint64_t provider_pid;
    uint8_t  used;
} infer_service_t;

/* Register a service name → socket path mapping. Replaces if exists. */
int  infer_register(const char *name, const char *sock_path, uint64_t pid);

/* Lookup a service by name, returns index or -1 */
int  infer_lookup(const char *name);

/* Get service by index */
infer_service_t *infer_get(int idx);

/* Unregister all services owned by a given pid */
void infer_unregister_pid(uint64_t pid);

#endif
