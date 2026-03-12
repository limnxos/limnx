#ifndef LIMNX_CONFIG_H
#define LIMNX_CONFIG_H

/*
 * Central kernel configuration — system-wide limits and constants.
 * Individual module headers should #include "config.h" instead of
 * defining their own MAX_* constants when the value is shared.
 */

/* Process / scheduling */
#define MAX_PROCS          64
#define MAX_FDS            32
#define MAX_SIGNALS        32
#define MAX_CPUS           8

/* Memory */
#define PAGE_SIZE          4096

/* IPC */
#define MAX_PIPES          8
#define MAX_EVENTFDS       16
#define MAX_UNIX_SOCKS     16
#define MAX_SHM_REGIONS    16
#define MAX_TOKENS         32
#define MAX_EPOLL_INSTANCES 8
#define MAX_URING_INSTANCES 4
#define MAX_TASKGRAPH_TASKS 32

/* Agent infrastructure */
#define MAX_SUPERVISORS    8
#define MAX_SUPER_CHILDREN 8
#define MAX_AGENT_ENTRIES  16
#define MAX_AGENT_NS       8

/* Filesystem */
#define MAX_PATH           256

/* Network */
#define MAX_TCP_CONNS      8

#endif
