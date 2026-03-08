#ifndef LIMNX_PROCESS_H
#define LIMNX_PROCESS_H

#include <stdint.h>
#include "sched/thread.h"
#include "fs/vfs.h"

/* Signal numbers */
#define SIGINT   2
#define SIGKILL  9
#define SIGTERM  15
#define SIGCONT  18
#define SIGSTOP  19
#define SIGCHLD  20

/* Signal handler constants */
#define SIG_DFL  0
#define SIG_IGN  1
#define MAX_SIGNALS 32

typedef struct {
    uint64_t sa_handler;  /* user function pointer, or SIG_DFL/SIG_IGN */
} sigaction_t;

#define MAX_PROCS         64
#define USER_CODE_BASE    0x0000000000400000ULL
#define USER_STACK_TOP    0x0000800000000000ULL
#define USER_STACK_SIZE   (64 * 1024)  /* 64 KB */

#define PROC_ARGV_BUF_SIZE 512
#define PROC_MAX_ARGS      16
#define PROC_ENV_BUF_SIZE  1024

/* Mmap region starts at 4GB, well below stack */
#define MMAP_REGION_BASE  0x0000000100000000ULL
#define MMAP_MAX_ENTRIES  64
#define MMAP_MAX_PAGES    524288  /* 2 GB max per mmap call */

typedef struct mmap_entry {
    uint64_t virt_addr;
    uint64_t phys_addr;   /* base physical address */
    uint32_t num_pages;
    uint32_t used;        /* 1 = active, 0 = free slot */
    int32_t  shm_id;      /* -1 = private, >=0 = shared memory region */
    uint8_t  demand;      /* 1 = demand-paged (no physical backing until fault) */
} mmap_entry_t;

/* Saved user-mode registers for fork child return */
typedef struct fork_context {
    uint64_t rip, rsp, rflags;
    uint64_t rbp, rbx, r12, r13, r14, r15;
} fork_context_t;

typedef struct process {
    uint64_t      pid;
    uint64_t      parent_pid;
    uint64_t      pgid;            /* process group ID */
    uint16_t      uid;             /* user ID, 0 = root */
    uint16_t      gid;             /* group ID, 0 = root */
    uint32_t      capabilities;    /* capability bitmask */
    uint64_t      rlimit_mem_pages;  /* max mmap pages, 0=unlimited */
    uint64_t      rlimit_cpu_ticks;  /* max CPU ticks, 0=unlimited */
    uint32_t      rlimit_nfds;       /* max open fds, 0=unlimited */
    uint64_t      used_mem_pages;    /* current mmap page count */
    uint64_t      seccomp_mask;    /* bit N set = syscall N allowed (0-63) */
    uint8_t       seccomp_strict;  /* 1=SIGKILL on denied, 0=return -EACCES */
    uint8_t       audit_flags;     /* AUDIT_* flags */
    int64_t       exit_status;
    volatile uint8_t exited;      /* set to 1 by sys_exit, safe to check after thread freed */
    uint64_t      cr3;            /* physical address of this process's PML4 */
    thread_t     *main_thread;
    fork_context_t fork_ctx;      /* saved user regs for fork child */
    uint64_t      user_entry;
    uint64_t      user_stack_top;
    fd_entry_t    fd_table[MAX_FDS];
    mmap_entry_t  mmap_table[MMAP_MAX_ENTRIES];
    uint64_t      mmap_next_addr;
    char          cwd[MAX_PATH];  /* per-process working directory */
    uint32_t      pending_signals; /* bitfield of pending signals */
    sigaction_t   sig_handlers[MAX_SIGNALS];
    uint64_t      signal_frame_addr; /* for sigreturn validation */
    int           argc;
    int           argv_buf_len;
    char          argv_buf[PROC_ARGV_BUF_SIZE];  /* packed "arg0\0arg1\0..." */
    int           env_count;
    int           env_buf_len;
    char          env_buf[PROC_ENV_BUF_SIZE];    /* packed "KEY=VALUE\0..." */
    int8_t        tcp_conns[8];   /* 1 = this process owns tcp conn slot i */
    uint32_t      ns_id;          /* agent namespace (0 = global) */
    char          name[32];       /* process name (from exec path) */
} process_t;

/* Signal delivery — returns 0 on success */
int process_deliver_signal(process_t *proc, int signum);

/* Kill all processes in a process group */
int process_kill_group(uint64_t pgid, int signum);

process_t *process_create(const uint8_t *code, uint64_t code_size);
process_t *process_create_from_elf(const uint8_t *elf, uint64_t size);
process_t *process_fork(process_t *parent, const fork_context_t *ctx);

/* Process registry (spinlock-protected for SMP safety) */
uint64_t   process_alloc_pid(void);
void       process_register(process_t *proc);
process_t *process_lookup(uint64_t pid);
void       process_unregister(uint64_t pid);

/* Wait for process to die, unregister, and free */
void       process_reap(process_t *proc);

#endif
