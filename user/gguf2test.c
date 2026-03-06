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

/* --- Test 1: F16 → F32 conversion --- */
static void test_f16_conversion(void) {
    const char *name = "F16 to F32 conversion";

    /* Build known F16 bit patterns and dequant them */
    uint16_t f16_vals[6];
    float expected[6];

    /* 0.0 = 0x0000 */
    f16_vals[0] = 0x0000; expected[0] = 0.0f;
    /* 1.0 = 0x3C00 */
    f16_vals[1] = 0x3C00; expected[1] = 1.0f;
    /* -1.0 = 0xBC00 */
    f16_vals[2] = 0xBC00; expected[2] = -1.0f;
    /* 0.5 = 0x3800 */
    f16_vals[3] = 0x3800; expected[3] = 0.5f;
    /* 65504.0 (max finite F16) = 0x7BFF */
    f16_vals[4] = 0x7BFF; expected[4] = 65504.0f;
    /* Subnormal: smallest positive F16 = 0x0001 ≈ 5.96e-8 */
    f16_vals[5] = 0x0001; expected[5] = 5.960464477539063e-8f;

    float out[6];
    if (dequant(f16_vals, out, 6, 1 /* GGML_TYPE_F16 */) != 0) {
        fail(name, "dequant returned error");
        return;
    }

    for (int i = 0; i < 6; i++) {
        float diff = out[i] - expected[i];
        if (diff < 0) diff = -diff;
        float threshold = (expected[i] == 0.0f) ? 1e-10f :
                          fabsf(expected[i]) * 0.001f;
        if (diff > threshold) {
            printf("  f16[%d]: got %.6f, expected %.6f\n", i, (double)out[i], (double)expected[i]);
            fail(name, "value mismatch");
            return;
        }
    }

    pass(name);
}

/* --- Test 2: Q8_0 dequantization --- */
static void test_q8_0_dequant(void) {
    const char *name = "Q8_0 dequant";

    /* Build a Q8_0 block: f16 scale (2B) + int8_t quants[32] (32B) = 34B */
    uint8_t block[34];
    /* scale = 0.5 in F16 = 0x3800 */
    block[0] = 0x00; block[1] = 0x38;
    int8_t *quants = (int8_t *)(block + 2);
    for (int i = 0; i < 32; i++)
        quants[i] = (int8_t)i;

    float out[32];
    if (dequant(block, out, 32, 8 /* GGML_TYPE_Q8_0 */) != 0) {
        fail(name, "dequant returned error");
        return;
    }

    for (int i = 0; i < 32; i++) {
        float expected = (float)i * 0.5f;
        float diff = out[i] - expected;
        if (diff < 0) diff = -diff;
        if (diff > 0.01f) {
            printf("  q8_0[%d]: got %.4f, expected %.4f\n", i, (double)out[i], (double)expected);
            fail(name, "value mismatch");
            return;
        }
    }

    pass(name);
}

/* --- Test 3: Q4_0 dequantization --- */
static void test_q4_0_dequant(void) {
    const char *name = "Q4_0 dequant";

    /* Build a Q4_0 block: f16 scale (2B) + uint8_t quants[16] (16B) = 18B */
    uint8_t block[18];
    /* scale = 2.0 in F16 = 0x4000 */
    block[0] = 0x00; block[1] = 0x40;

    /* Pack nibbles: lo = value & 0xF, hi = value >> 4
     * After decoding: lo_val = (lo - 8), hi_val = (hi - 8)
     * Store: quants[i] = (hi_nibble << 4) | lo_nibble */
    for (int i = 0; i < 16; i++) {
        /* lo nibble: i%16, hi nibble: (i+1)%16 to get variety */
        int lo = i & 0xF;       /* raw nibble for even index */
        int hi = (i + 8) & 0xF; /* raw nibble for odd index */
        block[2 + i] = (uint8_t)((hi << 4) | lo);
    }

    float out[32];
    if (dequant(block, out, 32, 2 /* GGML_TYPE_Q4_0 */) != 0) {
        fail(name, "dequant returned error");
        return;
    }

    /* Verify: out[2*i] = (lo-8)*2.0, out[2*i+1] = (hi-8)*2.0 */
    float scale = 2.0f;
    for (int i = 0; i < 16; i++) {
        int lo = i & 0xF;
        int hi = (i + 8) & 0xF;
        float expected_lo = (float)(lo - 8) * scale;
        float expected_hi = (float)(hi - 8) * scale;

        float diff_lo = out[i * 2] - expected_lo;
        float diff_hi = out[i * 2 + 1] - expected_hi;
        if (diff_lo < 0) diff_lo = -diff_lo;
        if (diff_hi < 0) diff_hi = -diff_hi;

        if (diff_lo > 0.01f || diff_hi > 0.01f) {
            printf("  q4_0[%d]: lo=%.2f(exp %.2f), hi=%.2f(exp %.2f)\n",
                   i, (double)out[i*2], (double)expected_lo,
                   (double)out[i*2+1], (double)expected_hi);
            fail(name, "value mismatch");
            return;
        }
    }

    pass(name);
}

