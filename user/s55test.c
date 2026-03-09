#include "libc/libc.h"

static int passed = 0, failed = 0;

static void check(int ok, const char *name) {
    if (ok) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

int main(void) {
    printf("=== Stage 55: malloc/free ===\n");

    /* Test 1: Basic malloc + free */
    {
        int *p = (int *)malloc(sizeof(int));
        check(p != NULL, "T1: malloc(4) returns non-NULL");
        *p = 42;
        check(*p == 42, "T1: write and read back");
        free(p);
    }

    /* Test 2: Multiple allocations — no overlap */
    {
        char *a = (char *)malloc(100);
        char *b = (char *)malloc(100);
        char *c = (char *)malloc(100);
        check(a != NULL && b != NULL && c != NULL, "T2: 3 mallocs succeed");

        /* Verify no overlap: each should be at least 100 bytes apart */
        uint64_t da = (uint64_t)a, db = (uint64_t)b, dc = (uint64_t)c;
        int no_overlap = 1;
        if (da < db) { if (da + 100 > db) no_overlap = 0; }
        else { if (db + 100 > da) no_overlap = 0; }
        if (db < dc) { if (db + 100 > dc) no_overlap = 0; }
        else { if (dc + 100 > db) no_overlap = 0; }
        check(no_overlap, "T2: allocations don't overlap");

        /* Write unique data to each */
        memset(a, 'A', 100);
        memset(b, 'B', 100);
        memset(c, 'C', 100);
        check(a[0] == 'A' && a[99] == 'A', "T2: block A intact");
        check(b[0] == 'B' && b[99] == 'B', "T2: block B intact");
        check(c[0] == 'C' && c[99] == 'C', "T2: block C intact");

        free(a);
        free(b);
        free(c);
    }

    /* Test 3: Free and reuse — freed block should be reused */
    {
        void *p1 = malloc(64);
        uint64_t addr1 = (uint64_t)p1;
        free(p1);
        void *p2 = malloc(64);
        /* p2 should reuse the same block or one nearby */
        check(p2 != NULL, "T3: reallocation after free succeeds");
        check((uint64_t)p2 == addr1, "T3: freed block is reused");
        free(p2);
    }

    /* Test 4: Coalescing — free adjacent blocks, next malloc gets merged space */
    {
        void *a = malloc(64);
        void *b = malloc(64);
        void *c = malloc(64);
        free(a);
        free(b);
        free(c);
        /* After freeing all three, they should coalesce into one large block.
         * A single 256-byte allocation should succeed from the merged space. */
        void *big = malloc(256);
        check(big != NULL, "T4: large alloc after coalescing works");
        free(big);
    }

    /* Test 5: Large allocation > 4KB — triggers multi-page mmap */
    {
        size_t big_size = 8192;
        char *p = (char *)malloc(big_size);
        check(p != NULL, "T5: malloc(8192) succeeds");
        /* Write to first and last bytes */
        p[0] = 'X';
        p[big_size - 1] = 'Y';
        check(p[0] == 'X' && p[big_size - 1] == 'Y', "T5: large block writable");
        free(p);
    }

    /* Test 6: realloc grow — preserves data */
    {
        char *p = (char *)malloc(32);
        check(p != NULL, "T6: initial malloc(32)");
        for (int i = 0; i < 32; i++) p[i] = (char)(i + 1);

        p = (char *)realloc(p, 128);
        check(p != NULL, "T6: realloc(128) succeeds");

        int data_ok = 1;
        for (int i = 0; i < 32; i++) {
            if (p[i] != (char)(i + 1)) { data_ok = 0; break; }
        }
        check(data_ok, "T6: realloc preserves original data");
        free(p);
    }

    /* Test 7: realloc shrink */
    {
        char *p = (char *)malloc(256);
        check(p != NULL, "T7: malloc(256)");
        p[0] = 'A';
        p[63] = 'Z';
        p = (char *)realloc(p, 64);
        check(p != NULL, "T7: realloc(64) shrink succeeds");
        check(p[0] == 'A' && p[63] == 'Z', "T7: data preserved after shrink");
        free(p);
    }

    /* Test 8: calloc — zeroed memory */
    {
        int *arr = (int *)calloc(16, sizeof(int));
        check(arr != NULL, "T8: calloc(16, 4) succeeds");
        int all_zero = 1;
        for (int i = 0; i < 16; i++) {
            if (arr[i] != 0) { all_zero = 0; break; }
        }
        check(all_zero, "T8: calloc memory is zeroed");
        free(arr);
    }

    /* Test 9: Many small allocations — stress test */
    {
        #define N 128
        void *ptrs[N];
        int ok = 1;
        for (int i = 0; i < N; i++) {
            ptrs[i] = malloc(16 + (i % 64));
            if (!ptrs[i]) { ok = 0; break; }
            /* Write a marker */
            *(int *)ptrs[i] = i;
        }
        check(ok, "T9: 128 small mallocs all succeed");

        /* Verify markers */
        int markers_ok = 1;
        for (int i = 0; i < N; i++) {
            if (ptrs[i] && *(int *)ptrs[i] != i) { markers_ok = 0; break; }
        }
        check(markers_ok, "T9: all markers intact");

        for (int i = 0; i < N; i++)
            free(ptrs[i]);
        #undef N
    }

    /* Test 10: Mixed sizes */
    {
        char *s1 = (char *)malloc(1);
        char *s2 = (char *)malloc(7);
        char *s3 = (char *)malloc(100);
        char *s4 = (char *)malloc(1000);
        char *s5 = (char *)malloc(4096);
        check(s1 && s2 && s3 && s4 && s5, "T10: mixed size mallocs succeed");
        *s1 = 'a';
        memset(s2, 'b', 7);
        memset(s3, 'c', 100);
        memset(s4, 'd', 1000);
        memset(s5, 'e', 4096);
        check(*s1 == 'a' && s2[6] == 'b' && s3[99] == 'c' &&
              s4[999] == 'd' && s5[4095] == 'e', "T10: all mixed blocks writable");
        free(s1);
        free(s2);
        free(s3);
        free(s4);
        free(s5);
    }

    /* Test 11: malloc(0) returns NULL */
    {
        void *p = malloc(0);
        check(p == NULL, "T11: malloc(0) returns NULL");
    }

    /* Test 12: free(NULL) doesn't crash */
    {
        free(NULL);
        check(1, "T12: free(NULL) doesn't crash");
    }

    /* Test 13: realloc(NULL, size) acts as malloc */
    {
        int *p = (int *)realloc(NULL, sizeof(int) * 4);
        check(p != NULL, "T13: realloc(NULL, 16) acts as malloc");
        p[0] = 1; p[1] = 2; p[2] = 3; p[3] = 4;
        check(p[3] == 4, "T13: realloc'd block writable");
        free(p);
    }

    printf("\n=== Stage 55 Results: %d/%d passed ===\n",
           passed, passed + failed);

    return (failed > 0) ? 1 : 0;
}
