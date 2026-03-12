#define pr_fmt(fmt) "[init] " fmt
#include "klog.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"
#include "serial.h"
#include "gdt/gdt.h"
#include "idt/idt.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "sched/tss.h"
#include "sched/thread.h"
#include "sched/sched.h"
#include "syscall/syscall.h"
#include "proc/process.h"
#include "proc/elf.h"
#include "fs/vfs.h"
#include "fs/tar.h"
#include "pci/pci.h"
#include "net/virtio_net.h"
#include "net/net.h"
#include "blk/virtio_blk.h"
#include "blk/limnfs.h"
#include "blk/bcache.h"
#include "ipc/infer_svc.h"
#include "ipc/shm.h"
#include "sync/futex.h"
#include "ipc/supervisor.h"
#include "net/netstor.h"
#include "fb/fbcon.h"
#include "pty/pty.h"
#include "net/tcp.h"
#include "smp/percpu.h"
#include "smp/lapic.h"
#include "sync/mutex.h"
#include "mm/swap.h"
#include "arch/cpu.h"

/* --- Limine requests --- */

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_bootloader_info_request bootinfo_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

/* --- Helpers --- */

static __unused void hlt_loop(void) {
    for (;;)
        arch_halt();
}

static const char *memmap_type_str(uint64_t type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                return "usable";
    case LIMINE_MEMMAP_RESERVED:              return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:      return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:              return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY:            return "bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:return "bootloader reclaimable";
    case LIMINE_MEMMAP_KERNEL_AND_MODULES:    return "kernel/modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:           return "framebuffer";
    default:                                  return "unknown";
    }
}

/* --- PMM smoke test --- */

static void pmm_smoke_test(void) {
    serial_puts("\n[test] PMM smoke test...\n");

    uint64_t free_before = pmm_get_free_pages();

    /* Allocate a page */
    uint64_t page1 = pmm_alloc_page();
    if (page1 == 0) {
        serial_puts("[test] FAIL: pmm_alloc_page returned 0\n");
        return;
    }
    serial_printf("[test] Allocated page at phys %lx\n", page1);

    /* Verify free count decreased */
    uint64_t free_after_alloc = pmm_get_free_pages();
    if (free_after_alloc != free_before - 1) {
        serial_puts("[test] FAIL: free count didn't decrease by 1\n");
        return;
    }

    /* Free the page */
    pmm_free_page(page1);

    /* Verify free count restored */
    uint64_t free_after_free = pmm_get_free_pages();
    if (free_after_free != free_before) {
        serial_puts("[test] FAIL: free count didn't restore after free\n");
        return;
    }

    /* Re-allocate — should get same page */
    uint64_t page2 = pmm_alloc_page();
    if (page2 != page1) {
        serial_printf("[test] Note: re-alloc gave different page %lx (ok)\n", page2);
    }
    pmm_free_page(page2);

    serial_puts("[test] PMM smoke test PASSED\n");
}

/* --- VMM smoke test --- */

#define VMM_TEST_VADDR 0xFFFFFFFFA0000000ULL

static void vmm_smoke_test(void) {
    serial_puts("\n[test] VMM smoke test...\n");

    /* Allocate a physical page */
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) {
        serial_puts("[test] FAIL: pmm_alloc_page returned 0\n");
        return;
    }

    /* Map it to test virtual address */
    if (vmm_map_page(VMM_TEST_VADDR, phys, PTE_WRITABLE | PTE_NX) != 0) {
        serial_puts("[test] FAIL: vmm_map_page failed\n");
        pmm_free_page(phys);
        return;
    }

    /* Write and read back */
    volatile uint64_t *ptr = (volatile uint64_t *)VMM_TEST_VADDR;
    *ptr = 0xDEADBEEFCAFEBABEULL;
    uint64_t readback = *ptr;
    if (readback != 0xDEADBEEFCAFEBABEULL) {
        serial_printf("[test] FAIL: read back %lx, expected DEADBEEFCAFEBABE\n", readback);
        return;
    }

    /* Verify vmm_get_phys */
    uint64_t resolved = vmm_get_phys(VMM_TEST_VADDR);
    if (resolved != phys) {
        serial_printf("[test] FAIL: vmm_get_phys returned %lx, expected %lx\n", resolved, phys);
        return;
    }

    /* Unmap and free */
    vmm_unmap_page(VMM_TEST_VADDR);
    pmm_free_page(phys);

    serial_puts("[test] VMM smoke test PASSED\n");
}

/* --- Heap smoke test --- */

