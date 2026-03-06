#include "libc/libc.h"

#define GEN_TOKENS   32
#define MAX_INPUT    128
#define MAX_TOKENS   128

static tf_config_t cfg = {
    .dim        = 48,
    .hidden_dim = 128,
    .n_heads    = 4,
    .n_layers   = 2,
    .vocab_size = 96,
    .max_seq_len = 64,
};

/* Reset transformer for a new prompt */
static void tf_reset(transformer_t *tf) {
    tf->pos = 0;
    uint32_t kv_count = 2 * tf->cfg.n_layers * tf->cfg.max_seq_len * tf->cfg.dim;
    memset(tf->kv_buf, 0, kv_count * sizeof(float));
}

/* Read a line from serial, with echo */
static int readline(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        long ch = sys_getchar();
        if (ch == '\n' || ch == '\r') {
            char nl = '\n';
            sys_write(&nl, 1);
            break;
        } else if (ch == '\b' || ch == 127) {
            if (pos > 0) {
                pos--;
                sys_write("\b \b", 3);
            }
        } else if (ch >= 32 && ch < 127) {
            buf[pos++] = (char)ch;
            char c = (char)ch;
            sys_write(&c, 1);
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* Try to load trained model, fall back to random init */
static int init_model(transformer_t *tf) {
    if (transformer_load(tf, &cfg, "/model.bin") == 0) {
        printf("Loaded trained model from /model.bin\n");
        return 0;
    }
    printf("No trained model found, using random init\n");
    return transformer_init(tf, &cfg, 42);
}

/* Run automated self-test (no user input needed) */
static int self_test(void) {
    printf("=== generate: self-test ===\n");

    tok_config_t tok;
    tok_default_config(&tok);
    printf("tokenizer: vocab_size=%u\n", tok.vocab_size);

    transformer_t tf;
    if (init_model(&tf) != 0) {
        printf("FAIL: transformer_init\n");
        return 1;
    }
    printf("transformer: dim=%u, layers=%u, vocab=%u\n",
           tf.cfg.dim, tf.cfg.n_layers, tf.cfg.vocab_size);

    /* Encode test input */
    const char *test_input = "Hello";
    uint32_t tokens[MAX_TOKENS];
    uint32_t n_tok = tok_encode(&tok, test_input, 5, tokens, MAX_TOKENS);
    printf("encoded \"%s\" -> %u tokens\n", test_input, n_tok);

    /* Feed input tokens */
    float *logits = NULL;
    for (uint32_t i = 0; i < n_tok; i++)
        logits = transformer_forward(&tf, tokens[i]);

    if (!logits) {
        printf("FAIL: forward pass returned NULL\n");
        transformer_destroy(&tf);
        return 1;
    }

    /* Generate continuation */
    printf("generating %d tokens: ", GEN_TOKENS);
    uint32_t gen_tokens[GEN_TOKENS + 1];
    for (int i = 0; i < GEN_TOKENS; i++) {
        /* Argmax */
        uint32_t best = 0;
        float best_val = logits[0];
        for (uint32_t j = 1; j < tf.cfg.vocab_size; j++) {
            if (logits[j] > best_val) {
                best_val = logits[j];
                best = j;
            }
        }
        gen_tokens[i] = best;
        logits = transformer_forward(&tf, best);
    }

    /* Decode and print */
    char out[GEN_TOKENS + 1];
    tok_decode(&tok, gen_tokens, GEN_TOKENS, out, GEN_TOKENS + 1);
    printf("%s\n", out);

    transformer_destroy(&tf);
    printf("=== generate: self-test PASSED ===\n");
    return 0;
}

/* Interactive generation loop */
static void interactive(void) {
    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    if (init_model(&tf) != 0) {
        printf("FAIL: transformer_init\n");
        return;
    }

    printf("\n");
    printf("========================================\n");
    printf("  Limnx Text Generator\n");
    printf("========================================\n");
    printf("Type a prompt and press Enter.\n");
    printf("The model will generate a continuation.\n");
    printf("Type 'quit' to exit.\n\n");

    char line[MAX_INPUT];
    uint32_t tokens[MAX_TOKENS];

    for (;;) {
        printf("prompt> ");
        int len = readline(line, MAX_INPUT);
        if (len == 0)
            continue;
        if (strcmp(line, "quit") == 0)
            break;

        /* Encode input */
        uint32_t n_tok = tok_encode(&tok, line, (uint32_t)len, tokens, MAX_TOKENS);
        if (n_tok == 0) {
            printf("(no encodable characters)\n");
            continue;
        }

        /* Reset transformer state for new prompt */
        tf_reset(&tf);

        /* Feed input tokens */
        float *logits = NULL;
        for (uint32_t i = 0; i < n_tok; i++)
            logits = transformer_forward(&tf, tokens[i]);

        /* Generate continuation */
        printf("[gen] ");
        for (int i = 0; i < GEN_TOKENS && logits; i++) {
            uint32_t best = 0;
            float best_val = logits[0];
            for (uint32_t j = 1; j < tf.cfg.vocab_size; j++) {
                if (logits[j] > best_val) {
                    best_val = logits[j];
                    best = j;
                }
            }
            /* Decode single token */
            char c = (best < tok.vocab_size) ? tok.chars[best] : '?';
            printf("%c", c);
            logits = transformer_forward(&tf, best);
        }
        printf("\n");
    }

    transformer_destroy(&tf);
    printf("Goodbye.\n");
}

int main(int argc, char **argv) {
    int test_only = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--test") == 0) test_only = 1;

    int rc = self_test();
    if (test_only)
        return rc;

    interactive();
    return 0;
}