/* --- Test 4: Q4_K dequantization --- */
static void test_q4_k_dequant(void) {
    const char *name = "Q4_K dequant";

    /* Build a Q4_K super-block: 144 bytes → 256 floats
     * Layout: f16 d (2B) + f16 dmin (2B) + scales[12] (12B) + quants[128] (128B)
     * Simple test: d=1.0, dmin=0.0, all scales=1, all quants=5 → all values = 1.0 * 1 * 5 - 0 = 5.0 */
    uint8_t block[144];
    memset(block, 0, 144);

    /* d = 1.0 in F16 = 0x3C00 */
    block[0] = 0x00; block[1] = 0x3C;
    /* dmin = 0.0 in F16 = 0x0000 */
    block[2] = 0x00; block[3] = 0x00;

    /* scales[12]: encode 8 sub-blocks with sc=1, mn=0
     * The encoding packs 6-bit values, but for sc=1, mn=0 the packed bytes
     * are simpler. sc[0]=1, mn[0]=0: byte0 = (mn0_hi2 << 6) | sc0_lo6 = 0|1 = 1
     * Actually the encoding is complex. Let's use sc=1, mn=0 across the board.
     * Byte layout for Q4_K scales (matching our dequantizer):
     *   sc[0] = raw[0] & 0x3F
     *   mn[0] = (raw[0] >> 6) | ((raw[1] & 0x0F) << 2)
     *   sc[1] = (raw[1] >> 4) | ((raw[2] & 0x03) << 4)
     *   mn[1] = raw[2] >> 2
     * So for sc[0]=1, mn[0]=0: raw[0]&0x3F=1, raw[0]>>6=0 → raw[0]=1
     * mn[0] continued: (raw[1]&0xF)<<2 = 0 → raw[1]&0xF=0
     * sc[1]=1: (raw[1]>>4)|((raw[2]&3)<<4) = 1 → raw[1]>>4=1 → raw[1]=0x10
     * mn[1]=0: raw[2]>>2=0 → raw[2]=0
     * Pattern repeats: pairs are (1, 0x10, 0) for each pair of sub-blocks */
    for (int pair = 0; pair < 4; pair++) {
        block[4 + pair * 3]     = 1;     /* sc[2*pair] = 1 */
        block[4 + pair * 3 + 1] = 0x10;  /* sc[2*pair+1] = 1, mn bits = 0 */
        block[4 + pair * 3 + 2] = 0;     /* mn = 0 */
    }

    /* quants[128]: all 0x55 = nibbles (5, 5) */
    memset(block + 16, 0x55, 128);

    float out[256];
    if (dequant(block, out, 256, 12 /* GGML_TYPE_Q4_K */) != 0) {
        fail(name, "dequant returned error");
        return;
    }

    /* Verify: with d=1.0, sc=1, mn=0, quant=5: value = 1.0 * 1 * 5 - 0 = 5.0 */
    int ok = 1;
    for (int i = 0; i < 256; i++) {
        if (!is_finite(out[i])) { ok = 0; break; }
        float diff = out[i] - 5.0f;
        if (diff < 0) diff = -diff;
        if (diff > 0.01f) { ok = 0; break; }
    }

    if (!ok) {
        printf("  q4_k: out[0]=%.2f, out[255]=%.2f (expected 5.0)\n",
               (double)out[0], (double)out[255]);
        fail(name, "value mismatch");
        return;
    }

    pass(name);
}

/* --- Test 5: GQA forward pass --- */
static void test_gqa_forward(void) {
    const char *name = "GQA forward (n_heads=4, n_kv_heads=2)";

    tf_config_t cfg = {
        .dim = 64, .hidden_dim = 192, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 1, .swiglu = 1, .n_kv_heads = 2
    };

    transformer_t tf;
    if (transformer_init(&tf, &cfg, 42) != 0) {
        fail(name, "init failed");
        return;
    }

    float *logits = transformer_forward(&tf, 65);

    int all_finite = 1;
    float max_val = logits[0], min_val = logits[0];
    for (uint32_t i = 0; i < cfg.vocab_size; i++) {
        if (!is_finite(logits[i])) { all_finite = 0; break; }
        if (logits[i] > max_val) max_val = logits[i];
        if (logits[i] < min_val) min_val = logits[i];
    }

    transformer_destroy(&tf);

    if (!all_finite) {
        fail(name, "non-finite logits");
        return;
    }
    if (max_val - min_val < 1e-6f) {
        fail(name, "degenerate logits");
        return;
    }

    pass(name);
}

