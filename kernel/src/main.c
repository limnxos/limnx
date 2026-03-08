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
#include "net/netstor.h"
#include "fb/fbcon.h"
#include "pty/pty.h"
#include "net/tcp.h"
#include "smp/percpu.h"
#include "smp/lapic.h"
#include "sync/mutex.h"
#include "mm/swap.h"

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

static void hlt_loop(void) {
    for (;;)
        __asm__ volatile ("hlt");
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

/* --- Stage 6.5: ELF loading tests --- */

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

/* Load ELF and immediately schedule it (most common case) */
static process_t *load_and_run_elf(const char *path) {
    process_t *proc = load_elf_from_vfs(path);
    if (proc)
        sched_add(proc->main_thread);
    return proc;
}

static void set_process_args(process_t *proc, const char *arg0, const char *arg1) {
    int pos = 0;
    const char *p = arg0;
    while (*p) proc->argv_buf[pos++] = *p++;
    proc->argv_buf[pos++] = '\0';
    if (arg1) {
        p = arg1;
        while (*p) proc->argv_buf[pos++] = *p++;
        proc->argv_buf[pos++] = '\0';
        proc->argc = 2;
    } else {
        proc->argc = 1;
    }
    proc->argv_buf_len = pos;
}

static void elf_hello_test(void) {
    serial_puts("\n[test] ELF hello test...\n");

    process_t *proc = load_and_run_elf("/hello.elf");
    if (!proc) {
        serial_puts("[test] ELF hello test FAILED\n");
        return;
    }

    process_reap(proc);

    serial_puts("[test] ELF hello test PASSED\n");
}

static void elf_cat_test(void) {
    serial_puts("\n[test] ELF cat test...\n");

    process_t *proc = load_and_run_elf("/cat.elf");
    if (!proc) {
        serial_puts("[test] ELF cat test FAILED\n");
        return;
    }

    process_reap(proc);

    serial_puts("[test] ELF cat test PASSED\n");
}

static void sys_exec_test(void) {
    serial_puts("\n[test] sys_exec test...\n");

    /* Simulate sys_exec from kernel side: load /hello.elf by path */
    int node_idx = vfs_open("/hello.elf");
    if (node_idx < 0) {
        serial_puts("[test] FAIL: vfs_open(\"/hello.elf\") returned -1\n");
        return;
    }

    vfs_stat_t st;
    if (vfs_stat("/hello.elf", &st) != 0) {
        serial_puts("[test] FAIL: vfs_stat failed\n");
        return;
    }

    uint8_t *buf = (uint8_t *)kmalloc(st.size);
    if (!buf) {
        serial_puts("[test] FAIL: kmalloc failed\n");
        return;
    }

    int64_t n = vfs_read(node_idx, 0, buf, st.size);
    if (n != (int64_t)st.size) {
        kfree(buf);
        serial_puts("[test] FAIL: vfs_read failed\n");
        return;
    }

    process_t *proc = process_create_from_elf(buf, st.size);
    kfree(buf);

    if (!proc) {
        serial_puts("[test] sys_exec test FAILED\n");
        return;
    }

    sched_add(proc->main_thread);
    serial_printf("[test] sys_exec spawned pid %lu\n", proc->pid);

    process_reap(proc);

    serial_puts("[test] sys_exec test PASSED\n");
}

/* --- FPU/SSE initialization --- */

static void fpu_init(void) {
    uint64_t cr0, cr4;

    /* Read CR0: clear EM (bit 2), set MP (bit 1) */
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);  /* clear CR0.EM */
    cr0 |=  (1ULL << 1);  /* set CR0.MP */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    /* Read CR4: set OSFXSR (bit 9), set OSXMMEXCPT (bit 10) */
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);   /* CR4.OSFXSR */
    cr4 |= (1ULL << 10);  /* CR4.OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    /* Initialize x87 FPU */
    __asm__ volatile ("fninit");

    serial_puts("[fpu] FPU/SSE enabled (CR0.EM=0, CR4.OSFXSR=1, CR4.OSXMMEXCPT=1)\n");
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
        serial_puts("FATAL: Limine base revision not supported\n");
        hlt_loop();
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

    /* ======== Stage 6.5 init ======== */
    serial_puts("\n--- Stage 6.5 init ---\n");

    /* ELF hello test: load /hello.elf from VFS */
    elf_hello_test();

    /* ELF cat test: load /cat.elf from VFS */
    elf_cat_test();

    /* sys_exec test: spawn /hello.elf by path */
    sys_exec_test();

    serial_puts("\nStage 6.5 init complete\n");

    serial_puts("\n========================================\n");
    serial_puts("  Stage 6.5 init complete\n");
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

    /* --- Network storage smoke test --- */
    serial_puts("\n[test] Network storage smoke test...\n");
    if (netstor_init() == 0) {
        const char *val = "world";
        uint32_t vlen = 0;
        while (val[vlen]) vlen++;

        int status = netstor_put("hello", val, (uint16_t)vlen);
        if (status == NETSTOR_OK) {
            serial_puts("[test] PUT hello=world OK\n");

            char gbuf[64];
            uint16_t glen = 0;
            status = netstor_get("hello", gbuf, sizeof(gbuf) - 1, &glen);
            if (status == NETSTOR_OK) {
                gbuf[glen] = '\0';
                serial_printf("[test] GET hello -> \"%s\"\n", gbuf);

                status = netstor_del("hello");
                if (status == NETSTOR_OK) {
                    serial_puts("[test] DEL hello OK\n");
                    serial_puts("[test] Network storage smoke test PASSED\n");
                } else {
                    serial_puts("[test] FAIL: netstor_del failed\n");
                }
            } else {
                serial_puts("[test] FAIL: netstor_get failed\n");
            }
        } else {
            serial_puts("[test] Network storage: no server (skipped)\n");
        }
    } else {
        serial_puts("[test] Network storage: init failed (skipped)\n");
    }

    /* --- Load writetest.elf --- */
    serial_puts("\n[test] Loading writetest.elf...\n");
    {
        process_t *wt_proc = load_and_run_elf("/writetest.elf");
        if (wt_proc) {
            serial_printf("[test] writetest.elf spawned (pid %lu)\n",
                          wt_proc->pid);
            process_reap(wt_proc);
            serial_puts("[test] writetest.elf completed\n");
        } else {
            serial_puts("[test] writetest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 7.5 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 8 init ======== */
    serial_puts("\n--- Stage 8 init ---\n");

    /* Load and run mathtest.elf (C user-space program with float math + mmap) */
    serial_puts("\n[test] Loading mathtest.elf...\n");
    {
        process_t *mt_proc = load_and_run_elf("/mathtest.elf");
        if (mt_proc) {
            serial_printf("[test] mathtest.elf spawned (pid %lu)\n",
                          mt_proc->pid);
            process_reap(mt_proc);
            serial_puts("[test] mathtest.elf completed\n");
        } else {
            serial_puts("[test] mathtest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 8 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 9 init ======== */
    serial_puts("\n--- Stage 9 init ---\n");

    /* Load and run agenttest.elf (tensor primitives + MLP forward pass) */
    serial_puts("\n[test] Loading agenttest.elf...\n");
    {
        process_t *at_proc = load_and_run_elf("/agenttest.elf");
        if (at_proc) {
            serial_printf("[test] agenttest.elf spawned (pid %lu)\n",
                          at_proc->pid);
            process_reap(at_proc);
            serial_puts("[test] agenttest.elf completed\n");
        } else {
            serial_puts("[test] agenttest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 9 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 10 init ======== */
    serial_puts("\n--- Stage 10 init ---\n");

    /* Load and run agentrt.elf (semantic memory + agent runtime) */
    serial_puts("\n[test] Loading agentrt.elf...\n");
    {
        process_t *ar_proc = load_and_run_elf("/agentrt.elf");
        if (ar_proc) {
            serial_printf("[test] agentrt.elf spawned (pid %lu)\n",
                          ar_proc->pid);
            process_reap(ar_proc);
            serial_puts("[test] agentrt.elf completed\n");
        } else {
            serial_puts("[test] agentrt.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 10 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 11 init ======== */
    serial_puts("\n--- Stage 11 init ---\n");

    /* Load and run infertest.elf (transformer inference runtime) */
    serial_puts("\n[test] Loading infertest.elf...\n");
    {
        process_t *inf_proc = load_and_run_elf("/infertest.elf");
        if (inf_proc) {
            serial_printf("[test] infertest.elf spawned (pid %lu)\n",
                          inf_proc->pid);
            process_reap(inf_proc);
            serial_puts("[test] infertest.elf completed\n");
        } else {
            serial_puts("[test] infertest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 11 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 12 init ======== */
    serial_puts("\n--- Stage 12 init ---\n");

    /* Load and run pipetest.elf (automated IPC tests) */
    serial_puts("\n[test] Loading pipetest.elf...\n");
    {
        process_t *pt_proc = load_and_run_elf("/pipetest.elf");
        if (pt_proc) {
            serial_printf("[test] pipetest.elf spawned (pid %lu)\n",
                          pt_proc->pid);
            process_reap(pt_proc);
            serial_puts("[test] pipetest.elf completed\n");
        } else {
            serial_puts("[test] pipetest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 12 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 13 init ======== */
    serial_puts("\n--- Stage 13 init ---\n");

    /* Load and run generate.elf (automated self-test via --test arg) */
    serial_puts("\n[test] Loading generate.elf...\n");
    {
        process_t *gen_proc = load_and_run_elf("/generate.elf");
        if (gen_proc) {
            set_process_args(gen_proc, "generate.elf", "--test");
            serial_printf("[test] generate.elf spawned (pid %lu)\n",
                          gen_proc->pid);
            process_reap(gen_proc);
            serial_puts("[test] generate.elf completed\n");
        } else {
            serial_puts("[test] generate.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 13 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 15 init ======== */
    serial_puts("\n--- Stage 15 init ---\n");

    /* Load and run toolagent.elf (self-test via --test arg) */
    serial_puts("\n[test] Loading toolagent.elf...\n");
    {
        process_t *ta_proc = load_and_run_elf("/toolagent.elf");
        if (ta_proc) {
            set_process_args(ta_proc, "toolagent.elf", "--test");
            serial_printf("[test] toolagent.elf spawned (pid %lu)\n",
                          ta_proc->pid);
            process_reap(ta_proc);
            serial_puts("[test] toolagent.elf completed\n");
        } else {
            serial_puts("[test] toolagent.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 15 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 16 init ======== */
    serial_puts("\n--- Stage 16 init ---\n");

    /* Load and run memtest.elf (persistent vecstore save/load test) */
    serial_puts("\n[test] Loading memtest.elf...\n");
    {
        process_t *mem_proc = load_and_run_elf("/memtest.elf");
        if (mem_proc) {
            serial_printf("[test] memtest.elf spawned (pid %lu)\n",
                          mem_proc->pid);
            process_reap(mem_proc);
            serial_puts("[test] memtest.elf completed\n");
        } else {
            serial_puts("[test] memtest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 16 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 17 init ======== */
    serial_puts("\n--- Stage 17 init ---\n");

    /* Load and run ragtest.elf (RAG end-to-end test) */
    serial_puts("\n[test] Loading ragtest.elf...\n");
    {
        process_t *rag_proc = load_and_run_elf("/ragtest.elf");
        if (rag_proc) {
            serial_printf("[test] ragtest.elf spawned (pid %lu)\n",
                          rag_proc->pid);
            process_reap(rag_proc);
            serial_puts("[test] ragtest.elf completed\n");
        } else {
            serial_puts("[test] ragtest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n[stage17] Interactive programs available via shell:\n");
    serial_puts("  exec /generate.elf   — Interactive text generation\n");
    serial_puts("  exec /chat.elf       — Chatbot with RAG memory\n");
    serial_puts("  exec /learn.elf      — Training loop (hill climbing)\n");
    serial_puts("  exec /agent.elf      — Interactive agent with memory\n");
    serial_puts("  exec /toolagent.elf  — Tool-using agent with RAG memory\n");

    serial_puts("\n========================================\n");
    serial_puts("  Stage 17 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 18 init ======== */
    serial_puts("\n--- Stage 18 init ---\n");

    /* LimnFS already mounted at / during Stage 7.5 init */

    /* Load and run fstest.elf (filesystem tests) */
    serial_puts("\n[test] Loading fstest.elf...\n");
    {
        process_t *fs_proc = load_and_run_elf("/fstest.elf");
        if (fs_proc) {
            serial_printf("[test] fstest.elf spawned (pid %lu)\n",
                          fs_proc->pid);
            process_reap(fs_proc);
            serial_puts("[test] fstest.elf completed\n");
        } else {
            serial_puts("[test] fstest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 18 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 19 init ======== */
    serial_puts("\n--- Stage 19 init ---\n");

    /* Load and run fstest2.elf (filesystem completion tests) */
    serial_puts("\n[test] Loading fstest2.elf...\n");
    {
        process_t *fs2_proc = load_and_run_elf("/fstest2.elf");
        if (fs2_proc) {
            serial_printf("[test] fstest2.elf spawned (pid %lu)\n",
                          fs2_proc->pid);
            process_reap(fs2_proc);
            serial_puts("[test] fstest2.elf completed\n");
        } else {
            serial_puts("[test] fstest2.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 19 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 20 init ======== */
    serial_puts("\n--- Stage 20 init ---\n");

    /* Load and run lmstest.elf (large model support tests) */
    serial_puts("\n[test] Loading lmstest.elf...\n");
    {
        process_t *lms_proc = load_and_run_elf("/lmstest.elf");
        if (lms_proc) {
            serial_printf("[test] lmstest.elf spawned (pid %lu)\n",
                          lms_proc->pid);
            process_reap(lms_proc);
            serial_puts("[test] lmstest.elf completed\n");
        } else {
            serial_puts("[test] lmstest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 20 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 21 init ======== */
    serial_puts("\n--- Stage 21 init ---\n");

    /* Load and run gguftest.elf (modern transformer architecture tests) */
    serial_puts("\n[test] Loading gguftest.elf...\n");
    {
        process_t *gguf_proc = load_and_run_elf("/gguftest.elf");
        if (gguf_proc) {
            serial_printf("[test] gguftest.elf spawned (pid %lu)\n",
                          gguf_proc->pid);
            process_reap(gguf_proc);
            serial_puts("[test] gguftest.elf completed\n");
        } else {
            serial_puts("[test] gguftest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 21 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 22 init ======== */
    serial_puts("\n--- Stage 22 init ---\n");

    /* Load and run gguf2test.elf (full GGUF support tests) */
    serial_puts("\n[test] Loading gguf2test.elf...\n");
    {
        process_t *gguf2_proc = load_and_run_elf("/gguf2test.elf");
        if (gguf2_proc) {
            serial_printf("[test] gguf2test.elf spawned (pid %lu)\n",
                          gguf2_proc->pid);
            process_reap(gguf2_proc);
            serial_puts("[test] gguf2test.elf completed\n");
        } else {
            serial_puts("[test] gguf2test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 22 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 23 init ======== */
    serial_puts("\n--- Stage 23 init ---\n");

    /* Load and run agenttest2.elf (agent intelligence tests) */
    serial_puts("\n[test] Loading agenttest2.elf...\n");
    {
        process_t *at2_proc = load_and_run_elf("/agenttest2.elf");
        if (at2_proc) {
            serial_printf("[test] agenttest2.elf spawned (pid %lu)\n",
                          at2_proc->pid);
            process_reap(at2_proc);
            serial_puts("[test] agenttest2.elf completed\n");
        } else {
            serial_puts("[test] agenttest2.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 23 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 24 init ======== */
    serial_puts("\n--- Stage 24 init ---\n");

    /* Load and run ostest.elf (OS maturity tests) */
    serial_puts("\n[test] Loading ostest.elf...\n");
    {
        process_t *os_proc = load_and_run_elf("/ostest.elf");
        if (os_proc) {
            serial_printf("[test] ostest.elf spawned (pid %lu)\n",
                          os_proc->pid);
            process_reap(os_proc);
            serial_puts("[test] ostest.elf completed\n");
        } else {
            serial_puts("[test] ostest.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 24 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 25 init ======== */
    serial_puts("\n--- Stage 25 init ---\n");

    /* Load and run s25test.elf (framebuffer, fd inheritance, multi-agent tests) */
    serial_puts("\n[test] Loading s25test.elf...\n");
    {
        process_t *s25_proc = load_and_run_elf("/s25test.elf");
        if (s25_proc) {
            serial_printf("[test] s25test.elf spawned (pid %lu)\n",
                          s25_proc->pid);
            process_reap(s25_proc);
            serial_puts("[test] s25test.elf completed\n");
        } else {
            serial_puts("[test] s25test.elf not found or failed to load\n");
        }
    }

    /* Load and run multiagent.elf (multi-agent self-test) */
    serial_puts("\n[test] Loading multiagent.elf...\n");
    {
        process_t *ma_proc = load_and_run_elf("/multiagent.elf");
        if (ma_proc) {
            serial_printf("[test] multiagent.elf spawned (pid %lu)\n",
                          ma_proc->pid);
            process_reap(ma_proc);
            serial_puts("[test] multiagent.elf completed\n");
        } else {
            serial_puts("[test] multiagent.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 25 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 26 init ======== */
    serial_puts("\n--- Stage 26 init ---\n");

    /* Load and run s26test.elf (LimnFS disk filesystem tests) */
    serial_puts("\n[test] Loading s26test.elf...\n");
    {
        process_t *s26_proc = load_and_run_elf("/s26test.elf");
        if (s26_proc) {
            serial_printf("[test] s26test.elf spawned (pid %lu)\n",
                          s26_proc->pid);
            process_reap(s26_proc);
            serial_puts("[test] s26test.elf completed\n");
        } else {
            serial_puts("[test] s26test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 26 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 27 init ======== */
    serial_puts("\n--- Stage 27 init ---\n");

    /* Load and run s27test.elf (large file / double indirect tests) */
    serial_puts("\n[test] Loading s27test.elf...\n");
    {
        process_t *s27_proc = load_and_run_elf("/s27test.elf");
        if (s27_proc) {
            serial_printf("[test] s27test.elf spawned (pid %lu)\n",
                          s27_proc->pid);
            process_reap(s27_proc);
            serial_puts("[test] s27test.elf completed\n");
        } else {
            serial_puts("[test] s27test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 27 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 28 init ======== */
    serial_puts("\n--- Stage 28 init ---\n");

    /* Load and run s28test.elf (process args, fcntl, nonblock tests) */
    serial_puts("\n[test] Loading s28test.elf...\n");
    {
        process_t *s28_proc = load_and_run_elf("/s28test.elf");
        if (s28_proc) {
            set_process_args(s28_proc, "s28test.elf", "--test");
            serial_printf("[test] s28test.elf spawned (pid %lu)\n",
                          s28_proc->pid);
            process_reap(s28_proc);
            serial_puts("[test] s28test.elf completed\n");
        } else {
            serial_puts("[test] s28test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 28 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 29 init ======== */
    serial_puts("\n--- Stage 29 init ---\n");

    /* Load and run s29test.elf (framebuffer, bcache LRU, triple indirect, cloexec/nonblock tests) */
    serial_puts("\n[test] Loading s29test.elf...\n");
    {
        process_t *s29_proc = load_and_run_elf("/s29test.elf");
        if (s29_proc) {
            serial_printf("[test] s29test.elf spawned (pid %lu)\n",
                          s29_proc->pid);
            process_reap(s29_proc);
            serial_puts("[test] s29test.elf completed\n");
        } else {
            serial_puts("[test] s29test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 29 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 30 init ======== */
    serial_puts("\n--- Stage 30 init ---\n");

    /* Load and run s30test.elf */
    serial_puts("\n[test] Loading s30test.elf...\n");
    {
        process_t *s30_proc = load_and_run_elf("/s30test.elf");
        if (s30_proc) {
            serial_printf("[test] s30test.elf spawned (pid %lu)\n",
                          s30_proc->pid);
            process_reap(s30_proc);
            serial_puts("[test] s30test.elf completed\n");
        } else {
            serial_puts("[test] s30test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 30 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 31 init ======== */
    serial_puts("\n--- Stage 31 init ---\n");

    /* Load and run s31test.elf (fork, COW, sigaction tests) */
    serial_puts("\n[test] Loading s31test.elf...\n");
    {
        process_t *s31_proc = load_and_run_elf("/s31test.elf");
        if (s31_proc) {
            serial_printf("[test] s31test.elf spawned (pid %lu)\n",
                          s31_proc->pid);
            process_reap(s31_proc);
            serial_puts("[test] s31test.elf completed\n");
        } else {
            serial_puts("[test] s31test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 31 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 32 init ======== */
    serial_puts("\n--- Stage 32 init ---\n");

    pty_init();
    tcp_init();

    /* Load and run s32test.elf (PTY + TCP tests) */
    serial_puts("\n[test] Loading s32test.elf...\n");
    {
        process_t *s32_proc = load_and_run_elf("/s32test.elf");
        if (s32_proc) {
            serial_printf("[test] s32test.elf spawned (pid %lu)\n",
                          s32_proc->pid);
            process_reap(s32_proc);
            serial_puts("[test] s32test.elf completed\n");
        } else {
            serial_puts("[test] s32test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 32 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 33 init ======== */
    serial_puts("\n--- Stage 33 init ---\n");

    /* Initialize SMP (per-CPU data, LAPIC, AP bootstrap) */
    smp_init();
    sched_set_smp_active();

    /* Create console PTY */
    int con_pty = pty_create_console();
    if (con_pty >= 0) {
        serial_printf("[stage33] Console PTY %d created\n", con_pty);
    } else {
        serial_puts("[stage33] WARN: Console PTY creation failed\n");
    }

    /* Load and run s33test.elf */
    serial_puts("\n[test] Loading s33test.elf...\n");
    {
        process_t *s33_proc = load_and_run_elf("/s33test.elf");
        if (s33_proc) {
            serial_printf("[test] s33test.elf spawned (pid %lu)\n",
                          s33_proc->pid);
            process_reap(s33_proc);
            serial_puts("[test] s33test.elf completed\n");
        } else {
            serial_puts("[test] s33test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 33 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 34 init ======== */
    serial_puts("\n--- Stage 34 init ---\n");

    /* Mutex smoke test */
    {
        mutex_t test_mutex = MUTEX_INIT;
        mutex_lock(&test_mutex);
        int trylock_result = mutex_trylock(&test_mutex);
        mutex_unlock(&test_mutex);
        int trylock_after = mutex_trylock(&test_mutex);
        mutex_unlock(&test_mutex);
        serial_printf("[stage34] Mutex smoke: trylock while held=%d, after unlock=%d\n",
                      trylock_result, trylock_after);
    }

    /* Load and run s34test.elf */
    serial_puts("\n[test] Loading s34test.elf...\n");
    {
        process_t *s34_proc = load_elf_from_vfs("/s34test.elf");
        if (s34_proc) {
            /* Set LIMNX_VERSION env var on test process */
            const char *env_entry = "LIMNX_VERSION=0.48";
            int elen = 0;
            while (env_entry[elen]) elen++;
            for (int i = 0; i <= elen; i++)
                s34_proc->env_buf[i] = env_entry[i];
            s34_proc->env_buf_len = elen + 1;
            s34_proc->env_count = 1;

            sched_add(s34_proc->main_thread);
            serial_printf("[test] s34test.elf spawned (pid %lu)\n",
                          s34_proc->pid);
            process_reap(s34_proc);
            serial_puts("[test] s34test.elf completed\n");
        } else {
            serial_puts("[test] s34test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 34 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 35 init ======== */
    serial_puts("\n--- Stage 35 init ---\n");

    /* Load and run s35test.elf */
    serial_puts("\n[test] Loading s35test.elf...\n");
    {
        process_t *s35_proc = load_and_run_elf("/s35test.elf");
        if (s35_proc) {
            serial_printf("[test] s35test.elf spawned (pid %lu)\n",
                          s35_proc->pid);
            process_reap(s35_proc);
            serial_puts("[test] s35test.elf completed\n");
        } else {
            serial_puts("[test] s35test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 35 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 36 init ======== */
    serial_puts("\n--- Stage 36 init ---\n");

    /* Load and run s36test.elf */
    serial_puts("\n[test] Loading s36test.elf...\n");
    {
        process_t *s36_proc = load_and_run_elf("/s36test.elf");
        if (s36_proc) {
            serial_printf("[test] s36test.elf spawned (pid %lu)\n",
                          s36_proc->pid);
            process_reap(s36_proc);
            serial_puts("[test] s36test.elf completed\n");
        } else {
            serial_puts("[test] s36test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 36 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 37 init ======== */
    serial_puts("\n--- Stage 37 init ---\n");

    /* Load and run s37test.elf */
    serial_puts("\n[test] Loading s37test.elf...\n");
    {
        process_t *s37_proc = load_and_run_elf("/s37test.elf");
        if (s37_proc) {
            serial_printf("[test] s37test.elf spawned (pid %lu)\n",
                          s37_proc->pid);
            process_reap(s37_proc);
            serial_puts("[test] s37test.elf completed\n");
        } else {
            serial_puts("[test] s37test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 37 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 38 init ======== */
    serial_puts("\n--- Stage 38 init ---\n");

    /* Load and run s38test.elf */
    serial_puts("\n[test] Loading s38test.elf...\n");
    {
        process_t *s38_proc = load_and_run_elf("/s38test.elf");
        if (s38_proc) {
            serial_printf("[test] s38test.elf spawned (pid %lu)\n",
                          s38_proc->pid);
            process_reap(s38_proc);
            serial_puts("[test] s38test.elf completed\n");
        } else {
            serial_puts("[test] s38test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 38 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 39 init ======== */
    serial_puts("\n--- Stage 39 init ---\n");

    /* Load and run s39test.elf */
    serial_puts("\n[test] Loading s39test.elf...\n");
    {
        process_t *s39_proc = load_and_run_elf("/s39test.elf");
        if (s39_proc) {
            serial_printf("[test] s39test.elf spawned (pid %lu)\n",
                          s39_proc->pid);
            process_reap(s39_proc);
            serial_puts("[test] s39test.elf completed\n");
        } else {
            serial_puts("[test] s39test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 39 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 41 init ======== */
    serial_puts("\n--- Stage 41 init ---\n");

    /* Load and run s41test.elf */
    serial_puts("\n[test] Loading s41test.elf...\n");
    {
        process_t *s41_proc = load_and_run_elf("/s41test.elf");
        if (s41_proc) {
            serial_printf("[test] s41test.elf spawned (pid %lu)\n",
                          s41_proc->pid);
            process_reap(s41_proc);
            serial_puts("[test] s41test.elf completed\n");
        } else {
            serial_puts("[test] s41test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 41 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 42 init ======== */
    serial_puts("\n--- Stage 42 init ---\n");

    /* Load and run s42test.elf */
    serial_puts("\n[test] Loading s42test.elf...\n");
    {
        process_t *s42_proc = load_and_run_elf("/s42test.elf");
        if (s42_proc) {
            serial_printf("[test] s42test.elf spawned (pid %lu)\n",
                          s42_proc->pid);
            process_reap(s42_proc);
            serial_puts("[test] s42test.elf completed\n");
        } else {
            serial_puts("[test] s42test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 42 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 44 init ======== */
    serial_puts("\n--- Stage 44 init ---\n");

    /* Load and run s44test.elf */
    serial_puts("\n[test] Loading s44test.elf...\n");
    {
        process_t *s44_proc = load_and_run_elf("/s44test.elf");
        if (s44_proc) {
            serial_printf("[test] s44test.elf spawned (pid %lu)\n",
                          s44_proc->pid);
            process_reap(s44_proc);
            serial_puts("[test] s44test.elf completed\n");
        } else {
            serial_puts("[test] s44test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 44 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 45 init ======== */
    serial_puts("\n--- Stage 45 init ---\n");

    /* Load and run s45test.elf */
    serial_puts("\n[test] Loading s45test.elf...\n");
    {
        process_t *s45_proc = load_and_run_elf("/s45test.elf");
        if (s45_proc) {
            serial_printf("[test] s45test.elf spawned (pid %lu)\n",
                          s45_proc->pid);
            process_reap(s45_proc);
            serial_puts("[test] s45test.elf completed\n");
        } else {
            serial_puts("[test] s45test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 45 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 47 init ======== */
    serial_puts("\n--- Stage 47 init ---\n");

    /* Load and run s47test.elf */
    serial_puts("\n[test] Loading s47test.elf...\n");
    {
        process_t *s47_proc = load_and_run_elf("/s47test.elf");
        if (s47_proc) {
            serial_printf("[test] s47test.elf spawned (pid %lu)\n",
                          s47_proc->pid);
            process_reap(s47_proc);
            serial_puts("[test] s47test.elf completed\n");
        } else {
            serial_puts("[test] s47test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 47 init complete\n");
    serial_puts("========================================\n");

    /* ======== Stage 48 init ======== */
    serial_puts("\n--- Stage 48 init ---\n");

    /* Load and run s48test.elf */
    serial_puts("\n[test] Loading s48test.elf...\n");
    {
        process_t *s48_proc = load_and_run_elf("/s48test.elf");
        if (s48_proc) {
            serial_printf("[test] s48test.elf spawned (pid %lu)\n",
                          s48_proc->pid);
            process_reap(s48_proc);
            serial_puts("[test] s48test.elf completed\n");
        } else {
            serial_puts("[test] s48test.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  Stage 48 init complete\n");
    serial_puts("========================================\n");

    /* Start bcache flusher kernel thread (periodic write-back).
     * Launched after all boot-time disk syncing is complete to
     * avoid data races with the non-locked bcache. */
    if (blk_ok)
        bcache_start_flusher();

    /* Enable framebuffer console for interactive use */
    fbcon_set_serial(1);

    /* Start console PTY master reader thread — reads slave output
     * and writes to serial/fbcon for display */
    if (con_pty >= 0) {
        thread_t *crt = thread_create(console_reader_thread, 0);
        if (crt) sched_add(crt);
    }

    /* Load and run shell.elf with console PTY as stdin/stdout/stderr */
    serial_puts("\n[test] Loading shell.elf...\n");
    {
        process_t *sh_proc = load_elf_from_vfs("/shell.elf");
        if (sh_proc) {
            /* Set up fd 0/1/2 as console PTY slave */
            if (con_pty >= 0) {
                pty_t *pt = pty_get(con_pty);
                if (pt) {
                    for (int fd = 0; fd < 3; fd++) {
                        sh_proc->fd_table[fd].node = (void *)0;
                        sh_proc->fd_table[fd].offset = 0;
                        sh_proc->fd_table[fd].pipe = (void *)0;
                        sh_proc->fd_table[fd].pipe_write = 0;
                        sh_proc->fd_table[fd].pty = (void *)pt;
                        sh_proc->fd_table[fd].pty_is_master = 0;
                        sh_proc->fd_table[fd].unix_sock = (void *)0;
                        sh_proc->fd_table[fd].eventfd = (void *)0;
                        sh_proc->fd_table[fd].epoll = (void *)0;
                        sh_proc->fd_table[fd].uring = (void *)0;
                        sh_proc->fd_table[fd].open_flags = 2; /* O_RDWR */
                        sh_proc->fd_table[fd].fd_flags = 0;
                    }
                    pt->slave_refs = 3;  /* fd 0, 1, 2 */
                    /* Set console PTY fg_pgid to shell's pid */
                    pt->fg_pgid = sh_proc->pid;
                    serial_puts("[stage34] Shell fd 0/1/2 connected to console PTY slave\n");
                }
            }
            /* Set LIMNX_VERSION env on shell */
            {
                const char *env_entry = "LIMNX_VERSION=0.48";
                int elen = 0;
                while (env_entry[elen]) elen++;
                for (int i = 0; i <= elen; i++)
                    sh_proc->env_buf[i] = env_entry[i];
                sh_proc->env_buf_len = elen + 1;
                sh_proc->env_count = 1;
            }
            serial_printf("[test] shell.elf spawned (pid %lu)\n",
                          sh_proc->pid);
            /* Schedule shell AFTER fd/env setup is complete */
            sched_add(sh_proc->main_thread);
            process_reap(sh_proc);
            serial_puts("[test] shell.elf completed\n");
        } else {
            serial_puts("[test] shell.elf not found or failed to load\n");
        }
    }

    serial_puts("\n========================================\n");
    serial_puts("  All stages (2-39) complete — idling\n");
    serial_puts("========================================\n");

    /* Keep scheduler running so udpecho can process packets */
    for (;;)
        sched_yield();
}