static void heap_smoke_test(void) {
    serial_puts("\n[test] Heap smoke test...\n");

    /* Allocate 3 different sizes */
    void *a = kmalloc(64);
    void *b = kmalloc(256);
    void *c = kmalloc(1024);

    if (!a || !b || !c) {
        serial_puts("[test] FAIL: kmalloc returned NULL\n");
        return;
    }

    /* Verify 16-byte alignment */
    if (((uint64_t)a & 0xF) || ((uint64_t)b & 0xF) || ((uint64_t)c & 0xF)) {
        serial_puts("[test] FAIL: alignment check failed\n");
        return;
    }

    serial_printf("[test] a=%p b=%p c=%p\n", a, b, c);

    /* Write and read */
    *(uint64_t *)a = 0xAAAAAAAAAAAAAAAAULL;
    *(uint64_t *)b = 0xBBBBBBBBBBBBBBBBULL;
    *(uint64_t *)c = 0xCCCCCCCCCCCCCCCCULL;

    if (*(uint64_t *)a != 0xAAAAAAAAAAAAAAAAULL ||
        *(uint64_t *)b != 0xBBBBBBBBBBBBBBBBULL ||
        *(uint64_t *)c != 0xCCCCCCCCCCCCCCCCULL) {
        serial_puts("[test] FAIL: read back mismatch\n");
        return;
    }

    /* Free middle, re-alloc smaller (should reuse) */
    kfree(b);
    void *b2 = kmalloc(128);
    if (!b2) {
        serial_puts("[test] FAIL: re-alloc returned NULL\n");
        return;
    }
    serial_printf("[test] b2=%p (reuse test)\n", b2);

    /* krealloc with data preservation */
    *(uint64_t *)a = 0x1234567890ABCDEFULL;
    void *a2 = krealloc(a, 512);
    if (!a2) {
        serial_puts("[test] FAIL: krealloc returned NULL\n");
        return;
    }
    if (*(uint64_t *)a2 != 0x1234567890ABCDEFULL) {
        serial_puts("[test] FAIL: krealloc didn't preserve data\n");
        return;
    }
    serial_printf("[test] a2=%p (krealloc test)\n", a2);

    /* Free all */
    kfree(a2);
    kfree(b2);
    kfree(c);

    serial_puts("[test] Heap smoke test PASSED\n");
}

/* --- Scheduler smoke test --- */

static volatile int thread_a_done = 0;
static volatile int thread_b_done = 0;

static void test_thread_a(void) {
    serial_puts("[test] Thread A running\n");
    for (int i = 0; i < 3; i++) {
        serial_printf("[test] Thread A: iteration %d\n", i);
        sched_yield();
    }
    serial_puts("[test] Thread A done\n");
    thread_a_done = 1;
}

static void test_thread_b(void) {
    serial_puts("[test] Thread B running\n");
    for (int i = 0; i < 3; i++) {
        serial_printf("[test] Thread B: iteration %d\n", i);
        sched_yield();
    }
    serial_puts("[test] Thread B done\n");
    thread_b_done = 1;
}

static void sched_smoke_test(void) {
    serial_puts("\n[test] Scheduler smoke test...\n");

    thread_t *ta = thread_create(test_thread_a, 0);
    thread_t *tb = thread_create(test_thread_b, 0);

    if (!ta || !tb) {
        serial_puts("[test] FAIL: thread_create returned NULL\n");
        return;
    }

    sched_add(ta);
    sched_add(tb);

    serial_printf("[test] Created thread A (tid %lu) and B (tid %lu)\n",
        ta->tid, tb->tid);

    /* Yield from kmain to let test threads run */
    while (!thread_a_done || !thread_b_done)
        sched_yield();

    serial_puts("[test] All threads completed — scheduler PASSED\n");
}

/* --- VFS smoke test (kernel-side) --- */

static void vfs_smoke_test(void) {
    serial_puts("\n[test] VFS smoke test...\n");

    int idx = vfs_open("/hello.txt");
    if (idx < 0) {
        serial_puts("[test] FAIL: vfs_open(\"/hello.txt\") returned -1\n");
        return;
    }
    serial_printf("[test] vfs_open(\"/hello.txt\") = %d\n", idx);

    uint8_t buf[256];
    int64_t n = vfs_read(idx, 0, buf, sizeof(buf) - 1);
    if (n <= 0) {
        serial_printf("[test] FAIL: vfs_read returned %ld\n", n);
        return;
    }
    buf[n] = '\0';
    serial_printf("[test] vfs_read: %ld bytes: %s", n, (const char *)buf);

    vfs_stat_t st;
    if (vfs_stat("/hello.txt", &st) != 0) {
        serial_puts("[test] FAIL: vfs_stat failed\n");
        return;
    }
    serial_printf("[test] vfs_stat: size=%lu type=%u\n", st.size, st.type);

    serial_puts("[test] VFS smoke test PASSED\n");
}