/* --- Test 6: QK-norm forward pass --- */
static void test_qknorm_forward(void) {
    const char *name = "QK-norm forward";

    tf_config_t cfg = {
        .dim = 64, .hidden_dim = 192, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 1, .swiglu = 1, .n_kv_heads = 2, .qk_norm = 1
    };

    transformer_t tf;
    if (transformer_init(&tf, &cfg, 42) != 0) {
        fail(name, "init failed");
        return;
    }

    /* Verify wq_norm/wk_norm are allocated */
    if (!tf.wq_norm || !tf.wk_norm) {
        fail(name, "norm weights not allocated");
        transformer_destroy(&tf);
        return;
    }

    float *logits = transformer_forward(&tf, 65);

    int all_finite = 1;
    for (uint32_t i = 0; i < cfg.vocab_size; i++) {
        if (!is_finite(logits[i])) { all_finite = 0; break; }
    }

    transformer_destroy(&tf);

    if (!all_finite) {
        fail(name, "non-finite logits");
        return;
    }

    pass(name);
}

/* --- Test 7: RoPE theta sensitivity --- */
static void test_rope_theta(void) {
    const char *name = "RoPE theta sensitivity";

    tf_config_t cfg1 = {
        .dim = 64, .hidden_dim = 192, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 1, .swiglu = 0, .rope_theta = 10000.0f
    };

    transformer_t tf1;
    if (transformer_init(&tf1, &cfg1, 42) != 0) {
        fail(name, "tf1 init failed");
        return;
    }
    transformer_forward(&tf1, 65);  /* pos 0 */
    float *logits1 = transformer_forward(&tf1, 66);  /* pos 1 */
    float l1_0 = logits1[0], l1_1 = logits1[1];
    transformer_destroy(&tf1);

    tf_config_t cfg2 = {
        .dim = 64, .hidden_dim = 192, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 1, .swiglu = 0, .rope_theta = 1000000.0f
    };

    transformer_t tf2;
    if (transformer_init(&tf2, &cfg2, 42) != 0) {
        fail(name, "tf2 init failed");
        return;
    }
    transformer_forward(&tf2, 65);
    float *logits2 = transformer_forward(&tf2, 66);
    float l2_0 = logits2[0], l2_1 = logits2[1];
    transformer_destroy(&tf2);

    float diff0 = l1_0 - l2_0; if (diff0 < 0) diff0 = -diff0;
    float diff1 = l1_1 - l2_1; if (diff1 < 0) diff1 = -diff1;

    if (diff0 < 1e-6f && diff1 < 1e-6f) {
        fail(name, "different rope_theta had no effect");
        return;
    }

    pass(name);
}

/* --- Test 8: GGUF GQA load --- */
static transformer_t gqa_tf;
static tf_config_t gqa_cfg;
static bpe_tokenizer_t gqa_bpe;
static int gqa_loaded = 0;

static void test_gguf_gqa_load(void) {
    const char *name = "GGUF GQA load";

    int ret = gguf_load("/test_gqa.gguf", &gqa_tf, &gqa_cfg, &gqa_bpe);
    if (ret != 0) {
        fail(name, "gguf_load failed");
        return;
    }

    if (gqa_cfg.dim != 64) {
        printf("  dim=%u expected 64\n", gqa_cfg.dim);
        fail(name, "dim mismatch"); return;
    }
    if (gqa_cfg.hidden_dim != 192) {
        printf("  hidden_dim=%u expected 192\n", gqa_cfg.hidden_dim);
        fail(name, "hidden_dim mismatch"); return;
    }
    if (gqa_cfg.n_heads != 4) {
        printf("  n_heads=%u expected 4\n", gqa_cfg.n_heads);
        fail(name, "n_heads mismatch"); return;
    }
    if (gqa_cfg.n_kv_heads != 2) {
        printf("  n_kv_heads=%u expected 2\n", gqa_cfg.n_kv_heads);
        fail(name, "n_kv_heads mismatch"); return;
    }
    if (gqa_cfg.qk_norm != 1) {
        printf("  qk_norm=%u expected 1\n", gqa_cfg.qk_norm);
        fail(name, "qk_norm mismatch"); return;
    }
    /* Check rope_theta approximately */
    float theta_diff = gqa_cfg.rope_theta - 1000000.0f;
    if (theta_diff < 0) theta_diff = -theta_diff;
    if (theta_diff > 1.0f) {
        printf("  rope_theta=%.1f expected 1000000.0\n", (double)gqa_cfg.rope_theta);
        fail(name, "rope_theta mismatch"); return;
    }

    gqa_loaded = 1;
    pass(name);
}

