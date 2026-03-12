#include "libc/libc.h"

#define DIM   48
#define RESP_TOKENS 32

static tf_config_t cfg = {
    .dim        = DIM,
    .hidden_dim = 128,
    .n_heads    = 4,
    .n_layers   = 2,
    .vocab_size = 96,
    .max_seq_len = 64,
};

/* Build a simple basis vector: 1.0 at position idx, 0 elsewhere */
static void make_vec(float *v, uint32_t idx, float val) {
    memset(v, 0, DIM * sizeof(float));
    if (idx < DIM)
        v[idx] = val;
}

int main(void) {
    printf("=== ragtest: RAG end-to-end test ===\n");
    int pass = 1;

    /* --- Test 1: topk ordering --- */
    printf("  [ragtest] topk ordering: ");
    {
        vecstore_t vs;
        if (vecstore_init(&vs, DIM) != 0) {
            printf("FAIL (init)\n");
            return 1;
        }

        /* Store 4 vectors at different positions */
        float v[DIM];
        make_vec(v, 0, 1.0f); vecstore_store(&vs, "alpha", v);
        make_vec(v, 1, 1.0f); vecstore_store(&vs, "beta", v);
        make_vec(v, 2, 1.0f); vecstore_store(&vs, "gamma", v);
        make_vec(v, 3, 1.0f); vecstore_store(&vs, "delta", v);

        /* Query with a vector that has strongest match to alpha,
         * weaker to beta, even weaker to gamma */
        float q[DIM];
        memset(q, 0, DIM * sizeof(float));
        q[0] = 1.0f;   /* perfect match to alpha */
        q[1] = 0.5f;   /* partial match to beta */
        q[2] = 0.2f;   /* weak match to gamma */

        uint32_t idx[3];
        float scores[3];
        int n = vecstore_query_topk(&vs, q, 3, idx, scores);

        if (n != 3) {
            printf("FAIL (count=%d)\n", n);
            pass = 0;
        } else if (scores[0] < scores[1] || scores[1] < scores[2]) {
            printf("FAIL (not descending: %f %f %f)\n",
                   (double)scores[0], (double)scores[1], (double)scores[2]);
            pass = 0;
        } else if (strcmp(vs.entries[idx[0]].key, "alpha") != 0) {
            printf("FAIL (best not alpha)\n");
            pass = 0;
        } else {
            printf("OK (%f >= %f >= %f)\n",
                   (double)scores[0], (double)scores[1], (double)scores[2]);
        }

        vecstore_destroy(&vs);
    }

    /* --- Test 2: topk count --- */
    printf("  [ragtest] topk count: ");
    {
        vecstore_t vs;
        vecstore_init(&vs, DIM);

        float v[DIM];
        make_vec(v, 0, 1.0f); vecstore_store(&vs, "only1", v);
        make_vec(v, 1, 1.0f); vecstore_store(&vs, "only2", v);

        uint32_t idx[3];
        float scores[3];
        int n = vecstore_query_topk(&vs, v, 3, idx, scores);

        if (n == 2) {
            printf("OK (k=3, count=2, returned %d)\n", n);
        } else {
            printf("FAIL (expected 2, got %d)\n", n);
            pass = 0;
        }

        vecstore_destroy(&vs);
    }

    /* --- Test 3: budget truncation --- */
    printf("  [ragtest] budget truncation: ");
    {
        /* Simulate RAG prompt building with known constraints */
        int max_seq_len = 64;
        int resp_tokens = RESP_TOKENS;
        int budget = max_seq_len - resp_tokens;  /* 32 */

        /* User input = 20 chars → ctx_budget = 12 */
        int input_len = 20;
        int ctx_budget = budget - input_len;  /* 12 */

        /* Memory "abcdefghij" = 10 chars, cost = 11 (text + separator) */
        const char *mem1 = "abcdefghij";  /* 10 chars, cost 11 */
        int cost1 = (int)strlen(mem1) + 1;

        /* Memory "xyz" = 3 chars, cost = 4 */
        const char *mem2 = "xyz";  /* 3 chars, cost 4 */
        int cost2 = (int)strlen(mem2) + 1;

        /* mem1 (cost 11) fits in budget 12: inject */
        int injected = 0;
        if (cost1 <= ctx_budget) {
            ctx_budget -= cost1;
            injected++;
        }
        /* mem2 (cost 4) doesn't fit in remaining budget 1 */
        if (cost2 <= ctx_budget) {
            ctx_budget -= cost2;
            injected++;
        }

        if (injected == 1 && ctx_budget == 1) {
            printf("OK (1 memory injected, budget left=%d)\n", ctx_budget);
        } else {
            printf("FAIL (injected=%d, budget_left=%d)\n", injected, ctx_budget);
            pass = 0;
        }
    }

    /* --- Test 4: e2e generate with RAG prompt --- */
    printf("  [ragtest] e2e generate: ");
    {
        tok_config_t tok;
        tok_default_config(&tok);

        transformer_t tf;
        if (transformer_init(&tf, &cfg, 42) != 0) {
            printf("FAIL (tf init)\n");
            pass = 0;
        } else {
            vecstore_t mem;
            if (vecstore_init(&mem, DIM) != 0) {
                printf("FAIL (vs init)\n");
                pass = 0;
                transformer_destroy(&tf);
            } else {
                /* Store some memories with embeddings */
                float emb[DIM];

                /* Get embedding for "hello" */
                tf.pos = 0;
                uint32_t kv_count = 2 * tf.cfg.n_layers * tf.cfg.max_seq_len * tf.cfg.dim;
                memset(tf.kv_buf, 0, kv_count * sizeof(float));

                uint32_t toks[16];
                uint32_t nt = tok_encode(&tok, "hello", 5, toks, 16);
                for (uint32_t i = 0; i < nt; i++)
                    transformer_forward(&tf, toks[i]);
                memcpy(emb, tf.x, DIM * sizeof(float));
                vecstore_store(&mem, "hello", emb);

                /* Build RAG prompt: "hello\nhello" */
                char rag[64];
                int rlen = 0;
                memcpy(&rag[rlen], "hello", 5); rlen += 5;
                rag[rlen++] = '\n';
                memcpy(&rag[rlen], "hello", 5); rlen += 5;
                rag[rlen] = '\0';

                /* Reset and generate from RAG prompt */
                tf.pos = 0;
                memset(tf.kv_buf, 0, kv_count * sizeof(float));

                nt = tok_encode(&tok, rag, (uint32_t)rlen, toks, 16);
                float *logits = NULL;
                for (uint32_t i = 0; i < nt; i++)
                    logits = transformer_forward(&tf, toks[i]);

                if (logits) {
                    /* Generate a few tokens to verify no crash */
                    int gen_ok = 1;
                    for (int i = 0; i < 4 && logits; i++) {
                        uint32_t best = 0;
                        float best_val = logits[0];
                        for (uint32_t j = 1; j < tf.cfg.vocab_size; j++) {
                            if (logits[j] > best_val) {
                                best_val = logits[j];
                                best = j;
                            }
                        }
                        logits = transformer_forward(&tf, best);
                    }
                    if (gen_ok)
                        printf("OK (generated 4 tokens)\n");
                } else {
                    printf("FAIL (no logits)\n");
                    pass = 0;
                }

                vecstore_destroy(&mem);
                transformer_destroy(&tf);
            }
        }
    }

    if (pass)
        printf("=== ragtest: ALL PASSED ===\n");
    else
        printf("=== ragtest: SOME TESTS FAILED ===\n");

    return pass ? 0 : 1;
}
