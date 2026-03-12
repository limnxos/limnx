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

static int is_finite(float x) {
    if (x != x) return 0;
    float diff = x - x;
    if (diff != diff) return 0;
    return 1;
}

/* --- Test 1: cosf/sinf accuracy --- */
static void test_trig_accuracy(void) {
    const char *name = "cosf/sinf accuracy";

    /* sin^2(x) + cos^2(x) = 1 for various angles */
    float angles[] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 3.14159f, -1.0f, -2.5f, 6.0f };
    int n_angles = 10;

    for (int i = 0; i < n_angles; i++) {
        float s = sinf(angles[i]);
        float c = cosf(angles[i]);
        float sum = s * s + c * c;
        float err = sum - 1.0f;
        if (err < 0) err = -err;
        if (err > 0.001f) {
            printf("  sin^2(%f)+cos^2(%f)=%f, err=%f\n",
                   (double)angles[i], (double)angles[i], (double)sum, (double)err);
            fail(name, "sin^2+cos^2 != 1");
            return;
        }
    }

    /* Known values */
    float s0 = sinf(0.0f);
    if (s0 < -0.001f || s0 > 0.001f) {
        fail(name, "sin(0) != 0");
        return;
    }

    float c0 = cosf(0.0f);
    if (c0 < 0.999f || c0 > 1.001f) {
        fail(name, "cos(0) != 1");
        return;
    }

    pass(name);
}

/* --- Test 2: SwiGLU forward pass --- */
static void test_swiglu_forward(void) {
    const char *name = "SwiGLU forward";

    tf_config_t cfg = {
        .dim = 48, .hidden_dim = 128, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 0, .swiglu = 1
    };

    transformer_t tf;
    if (transformer_init(&tf, &cfg, 42) != 0) {
        fail(name, "transformer_init failed");
        return;
    }

    /* Verify w3 and hb2 are allocated */
    if (!tf.w3 || !tf.hb2) {
        fail(name, "w3 or hb2 is NULL");
        transformer_destroy(&tf);
        return;
    }

    float *logits = transformer_forward(&tf, 65);

    int all_finite = 1;
    for (uint32_t i = 0; i < cfg.vocab_size; i++) {
        if (!is_finite(logits[i])) {
            all_finite = 0;
            break;
        }
    }

    transformer_destroy(&tf);

    if (!all_finite) {
        fail(name, "non-finite logits");
        return;
    }

    pass(name);
}

/* --- Test 3: RoPE position sensitivity --- */
static void test_rope_position(void) {
    const char *name = "RoPE position sensitivity";

    tf_config_t cfg_rope = {
        .dim = 48, .hidden_dim = 128, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 1, .swiglu = 0
    };

    /* Model WITH RoPE: forward token A at pos 0, then token B at pos 1 */
    transformer_t tf1;
    if (transformer_init(&tf1, &cfg_rope, 42) != 0) {
        fail(name, "tf1 init failed");
        return;
    }
    transformer_forward(&tf1, 65);  /* pos 0: token 'A' */
    float *logits1 = transformer_forward(&tf1, 66);  /* pos 1: token 'B' */
    float rope_logit0 = logits1[0];
    float rope_logit1 = logits1[1];
    transformer_destroy(&tf1);

    /* Model WITHOUT RoPE: same tokens, same seed */
    tf_config_t cfg_norope = {
        .dim = 48, .hidden_dim = 128, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 0, .swiglu = 0
    };

    transformer_t tf2;
    if (transformer_init(&tf2, &cfg_norope, 42) != 0) {
        fail(name, "tf2 init failed");
        return;
    }
    transformer_forward(&tf2, 65);  /* pos 0: token 'A' */
    float *logits2 = transformer_forward(&tf2, 66);  /* pos 1: token 'B' */
    float norope_logit0 = logits2[0];
    float norope_logit1 = logits2[1];
    transformer_destroy(&tf2);

    /* Logits should differ between RoPE and no-RoPE models */
    float diff0 = rope_logit0 - norope_logit0;
    float diff1 = rope_logit1 - norope_logit1;
    if (diff0 < 0) diff0 = -diff0;
    if (diff1 < 0) diff1 = -diff1;

    if (diff0 < 1e-6f && diff1 < 1e-6f) {
        fail(name, "RoPE had no effect on logits");
        return;
    }

    pass(name);
}

