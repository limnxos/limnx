/*
 * ARM64 kernel entry point — full subsystem boot + shell launch
 * Mirrors x86_64 main.c init sequence using shared kernel code.
 */

#define pr_fmt(fmt) "[arm64] " fmt
#include "klog.h"

#include "arch/serial.h"
#include "arch/cpu.h"
#include "arch/boot.h"
#include "arch/interrupt.h"
#include "arch/smp_hal.h"
#include "arch/timer.h"
#include "arch/syscall_arch.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "mm/swap.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "syscall/syscall.h"
#include "fs/vfs.h"
#include "fs/tar.h"
#include "pty/pty.h"
#include "sync/futex.h"
#include "proc/process.h"
#include "net/virtio_net.h"
#include "net/net.h"
#include "net/tcp.h"
#include "blk/virtio_blk.h"
#include "blk/bcache.h"
#include "blk/limnfs.h"
#include "ipc/supervisor.h"
#include "ipc/shm.h"
#include "ipc/infer_svc.h"

/* Linker symbols for embedded initrd (from ld -b binary) */
extern char _binary_initrd_tar_start[];
extern char _binary_initrd_tar_end[];
#define _initrd_start _binary_initrd_tar_start
#define _initrd_end   _binary_initrd_tar_end

/* Load an ELF from VFS and create a process (mirrors x86_64 main.c) */
static process_t *load_elf_from_vfs(const char *path) {
    int node_idx = vfs_open(path);
    if (node_idx < 0) return NULL;

    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0) return NULL;

    uint8_t *buf = (uint8_t *)kmalloc(st.size);
    if (!buf) return NULL;

    int64_t n = vfs_read(node_idx, 0, buf, st.size);
    if (n != (int64_t)st.size) { kfree(buf); return NULL; }

    process_t *proc = process_create_from_elf(buf, st.size);
    kfree(buf);
    if (proc) {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/') base = p + 1;
        int ni = 0;
        while (base[ni] && ni < 31) { proc->name[ni] = base[ni]; ni++; }
        proc->name[ni] = '\0';
        if (ni > 4 && proc->name[ni-4] == '.' && proc->name[ni-3] == 'e' &&
            proc->name[ni-2] == 'l' && proc->name[ni-1] == 'f')
            proc->name[ni-4] = '\0';
    }
    return proc;
}

/* Console PTY reader thread — bridges serial I/O to PTY */
static void console_reader_thread(void) {
    uint8_t buf[128];
    for (;;) {
        int con = pty_get_console();
        if (con < 0) { sched_yield(); continue; }

        int did_work = 0;

        /* Read slave output and send to serial */
        int64_t n = pty_master_read(con, buf, sizeof(buf));
        if (n > 0) {
            for (int64_t i = 0; i < n; i++)
                serial_putc((char)buf[i]);
            did_work = 1;
        }

        /* Read serial input and feed to PTY */
        char ch = serial_getchar();
        if (ch) {
            pty_console_input(ch);
            did_work = 1;
        }

        if (!did_work) sched_yield();
    }
}