/* --- Test 9: GGUF GQA forward pass --- */
static void test_gguf_gqa_forward(void) {
    const char *name = "GGUF GQA forward";

    if (!gqa_loaded) {
        fail(name, "skipped (load failed)");
        return;
    }

    gqa_tf.pos = 0;
    float *logits = transformer_forward(&gqa_tf, 65);

    int all_finite = 1;
    float max_val = logits[0], min_val = logits[0];
    for (uint32_t i = 0; i < gqa_cfg.vocab_size; i++) {
        if (!is_finite(logits[i])) { all_finite = 0; break; }
        if (logits[i] > max_val) max_val = logits[i];
        if (logits[i] < min_val) min_val = logits[i];
    }

    if (!all_finite) {
        fail(name, "non-finite logits");
        return;
    }
    if (max_val - min_val < 1e-6f) {
        fail(name, "degenerate logits");
        return;
    }

    pass(name);
}

/* --- Test 10: Save/load v2 round-trip --- */
static void test_save_load_v2(void) {
    const char *name = "save/load v2 round-trip";

    tf_config_t cfg = {
        .dim = 48, .hidden_dim = 128, .n_heads = 4,
        .n_layers = 2, .vocab_size = 128, .max_seq_len = 64,
        .rope = 1, .swiglu = 1,
        .n_kv_heads = 2, .qk_norm = 1, .rope_theta = 500000.0f
    };

    transformer_t tf;
    if (transformer_init(&tf, &cfg, 42) != 0) {
        fail(name, "init failed");
        return;
    }

    /* Forward to get reference logit */
    tf.pos = 0;
    float *logits = transformer_forward(&tf, 65);
    float ref_logit0 = logits[0];

    /* Save */
    if (transformer_save(&tf, "/gqa_test_model.bin") != 0) {
        fail(name, "save failed");
        transformer_destroy(&tf);
        return;
    }
    transformer_destroy(&tf);

    /* Load back */
    transformer_t loaded;
    tf_config_t lcfg;
    if (transformer_load(&loaded, &lcfg, "/gqa_test_model.bin") != 0) {
        fail(name, "load failed");
        return;
    }

    /* Verify new config fields preserved */
    if (lcfg.n_kv_heads != 2) {
        printf("  n_kv_heads=%u expected 2\n", lcfg.n_kv_heads);
        fail(name, "n_kv_heads not preserved");
        transformer_destroy(&loaded);
        sys_unlink("/gqa_test_model.bin");
        return;
    }
    if (lcfg.qk_norm != 1) {
        printf("  qk_norm=%u expected 1\n", lcfg.qk_norm);
        fail(name, "qk_norm not preserved");
        transformer_destroy(&loaded);
        sys_unlink("/gqa_test_model.bin");
        return;
    }
    float tdiff = lcfg.rope_theta - 500000.0f;
    if (tdiff < 0) tdiff = -tdiff;
    if (tdiff > 1.0f) {
        printf("  rope_theta=%.1f expected 500000.0\n", (double)lcfg.rope_theta);
        fail(name, "rope_theta not preserved");
        transformer_destroy(&loaded);
        sys_unlink("/gqa_test_model.bin");
        return;
    }

    /* Forward pass and compare logit[0] */
    loaded.pos = 0;
    float *logits2 = transformer_forward(&loaded, 65);
    float diff = logits2[0] - ref_logit0;
    if (diff < 0) diff = -diff;

    transformer_destroy(&loaded);
    sys_unlink("/gqa_test_model.bin");

    if (diff > 0.001f) {
        printf("  logit diff = %.4f\n", (double)diff);
        fail(name, "logit mismatch after load");
        return;
    }

    pass(name);
}

int main(void) {
    printf("=== gguf2test: Full GGUF Support ===\n");

    test_f16_conversion();
    test_q8_0_dequant();
    test_q4_0_dequant();
    test_q4_k_dequant();
    test_gqa_forward();
    test_qknorm_forward();
    test_rope_theta();
    test_gguf_gqa_load();
    test_gguf_gqa_forward();
    test_save_load_v2();

    /* Cleanup GQA resources */
    if (gqa_loaded) {
        transformer_destroy(&gqa_tf);
        bpe_destroy(&gqa_bpe);
    }

    printf("=== gguf2test: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("=== gguf2test: ALL PASSED ===\n");

    return tests_failed;
}
