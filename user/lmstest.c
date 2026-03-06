#include "libc/libc.h"

static int tests_passed = 0;
static int tests_failed = 0;

static void pass(const char *name) {
    printf("  [PASS] %s\n", name);
    tests_passed++;
}

static void fail(const char *name, const char *reason) {
    printf("  [FAIL] %s: %s\n", name, reason);
    tests_failed++;
}

/* Check if a float is finite (not NaN, not Inf) */
static int is_finite(float x) {
    /* NaN: x != x; Inf: x - x != 0 for non-zero x */
    if (x != x) return 0;
    float diff = x - x;
    if (diff != diff) return 0;
    return 1;
}

/* --- Test 1: Large mmap (128 MB) --- */
static void test_large_mmap(void) {
    const char *name = "large mmap (128MB)";
    uint32_t pages = 128 * 1024 * 1024 / 4096;  /* 32768 pages */
    long addr = sys_mmap(pages);
    if (addr <= 0) {
        fail(name, "sys_mmap returned <= 0");
        return;
    }

    /* Write to first and last pages */
    volatile uint32_t *first = (volatile uint32_t *)addr;
    volatile uint32_t *last = (volatile uint32_t *)(addr + (uint64_t)(pages - 1) * 4096);
    *first = 0xDEADBEEF;
    *last = 0xCAFEBABE;

    if (*first != 0xDEADBEEF || *last != 0xCAFEBABE) {
        fail(name, "readback mismatch");
        sys_munmap((uint64_t)addr);
        return;
    }

    sys_munmap((uint64_t)addr);
    pass(name);
}

/* --- Test 2: Multiple mmap (8 x 16 MB) --- */
static void test_multiple_mmap(void) {
    const char *name = "multiple mmap (8x16MB)";
    uint32_t pages = 16 * 1024 * 1024 / 4096;  /* 4096 pages */
    long addrs[8];

    for (int i = 0; i < 8; i++) {
        addrs[i] = sys_mmap(pages);
        if (addrs[i] <= 0) {
            printf("  alloc %d failed\n", i);
            fail(name, "sys_mmap failed");
            /* Cleanup */
            for (int j = 0; j < i; j++)
                sys_munmap((uint64_t)addrs[j]);
            return;
        }
        /* Write pattern */
        volatile uint32_t *p = (volatile uint32_t *)addrs[i];
        *p = (uint32_t)(0xAA000000 | i);
    }

    /* Verify all 8 */
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        volatile uint32_t *p = (volatile uint32_t *)addrs[i];
        if (*p != (uint32_t)(0xAA000000 | i)) {
            ok = 0;
            break;
        }
    }

    for (int i = 0; i < 8; i++)
        sys_munmap((uint64_t)addrs[i]);

    if (ok) pass(name);
    else fail(name, "pattern mismatch");
}

/* --- Test 3: Large model init (12 layers, ~33 MB) --- */
static transformer_t large_tf;
static int large_tf_ok = 0;

static void test_large_model_init(void) {
    const char *name = "large model init (12 layers)";
    tf_config_t cfg = {
        .dim = 256,
        .hidden_dim = 768,
        .n_heads = 8,
        .n_layers = 12,
        .vocab_size = 1024,
        .max_seq_len = 256
    };

    if (transformer_init(&large_tf, &cfg, 42) != 0) {
        fail(name, "transformer_init failed");
        return;
    }

    /* Verify config was stored */
    if (large_tf.cfg.n_layers != 12 || large_tf.cfg.dim != 256) {
        fail(name, "config mismatch");
        transformer_destroy(&large_tf);
        return;
    }

    large_tf_ok = 1;
    pass(name);
}

/* --- Test 4: Large model forward pass --- */
static float saved_logit0 = 0.0f;

static void test_large_model_forward(void) {
    const char *name = "large model forward";
    if (!large_tf_ok) {
        fail(name, "skipped (init failed)");
        return;
    }

    large_tf.pos = 0;
    float *logits = transformer_forward(&large_tf, 1);

    /* Check logits are finite */
    int ok = 1;
    for (uint32_t i = 0; i < large_tf.cfg.vocab_size; i++) {
        if (!is_finite(logits[i])) {
            ok = 0;
            break;
        }
    }

    if (!ok) {
        fail(name, "non-finite logits");
        return;
    }

    saved_logit0 = logits[0];
    pass(name);
}