void kmain(void) {
    serial_init();

    serial_puts("\n========================================\n");
    serial_puts("  Limnx Kernel — ARM64 (aarch64)\n");
    serial_puts("  Target: QEMU virt (Cortex-A57)\n");
    serial_puts("========================================\n\n");

    /* === Stage 1: Arch early init === */
    arch_early_init();
    arch_interrupt_init();

    /* === Stage 2: Physical memory === */
    serial_puts("\n--- PMM init ---\n");
    pmm_init();

    /* PMM smoke test */
    {
        uint64_t p = pmm_alloc_page();
        if (p) {
            serial_puts("[test] PMM smoke test PASSED\n");
            pmm_free_page(p);
        } else {
            serial_puts("[test] PMM smoke test FAILED\n");
        }
    }

    /* === Stage 3: VMM + heap === */
    serial_puts("\n--- VMM + kheap init ---\n");
    vmm_init();
    kheap_init();

    /* Heap smoke test */
    {
        void *p = kmalloc(128);
        if (p) {
            serial_puts("[test] Heap smoke test PASSED\n");
            kfree(p);
        } else {
            serial_puts("[test] Heap smoke test FAILED\n");
        }
    }

    /* === Stage 4: Scheduler === */
    serial_puts("\n--- Scheduler init ---\n");
    arch_late_init();
    sched_init();
    arch_timer_enable_sched();

    /* === Stage 5: Syscalls === */
    serial_puts("\n--- Syscall init ---\n");
    arch_syscall_init();
    syscall_init();

    /* === Stage 6: VFS + initrd === */
    serial_puts("\n--- VFS init ---\n");
    vfs_init();

    /* Load embedded initrd */
    {
        uint64_t initrd_size = (uint64_t)(_initrd_end - _initrd_start);
        if (initrd_size > 0) {
            serial_printf("[initrd] Embedded initrd: %lu bytes at %p\n",
                          initrd_size, _initrd_start);
            tar_init(_initrd_start, initrd_size);
        } else {
            serial_puts("[initrd] No embedded initrd found\n");
        }
    }

    /* === Stage 7: Drivers === */
    serial_puts("\n--- Driver init ---\n");

    /* Virtio-net (MMIO transport) */
    int net_ok = 0;
    if (virtio_net_init() == 0) {
        net_init();
        net_ok = 1;
    } else {
        serial_puts("[net]  Skipping network stack (no virtio-net)\n");
    }

    /* Virtio-blk (MMIO transport) */
    int blk_ok = 0;
    if (virtio_blk_init() == 0) {
        blk_ok = 1;
    } else {
        serial_puts("[blk]  Skipping virtio-blk (no device)\n");
    }

    /* LimnFS init */
    if (blk_ok) {
        bcache_init();
        swap_init();

        /* Check if disk already has LimnFS */
        uint8_t sb_buf[4096];
        bcache_read(0, sb_buf);
        uint32_t magic = *(uint32_t *)sb_buf;

        uint32_t disk_blocks = 14336;  /* 56MB / 4KB = 14336 (last 8MB for swap) */
        if (magic != LIMNFS_MAGIC) {
            /* First boot: format */
            if (limnfs_format(disk_blocks) != 0)
                serial_puts("[test] FAIL: limnfs_format failed\n");
        } else {
            /* Existing filesystem: mount */
            if (limnfs_mount() != 0)
                serial_puts("[test] FAIL: limnfs_mount failed\n");
        }

        /* Sync initrd files to disk */
        if (limnfs_mounted()) {
            serial_puts("\n[limnfs] Syncing initrd to disk...\n");
            int synced = 0, skipped = 0;

            for (int i = 1; i < vfs_get_node_count(); i++) {
                vfs_node_t *n = vfs_get_node(i);
                if (!n || n->parent != 0) continue;

                if (n->type == VFS_FILE) {
                    int existing = limnfs_dir_lookup(0, n->name);
                    if (existing >= 0) {
                        n->disk_inode = (int32_t)existing;
                        skipped++;
                    } else {
                        int disk_ino = limnfs_create_file(0, n->name);
                        if (disk_ino >= 0 && n->size > 0 && n->data) {
                            limnfs_write_data((uint32_t)disk_ino, 0,
                                              n->data, n->size);
                        }
                        if (disk_ino >= 0) {
                            limnfs_inode_t inode;
                            if (limnfs_read_inode((uint32_t)disk_ino, &inode) == 0) {
                                inode.mode = n->mode;
                                limnfs_write_inode((uint32_t)disk_ino, &inode);
                            }
                            n->disk_inode = (int32_t)disk_ino;
                            synced++;
                        }
                    }
                }
            }

            serial_printf("[limnfs] Sync complete: %d copied, %d already exist\n",
                          synced, skipped);

            vfs_mount_limnfs();
        }
    }

    /* === Stage 8: Subsystem init === */
    serial_puts("\n--- Subsystem init ---\n");
    pty_init();
    if (net_ok) tcp_init();
    futex_init();
    supervisor_init();
    shm_init();

    /* === Stage 9: SMP === */
    arch_smp_init();

    serial_puts("\n========================================\n");
    serial_puts("  ARM64 boot complete — all subsystems\n");
    serial_puts("========================================\n");

    /* Start bcache flusher kernel thread */
    if (blk_ok)
        bcache_start_flusher();

    /* Start async inference worker kernel thread */
    infer_async_start_worker();

    /* Create console PTY and reader thread */
    int con_pty = pty_create_console();
    if (con_pty >= 0) {
        pr_info("Console PTY %d created\n", con_pty);
    }

    if (con_pty >= 0) {
        thread_t *crt = thread_create(console_reader_thread, 0);
        if (crt) {
            crt->process = NULL;
            sched_add(crt);
            pr_info("Console reader thread started\n");
        }
    }

    /* Create /etc/services config for serviced */
    {
        vfs_mkdir("/etc");
        int svc_node = vfs_create("/etc/services");
        if (svc_node >= 0) {
            const char *default_cfg =
                "# Service config: name|path|policy|after\n"
                "# policy: one-for-one, one-for-all\n"
                "# after: dependency name, or none\n";
            int len = 0;
            while (default_cfg[len]) len++;
            vfs_write(svc_node, 0, (const uint8_t *)default_cfg, len);
            pr_info("Created /etc/services\n");
        }
    }

    /* Load and run serviced.elf — persistent service daemon */
    pr_info("\nLoading serviced.elf...\n");
    {
        process_t *sd_proc = load_elf_from_vfs("/serviced.elf");
        if (sd_proc) {
            sd_proc->capabilities = 0xFFF;  /* full caps for service management */
            sd_proc->daemon = 1;
            pr_info("serviced.elf spawned (pid %lu)\n", sd_proc->pid);
            sched_add(sd_proc->main_thread);
        } else {
            pr_warn("serviced.elf not found\n");
        }
    }

    /* Load and run shell.elf with console PTY as stdin/stdout/stderr */
    pr_info("\nLoading shell.elf...\n");
    {
        process_t *sh_proc = load_elf_from_vfs("/shell.elf");
        if (sh_proc) {
            /* Set up fd 0/1/2 as console PTY slave */
            if (con_pty >= 0) {
                pty_t *pt = pty_get(con_pty);
                if (pt) {
                    for (int fd = 0; fd < 3; fd++) {
                        sh_proc->fd_table[fd].node = NULL;
                        sh_proc->fd_table[fd].offset = 0;
                        sh_proc->fd_table[fd].pipe = NULL;
                        sh_proc->fd_table[fd].pipe_write = 0;
                        sh_proc->fd_table[fd].pty = (void *)pt;
                        sh_proc->fd_table[fd].pty_is_master = 0;
                        sh_proc->fd_table[fd].unix_sock = NULL;
                        sh_proc->fd_table[fd].eventfd = NULL;
                        sh_proc->fd_table[fd].epoll = NULL;
                        sh_proc->fd_table[fd].uring = NULL;
                        sh_proc->fd_table[fd].open_flags = 2; /* O_RDWR */
                        sh_proc->fd_table[fd].fd_flags = 0;
                    }
                    pt->slave_refs = 3;
                    pt->fg_pgid = sh_proc->pid;
                    pr_info("Shell fd 0/1/2 connected to console PTY slave\n");
                }
            }
            /* Set LIMNX_VERSION env */
            {
                const char *env_entry = "LIMNX_VERSION=0.100";
                int elen = 0;
                while (env_entry[elen]) elen++;
                for (int i = 0; i <= elen; i++)
                    sh_proc->env_buf[i] = env_entry[i];
                sh_proc->env_buf_len = elen + 1;
                sh_proc->env_count = 1;
            }

            /* Set capabilities */
            sh_proc->capabilities = 0x1FFF; /* CAP_ALL */

            pr_info("shell.elf spawned (pid %lu)\n", sh_proc->pid);
            sched_add(sh_proc->main_thread);
            process_reap(sh_proc);
            pr_info("shell.elf completed\n");
        } else {
            pr_err("shell.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Kernel init complete — idling\n");
    serial_puts("========================================\n");

    /* Keep scheduler running */
    for (;;)
        sched_yield();
}
