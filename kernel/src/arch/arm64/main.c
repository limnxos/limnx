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
#include "dtb/dtb.h"

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

void kmain(uint64_t dtb_addr) {
    serial_init();

    serial_puts("\n========================================\n");
    serial_puts("  Limnx Kernel — ARM64 (aarch64)\n");
    serial_puts("  Target: QEMU virt (Cortex-A57)\n");
    serial_puts("========================================\n\n");

    /* Parse device tree blob (passed by QEMU in X0, saved by boot.S) */
    {
        serial_printf("[dtb]  DTB address from boot: %lx\n", (unsigned long)dtb_addr);
        /* If QEMU didn't pass DTB in X0 (bare-metal ELF boot), scan
         * the first 2MB of RAM for FDT magic. QEMU virt places DTB at
         * RAM_BASE + 0x100 or nearby. Kernel is at 0x40200000+ to avoid overlap. */
        if (!dtb_addr) {
            for (uint64_t probe = 0x40000000ULL; probe < 0x40200000ULL; probe += 4) {
                uint32_t raw = *(volatile uint32_t *)probe;
                if (raw == 0xedfe0dd0) {  /* big-endian 0xd00dfeed in LE */
                    serial_printf("[dtb]  found DTB at %lx\n", (unsigned long)probe);
                    dtb_addr = probe;
                    break;
                }
            }
        }
        if (dtb_init((void *)dtb_addr) != 0)
            serial_puts("[dtb]  WARNING: DTB parse failed, using defaults\n");

        /* Update virtio-mmio runtime params from DTB */
        const dtb_platform_info_t *plat = dtb_get_platform();
        if (plat && plat->valid) {
            extern uint64_t virtio_mmio_base_addr;
            extern uint32_t virtio_mmio_num_devices;
            virtio_mmio_base_addr = plat->virtio_mmio_base;
            virtio_mmio_num_devices = plat->virtio_mmio_num_slots;
        }
    }

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
    vfs_procfs_init();

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

    /* Virtio-GPU framebuffer (MMIO transport) */
    {
        extern int virtio_gpu_mmio_init(void);
        virtio_gpu_mmio_init();  /* non-fatal: no GPU just means serial-only */
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

    /* Create /etc config directory and default configs */
    vfs_mkdir("/etc");
    {
        int svc_node = vfs_create("/etc/services");
        if (svc_node >= 0) {
            const char *default_cfg =
                "# Service config: name|path|policy|after\n";
            int len = 0;
            while (default_cfg[len]) len++;
            vfs_write(svc_node, 0, (const uint8_t *)default_cfg, len);
        }

        int tab_node = vfs_create("/etc/inittab");
        if (tab_node >= 0) {
            const char *inittab =
                "# Init config: name:path:flags\n"
                "serviced:/serviced.elf:respawn\n"
                "shell:/shell.elf:wait\n";
            int len = 0;
            while (inittab[len]) len++;
            vfs_write(tab_node, 0, (const uint8_t *)inittab, len);
            pr_info("Created /etc/inittab\n");
        }

        int pw_node = vfs_create("/etc/passwd");
        if (pw_node >= 0) {
            const char *passwd =
                "root:x:0:0:root:/:/shell.elf\n"
                "nobody:x:65534:65534:nobody:/:/shell.elf\n";
            int len = 0;
            while (passwd[len]) len++;
            vfs_write(pw_node, 0, (const uint8_t *)passwd, len);
            pr_info("Created /etc/passwd\n");
        }
    }

    vfs_mkdir("/root");

    /* Create /dev with device nodes */
    vfs_mkdir("/dev");
    {
        const struct { const char *name; int minor; } devs[] = {
            {"null", DEV_NULL}, {"zero", DEV_ZERO},
            {"urandom", DEV_URANDOM}, {"tty", DEV_TTY}, {(void*)0, 0}
        };
        int dev_dir = vfs_resolve_path("/dev");
        for (int i = 0; devs[i].name; i++) {
            int idx = vfs_register_node(dev_dir, devs[i].name, VFS_DEVICE, 0, (void*)0);
            if (idx >= 0) {
                vfs_get_node(idx)->disk_inode = devs[i].minor;
                vfs_get_node(idx)->mode = 0666;
                vfs_get_node(idx)->flags = VFS_FLAG_WRITABLE;
            }
        }
    }

    /* Create /bin with busybox symlinks */
    vfs_mkdir("/bin");
    {
        const char *applets[] = {
            "vi", "ash", "sh", "sed", "awk", "grep", "cat", "ls", "cp",
            "mv", "rm", "mkdir", "rmdir", "echo", "printf", "head", "tail",
            "wc", "sort", "uniq", "cut", "tr", "tee", "find", "xargs",
            "chmod", "chown", "touch", "ps", "kill", "sleep",
            "true", "false", "test", "uname", "whoami", "id", "env",
            "readlink", "basename", "dirname", "seq", "yes", "pwd",
            "tar", "diff", "less", "more",
            NULL
        };
        for (int i = 0; applets[i]; i++) {
            char path[64];
            int p = 0;
            const char *pfx = "/bin/";
            while (*pfx) path[p++] = *pfx++;
            const char *a = applets[i];
            while (*a) path[p++] = *a++;
            path[p] = '\0';
            vfs_symlink(path, "/busybox.elf");
        }
    }

    /* Launch init (pid 1) — Unix standard */
    {
        extern volatile int kernel_quiet;
        kernel_quiet = 1;
    }
    pr_info("\nLaunching init...\n");
    {
        process_t *init_proc = load_elf_from_vfs("/init.elf");
        if (init_proc) {
            if (con_pty >= 0) {
                pty_t *pt = pty_get(con_pty);
                if (pt) {
                    for (int fd = 0; fd < 3; fd++) {
                        init_proc->fd_table[fd].node = NULL;
                        init_proc->fd_table[fd].offset = 0;
                        init_proc->fd_table[fd].pipe = NULL;
                        init_proc->fd_table[fd].pipe_write = 0;
                        init_proc->fd_table[fd].pty = (void *)pt;
                        init_proc->fd_table[fd].pty_is_master = 0;
                        init_proc->fd_table[fd].unix_sock = NULL;
                        init_proc->fd_table[fd].eventfd = NULL;
                        init_proc->fd_table[fd].epoll = NULL;
                        init_proc->fd_table[fd].uring = NULL;
                        init_proc->fd_table[fd].open_flags = 2;
                        init_proc->fd_table[fd].fd_flags = 0;
                    }
                    pt->slave_refs = 3;
                    pt->fg_pgid = init_proc->pid;
                    pr_info("init fd 0/1/2 connected to console PTY slave\n");
                }
            }
            init_proc->capabilities = 0xFFFFFFFF;
            pr_info("init.elf spawned (pid %lu)\n", init_proc->pid);
            sched_add(init_proc->main_thread);
            process_reap(init_proc);
            pr_err("init exited — system halted\n");
        } else {
            pr_err("init.elf not found — falling back to shell\n");
            process_t *sh_proc = load_elf_from_vfs("/shell.elf");
            if (sh_proc) {
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
                            sh_proc->fd_table[fd].open_flags = 2;
                            sh_proc->fd_table[fd].fd_flags = 0;
                        }
                        pt->slave_refs = 3;
                        pt->fg_pgid = sh_proc->pid;
                    }
                }
                sh_proc->capabilities = 0xFFFFFFFF;
                sched_add(sh_proc->main_thread);
                process_reap(sh_proc);
            }
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Kernel init complete — idling\n");
    serial_puts("========================================\n");

    /* Keep scheduler running */
    for (;;)
        sched_yield();
}
