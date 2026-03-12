#include "libc/libc.h"

static tf_config_t test_cfg = {
    .dim        = 48,
    .hidden_dim = 128,
    .n_heads    = 4,
    .n_layers   = 2,
    .vocab_size = 128,
    .max_seq_len = 64,
};

/* --- Part 1: Forward pass test --- */

static int test_forward_pass(void) {
    printf("--- forward pass test ---\n");

    transformer_t tf;
    if (transformer_init(&tf, &test_cfg, 42) != 0) {
        printf("FAIL: transformer_init\n");
        return 0;
    }
    printf("transformer_init OK (dim=%u, layers=%u, vocab=%u)\n",
           tf.cfg.dim, tf.cfg.n_layers, tf.cfg.vocab_size);

    /* Forward token 'A' (65) */
    float *logits = transformer_forward(&tf, 65);
    if (!logits) {
        printf("FAIL: transformer_forward returned NULL\n");
        transformer_destroy(&tf);
        return 0;
    }

    /* Find argmax and range */
    uint32_t argmax_idx = 0;
    float max_val = logits[0];
    float min_val = logits[0];
    for (uint32_t i = 1; i < tf.cfg.vocab_size; i++) {
        if (logits[i] > max_val) {
            max_val = logits[i];
            argmax_idx = i;
        }
        if (logits[i] < min_val)
            min_val = logits[i];
    }

    float range = max_val - min_val;
    printf("logits OK: argmax=%u, max=%f, range=%f\n",
           argmax_idx, (double)max_val, (double)range);

    /* Verify logits are non-degenerate (range > 0) */
    if (range < 1e-6f) {
        printf("FAIL: logits degenerate (range too small)\n");
        transformer_destroy(&tf);
        return 0;
    }

    /* Verify pos was incremented */
    if (tf.pos != 1) {
        printf("FAIL: pos=%u, expected 1\n", tf.pos);
        transformer_destroy(&tf);
        return 0;
    }

    printf("forward pass OK\n");
    transformer_destroy(&tf);
    return 1;
}

/* --- Part 2: Generation test --- */

static int test_generation(void) {
    printf("--- generation test ---\n");

    transformer_t tf;
    if (transformer_init(&tf, &test_cfg, 42) != 0) {
        printf("FAIL: transformer_init\n");
        return 0;
    }

    uint32_t tokens[33];
    uint32_t count = transformer_generate(&tf, 'A', tokens, 32);

    if (count != 32) {
        printf("FAIL: generated %u tokens, expected 32\n", count);
        transformer_destroy(&tf);
        return 0;
    }

    /* Print generated tokens as characters */
    printf("generated %u tokens: ", count);
    int valid = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (tokens[i] >= tf.cfg.vocab_size) {
            valid = 0;
            break;
        }
        /* Print printable ASCII, '.' for others */
        char c = (tokens[i] >= 32 && tokens[i] < 127) ? (char)tokens[i] : '.';
        printf("%c", c);
    }
    printf("\n");

    if (!valid) {
        printf("FAIL: token out of range [0, %u)\n", tf.cfg.vocab_size);
        transformer_destroy(&tf);
        return 0;
    }

    printf("generation OK\n");
    transformer_destroy(&tf);
    return 1;
}

/* --- Part 3: KV cache test --- */

static int test_kv_cache(void) {
    printf("--- KV cache test ---\n");

    transformer_t tf;
    if (transformer_init(&tf, &test_cfg, 42) != 0) {
        printf("FAIL: transformer_init\n");
        return 0;
    }

    /* Generate 8 tokens to fill KV cache */
    uint32_t tokens[9];
    transformer_generate(&tf, 'H', tokens, 8);

    /* 8 tokens = 1 start + 7 forward calls = pos 7 */
    if (tf.pos != 7) {
        printf("FAIL: pos=%u, expected 7\n", tf.pos);
        transformer_destroy(&tf);
        return 0;
    }

    /* Verify KV cache has non-zero entries */
    int key_nonzero = 0;
    int val_nonzero = 0;
    uint32_t kv_per_layer = tf.cfg.max_seq_len * tf.cfg.dim;

    for (uint32_t l = 0; l < tf.cfg.n_layers && (!key_nonzero || !val_nonzero); l++) {
        for (uint32_t i = 0; i < 7 * tf.cfg.dim; i++) {
            if (tf.key_cache[l * kv_per_layer + i] != 0.0f)
                key_nonzero = 1;
            if (tf.value_cache[l * kv_per_layer + i] != 0.0f)
                val_nonzero = 1;
        }
    }

    if (!key_nonzero || !val_nonzero) {
        printf("FAIL: KV cache empty (key_nz=%d, val_nz=%d)\n",
               key_nonzero, val_nonzero);
        transformer_destroy(&tf);
        return 0;
    }

    printf("KV cache pos=%u, non-zero entries confirmed\n", tf.pos);
    printf("KV cache OK\n");

    transformer_destroy(&tf);
    return 1;
}

/* --- Main --- */

int main(void) {
    printf("=== infertest: transformer inference runtime test ===\n");

    int pass = 1;

    if (!test_forward_pass())
        pass = 0;

    if (!test_generation())
        pass = 0;

    if (!test_kv_cache())
        pass = 0;

    if (pass)
        printf("Inference runtime OK\n");

    printf("=== infertest: %s ===\n", pass ? "ALL PASSED" : "SOME FAILED");

    return pass ? 0 : 1;
}