/* --- Test 4: BPE round-trip --- */
static void test_bpe_roundtrip(void) {
    const char *name = "BPE round-trip";

    /* Create a simple BPE with 259 vocab (256 bytes + 3 merged) and 3 merges */
    bpe_tokenizer_t bpe;
    if (bpe_init(&bpe, 259, 3) != 0) {
        fail(name, "bpe_init failed");
        return;
    }

    /* Set byte vocab (0-255) */
    for (uint32_t i = 0; i < 256; i++) {
        char ch = (char)i;
        bpe_set_vocab(&bpe, i, &ch, 1);
    }

    /* Set merged tokens: "He" = 256, "ll" = 257, "lo" = 258 */
    bpe_set_vocab(&bpe, 256, "He", 2);
    bpe_set_vocab(&bpe, 257, "ll", 2);
    bpe_set_vocab(&bpe, 258, "lo", 2);

    /* Merges: H+e→256, l+l→257, l+o→258 */
    bpe_set_merge(&bpe, 0, 'H', 'e', 256);
    bpe_set_merge(&bpe, 1, 'l', 'l', 257);
    bpe_set_merge(&bpe, 2, 'l', 'o', 258);

    /* Encode "Hello" */
    uint32_t tokens[16];
    uint32_t n = bpe_encode(&bpe, "Hello", 5, tokens, 16);

    /* Expected: "He" "ll" "o" → 256, 257, 'o'(111)
     * But merges are applied in order:
     * Start: H(72) e(101) l(108) l(108) o(111)
     * Merge 0 (H+e→256): 256 l(108) l(108) o(111)
     * Merge 1 (l+l→257): 256 257 o(111)
     * Merge 2 (l+o→258): no match (257 is not 'l')
     * Result: 256, 257, 111 */
    if (n != 3) {
        printf("  encoded %u tokens, expected 3\n", n);
        fail(name, "wrong token count");
        bpe_destroy(&bpe);
        return;
    }
    if (tokens[0] != 256 || tokens[1] != 257 || tokens[2] != 111) {
        printf("  tokens: %u %u %u\n", tokens[0], tokens[1], tokens[2]);
        fail(name, "wrong token IDs");
        bpe_destroy(&bpe);
        return;
    }

    /* Decode back */
    char decoded[64];
    uint32_t dlen = bpe_decode(&bpe, tokens, n, decoded, 64);
    if (dlen != 5 || strcmp(decoded, "Hello") != 0) {
        printf("  decoded: \"%s\" (len %u)\n", decoded, dlen);
        fail(name, "decode mismatch");
        bpe_destroy(&bpe);
        return;
    }

    bpe_destroy(&bpe);
    pass(name);
}

/* --- Test 5: GGUF load --- */
static transformer_t gguf_tf;
static tf_config_t gguf_cfg;
static bpe_tokenizer_t gguf_bpe;
static int gguf_loaded = 0;

static void test_gguf_load(void) {
    const char *name = "GGUF load";

    int ret = gguf_load("/test.gguf", &gguf_tf, &gguf_cfg, &gguf_bpe);
    if (ret != 0) {
        fail(name, "gguf_load failed");
        return;
    }

    /* Verify config matches expected */
    if (gguf_cfg.dim != 64) {
        printf("  dim=%u, expected 64\n", gguf_cfg.dim);
        fail(name, "dim mismatch");
        return;
    }
    if (gguf_cfg.hidden_dim != 192) {
        printf("  hidden_dim=%u, expected 192\n", gguf_cfg.hidden_dim);
        fail(name, "hidden_dim mismatch");
        return;
    }
    if (gguf_cfg.n_heads != 4) {
        printf("  n_heads=%u, expected 4\n", gguf_cfg.n_heads);
        fail(name, "n_heads mismatch");
        return;
    }
    if (gguf_cfg.n_layers != 2) {
        printf("  n_layers=%u, expected 2\n", gguf_cfg.n_layers);
        fail(name, "n_layers mismatch");
        return;
    }
    if (gguf_cfg.vocab_size != 320) {
        printf("  vocab_size=%u, expected 320\n", gguf_cfg.vocab_size);
        fail(name, "vocab_size mismatch");
        return;
    }
    if (gguf_cfg.max_seq_len != 128) {
        printf("  max_seq_len=%u, expected 128\n", gguf_cfg.max_seq_len);
        fail(name, "max_seq_len mismatch");
        return;
    }
    if (gguf_cfg.rope != 1 || gguf_cfg.swiglu != 1) {
        fail(name, "rope/swiglu not set");
        return;
    }

    gguf_loaded = 1;
    pass(name);
}

