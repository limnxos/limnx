#include "libc/libc.h"

/* Simple float comparison: |a - b| < epsilon */
static int approx(float a, float b, float eps) {
    float diff = a - b;
    if (diff < 0.0f) diff = -diff;
    return diff < eps;
}

int main(void) {
    printf("=== mathtest: float math + mmap test ===\n");

    int pass = 1;

    /* Test sqrtf(2.0) ≈ 1.4142 */
    float s = sqrtf(2.0f);
    printf("sqrtf(2.0) = %f\n", (double)s);
    if (!approx(s, 1.4142f, 0.001f)) {
        printf("  FAIL: expected ~1.4142\n");
        pass = 0;
    }

    /* Test expf(1.0) ≈ 2.7183 */
    float e = expf(1.0f);
    printf("expf(1.0) = %f\n", (double)e);
    if (!approx(e, 2.7183f, 0.01f)) {
        printf("  FAIL: expected ~2.7183\n");
        pass = 0;
    }

    /* Test logf(e) ≈ 1.0 */
    float l = logf(2.7182818f);
    printf("logf(e) = %f\n", (double)l);
    if (!approx(l, 1.0f, 0.01f)) {
        printf("  FAIL: expected ~1.0\n");
        pass = 0;
    }

    /* Test tanhf(0) ≈ 0 */
    float t0 = tanhf(0.0f);
    printf("tanhf(0.0) = %f\n", (double)t0);
    if (!approx(t0, 0.0f, 0.001f)) {
        printf("  FAIL: expected ~0.0\n");
        pass = 0;
    }

    /* Test tanhf(10) ≈ 1.0 */
    float t10 = tanhf(10.0f);
    printf("tanhf(10.0) = %f\n", (double)t10);
    if (!approx(t10, 1.0f, 0.001f)) {
        printf("  FAIL: expected ~1.0\n");
        pass = 0;
    }

    /* Test fabsf */
    float ab = fabsf(-3.14f);
    printf("fabsf(-3.14) = %f\n", (double)ab);
    if (!approx(ab, 3.14f, 0.001f)) {
        printf("  FAIL: expected ~3.14\n");
        pass = 0;
    }

    /* Test floorf, ceilf */
    float fl = floorf(2.7f);
    float cl = ceilf(2.3f);
    printf("floorf(2.7) = %f, ceilf(2.3) = %f\n", (double)fl, (double)cl);
    if (!approx(fl, 2.0f, 0.001f) || !approx(cl, 3.0f, 0.001f)) {
        printf("  FAIL: floor/ceil\n");
        pass = 0;
    }

    /* Test SYS_MMAP: allocate 256 pages (1 MB) */
    printf("\n--- mmap test ---\n");
    long addr = sys_mmap(256);
    if (addr <= 0) {
        printf("FAIL: sys_mmap returned %ld\n", addr);
        pass = 0;
    } else {
        printf("sys_mmap(256) = 0x%lx\n", (unsigned long)addr);

        /* Write pattern */
        unsigned char *p = (unsigned char *)addr;
        for (int i = 0; i < 4096; i++)
            p[i] = (unsigned char)(i & 0xFF);

        /* Verify pattern */
        int ok = 1;
        for (int i = 0; i < 4096; i++) {
            if (p[i] != (unsigned char)(i & 0xFF)) {
                ok = 0;
                break;
            }
        }

        if (ok) {
            printf("mmap write/verify OK\n");
        } else {
            printf("FAIL: mmap pattern mismatch\n");
            pass = 0;
        }

        /* Unmap */
        long ret = sys_munmap((unsigned long)addr);
        if (ret != 0) {
            printf("FAIL: sys_munmap returned %ld\n", ret);
            pass = 0;
        } else {
            printf("sys_munmap OK\n");
        }
    }

    /* Summary */
    printf("\n=== mathtest: %s ===\n", pass ? "ALL PASSED" : "SOME FAILED");

    return pass ? 0 : 1;
}
