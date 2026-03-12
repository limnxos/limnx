#include "libc/libc.h"

#define MAX_INPUT    128
#define MAX_TOKENS   128
#define DIM          48
#define TRAIN_ITERS  50
#define PERTURB_PER_ITER 10
#define GEN_TOKENS   32

static tf_config_t cfg = {
    .dim        = DIM,
    .hidden_dim = 128,
    .n_heads    = 4,
    .n_layers   = 2,
    .vocab_size = 96,
    .max_seq_len = 64,
};

/* Reset transformer state */
static void tf_reset(transformer_t *tf) {
    tf->pos = 0;
    uint32_t kv_count = 2 * tf->cfg.n_layers * tf->cfg.max_seq_len * tf->cfg.dim;
    memset(tf->kv_buf, 0, kv_count * sizeof(float));
}

/* Compute total weight count */
static uint32_t weight_count(const tf_config_t *c) {
    uint32_t d = c->dim, h = c->hidden_dim, nl = c->n_layers, v = c->vocab_size;
    return v * d + nl * d + nl * d * d * 4 + nl * d
           + nl * d * h + nl * h * d + d + d * v;
}

/* Compute average loss over token sequence: -log(softmax[correct_next]) */
static float compute_loss(transformer_t *tf, const uint32_t *tokens,
                           uint32_t n_tokens) {
    if (n_tokens < 2) return 100.0f;

    tf_reset(tf);
    float total_loss = 0.0f;
    uint32_t count = 0;

    for (uint32_t i = 0; i < n_tokens - 1; i++) {
        float *logits = transformer_forward(tf, tokens[i]);
        if (!logits) continue;

        /* Compute softmax of logits for the correct next token */
        uint32_t target = tokens[i + 1];

        /* Find max for numerical stability */
        float max_val = logits[0];
        for (uint32_t j = 1; j < tf->cfg.vocab_size; j++)
            if (logits[j] > max_val)
                max_val = logits[j];

        /* Compute log-sum-exp */
        float sum_exp = 0.0f;
        for (uint32_t j = 0; j < tf->cfg.vocab_size; j++)
            sum_exp += expf(logits[j] - max_val);

        float log_prob = (logits[target] - max_val) - logf(sum_exp);
        total_loss += -log_prob;
        count++;
    }

    return (count > 0) ? total_loss / (float)count : 100.0f;
}

/* Compute loss with context prefix: feed context tokens without loss,
 * then accumulate loss only on the remaining training tokens. */
static float compute_loss_with_context(transformer_t *tf, const uint32_t *tokens,
                                        uint32_t n_tokens, uint32_t context_len) {
    if (context_len == 0)
        return compute_loss(tf, tokens, n_tokens);
    if (n_tokens < context_len + 2)
        return 100.0f;

    tf_reset(tf);

    /* Feed context tokens without accumulating loss */
    for (uint32_t i = 0; i < context_len; i++)
        transformer_forward(tf, tokens[i]);

    /* Accumulate loss on training tokens only */
    float total_loss = 0.0f;
    uint32_t count = 0;

    for (uint32_t i = context_len; i < n_tokens - 1; i++) {
        float *logits = transformer_forward(tf, tokens[i]);
        if (!logits) continue;

        uint32_t target = tokens[i + 1];

        float max_val = logits[0];
        for (uint32_t j = 1; j < tf->cfg.vocab_size; j++)
            if (logits[j] > max_val)
                max_val = logits[j];

        float sum_exp = 0.0f;
        for (uint32_t j = 0; j < tf->cfg.vocab_size; j++)
            sum_exp += expf(logits[j] - max_val);

        float log_prob = (logits[target] - max_val) - logf(sum_exp);
        total_loss += -log_prob;
        count++;
    }

    return (count > 0) ? total_loss / (float)count : 100.0f;
}

/* Read a line with echo */
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

/* Generate and print tokens starting from a given token */
static void generate_from(transformer_t *tf, tok_config_t *tok,
                           uint32_t start_token, int count) {
    tf_reset(tf);
    float *logits = transformer_forward(tf, start_token);

    /* Print start char */
    char c = (start_token < tok->vocab_size) ? tok->chars[start_token] : '?';
    printf("%c", c);

    for (int i = 0; i < count && logits; i++) {
        uint32_t best = 0;
        float best_val = logits[0];
        for (uint32_t j = 1; j < tf->cfg.vocab_size; j++) {
            if (logits[j] > best_val) {
                best_val = logits[j];
                best = j;
            }
        }
        c = (best < tok->vocab_size) ? tok->chars[best] : '?';
        printf("%c", c);
        logits = transformer_forward(tf, best);
    }
    printf("\n");
}