/* --- Test 6: GGUF forward pass --- */
static void test_gguf_forward(void) {
    const char *name = "GGUF forward";

    if (!gguf_loaded) {
        fail(name, "skipped (load failed)");
        return;
    }

    gguf_tf.pos = 0;
    float *logits = transformer_forward(&gguf_tf, 65);

    int all_finite = 1;
    for (uint32_t i = 0; i < gguf_cfg.vocab_size; i++) {
        if (!is_finite(logits[i])) {
            all_finite = 0;
            break;
        }
    }

    if (!all_finite) {
        fail(name, "non-finite logits");
        return;
    }

    /* Check logits are non-degenerate */
    float max_val = logits[0], min_val = logits[0];
    for (uint32_t i = 1; i < gguf_cfg.vocab_size; i++) {
        if (logits[i] > max_val) max_val = logits[i];
        if (logits[i] < min_val) min_val = logits[i];
    }
    if (max_val - min_val < 1e-6f) {
        fail(name, "degenerate logits");
        return;
    }

    pass(name);
}

/* --- Test 7: New format save/load round-trip --- */
static void test_new_format_roundtrip(void) {
    const char *name = "new format save/load";

    tf_config_t cfg = {
        .dim = 48, .hidden_dim = 128, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 1, .swiglu = 1
    };

    transformer_t tf;
    if (transformer_init(&tf, &cfg, 42) != 0) {
        fail(name, "init failed");
        return;
    }

    /* Forward pass to get reference logit */
    tf.pos = 0;
    float *logits = transformer_forward(&tf, 65);
    float ref_logit0 = logits[0];

    /* Save */
    if (transformer_save(&tf, "/gguf_test_model.bin") != 0) {
        fail(name, "save failed");
        transformer_destroy(&tf);
        return;
    }
    transformer_destroy(&tf);

    /* Load back */
    transformer_t loaded;
    tf_config_t lcfg;
    if (transformer_load(&loaded, &lcfg, "/gguf_test_model.bin") != 0) {
        fail(name, "load failed");
        return;
    }

    /* Verify config */
    if (lcfg.rope != 1 || lcfg.swiglu != 1) {
        fail(name, "rope/swiglu not preserved");
        transformer_destroy(&loaded);
        sys_unlink("/gguf_test_model.bin");
        return;
    }

    /* Forward pass and compare */
    loaded.pos = 0;
    float *logits2 = transformer_forward(&loaded, 65);
    float diff = logits2[0] - ref_logit0;
    if (diff < 0) diff = -diff;

    transformer_destroy(&loaded);
    sys_unlink("/gguf_test_model.bin");

    if (diff > 0.001f) {
        printf("  logit diff = %.4f\n", (double)diff);
        fail(name, "logit mismatch after load");
        return;
    }

    pass(name);
}

int main(void) {
    printf("=== gguftest: Modern Transformer Architecture ===\n");

    test_trig_accuracy();
    test_swiglu_forward();
    test_rope_position();
    test_bpe_roundtrip();
    test_gguf_load();
    test_gguf_forward();
    test_new_format_roundtrip();

    /* Cleanup GGUF resources */
    if (gguf_loaded) {
        transformer_destroy(&gguf_tf);
        bpe_destroy(&gguf_bpe);
    }

    printf("=== gguftest: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("=== gguftest: ALL PASSED ===\n");

    return tests_failed;
}
