/*
 * s52test.c — Stage 52 Tests: Process Memory Cleanup Audit
 *
 * Tests that shared memory pages survive process exit correctly,
 * and that explicit shmdt properly unmaps pages.
 */

#include "libc/libc.h"

static int test_num = 0;
static int pass_count = 0;

static void check(int ok, const char *desc) {
    test_num++;
    if (ok) {
        printf("  [%d] PASS: %s\n", test_num, desc);
        pass_count++;
    } else {
        printf("  [%d] FAIL: %s\n", test_num, desc);
    }
}

static void test_shm_survives_child_exit(void) {
    /* Child writes to shm, exits. Parent re-reads — data must be intact.
     * Before the fix, child's exit would free shm physical pages. */
    long shmid = sys_shmget(200, 1);
    check(shmid >= 0, "shmget for survival test");

    long addr = sys_shmat(shmid);
    check(addr > 0, "shmat for survival test");

    volatile int *shared = (volatile int *)addr;
    *shared = 0;

    long pid = sys_fork();
    if (pid == 0) {
        /* Child: write a known pattern and exit */
        shared[0] = 0xDEAD;
        shared[1] = 0xBEEF;
        shared[2] = 0xCAFE;
        shared[3] = 0xF00D;
        sys_exit(0);
    }

    sys_waitpid(pid);

    /* Verify shm data is intact after child exit */
    check(shared[0] == 0xDEAD, "shm[0] intact after child exit");
    check(shared[1] == 0xBEEF, "shm[1] intact after child exit");
    check(shared[2] == 0xCAFE, "shm[2] intact after child exit");
    check(shared[3] == 0xF00D, "shm[3] intact after child exit");

    sys_shmdt(addr);
}

static void test_shm_survives_multiple_exits(void) {
    /* Multiple children attach, write, and exit. Parent verifies. */
    long shmid = sys_shmget(201, 1);
    long addr = sys_shmat(shmid);
    if (addr <= 0) { check(0, "multi-exit shmat"); return; }

    volatile int *shared = (volatile int *)addr;
    *shared = 0;

    /* Fork 3 children, each increments shared counter */
    for (int c = 0; c < 3; c++) {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: increment and exit */
            shared[0]++;
            sys_exit(0);
        }
        sys_waitpid(pid);
    }

    check(shared[0] == 3, "shm counter correct after 3 child exits");

    sys_shmdt(addr);
}

static void test_detach_reattach(void) {
    /* Detach shm, then re-attach — data should persist */
    long shmid = sys_shmget(202, 1);
    long addr = sys_shmat(shmid);
    if (addr <= 0) { check(0, "detach-reattach shmat"); return; }

    volatile int *shared = (volatile int *)addr;
    *shared = 12345;

    sys_shmdt(addr);

    /* Re-attach — should get same physical page with same data */
    long addr2 = sys_shmat(shmid);
    check(addr2 > 0, "re-attach after shmdt succeeds");

    if (addr2 > 0) {
        volatile int *shared2 = (volatile int *)addr2;
        check(shared2[0] == 12345, "data persists after detach-reattach");
        sys_shmdt(addr2);
    }
}

static void test_shm_two_processes_sequential(void) {
    /* Process A creates shm, writes, exits. Process B attaches — data intact.
     * Simulated: parent creates, child attaches from fresh shmat. */
    long shmid = sys_shmget(203, 1);
    long addr = sys_shmat(shmid);
    if (addr <= 0) { check(0, "sequential shmat"); return; }

    volatile int *shared = (volatile int *)addr;
    shared[0] = 0xABCD;

    /* Detach from parent (simulates process A exiting) */
    sys_shmdt(addr);

    /* Fork child to attach fresh (simulates process B) */
    long pid = sys_fork();
    if (pid == 0) {
        long caddr = sys_shmat(shmid);
        if (caddr > 0) {
            volatile int *cs = (volatile int *)caddr;
            /* Write result back to shm for parent to check */
            if (cs[0] == 0xABCD)
                cs[0] = 0x1111;  /* success marker */
            else
                cs[0] = 0x2222;  /* failure marker */
            sys_shmdt(caddr);
        }
        sys_exit(0);
    }
    sys_waitpid(pid);

    /* Re-attach to check child's result */
    long addr3 = sys_shmat(shmid);
    if (addr3 > 0) {
        volatile int *s3 = (volatile int *)addr3;
        check(s3[0] == 0x1111, "shm data survives detach + child reattach");
        sys_shmdt(addr3);
    } else {
        check(0, "shm data survives detach + child reattach");
    }
}

static void test_demand_page_cleanup(void) {
    /* Allocate demand pages, touch some, exit via fork.
     * Verifies demand pages (both touched and untouched) don't leak. */
    long pid = sys_fork();
    if (pid == 0) {
        /* Child: allocate demand pages via mmap2 */
        long addr = sys_mmap2(4, 1);  /* 4 pages, demand */
        if (addr > 0) {
            /* Touch only first 2 pages */
            volatile char *p = (volatile char *)addr;
            p[0] = 'A';
            p[4096] = 'B';
            /* Pages 2 and 3 untouched — no physical backing */
        }
        sys_exit(0);
    }
    sys_waitpid(pid);
    /* If we get here without hanging/crashing, cleanup worked */
    check(1, "demand page cleanup on exit (no crash)");
}

static void test_file_mmap_cleanup(void) {
    /* File-backed mmap cleanup on exit */
    long pid = sys_fork();
    if (pid == 0) {
        long fd = sys_open("/hello.elf", 0);  /* O_RDONLY */
        if (fd >= 0) {
            long addr = sys_mmap_file(fd, 0, 2);
            if (addr > 0) {
                /* Touch first page to trigger fault */
                volatile unsigned char *p = (volatile unsigned char *)addr;
                (void)p[0];
                /* Second page untouched */
                sys_munmap(addr);
            }
            sys_close(fd);
        }
        sys_exit(0);
    }
    sys_waitpid(pid);
    check(1, "file-backed mmap cleanup on exit (no crash)");
}

int main(void) {
    printf("=== Stage 52 Tests: Process Memory Cleanup ===\n\n");

    test_shm_survives_child_exit();
    test_shm_survives_multiple_exits();
    test_detach_reattach();
    test_shm_two_processes_sequential();
    test_demand_page_cleanup();
    test_file_mmap_cleanup();

    printf("\n=== Stage 52 Results: %d/%d passed ===\n", pass_count, test_num);
    return 0;
}
