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

static void tf_reset(transformer_t *tf) {
    tf->pos = 0;
    uint32_t kv_count = 2 * tf->cfg.n_layers * tf->cfg.max_seq_len * tf->cfg.dim;
    memset(tf->kv_buf, 0, kv_count * sizeof(float));
}

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

static int init_model(transformer_t *tf) {
    if (transformer_load(tf, &cfg, "/model.bin") == 0) {
        printf("Loaded trained model from /model.bin\n");
        return 0;
    }
    printf("No trained model found, using random init\n");
    return transformer_init(tf, &cfg, 42);
}

/* Try to generate via the inference service (inferd) */
static int remote_generate(const char *prompt, int len) {
    char resp[512];
    long ret = sys_infer_request("default", prompt, (unsigned long)len,
                                 resp, sizeof(resp) - 1);
    if (ret <= 0) return 0;  /* no service or error */

    resp[ret] = '\0';
    printf("[gen] %s\n", resp);
    return 1;
}

/* Generate locally using transformer */
static void local_generate(transformer_t *tf, tok_config_t *tok,
                           const char *prompt, int len) {
    uint32_t tokens[MAX_TOKENS];
    uint32_t n_tok = tok_encode(tok, prompt, (uint32_t)len, tokens, MAX_TOKENS);
    if (n_tok == 0) {
        printf("(no encodable characters)\n");
        return;
    }

    tf_reset(tf);
    transformer_seed_rng((uint64_t)len * 31 + 7);

    float *logits = NULL;
    for (uint32_t i = 0; i < n_tok; i++)
        logits = transformer_forward(tf, tokens[i]);

    printf("[gen] ");
    for (int i = 0; i < GEN_TOKENS && logits; i++) {
        uint32_t best = transformer_sample(logits, tf->cfg.vocab_size, 0.8f, 20);
        char c = (best < tok->vocab_size) ? tok->chars[best] : '?';
        printf("%c", c);
        logits = transformer_forward(tf, best);
    }
    printf("\n");
}

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

    const char *test_input = "Hello";
    uint32_t tokens[MAX_TOKENS];
    uint32_t n_tok = tok_encode(&tok, test_input, 5, tokens, MAX_TOKENS);
    printf("encoded \"%s\" -> %u tokens\n", test_input, n_tok);

    float *logits = NULL;
    for (uint32_t i = 0; i < n_tok; i++)
        logits = transformer_forward(&tf, tokens[i]);

    if (!logits) {
        printf("FAIL: forward pass returned NULL\n");
        transformer_destroy(&tf);
        return 1;
    }

    printf("generating %d tokens: ", GEN_TOKENS);
    for (int i = 0; i < GEN_TOKENS; i++) {
        uint32_t best = transformer_sample(logits, tf.cfg.vocab_size, 0.0f, 1);
        char c = (best < tok.vocab_size) ? tok.chars[best] : '?';
        printf("%c", c);
        logits = transformer_forward(&tf, best);
    }
    printf("\n");

    transformer_destroy(&tf);
    printf("=== generate: self-test PASSED ===\n");
    return 0;
}

static void interactive(void) {
    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    int have_local = (init_model(&tf) == 0);

    /* Check if inference service is available */
    char probe_resp[16];
    int have_remote = (sys_infer_request("default", "?", 1,
                                          probe_resp, sizeof(probe_resp)) > 0);

    printf("\n");
    printf("========================================\n");
    printf("  Limnx Text Generator\n");
    printf("========================================\n");
    if (have_remote)
        printf("  Mode: inference service (inferd)\n");
    else if (have_local)
        printf("  Mode: local model\n");
    else {
        printf("  No model available. Run inferd first.\n");
        return;
    }
    printf("Type a prompt and press Enter.\n");
    printf("Type 'quit' to exit.\n\n");

    char line[MAX_INPUT];
    for (;;) {
        printf("prompt> ");
        int len = readline(line, MAX_INPUT);
        if (len == 0) continue;
        if (strcmp(line, "quit") == 0) break;

        /* Try remote first, fall back to local */
        if (have_remote) {
            if (remote_generate(line, len))
                continue;
            printf("(service unavailable, using local)\n");
        }
        if (have_local)
            local_generate(&tf, &tok, line, len);
    }

    if (have_local) transformer_destroy(&tf);
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