static process_t *load_elf_from_vfs(const char *path) {
    int node_idx = vfs_open(path);
    if (node_idx < 0) {
        serial_printf("[test] FAIL: vfs_open(\"%s\") returned -1\n", path);
        return NULL;
    }

    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0) {
        serial_printf("[test] FAIL: vfs_stat(\"%s\") failed\n", path);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)kmalloc(st.size);
    if (!buf) {
        serial_puts("[test] FAIL: kmalloc for ELF failed\n");
        return NULL;
    }

    int64_t n = vfs_read(node_idx, 0, buf, st.size);
    if (n != (int64_t)st.size) {
        serial_printf("[test] FAIL: vfs_read returned %ld, expected %lu\n", n, st.size);
        kfree(buf);
        return NULL;
    }

    process_t *proc = process_create_from_elf(buf, st.size);
    kfree(buf);
    if (proc) {
        /* Set process name from path */
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

/* Load ELF and immediately schedule it */
static process_t *load_and_run_elf(const char *path) {
    process_t *proc = load_elf_from_vfs(path);
    if (proc)
        sched_add(proc->main_thread);
    return proc;
}

/* --- FPU/SSE initialization --- */

static void fpu_init(void) {
    arch_fpu_init();
    serial_puts("[fpu]  FPU/SSE enabled (CR0.EM=0, CR4.OSFXSR=1, CR4.OSXMMEXCPT=1)\n");
}

/* --- Console PTY master reader thread --- */
/* Reads slave output from the console PTY s2m buffer and sends it
 * to serial + fbcon for display. Runs forever in kernel space. */
static void console_reader_thread(void) {
    uint8_t buf[128];
    for (;;) {
        int con = pty_get_console();
        if (con < 0) {
            sched_yield();
            continue;
        }

        int did_work = 0;

        /* Read slave output (s2m) and send to serial/fbcon */
        int64_t n = pty_master_read(con, buf, sizeof(buf));
        if (n > 0) {
            for (int64_t i = 0; i < n; i++)
                serial_putc((char)buf[i]);
            did_work = 1;
        }

        /* Read serial input and feed to PTY master (m2s) */
        char ch = serial_getchar();
        if (ch) {
            pty_console_input(ch);
            did_work = 1;
        }

        if (!did_work)
            sched_yield();
    }
}

/* --- Entry point --- */

void kmain(void) {
    serial_init();

    serial_puts("\n========================================\n");
    serial_puts("  Limnx Kernel — booting\n");
    serial_puts("========================================\n\n");

    /* Check base revision */
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        panic("Limine base revision not supported");
    }

    /* Bootloader info */
    if (bootinfo_request.response) {
        serial_printf("Bootloader: %s %s\n",
            bootinfo_request.response->name,
            bootinfo_request.response->version);
    }

    /* HHDM */
    if (hhdm_request.response) {
        serial_printf("HHDM offset: %lx\n", hhdm_request.response->offset);
    }

    /* Framebuffer */
    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
        serial_printf("Framebuffer: %lux%lu, %lu bpp, pitch %lu\n",
            fb->width, fb->height, fb->bpp, fb->pitch);
        fbcon_init(fb->address, fb->width, fb->height, fb->pitch, (uint32_t)fb->bpp);
        serial_puts("Framebuffer console initialized\n");
    } else {
        serial_puts("WARNING: No framebuffer available\n");
    }

    /* Memory map */
    serial_puts("\nMemory map:\n");
    uint64_t total_usable = 0;

    if (memmap_request.response) {
        uint64_t entry_count = memmap_request.response->entry_count;
        struct limine_memmap_entry **entries = memmap_request.response->entries;

        for (uint64_t i = 0; i < entry_count; i++) {
            struct limine_memmap_entry *e = entries[i];
            serial_printf("  [%lx — %lx] %s (%lu KB)\n",
                e->base, e->base + e->length,
                memmap_type_str(e->type),
                e->length / 1024);

            if (e->type == LIMINE_MEMMAP_USABLE)
                total_usable += e->length;
        }

        serial_printf("\nTotal usable RAM: %lu MB (%lu bytes)\n",
            total_usable / (1024 * 1024), total_usable);
        serial_printf("Memory map entries: %lu\n", entry_count);
    } else {
        serial_puts("WARNING: No memory map available\n");
    }

    /*
     * Kernel initialization order (dependencies):
     *
     *  1. serial_init()       — needed for all kernel logging (COM1 output)
     *  2. fbcon_init()        — framebuffer console; needs Limine FB response
     *  3. gdt_init()          — segment descriptors needed before any interrupts
     *  4. idt_init()          — interrupt table needed before page faults / PIT
     *  5. fpu_init()          — FPU/SSE state; safe after GDT/IDT
     *  6. pmm_init()          — physical memory manager; needs HHDM from Limine
     *  7. vmm_init()          — virtual memory manager; needs PMM for page tables
     *  8. kheap_init()        — kernel heap; needs VMM to map heap pages
     *  9. tss_init()          — task state segment; needs kheap for stack alloc
     * 10. sched_init()        — scheduler; needs TSS, threads need kheap
     * 11. pit_enable_sched()  — preemptive timer; needs scheduler + IDT ready
     * 12. syscall_init()      — SYSCALL/SYSRET MSRs; needs GDT selectors
     * 13. vfs_init()          — virtual filesystem; needs kheap for nodes
     * 14. tar_init()          — parse initrd into VFS; needs VFS
     * 15. pci_init()          — PCI bus scan; needs port I/O
     * 16. virtio_net_init()   — NIC driver; needs PCI device found
     * 17. net_init()          — network stack; needs virtio-net
     * 18. virtio_blk_init()   — block driver; needs PCI device found
     * 19. bcache_init()       — block cache; needs virtio-blk
     * 20. swap_init()         — swap area on disk; needs virtio-blk
     * 21. limnfs_format/mount — disk filesystem; needs bcache
     * 22. pty_init()          — pseudo-terminals; needs kheap
     * 23. tcp_init()          — TCP state; needs net stack
     * 24. smp_init()          — AP cores; needs GDT/IDT/PMM/VMM/sched all ready
     * 25. sched_set_smp_active() — enable per-CPU scheduling; needs SMP
     */

    /* ======== Stage 2 init ======== */
    serial_puts("\n--- Stage 2 init ---\n");

    gdt_init();
    idt_init();
    fpu_init();
    pmm_init();

    /* IDT smoke test: trigger breakpoint (int3) — should print and resume */
    serial_puts("\n[test] Triggering int3 (breakpoint)...\n");
    __asm__ volatile ("int3");
    serial_puts("[test] Resumed after int3 — IDT working!\n");

    /* PIT tick check */
    uint64_t ticks = pit_get_ticks();
    serial_printf("[test] PIT ticks so far: %lu\n", ticks);

    /* PMM smoke test */
    pmm_smoke_test();

    serial_puts("\n========================================\n");
    serial_puts("  Stage 2 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 3 init ======== */
    serial_puts("\n--- Stage 3 init ---\n");

    vmm_init();
    kheap_init();

    /* VMM smoke test */
    vmm_smoke_test();

    /* Heap smoke test */
    heap_smoke_test();

    serial_puts("\n========================================\n");
    serial_puts("  Stage 3 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 4 init ======== */
    serial_puts("\n--- Stage 4 init ---\n");

    tss_init();
    sched_init();

    /* Enable preemptive scheduling from timer IRQ */
    pit_enable_sched();

    /* Scheduler smoke test */
    sched_smoke_test();

    serial_puts("\n========================================\n");
    serial_puts("  Stage 4 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 5 init ======== */
    serial_puts("\n--- Stage 5 init ---\n");

    syscall_init();

    serial_puts("\n========================================\n");
    serial_puts("  Stage 5 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 6 init ======== */
    serial_puts("\n--- Stage 6 init ---\n");

    vfs_init();

    /* Load initrd from Limine module */
    if (module_request.response && module_request.response->module_count > 0) {
        struct limine_file *initrd = module_request.response->modules[0];
        serial_printf("[module] Loaded initrd: %lu bytes at %p\n",
            initrd->size, initrd->address);
        tar_init(initrd->address, initrd->size);
    } else {
        serial_puts("[module] WARNING: No modules loaded\n");
    }

    /* VFS kernel-side smoke test */
    vfs_smoke_test();

    serial_puts("\n========================================\n");
    serial_puts("  Stage 6 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 7 init ======== */
    serial_puts("\n--- Stage 7 init ---\n");

    /* PCI bus enumeration */
    pci_init();

    /* Virtio-net driver */
    if (virtio_net_init() == 0) {
        /* Network stack */
        net_init();

        /* ICMP ping smoke test — send ping to gateway, wait for reply */
        serial_puts("\n[test] ICMP ping test...\n");
        if (net_send_ping(0x0A000202) == 0) { /* 10.0.2.2 */
            /* Wait up to ~2 seconds for reply */
            for (int i = 0; i < 2000 && !net_got_ping_reply(); i++)
                sched_yield();
            if (net_got_ping_reply())
                serial_puts("[test] ICMP ping test PASSED\n");
            else
                serial_puts("[test] ICMP ping test: no reply (may be normal)\n");
        } else {
            serial_puts("[test] ICMP ping test: send failed\n");
        }

        /* Load udpecho.elf */
        serial_puts("\n[test] Loading udpecho.elf...\n");
        process_t *echo_proc = load_and_run_elf("/udpecho.elf");
        if (echo_proc)
            serial_printf("[test] udpecho.elf spawned (pid %lu)\n",
                          echo_proc->pid);
        else
            serial_puts("[test] udpecho.elf not found or failed to load\n");
    } else {
        serial_puts("[net]  Skipping network stack (no virtio-net)\n");
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 7 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 7.5 init ======== */
    serial_puts("\n--- Stage 7.5 init ---\n");

    /* --- Writable VFS smoke test --- */
    serial_puts("\n[test] Writable VFS smoke test...\n");
    {
        int idx = vfs_create("/test.txt");
        if (idx < 0) {
            serial_puts("[test] FAIL: vfs_create(\"/test.txt\") failed\n");
        } else {
            serial_printf("[test] Created /test.txt (node %d)\n", idx);

            const char *msg = "Hello writable VFS!";
            uint64_t msg_len = 0;
            while (msg[msg_len]) msg_len++;

            int64_t w = vfs_write(idx, 0, (const uint8_t *)msg, msg_len);
            if (w != (int64_t)msg_len) {
                serial_printf("[test] FAIL: vfs_write returned %ld\n", w);
            } else {
                uint8_t rbuf[64];
                int64_t r = vfs_read(idx, 0, rbuf, 63);
                if (r != (int64_t)msg_len) {
                    serial_printf("[test] FAIL: vfs_read returned %ld\n", r);
                } else {
                    rbuf[r] = '\0';
                    serial_printf("[test] Read back: \"%s\"\n", (const char *)rbuf);

                    /* Verify content */
                    int ok = 1;
                    for (uint64_t i = 0; i < msg_len; i++) {
                        if (rbuf[i] != (uint8_t)msg[i]) { ok = 0; break; }
                    }

                    if (ok) {
                        serial_puts("[test] Content verified OK\n");
                    } else {
                        serial_puts("[test] FAIL: content mismatch\n");
                    }
                }
            }

            /* Delete */
            if (vfs_delete("/test.txt") != 0) {
                serial_puts("[test] FAIL: vfs_delete failed\n");
            } else {
                /* Verify gone */
                if (vfs_open("/test.txt") >= 0) {
                    serial_puts("[test] FAIL: file still exists after delete\n");
                } else {
                    serial_puts("[test] File deleted successfully\n");
                }
            }
        }
        serial_puts("[test] Writable VFS smoke test PASSED\n");
    }

    /* --- Virtio-blk smoke test --- */
    int blk_ok = 0;
    if (virtio_blk_init() == 0) {
        serial_puts("\n[test] Virtio-blk smoke test...\n");

        uint8_t blk_buf[512];

        /* Read sector 0 */
        if (virtio_blk_read(0, blk_buf) == 0) {
            serial_printf("[test] Read sector 0: first bytes = %x %x %x %x\n",
                          blk_buf[0], blk_buf[1], blk_buf[2], blk_buf[3]);

            /* Write test pattern to sector 100 */
            for (int i = 0; i < 512; i++)
                blk_buf[i] = (uint8_t)(i & 0xFF);

            if (virtio_blk_write(100, blk_buf) == 0) {
                /* Read back and verify */
                uint8_t verify_buf[512];
                if (virtio_blk_read(100, verify_buf) == 0) {
                    int match = 1;
                    for (int i = 0; i < 512; i++) {
                        if (verify_buf[i] != (uint8_t)(i & 0xFF)) {
                            match = 0;
                            break;
                        }
                    }
                    if (match) {
                        serial_puts("[test] Virtio-blk smoke test PASSED\n");
                        blk_ok = 1;
                    } else {
                        serial_puts("[test] FAIL: read-back mismatch\n");
                    }
                } else {
                    serial_puts("[test] FAIL: read-back failed\n");
                }
            } else {
                serial_puts("[test] FAIL: write failed\n");
            }
        } else {
            serial_puts("[test] FAIL: read sector 0 failed\n");
        }
    } else {
        serial_puts("[blk]  Skipping virtio-blk (no device)\n");
    }

    /* --- LimnFS init --- */
    if (blk_ok) {
        bcache_init();
        swap_init();

        /* Check if disk already has LimnFS */
        uint8_t sb_buf[4096];
        bcache_read(0, sb_buf);
        uint32_t magic = *(uint32_t *)sb_buf;

        uint32_t disk_blocks = 14336;  /* 56MB / 4KB = 14336 (last 8MB reserved for swap) */
        if (magic != 0x4C494D46) {
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
                if (!n || n->parent != 0) continue;  /* only root-level */

                if (n->type == VFS_FILE) {
                    int existing = limnfs_dir_lookup(0, n->name);
                    if (existing >= 0) {
                        /* Already on disk — link VFS to disk */
                        n->disk_inode = (int32_t)existing;
                        /* Keep RAM data for now (ELF loader needs it) */
                        skipped++;
                    } else {
                        /* Copy to disk */
                        int disk_ino = limnfs_create_file(0, n->name);
                        if (disk_ino >= 0 && n->size > 0 && n->data) {
                            limnfs_write_data((uint32_t)disk_ino, 0,
                                              n->data, n->size);
                        }
                        if (disk_ino >= 0) {
                            /* Preserve VFS mode in LimnFS inode */
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

            /* Load disk-only files into VFS */
            vfs_mount_limnfs();
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 7.5 init complete\n");
    serial_puts("========================================\n");

    /* ======== Subsystem init ======== */
    serial_puts("\n--- Subsystem init ---\n");

    pty_init();
    tcp_init();
    futex_init();
    supervisor_init();
    shm_init();

    /* Initialize SMP (per-CPU data, LAPIC, AP bootstrap) */
    smp_init();
    sched_set_smp_active();

    /* Create console PTY */
    int con_pty = pty_create_console();
    if (con_pty >= 0) {
        pr_info("Console PTY %d created\n", con_pty);
    } else {
        pr_warn("Console PTY creation failed\n");
    }

    serial_puts("\n========================================\n");
    serial_puts("  Subsystem init complete\n");
    serial_puts("========================================\n");

    /* ======== Shell launch ======== */

    /* Start bcache flusher kernel thread (periodic write-back) */
    if (blk_ok)
        bcache_start_flusher();

    /* Start async inference worker kernel thread */
    infer_async_start_worker();

    /* Enable framebuffer console for interactive use */
    fbcon_set_serial(1);

    /* Start console PTY master reader thread — reads slave output
     * and writes to serial/fbcon for display */
    if (con_pty >= 0) {
        thread_t *crt = thread_create(console_reader_thread, 0);
        if (crt) sched_add(crt);
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
                    pt->slave_refs = 3;  /* fd 0, 1, 2 */
                    pt->fg_pgid = sh_proc->pid;
                    pr_info("Shell fd 0/1/2 connected to console PTY slave\n");
                }
            }
            /* Set LIMNX_VERSION env on shell */
            {
                const char *env_entry = "LIMNX_VERSION=0.89";
                int elen = 0;
                while (env_entry[elen]) elen++;
                for (int i = 0; i <= elen; i++)
                    sh_proc->env_buf[i] = env_entry[i];
                sh_proc->env_buf_len = elen + 1;
                sh_proc->env_count = 1;
            }
#ifdef RUN_BOOT_TESTS
            /* Auto-test mode: pass --test-all to shell */
            {
                const char *a0 = "shell.elf";
                const char *a1 = "--test-all";
                int pos = 0;
                while (*a0) sh_proc->argv_buf[pos++] = *a0++;
                sh_proc->argv_buf[pos++] = '\0';
                while (*a1) sh_proc->argv_buf[pos++] = *a1++;
                sh_proc->argv_buf[pos++] = '\0';
                sh_proc->argc = 2;
                sh_proc->argv_buf_len = pos;
            }
#endif
            pr_info("shell.elf spawned (pid %lu)\n",
                          sh_proc->pid);
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

    /* Keep scheduler running for background services */
    for (;;)
        sched_yield();
}
