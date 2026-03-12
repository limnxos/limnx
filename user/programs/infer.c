#include "libc/libc.h"

#define GEN_TOKENS   32
#define MAX_INPUT    256
#define MAX_TOKENS   128
#define SERVER_PORT  9000

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

static int init_model(transformer_t *tf) {
    if (transformer_load(tf, &cfg, "/model.bin") == 0) {
        printf("[infer] Loaded trained model from /model.bin\n");
        return 0;
    }
    printf("[infer] No trained model found, using random init\n");
    return transformer_init(tf, &cfg, 42);
}

/* Generate continuation text from input */
static int generate(transformer_t *tf, tok_config_t *tok,
                    const char *input, int input_len,
                    char *output, int max_out, int gen_count) {
    tf_reset(tf);

    uint32_t tokens[MAX_TOKENS];
    uint32_t n_tok = tok_encode(tok, input, (uint32_t)input_len,
                                tokens, MAX_TOKENS);
    if (n_tok == 0) {
        output[0] = '\0';
        return 0;
    }

    /* Feed input tokens */
    float *logits = NULL;
    for (uint32_t i = 0; i < n_tok; i++)
        logits = transformer_forward(tf, tokens[i]);

    if (!logits) {
        output[0] = '\0';
        return 0;
    }

    /* Generate continuation */
    uint32_t gen_tokens[64];
    int count = gen_count;
    if (count > 64) count = 64;
    for (int i = 0; i < count && logits; i++) {
        uint32_t best = 0;
        float best_val = logits[0];
        for (uint32_t j = 1; j < tf->cfg.vocab_size; j++) {
            if (logits[j] > best_val) {
                best_val = logits[j];
                best = j;
            }
        }
        gen_tokens[i] = best;
        logits = transformer_forward(tf, best);
    }

    tok_decode(tok, gen_tokens, (uint32_t)count, output, (uint32_t)max_out);
    return count;
}

/* Self-test: load model, generate, exit */
static int self_test(void) {
    printf("[infer] self-test start\n");

    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    if (init_model(&tf) != 0) {
        printf("[infer] FAIL: model init\n");
        return 1;
    }

    char output[64];
    int n = generate(&tf, &tok, "Hello", 5, output, 63, 16);
    printf("[infer] generated %d tokens: %s\n", n, output);

    transformer_destroy(&tf);
    printf("[infer] self-test PASSED\n");
    return 0;
}

/* Server mode: listen on UDP port and serve inference */
static void server(void) {
    printf("[infer] starting server on port %d\n", SERVER_PORT);

    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    if (init_model(&tf) != 0) {
        printf("[infer] FAIL: model init, exiting\n");
        return;
    }

    long sockfd = sys_socket();
    if (sockfd < 0) {
        printf("[infer] FAIL: socket\n");
        transformer_destroy(&tf);
        return;
    }

    if (sys_bind(sockfd, SERVER_PORT) < 0) {
        printf("[infer] FAIL: bind port %d\n", SERVER_PORT);
        sys_close(sockfd);
        transformer_destroy(&tf);
        return;
    }

    printf("[infer] listening on UDP port %d\n", SERVER_PORT);

    char recv_buf[MAX_INPUT];
    char resp_buf[128];

    for (;;) {
        uint32_t src_ip = 0;
        uint16_t src_port = 0;
        long n = sys_recvfrom(sockfd, recv_buf, MAX_INPUT - 1,
                              &src_ip, &src_port);
        if (n <= 0) {
            sys_yield();
            continue;
        }
        recv_buf[n] = '\0';

        /* Generate response */
        generate(&tf, &tok, recv_buf, (int)n, resp_buf, 127, GEN_TOKENS);

        /* Send response back */
        int resp_len = (int)strlen(resp_buf);
        sys_sendto(sockfd, resp_buf, (unsigned long)resp_len,
                   (unsigned long)src_ip, (unsigned long)src_port);

        printf("[infer] %lu.%lu.%lu.%lu:%u \"%s\" -> \"%s\"\n",
               (unsigned long)(src_ip & 0xFF),
               (unsigned long)((src_ip >> 8) & 0xFF),
               (unsigned long)((src_ip >> 16) & 0xFF),
               (unsigned long)((src_ip >> 24) & 0xFF),
               src_port, recv_buf, resp_buf);
    }
}

int main(int argc, char **argv) {
    int test_only = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--test") == 0) test_only = 1;

    int rc = self_test();
    if (test_only)
        return rc;

    /* Interactive server mode when launched from shell */
    server();
    return 0;
}