/* --- Test 5: Large model save --- */
static void test_large_model_save(void) {
    const char *name = "large model save";
    if (!large_tf_ok) {
        fail(name, "skipped (init failed)");
        return;
    }

    if (transformer_save(&large_tf, "/lms_model.bin") != 0) {
        fail(name, "transformer_save failed");
        return;
    }

    /* Verify file size */
    long fd = sys_open("/lms_model.bin", O_RDONLY);
    if (fd < 0) {
        fail(name, "cannot open saved file");
        return;
    }

    /* Use fstat to check size */
    struct {
        uint64_t size;
        uint8_t  type;
        uint8_t  pad[7];
    } st;
    if (sys_fstat(fd, &st) != 0) {
        fail(name, "fstat failed");
        sys_close(fd);
        return;
    }
    sys_close(fd);

    /* Expected: 52 bytes header (Limnx v2 format) + weight_bytes */
    uint64_t dim = 256, hdim = 768, nl = 12, vs = 1024;
    uint64_t wcount = vs * dim + nl * dim + nl * dim * dim * 4
        + nl * dim + nl * dim * hdim + nl * hdim * dim + dim + dim * vs;
    uint64_t expected = 52 + wcount * 4;

    if (st.size != expected) {
        printf("  expected %lu, got %lu\n", expected, st.size);
        fail(name, "file size mismatch");
        return;
    }

    pass(name);
}

/* --- Test 6: Large model load + verify --- */
static void test_large_model_load(void) {
    const char *name = "large model load + verify";
    if (!large_tf_ok) {
        fail(name, "skipped (init failed)");
        return;
    }

    /* Destroy original */
    transformer_destroy(&large_tf);
    large_tf_ok = 0;

    /* Load from file */
    transformer_t loaded;
    tf_config_t lcfg;
    if (transformer_load(&loaded, &lcfg, "/lms_model.bin") != 0) {
        fail(name, "transformer_load failed");
        return;
    }

    /* Run forward pass and compare logit[0] */
    loaded.pos = 0;
    float *logits = transformer_forward(&loaded, 1);

    float diff = logits[0] - saved_logit0;
    if (diff < 0) diff = -diff;

    transformer_destroy(&loaded);

    /* Clean up the saved file */
    sys_unlink("/lms_model.bin");

    if (diff > 0.001f) {
        printf("  logit diff = %.4f\n", diff);
        fail(name, "logit mismatch after load");
        return;
    }

    pass(name);
}

/* --- Test 7: Huge model init (24 layers, ~200 MB) --- */
static void test_huge_model_init(void) {
    const char *name = "huge model init (24 layers, ~200MB)";
    tf_config_t cfg = {
        .dim = 512,
        .hidden_dim = 1536,
        .n_heads = 16,
        .n_layers = 24,
        .vocab_size = 2048,
        .max_seq_len = 512
    };

    transformer_t huge;
    if (transformer_init(&huge, &cfg, 99) != 0) {
        fail(name, "transformer_init failed");
        return;
    }

    /* Verify basic structure */
    if (huge.cfg.n_layers != 24 || huge.cfg.dim != 512) {
        fail(name, "config mismatch");
        transformer_destroy(&huge);
        return;
    }

    /* Spot-check: write and read from a weight */
    huge.wq[23][0] = 3.14f;
    if (huge.wq[23][0] != 3.14f) {
        fail(name, "weight readback failed");
        transformer_destroy(&huge);
        return;
    }

    transformer_destroy(&huge);
    pass(name);
}

/* --- Test 8: Memory cleanup verification --- */
static void test_memory_cleanup(void) {
    const char *name = "memory cleanup";

    /* Allocate and destroy a model, then allocate memory to verify no leak/crash */
    tf_config_t cfg = {
        .dim = 64, .hidden_dim = 192, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 32
    };
    transformer_t tf;
    if (transformer_init(&tf, &cfg, 1) != 0) {
        fail(name, "init failed");
        return;
    }
    transformer_destroy(&tf);

    /* Verify we can still allocate memory after cleanup */
    long addr = sys_mmap(1);
    if (addr <= 0) {
        fail(name, "post-cleanup mmap failed");
        return;
    }
    sys_munmap((uint64_t)addr);

    pass(name);
}

int main(void) {
    printf("=== lmstest: Large Model Support ===\n");

    test_large_mmap();
    test_multiple_mmap();
    test_large_model_init();
    test_large_model_forward();
    test_large_model_save();
    test_large_model_load();
    test_huge_model_init();
    test_memory_cleanup();

    printf("=== lmstest: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("=== lmstest: ALL PASSED ===\n");

    return tests_failed;
}