int main(void) {
    printf("\n");
    printf("========================================\n");
    printf("  Limnx Learning Lab\n");
    printf("========================================\n");
    printf("Train a transformer on character patterns.\n\n");

    tok_config_t tok;
    tok_default_config(&tok);

    transformer_t tf;
    if (transformer_init(&tf, &cfg, 42) != 0) {
        printf("FAIL: transformer_init\n");
        return 1;
    }

    printf("Transformer initialized (dim=%u, vocab=%u)\n", tf.cfg.dim, tf.cfg.vocab_size);

    /* Ask for training text */
    printf("Enter training text (or press Enter for default):\n");
    printf("text> ");
    char text[MAX_INPUT];
    int tlen = readline(text, MAX_INPUT);

    if (tlen == 0) {
        const char *def = "the cat sat on the mat";
        tlen = 0;
        while (def[tlen]) { text[tlen] = def[tlen]; tlen++; }
        text[tlen] = '\0';
        printf("Using default: \"%s\"\n", text);
    }

    /* Encode training text */
    uint32_t tokens[MAX_TOKENS];
    uint32_t n_tok = tok_encode(&tok, text, (uint32_t)tlen, tokens, MAX_TOKENS);
    printf("Encoded %u tokens\n", n_tok);

    if (n_tok < 2) {
        printf("Need at least 2 tokens to train.\n");
        transformer_destroy(&tf);
        return 1;
    }

    /* Show pre-training generation */
    printf("\nBefore training (from '%c'):\n  ", text[0]);
    generate_from(&tf, &tok, tokens[0], GEN_TOKENS);

    /* Compute initial loss */
    float loss = compute_loss(&tf, tokens, n_tok);
    printf("\nInitial loss: %.4f\n", (double)loss);

    /* Initialize vecstore for RAG-conditioned training */
    vecstore_t vs;
    int has_rag = (vecstore_init(&vs, DIM) == 0);
    if (has_rag) {
        /* Seed vecstore with training text segments */
        float emb[DIM];
        tf_reset(&tf);
        for (uint32_t i = 0; i < n_tok; i++)
            transformer_forward(&tf, tokens[i]);
        memcpy(emb, tf.x, DIM * sizeof(float));
        vecstore_store(&vs, text, emb);

        /* Store first half */
        if (tlen > 4) {
            int half = tlen / 2;
            char half_text[MAX_INPUT];
            memcpy(half_text, text, (size_t)half);
            half_text[half] = '\0';
            tf_reset(&tf);
            uint32_t htokens[MAX_TOKENS];
            uint32_t hn = tok_encode(&tok, half_text, (uint32_t)half, htokens, MAX_TOKENS);
            for (uint32_t i = 0; i < hn; i++)
                transformer_forward(&tf, htokens[i]);
            memcpy(emb, tf.x, DIM * sizeof(float));
            vecstore_store(&vs, half_text, emb);
        }

        /* Store second half */
        if (tlen > 4) {
            int half = tlen / 2;
            const char *second = text + half;
            int slen = tlen - half;
            tf_reset(&tf);
            uint32_t htokens[MAX_TOKENS];
            uint32_t hn = tok_encode(&tok, second, (uint32_t)slen, htokens, MAX_TOKENS);
            for (uint32_t i = 0; i < hn; i++)
                transformer_forward(&tf, htokens[i]);
            memcpy(emb, tf.x, DIM * sizeof(float));
            char sbuf[MAX_INPUT];
            if (slen >= MAX_INPUT) slen = MAX_INPUT - 1;
            memcpy(sbuf, second, (size_t)slen);
            sbuf[slen] = '\0';
            vecstore_store(&vs, sbuf, emb);
        }

        printf("RAG context seeded (%u entries)\n", vecstore_count(&vs));
    }

    /* Training loop: hill climbing with random weight perturbation */
    printf("\nTraining (%d iterations)...\n", TRAIN_ITERS);

    uint32_t seed = 12345;
    uint32_t wc = weight_count(&cfg);
    uint32_t accepted = 0;

    /* Context budget: up to half of max_seq_len (32 chars) */
    int ctx_max_chars = (int)cfg.max_seq_len / 2;

    for (int iter = 0; iter < TRAIN_ITERS; iter++) {
        for (int p = 0; p < PERTURB_PER_ITER; p++) {
            /* Pick a random weight */
            uint32_t idx = prng_next(&seed) % wc;
            float old_val = tf.weights_buf[idx];
            float delta = prng_float(&seed) * 0.05f;

            /* Perturb */
            tf.weights_buf[idx] += delta;

            /* 50% chance: inject RAG context prefix */
            float new_loss;
            if (has_rag && vecstore_count(&vs) > 0 && (prng_next(&seed) % 2) == 0) {
                /* Pick a random vecstore entry as context */
                uint32_t vs_idx = prng_next(&seed) % vecstore_count(&vs);
                const char *ctx_text = vs.entries[vs_idx].key;
                int ctx_len = (int)strlen(ctx_text);
                if (ctx_len > ctx_max_chars)
                    ctx_len = ctx_max_chars;

                /* Build: context + '\n' + training text */
                uint32_t combined[MAX_TOKENS];
                uint32_t cn = tok_encode(&tok, ctx_text, (uint32_t)ctx_len,
                                          combined, MAX_TOKENS / 2);
                uint32_t ctx_tok_len = cn;
                /* Add separator token ('\n') */
                if (cn < MAX_TOKENS - 1) {
                    uint32_t sep_tok[4];
                    uint32_t sn = tok_encode(&tok, "\n", 1, sep_tok, 4);
                    for (uint32_t s = 0; s < sn && cn < MAX_TOKENS - 1; s++)
                        combined[cn++] = sep_tok[s];
                }
                /* Add training tokens */
                for (uint32_t t = 0; t < n_tok && cn < MAX_TOKENS; t++)
                    combined[cn++] = tokens[t];

                new_loss = compute_loss_with_context(&tf, combined, cn, ctx_tok_len);
            } else {
                new_loss = compute_loss(&tf, tokens, n_tok);
            }

            if (new_loss < loss) {
                /* Keep perturbation */
                loss = new_loss;
                accepted++;
            } else {
                /* Revert */
                tf.weights_buf[idx] = old_val;
            }
        }

        if ((iter + 1) % 10 == 0 || iter == 0)
            printf("  iter %3d: loss=%.4f (accepted=%u)\n",
                   iter + 1, (double)loss, accepted);
    }

    /* Show post-training generation */
    printf("\nAfter training (from '%c'):\n  ", text[0]);
    generate_from(&tf, &tok, tokens[0], GEN_TOKENS);
    printf("Final loss: %.4f\n", (double)loss);

    /* Show generation with context prefix */
    if (has_rag && vecstore_count(&vs) > 0) {
        printf("\nWith context prefix:\n");
        const char *ctx = vs.entries[0].key;
        int clen = (int)strlen(ctx);
        if (clen > ctx_max_chars) clen = ctx_max_chars;
        printf("  context: \"%.*s\"\n  ", clen, ctx);

        tf_reset(&tf);
        uint32_t ctokens[MAX_TOKENS];
        uint32_t cn = tok_encode(&tok, ctx, (uint32_t)clen, ctokens, MAX_TOKENS / 2);
        /* Feed context */
        for (uint32_t i = 0; i < cn; i++)
            transformer_forward(&tf, ctokens[i]);
        /* Generate from training start token */
        float *logits = transformer_forward(&tf, tokens[0]);
        char c = (tokens[0] < tok.vocab_size) ? tok.chars[tokens[0]] : '?';
        printf("%c", c);
        for (int i = 0; i < GEN_TOKENS && logits; i++) {
            uint32_t best = 0;
            float best_val = logits[0];
            for (uint32_t j = 1; j < tf.cfg.vocab_size; j++) {
                if (logits[j] > best_val) {
                    best_val = logits[j];
                    best = j;
                }
            }
            c = (best < tok.vocab_size) ? tok.chars[best] : '?';
            printf("%c", c);
            logits = transformer_forward(&tf, best);
        }
        printf("\n");
    }

    /* Save model */
    printf("\nSaving model to /model.bin...\n");
    if (transformer_save(&tf, "/model.bin") == 0)
        printf("Model saved OK\n");
    else
        printf("Save failed (VFS write error)\n");

    /* Offer to load */
    printf("\nLoad saved model? (y/n): ");
    char ans[8];
    int alen = readline(ans, 8);
    if (alen > 0 && ans[0] == 'y') {
        transformer_destroy(&tf);
        tf_config_t load_cfg;
        if (transformer_load(&tf, &load_cfg, "/model.bin") == 0) {
            printf("Model loaded (dim=%u, vocab=%u)\n",
                   load_cfg.dim, load_cfg.vocab_size);
            printf("Generating from loaded model:\n  ");
            generate_from(&tf, &tok, tokens[0], GEN_TOKENS);
        } else {
            printf("Load failed\n");
        }
    }

    if (has_rag)
        vecstore_destroy(&vs);
    transformer_destroy(&tf);
    printf("Done.\n");
    return 0;
}
